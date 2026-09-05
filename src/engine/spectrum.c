/*
 * spectrum.c — see spectrum.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "spectrum.h"

#include <fftw3.h>
#include <math.h>
#include <string.h>

struct _SkimSpectrum {
  guint          n;                            /* FFT size (power of two)    */
  guint          hop;                          /* frames per row             */
  double         rate;
  float         *win;                          /* Hann, n                    */
  double        *w2cum;                        /* prefix sums of win², n+1   */
  float         *ring;                         /* n complex frames, circular */
  guint          head;                         /* next write slot            */
  guint          filled;                       /* frames ever written, ≤ n   */
  guint          since;                        /* frames since the last row  */
  guint64        total;                        /* frames ever pushed         */
  double         cur_hz;                       /* centre of the newest frame */
  struct { guint64 at; double hz; } tr[16];    /* centre transitions: from
                                                * frame `at` on, `hz`        */
  guint          ntr;
  fftwf_complex *in, *out;
  fftwf_plan     plan;
  guint8        *row;
  float          norm_db;                      /* −20·log10(Σw / 2): a full-
                                                * scale tone at bin centre
                                                * reads 0 dBFS               */
  SkimSpectrumRowCb cb;
  gpointer          user;
};

static guint pick_n(double rate) {
  /* Nearest power of two to rate / BIN_HZ, so the bin width is the same at
   * every rate the radio can announce (48 k → 2048 … 384 k → 16384). */
  double want = rate / SKIM_SPECTRUM_BIN_HZ;
  guint  n    = 1u << (guint)lrint(log2(MAX(want, 64.0)));
  return n;
}

SkimSpectrum *skim_spectrum_new(double rate) {
  SkimSpectrum *s = g_new0(SkimSpectrum, 1);
  s->rate = rate;
  s->n    = pick_n(rate);
  s->hop  = MAX(s->n / SKIM_SPECTRUM_HOP_DIV, 1u);
  s->win  = g_new(float, s->n);
  s->ring = g_new0(float, 2 * s->n);
  s->row  = g_new(guint8, s->n);
  s->in   = fftwf_alloc_complex(s->n);
  s->out  = fftwf_alloc_complex(s->n);
  /* FORWARD: the wire is the true spectrum, +f → positive bin. The
   * channelizer's BACKWARD plan belongs to its polyphase structure, not to
   * the orientation of the stream. Same planner flag as the channelizer. */
  s->plan = fftwf_plan_dft_1d((int)s->n, s->in, s->out, FFTW_FORWARD,
                              FFTW_ESTIMATE);
  double wsum = 0.0;
  s->w2cum = g_new(double, s->n + 1);
  s->w2cum[0] = 0.0;
  for (guint i = 0; i < s->n; i++) {
    s->win[i] = 0.5f - 0.5f * cosf((float)(2.0 * G_PI * i / s->n));
    wsum += s->win[i];
    s->w2cum[i + 1] = s->w2cum[i] + (double)s->win[i] * (double)s->win[i];
  }
  s->norm_db = (float)(-20.0 * log10(wsum));  /* |X| of a unit tone = Σw    */
  return s;
}

void skim_spectrum_free(SkimSpectrum *s) {
  if (!s)
    return;
  fftwf_destroy_plan(s->plan);
  fftwf_free(s->in);
  fftwf_free(s->out);
  g_free(s->win);
  g_free(s->w2cum);
  g_free(s->ring);
  g_free(s->row);
  g_free(s);
}

guint  skim_spectrum_bins(const SkimSpectrum *s)   { return s->n; }
double skim_spectrum_bin_hz(const SkimSpectrum *s) { return s->rate / s->n; }
guint  skim_spectrum_hop(const SkimSpectrum *s)    { return s->hop; }

void skim_spectrum_set_row_cb(SkimSpectrum *s, SkimSpectrumRowCb cb, gpointer user) {
  s->cb   = cb;
  s->user = user;
}

void skim_spectrum_reset(SkimSpectrum *s) {
  s->head = s->filled = s->since = 0;
}

