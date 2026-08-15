/* decode_rtty.c — RTTY decode backend (M7, docs/SCOPE.md).
 *
 * Per channel (complex baseband, 500 Hz from the 250 Hz-spaced wide-passband
 * bank):
 *
 *   pair finder   Hann 128-pt periodogram (hop 32), EMA-averaged; the best
 *                 mark/space pair 170 Hz apart anywhere in the channel —
 *                 scored by the WEAKER tone (a lone carrier can never win) —
 *                 acquires and is then centre-tracked.
 *   demodulator   two NCOs ride the tracked centre ±85 Hz; each tone goes
 *                 through a one-bit moving-sum filter (the matched filter
 *                 for rectangular FSK bits). Per-tone peak trackers
 *                 normalise before comparison — the classic RTTY ATC, so a
 *                 selectively faded mark still slices correctly.
 *   slicer        y = mark/mark_peak − space/space_peak.
 *   UART ×2       start-bit-anchored: a mark→space edge (confirmed mid
 *                 start bit) anchors the 5 data samples + stop check at
 *                 fixed 45.45 Bd offsets — every character re-syncs on its
 *                 own start edge, no clock recovery to lose. Two framers
 *                 run on y and −y; whichever sustains valid framing is
 *                 elected (reversed/wrong-sideband signals copy too).
 *   ITA2          letters/US-TTY figures, unshift-on-space; CR/LF → space.
 *
 * Squelch is layered like the CW backend's: characters leave the channel
 * only while (1) the pair holds above the floor, (2) the two tone envelopes
 * are ANTI-correlated (true FSK keys exactly one tone at a time; two
 * unrelated stations 170 Hz apart are uncorrelated and never pass), and
 * (3) the elected framer's valid-frame rate is high. Characters framed
 * between acquisition and the squelch opening are buffered and flushed when
 * it opens, so the head of an over is not eaten while the stats build.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_rtty.h"

#include <fftw3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RT_DBG(...) \
  do { if (g_getenv("SKIM_RTTY_DEBUG")) { fprintf(stderr, __VA_ARGS__); } } while (0)

#define RT_FFT      128           /* pair-finder FFT length                  */
#define RT_HOP      32            /* periodogram hop                         */
#define RT_BAUD     45.45
#define RT_HALF     85.0          /* half the 170 Hz shift                   */
#define RT_TONE_WIN 2             /* per-tone search: ± bins around ±85 Hz   */
#define RT_MS_MAX   64            /* moving-sum ring cap (rate/baud ≤ this)  */
#define RT_PEND     24            /* chars buffered while the squelch proves */

/* Acquisition/release, measured on the gate's SNR/noise/CW cases: the
 * weaker tone must clear the median PSD bin by 8× (9 dB in-bin) to acquire;
 * it releases after 2 s under 3× — the averaged periodogram's noise-only
 * maximum sits near 2× its median, so a released channel must not hover. */
#define RT_ACQ_RATIO 8.0
#define RT_REL_RATIO 3.0
#define RT_REL_SECS  2.0

/* Emission gates (see the layered-squelch note above). True FSK measures a
 * mark/space envelope correlation of −0.8…−1.0 (exactly one tone on at a
 * time) — two independent stations 170 Hz apart hover near 0, so the open
 * bar sits far from both. The presence gate is the FAST per-character
 * carrier check: the slow periodogram release takes ~2 s after a
 * transmitter stops, and without it those seconds framed noise into
 * printable garbage on the tail of every over (gate-caught). */
#define RT_OK_BAR    0.55         /* valid-frame EMA to open                 */
#define RT_OK_CLOSE  0.35
#define RT_CORR_BAR  (-0.35)      /* envelope correlation to open below      */
#define RT_CORR_CLOSE (-0.15)     /* … and close above                       */
#define RT_PRES_BAR  0.35         /* tone envelope / peak to accept a char —
                                   * after a transmitter stops the ratio
                                   * falls to ~noise/peak (≪ 0.2) within the
                                   * EMA, while a deep QSB dip with the ATC
                                   * lagging bottoms out near 0.5; 0.45 sat
                                   * close enough to clip chars out of the
                                   * dip (gate-caught)                       */
