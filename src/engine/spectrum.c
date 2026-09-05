/*
 * spectrum.c — see spectrum.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "spectrum.h"

#include <fftw3.h>
#include <math.h>

struct _SkimSpectrum {
  guint          n;                            /* FFT size (power of two)    */
  guint          hop;                          /* frames per row             */
  double         rate;
  float         *win;                          /* Hann, n                    */
  float         *ring;                         /* n complex frames, circular */
  guint          head;                         /* next write slot            */
  guint          filled;                       /* frames ever written, ≤ n   */
  guint          since;                        /* frames since the last row  */
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
  for (guint i = 0; i < s->n; i++) {
    s->win[i] = 0.5f - 0.5f * cosf((float)(2.0 * G_PI * i / s->n));
    wsum += s->win[i];
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
  /* Oldest frame first: the ring's head is the next write slot, i.e. the
   * oldest of the last n frames. */
  for (guint i = 0; i < n; i++) {
    const guint j = (s->head + i) % n;
    s->in[i][0] = s->ring[2 * j]     * s->win[i];
    s->in[i][1] = s->ring[2 * j + 1] * s->win[i];
  }
  fftwf_execute(s->plan);
  const guint half = n / 2;
  for (guint i = 0; i < n; i++) {
    const guint k  = (i + half) % n;             /* fftshift               */
    const float re = s->out[k][0], im = s->out[k][1];
    const float pw = re * re + im * im;
    /* 10·log10(pw) = 20·log10(|X|); guard the exact zero of a silent row. */
    const float db = 10.0f * log10f(pw + 1e-30f) + s->norm_db;
    float v = db + (float)SKIM_SPECTRUM_DB_OFFSET;
    v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
    s->row[i] = (guint8)(v + 0.5f);
  }
  if (s->cb) { s->cb(s->row, n, s->user); }
}

void skim_spectrum_push(SkimSpectrum *s, const float *iq, guint nframes) {
  for (guint f = 0; f < nframes; f++) {
    s->ring[2 * s->head]     = iq[2 * f];
    s->ring[2 * s->head + 1] = iq[2 * f + 1];
    s->head = (s->head + 1) % s->n;
    if (s->filled < s->n) { s->filled++; }
    s->since++;
    if (s->filled == s->n && s->since >= s->hop) {
      s->since = 0;
      emit_row(s);
    }
  }
}