static void emit_row(SkimSpectrum *s) {
  const guint n = s->n;
  const guint64 w0 = s->total - (guint64)n;      /* index of the window's frame 0 */

  /* A retune INSIDE the window: the frames before it were captured at another
   * centre, so a line sits at two baseband positions in the same row — drawn
   * on one label that is a spike the height of the step (Richard's second
   * recording, 2026-09-05: vertical bars on every wheel notch, the DC line
   * at S9+20 made them glaring). The row is therefore computed from its
   * LARGEST single-centre segment alone (ties → the newest), zeroing the rest
   * of the window, and labelled with that segment's centre: a discrete step
   * shows a 2-column widening instead of a spike, a knob sweep shows lines
   * ~4× wider while it turns — correctly placed and continuous. */
  guint  seg_lo = 0, seg_hi = n;
  double seg_hz = s->cur_hz;
  {
    guint  first = 0;                            /* first transition inside */
    double hz0   = s->cur_hz;                    /* centre at frame 0       */
    for (guint i = 0; i < s->ntr; i++) {
      if (s->tr[i].at <= w0) { hz0 = s->tr[i].hz; first = i + 1; } else { break; }
    }
    guint lo = 0, best = 0; double hz = hz0;
    for (guint i = first; i <= s->ntr; i++) {
      const guint hi  = (i < s->ntr) ? (guint)(s->tr[i].at - w0) : n;
      const guint len = hi - lo;
      if (len >= best) { best = len; seg_lo = lo; seg_hi = hi; seg_hz = hz; }
      if (i < s->ntr) { lo = hi; hz = s->tr[i].hz; }
    }
  }
  const gboolean cut = seg_lo != 0 || seg_hi != n;

  /* Oldest frame first: the ring's head is the next write slot, i.e. the
   * oldest of the last n frames. A cut row gets a fresh Hann over the kept
   * segment (a chopped edge of the full window would leak at −25 dB — the
   * gate's ghost check caught it); the full row uses the table. */
  double w2kept = 0.0;
  const guint seg_len = seg_hi - seg_lo;
  for (guint i = 0; i < n; i++) {
    const guint j = (s->head + i) % n;
    float w = 0.0f;
    if (!cut) { w = s->win[i]; }
    else if (i >= seg_lo && i < seg_hi) {
      w = 0.5f - 0.5f * cosf((float)(2.0 * G_PI * (double)(i - seg_lo) / (double)seg_len));
      w2kept += (double)w * (double)w;
    }
    s->in[i][0] = s->ring[2 * j]     * w;
    s->in[i][1] = s->ring[2 * j + 1] * w;
  }
  fftwf_execute(s->plan);
  /* A cut row sums fewer noise samples; lift it by Σw²_full / Σw²_kept so
   * the floor reads the same as a full row (a carrier then reads a few dB
   * lower for those rows — nobody reads dB while the knob turns). */
  float norm = s->norm_db;
  if (cut && w2kept > 0.0) { norm += (float)(10.0 * log10(s->w2cum[n] / w2kept)); }
  const guint half = n / 2;
  for (guint i = 0; i < n; i++) {
    const guint k  = (i + half) % n;             /* fftshift               */
    const float re = s->out[k][0], im = s->out[k][1];
    const float pw = re * re + im * im;
    /* 10·log10(pw) = 20·log10(|X|); guard the exact zero of a silent row. */
    const float db = 10.0f * log10f(pw + 1e-30f) + norm;
    float v = db + (float)SKIM_SPECTRUM_DB_OFFSET;
    v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
    s->row[i] = (guint8)(v + 0.5f);
  }
  if (s->cb) {
    s->cb(s->row, n, seg_hz, s->user);
  }
}

void skim_spectrum_push(SkimSpectrum *s, const float *iq, guint nframes, double center_hz) {
  if (nframes == 0) { return; }
  if (s->ntr == 0 || center_hz != s->cur_hz) {
    /* Record the transition at the index of the first frame that carries
     * the new centre. tr[0] always holds the centre of the OLDEST frame the
     * window can still contain, so a row is never labelled with nothing:
     * entries older than a whole window (plus one) are dropped from the
     * front, and the ring is one shorter than the worst case a window can
     * straddle (a change per hop → 4 entries + 1 slack). */
    if (s->ntr == G_N_ELEMENTS(s->tr)) {
      memmove(&s->tr[0], &s->tr[1], (s->ntr - 1) * sizeof(s->tr[0]));
      s->ntr--;
    }
    s->tr[s->ntr].at = s->total;
    s->tr[s->ntr].hz = center_hz;
    s->ntr++;
    s->cur_hz = center_hz;
  }
  /* drop entries no row can need any more (all older than the window) */
  while (s->ntr > 1 && s->tr[1].at + (guint64)s->n <= s->total) {
    memmove(&s->tr[0], &s->tr[1], (s->ntr - 1) * sizeof(s->tr[0]));
    s->ntr--;
  }
  for (guint f = 0; f < nframes; f++) {
    s->ring[2 * s->head]     = iq[2 * f];
    s->ring[2 * s->head + 1] = iq[2 * f + 1];
    s->head = (s->head + 1) % s->n;
    if (s->filled < s->n) { s->filled++; }
    s->since++;
    s->total++;
    if (s->filled == s->n && s->since >= s->hop) {
      s->since = 0;
      emit_row(s);
    }
  }
}