#define RT_ACQ_PROVE 1.2          /* seconds acquired before opening         */
#define RT_SETTLE    0.7          /* seconds after acquisition before the
                                   * framers arm — the centre track and ATC
                                   * are still converging and a half-settled
                                   * slicer frames plausible garbage
                                   * (gate-caught: a phantom V in front of a
                                   * straddling channel's clean copy)        */

#define RT_BREAK "\xC2\xB7 "      /* "· " over separator, as the CW panes    */

/* --- ITA2 (CCITT-2), first-received bit = LSB; 0 = no printable ----------- */

#define ITA2_FIGS 0x1B
#define ITA2_LTRS 0x1F
#define ITA2_SP   0x04

static const char ita2_ltrs[32] = {
  0,   'E', '\n', 'A', ' ', 'S', 'I', 'U',
  0,   'D', 'R',  'J', 'N', 'F', 'C', 'K',
  'T', 'Z', 'L',  'W', 'H', 'Y', 'P', 'Q',
  'O', 'B', 'G',  0,   'M', 'X', 'V', 0,
};
/* Figures per US-TTY (the ham convention; the ITU set differs only in the
 * corners no contest exchange uses). */
static const char ita2_figs[32] = {
  0,   '3', '\n', '-', ' ', 0,   '8', '7',
  0,   '$', '4',  '\'', ',', '!', ':', '(',
  '5', '"', ')',  '2', '#', '6', '0', '1',
  '9', '?', '&',  0,   '.', '/', ';', 0,
};

/* --- UART framer (one polarity) -------------------------------------------- */

typedef enum { UART_HUNT = 0, UART_START, UART_DATA } UartPhase;

typedef struct {
  UartPhase phase;
  double    t;                    /* samples since the start edge            */
  double    next_pt;              /* next sampling instant                   */
  guint     bit;                  /* data bits collected                     */
  guint     code;
  double    ok_ema;               /* valid-frame rate (per framed char)      */
  gboolean  prev_mark;
} RtUart;

/* Feed one slicer sample (mark = TRUE); returns a 5-bit code via *code when
 * a character framed with a VALID stop bit, otherwise −1 is signalled by
 * FALSE. Invalid stops only decay ok_ema. */
static gboolean uart_step(RtUart *u, gboolean mark, double bit_len,
                          guint *code) {
  gboolean framed = FALSE;
  switch (u->phase) {
  case UART_HUNT:
    if (u->prev_mark && !mark) {
      u->phase = UART_START;
      u->t     = 0.0;
    }
    break;
  case UART_START:
    u->t += 1.0;
    if (u->t >= 0.5 * bit_len) {
      if (!mark) {                /* still space mid start bit — real       */
        u->phase   = UART_DATA;
        u->bit     = 0;
        u->code    = 0;
        u->next_pt = 1.5 * bit_len;
      } else {
        u->phase = UART_HUNT;     /* glitch                                  */
      }
    }
    break;
  case UART_DATA:
    u->t += 1.0;
    if (u->t >= u->next_pt) {
      if (u->bit < 5) {
        if (mark) { u->code |= 1u << u->bit; }
        u->bit++;
        u->next_pt = (1.5 + u->bit) * bit_len;
      } else {                    /* stop bit — mark required                */
        if (mark) {
          u->ok_ema += 0.15 * (1.0 - u->ok_ema);
          *code  = u->code;
          framed = TRUE;
        } else {
          u->ok_ema += 0.15 * (0.0 - u->ok_ema);
        }
        u->phase = UART_HUNT;
      }
    }
    break;
  }
  u->prev_mark = mark;
  return framed;
}

static void uart_reset(RtUart *u) {
  u->phase     = UART_HUNT;
  u->prev_mark = TRUE;
}

/* --- per-channel state ------------------------------------------------------ */

typedef struct {
  double rate;
  double bit_len;                 /* samples per bit                         */
  guint  ms_len;                  /* moving-sum length ≈ one bit             */

  /* pair finder */
  fftwf_complex *fin, *fout;
  fftwf_plan     plan;
  float   win[RT_FFT];
  float   wbuf[2 * RT_FFT];       /* FFT input ring (oldest at wpos)         */
  guint   wpos;
  guint   wfill;                  /* valid frames in wbuf                    */
  guint   hop_fill;               /* samples since the last periodogram      */
  double  psd[RT_FFT];
  gboolean primed;
  double  alpha;                  /* PSD EMA per hop                         */

  gboolean acq;
  double   center_hz;             /* tracked pair centre                     */
  double   pair_q;                /* weaker-tone / floor of the last scan    */
  double   lost_s;                /* seconds spent under the release bar     */

  /* demodulator */
  double ph_m, ph_s;              /* NCO phases                              */
  float  mr[RT_MS_MAX], mi[RT_MS_MAX];
  float  sr[RT_MS_MAX], si[RT_MS_MAX];
  guint  ms_pos;
  double msum_r, msum_i, ssum_r, ssum_i;
  double mpk, spk;                /* per-tone peak trackers (ATC)            */
  double k_pk;

  /* mark/space envelope moments (anti-correlation squelch) */
  double c_m, c_s, c_ms, c_mm, c_ss;
  double k_corr;
  double corr;                    /* current correlation estimate            */
  double presence;                /* EMA of tone envelope / peak (fast)      */
  double k_pres;
  double acq_samps;               /* samples since acquisition               */

  RtUart   uart[2];               /* 0 = normal, 1 = reversed                */
  guint    elected;
  guint    shift;                 /* 0 = letters, 1 = figures                */
  gboolean last_was_sp;           /* collapse space runs                     */

  char     pend[RT_PEND];         /* chars framed before the squelch opened  */
  guint    npend;
  gboolean emitted;               /* any text left this acquisition          */
  gboolean sq_open;

  double freq_hz;                 /* absolute channel label (diagnostics)    */
} RttyState;

static gpointer rtty_channel_new(double sample_rate) {
  RttyState *st = g_new0(RttyState, 1);
  st->rate    = sample_rate > 0 ? sample_rate : 500.0;
  st->bit_len = st->rate / RT_BAUD;
  st->ms_len  = CLAMP((guint)(st->bit_len + 0.5), 2u, (guint)RT_MS_MAX);
  st->fin     = fftwf_alloc_complex(RT_FFT);
  st->fout    = fftwf_alloc_complex(RT_FFT);
  st->plan    = fftwf_plan_dft_1d(RT_FFT, st->fin, st->fout, FFTW_FORWARD,
                                  FFTW_ESTIMATE);
  for (guint i = 0; i < RT_FFT; i++) {
    st->win[i] = 0.5f - 0.5f * cosf(2.0f * (float)G_PI * i / (RT_FFT - 1));
  }
  st->alpha  = (double)RT_HOP / (st->rate * 0.8);       /* PSD τ ≈ 0.8 s     */
  st->k_pk   = 1.0 - exp(-1.0 / (st->rate * 2.5));      /* ATC decay 2.5 s   */
  st->k_corr = 1.0 - exp(-1.0 / (st->rate * 1.0));      /* moments τ 1 s     */
  st->k_pres = 1.0 - exp(-1.0 / (st->rate * 0.06));     /* presence τ 60 ms  */
  uart_reset(&st->uart[0]);
  uart_reset(&st->uart[1]);
  return st;
}

static void rtty_channel_free(gpointer state) {
  RttyState *st = state;
  if (!st)
    return;
  fftwf_destroy_plan(st->plan);
  fftwf_free(st->fin);
  fftwf_free(st->fout);
  g_free(st);
}

/* --- pair finder ------------------------------------------------------------ */

static guint bin_at(const RttyState *st, double hz) {
  const double bin_hz = st->rate / RT_FFT;
  gint b = (gint)floor(hz / bin_hz + 0.5);
  return (guint)((b % RT_FFT + RT_FFT) % RT_FFT);
}

/* Strongest bin within ±RT_TONE_WIN of hz. */
static double tone_power(const RttyState *st, double hz) {
  const guint c = bin_at(st, hz);
  double p = 0.0;
  for (gint d = -RT_TONE_WIN; d <= RT_TONE_WIN; d++) {
    p = MAX(p, st->psd[(c + (guint)(d + RT_FFT)) % RT_FFT]);
  }
  return p;
}

/* Sub-bin tone position: floor-subtracted power centroid over the search
 * window — the bin grid alone (3.9 Hz) plus a flat ±2-bin score plateau let
 * the reported centre wander several Hz (gate-caught at −120 Hz offset). */
static double tone_centroid(const RttyState *st, double hz, double floor_p) {
  const guint  c      = bin_at(st, hz);
  const double bin_hz = st->rate / RT_FFT;
  double psum = 0.0, wsum = 0.0;
  for (gint d = -RT_TONE_WIN; d <= RT_TONE_WIN; d++) {
    const guint  b = (c + (guint)(d + RT_FFT)) % RT_FFT;
    const double w = MAX(st->psd[b] - floor_p, 0.0);
    const double f =
        ((b <= RT_FFT / 2) ? (double)b : (double)b - RT_FFT) * bin_hz;
    psum += w;
    wsum += w * f;
  }
  return psum > 0.0 ? wsum / psum : hz;
}

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static void release(RttyState *st, gboolean full);

/* Run one periodogram + pair scan (every RT_HOP samples). */
static void pair_scan(RttyState *st) {
  for (guint i = 0; i < RT_FFT; i++) {
    const guint idx = (st->wpos + i) % RT_FFT;  /* oldest first              */
    st->fin[i][0] = st->wbuf[2 * idx] * st->win[i];
    st->fin[i][1] = st->wbuf[2 * idx + 1] * st->win[i];
  }
  fftwf_execute(st->plan);
  for (guint i = 0; i < RT_FFT; i++) {
    const double p = (double)st->fout[i][0] * st->fout[i][0] +
                     (double)st->fout[i][1] * st->fout[i][1];
    st->psd[i] = st->primed ? st->psd[i] + st->alpha * (p - st->psd[i]) : p;
  }
  st->primed = TRUE;

  double sorted[RT_FFT];
  memcpy(sorted, st->psd, sizeof(sorted));
  qsort(sorted, RT_FFT, sizeof(double), cmp_double);
  const double floor_p = MAX(sorted[RT_FFT / 2], 1e-30);

  /* Candidate centres on the bin grid; the weaker tone scores (a lone
   * carrier or one-sided splatter can never acquire). While acquired the
   * scan tightens around the tracked centre — the pair is followed, not
   * re-elected, so a stronger neighbour cannot steal the lock. */
  const double bin_hz = st->rate / RT_FFT;
  const double lim    = st->rate / 2.0 - RT_HALF - RT_TONE_WIN * bin_hz;
  double lo = -lim, hi = lim;
  if (st->acq) {
    lo = st->center_hz - 3.0 * bin_hz;
    hi = st->center_hz + 3.0 * bin_hz;
  }
  double best_q = 0.0, best_c = 0.0, best_sum = 0.0;
  for (double c = lo; c <= hi; c += bin_hz) {
    if (fabs(c) > lim)
      continue;
    const double pm = tone_power(st, c + RT_HALF);
    const double ps = tone_power(st, c - RT_HALF);
    const double q  = MIN(pm, ps) / floor_p;
    if (q > best_q || (q == best_q && pm + ps > best_sum)) {
      best_q   = q;
      best_c   = c;
      best_sum = pm + ps;
    }
  }
  st->pair_q = best_q;

  /* Sub-bin centre: mean of the two tones' floor-subtracted centroids. */
  const double c_ref = 0.5 * (tone_centroid(st, best_c + RT_HALF, floor_p) +
                              tone_centroid(st, best_c - RT_HALF, floor_p));

  if (!st->acq) {
    if (best_q >= RT_ACQ_RATIO) {
      st->acq       = TRUE;
      st->center_hz = c_ref;
      st->lost_s    = 0.0;
      st->npend     = 0;
      st->emitted   = FALSE;
      st->sq_open   = FALSE;
      st->shift     = 0;
      st->acq_samps = 0.0;
      /* Prime the envelope moments NEUTRAL (corr = 0): unprimed zeros made
       * the early correlation estimate swing hard negative and opened the
       * squelch on two unrelated carriers (gate-caught). */
      st->c_m  = st->c_s  = 0.5;
      st->c_mm = st->c_ss = 0.30;
      st->c_ms = 0.25;
      st->corr = 0.0;
      st->presence = 1.0;
      uart_reset(&st->uart[0]);
      uart_reset(&st->uart[1]);
      RT_DBG("rtty acq @ %+.0f Hz q %.1f\n", st->center_hz, best_q);
    }
    return;
  }

  st->center_hz += 0.3 * (c_ref - st->center_hz);
  if (best_q < RT_REL_RATIO) {
    st->lost_s += (double)RT_HOP / st->rate;
    if (st->lost_s > RT_REL_SECS) { release(st, FALSE); }
  } else {
    st->lost_s = 0.0;
  }
}

/* Pair lost. full = channel teardown (no break mark wanted). */
static void release(RttyState *st, gboolean full) {
  RT_DBG("rtty release @ %+.0f Hz\n", st->center_hz);
  st->acq     = FALSE;
  st->npend   = 0;
  st->sq_open = FALSE;
  st->shift   = 0;
  /* Keep half the framing history — a QSB dip re-proves quickly. */
  st->uart[0].ok_ema *= 0.5;
  st->uart[1].ok_ema *= 0.5;
  uart_reset(&st->uart[0]);
  uart_reset(&st->uart[1]);
  if (full) { st->emitted = FALSE; }
}

/* --- emission --------------------------------------------------------------- */

static void emit_ch(SkimDecode *out, guint *pos, char c) {
  if (*pos + 1 < SKIM_DECODE_TEXT_MAX) {
    out->text[(*pos)++] = c;
    out->text[*pos] = '\0';
  }
}

static void emit_str(SkimDecode *out, guint *pos, const char *s) {
  gsize n = strlen(s);
  if (*pos + n < SKIM_DECODE_TEXT_MAX) {
    memcpy(out->text + *pos, s, n + 1);
    *pos += (guint)n;
  }
}

/* ITA2 code → printable char through the shift state; 0 = nothing. */
static char ita2_decode(RttyState *st, guint code) {
  if (code == ITA2_LTRS) {
    st->shift = 0;
    return 0;
  }
  if (code == ITA2_FIGS) {
    st->shift = 1;
    return 0;
  }
  char c = st->shift ? ita2_figs[code] : ita2_ltrs[code];
  if (code == ITA2_SP) { st->shift = 0; }       /* unshift on space          */
  if (c == '\n') { c = ' '; }
  return c;
}

/* A framed character from the ELECTED polarity: collapse space runs, then
 * pend or emit depending on the squelch. */
static void take_char(RttyState *st, char c, SkimDecode *out, guint *pos) {
  if (!c)
    return;
  if (c == ' ') {
    if (st->last_was_sp)
      return;
    st->last_was_sp = TRUE;
  } else {
    st->last_was_sp = FALSE;
  }
  if (st->sq_open) {
    emit_ch(out, pos, c);
    st->emitted = TRUE;
  } else if (st->npend < RT_PEND) {
    st->pend[st->npend++] = c;
  } else {
    memmove(st->pend, st->pend + 1, RT_PEND - 1);
    st->pend[RT_PEND - 1] = c;
  }
}

/* --- process ---------------------------------------------------------------- */

static gboolean rtty_process(gpointer state, const float *iq, guint nframes,
                             SkimDecode *out) {
  RttyState *st = state;
  guint pos = 0;
  out->text[0] = '\0';
  out->pane_own = FALSE;

  for (guint n = 0; n < nframes; n++) {
    const float xi = iq[2 * n], xq = iq[2 * n + 1];

    /* Periodogram input ring (hop RT_HOP). */
    st->wbuf[2 * st->wpos]     = xi;
    st->wbuf[2 * st->wpos + 1] = xq;
    st->wpos = (st->wpos + 1) % RT_FFT;
    if (st->wfill < RT_FFT) { st->wfill++; }
    st->hop_fill++;
    if (st->hop_fill >= RT_HOP && st->wfill == RT_FFT) {
      st->hop_fill = 0;
      const gboolean was_acq = st->acq;
      pair_scan(st);
      if (was_acq && !st->acq && st->emitted) {
        emit_str(out, &pos, RT_BREAK);          /* transmission over         */
        st->emitted     = FALSE;
        st->last_was_sp = TRUE;
      }
    }

    if (!st->acq)
      continue;

    /* Tone mixers + one-bit moving sums. */
    const double wm = 2.0 * G_PI * (st->center_hz + RT_HALF) / st->rate;
    const double ws = 2.0 * G_PI * (st->center_hz - RT_HALF) / st->rate;
    st->ph_m = fmod(st->ph_m + wm, 2.0 * G_PI);
    st->ph_s = fmod(st->ph_s + ws, 2.0 * G_PI);
    const float cm = (float)cos(st->ph_m), sm = (float)sin(st->ph_m);
    const float cs = (float)cos(st->ph_s), ss = (float)sin(st->ph_s);
    /* x · e^{−jφ} */
    const float zmr = xi * cm + xq * sm, zmi = xq * cm - xi * sm;
    const float zsr = xi * cs + xq * ss, zsi = xq * cs - xi * ss;
    const guint p = st->ms_pos;
    st->msum_r += zmr - st->mr[p];
    st->msum_i += zmi - st->mi[p];
    st->ssum_r += zsr - st->sr[p];
    st->ssum_i += zsi - st->si[p];
    st->mr[p] = zmr;
    st->mi[p] = zmi;
    st->sr[p] = zsr;
    st->si[p] = zsi;
    st->ms_pos = (p + 1) % st->ms_len;
    const double mag_m =
        sqrt(st->msum_r * st->msum_r + st->msum_i * st->msum_i) / st->ms_len;
    const double mag_s =
        sqrt(st->ssum_r * st->ssum_r + st->ssum_i * st->ssum_i) / st->ms_len;

    /* ATC + slicer. */
    st->mpk = mag_m > st->mpk ? mag_m : st->mpk + st->k_pk * (mag_m - st->mpk);
    st->spk = mag_s > st->spk ? mag_s : st->spk + st->k_pk * (mag_s - st->spk);
    const double mn = mag_m / MAX(st->mpk, 1e-12);
    const double sn = mag_s / MAX(st->spk, 1e-12);
    const double y  = mn - sn;
    st->acq_samps += 1.0;

    /* Presence: FSK keys exactly one tone at any instant, so the ACTIVE
     * tone rides near ITS OWN peak (per-tone ratio — a selectively faded
     * mark still counts as present) the whole transmission; when the
     * transmitter stops both ratios collapse within the 60 ms EMA while
     * the periodogram release still needs seconds. */
    st->presence += st->k_pres * (MAX(mn, sn) - st->presence);

    /* Envelope moments → anti-correlation squelch input. */
    st->c_m  += st->k_corr * (mn - st->c_m);
    st->c_s  += st->k_corr * (sn - st->c_s);
    st->c_ms += st->k_corr * (mn * sn - st->c_ms);
    st->c_mm += st->k_corr * (mn * mn - st->c_mm);
    st->c_ss += st->k_corr * (sn * sn - st->c_ss);
    {
      const double vm = st->c_mm - st->c_m * st->c_m;
      const double vs = st->c_ss - st->c_s * st->c_s;
      st->corr = (st->c_ms - st->c_m * st->c_s) / sqrt(MAX(vm * vs, 1e-20));
    }

    /* Both framers step every sample once the track has settled; only the
     * elected one prints, and only while the carrier is actually THERE
     * (presence). */
    if (st->acq_samps < RT_SETTLE * st->rate)
      continue;
    for (guint f = 0; f < 2; f++) {
      guint code;
      const gboolean mark = f ? (y < 0.0) : (y >= 0.0);
      if (uart_step(&st->uart[f], mark, st->bit_len, &code) &&
          f == st->elected && st->presence >= RT_PRES_BAR) {
        take_char(st, ita2_decode(st, code), out, &pos);
      }
    }
    if (st->uart[1 - st->elected].ok_ema >
            st->uart[st->elected].ok_ema + 0.2 &&
        st->uart[1 - st->elected].ok_ema > 0.5) {
      st->elected = 1 - st->elected;
      st->shift   = 0;
      st->npend   = 0;                          /* other polarity's garbage  */
      RT_DBG("rtty polarity → %s\n", st->elected ? "reversed" : "normal");
    }

    /* Squelch: open on proven framing + anti-correlated tones; flush what
     * framed while it proved. Hysteresis close on either stat: a channel
     * that degrades into garbled framing, or whose "pair" stops keying
     * like FSK, goes back to proving instead of printing. */
    if (st->sq_open && (st->uart[st->elected].ok_ema < RT_OK_CLOSE ||
                        st->corr > RT_CORR_CLOSE)) {
      st->sq_open = FALSE;
      RT_DBG("rtty squelch close (ok %.2f corr %.2f)\n",
             st->uart[st->elected].ok_ema, st->corr);
    }
    if (!st->sq_open && st->acq_samps > RT_ACQ_PROVE * st->rate &&
        st->uart[st->elected].ok_ema >= RT_OK_BAR &&
        st->corr < RT_CORR_BAR) {
      st->sq_open = TRUE;
      RT_DBG("rtty squelch OPEN (ok %.2f corr %.2f)\n",
             st->uart[st->elected].ok_ema, st->corr);
      for (guint i = 0; i < st->npend; i++) {
        emit_ch(out, &pos, st->pend[i]);
      }
      if (st->npend) { st->emitted = TRUE; }
      st->npend = 0;
    }
  }

  if (pos == 0)
    return FALSE;

  out->confidence = CLAMP(st->uart[st->elected].ok_ema, 0.0, 1.0);
  out->freq_offset_hz = st->center_hz;
  out->speed  = RT_BAUD;
  out->snr_db = 10.0 * log10(MAX(st->pair_q, 1.0));
  return TRUE;
}

static double rtty_level(gpointer state) {
  const RttyState *st = state;
  return st->acq ? MAX(st->mpk, st->spk) : 0.0;
}

static double rtty_tone_offset_hz(gpointer state) {
  const RttyState *st = state;
  return st->acq ? st->center_hz : 0.0;
}

static void rtty_set_freq(gpointer state, double freq_hz) {
  ((RttyState *)state)->freq_hz = freq_hz;
}

const SkimDecodeBackend *skim_decode_rtty(void) {
  static const SkimDecodeBackend backend = {
    .name           = "rtty",
    .channel_new    = rtty_channel_new,
    .channel_free   = rtty_channel_free,
    .process        = rtty_process,
    .level          = rtty_level,
    .tone_offset_hz = rtty_tone_offset_hz,
    .set_freq       = rtty_set_freq,
  };
  return &backend;
}
