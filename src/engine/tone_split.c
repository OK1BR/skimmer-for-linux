/* tone_split.c — per-channel carrier splitter. See tone_split.h for the idea.
 *
 * Detection: Hann-windowed 64-point FFTs (hop 32) of the channel IQ,
 * magnitude² EMA'd with a ~2 s time constant (Welch) — keying averages
 * toward its duty cycle while a carrier stays a sharp line. Once a second
 * the peak list is re-evaluated:
 *
 *   floor   = median bin power (a carrier + sidebands occupy few of 64 bins),
 *   peak    = interpolated local max ≥ 8 dB over the floor, within ±60 Hz,
 *   carrier = peak that is NOT a keying sideband. Hard 50 %-duty keying puts
 *             the first sideband pair only ~4 dB under the carrier — far
 *             above any noise rule, so naive peak-picking would split every
 *             strong signal against its own keying. Sidebands come in
 *             SYMMETRIC pairs about their carrier; a real second station has
 *             no mirror twin. Any peak whose mirror about an accepted
 *             stronger carrier holds ≥ 0.3× its power is dropped.
 *
 * Split engages after 2 consecutive evals agree (≥2 carriers ≥ 22 Hz apart,
 * stable within ±10 Hz) and collapses when a slot's carrier stays unseen
 * for 90 s: a QSO pair alternates overs, so a slot must ride out the other
 * side's whole transmission — an idle slot only costs one parked decoder.
 *
 * Two lines closer than the engage bar overlap in keying bandwidth and
 * cannot be separated linearly (that needs joint demodulation / SIC — a
 * later stage); the affected slot is flagged CONTESTED instead, so the
 * pipeline can at least stop the beat mutations from reaching the callsign
 * candidates.
 *
 * Slot routing: phase-continuous NCO to ~0 Hz, then a 31-tap windowed-sinc
 * lowpass whose cutoff rides the nearest-carrier spacing,
 * clamp(0.55·Δf, 10, 32) Hz — near the keying bandwidth the filter trades
 * edge softness for rejection; the decoder tolerates soft edges far better
 * than a beating neighbour.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "tone_split.h"

#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TS_FFT       64                     /* detection FFT length          */
#define TS_HOP       32                     /* 50 % overlap                  */
#define TS_TAPS      31                     /* slot FIR length               */
#define TS_RING      1024                   /* frames per slot ring (2^n)    */
#define TS_EVAL_S    1.0                    /* topology evaluation cadence   */
#define TS_AVG_TC_S  2.0                    /* periodogram EMA time constant */
#define TS_EDGE_HZ   60.0                   /* peak search span (passband)   */
#define TS_FLOOR_DB  8.0                    /* peak over median floor        */
#define TS_REL_DB    12.0                   /* 2nd carrier within of primary */

/* OFF-gated verification: a REAL second carrier stays lit when the primary
 * keys OFF; keying sidebands and filter skirt vanish with it. The symmetric
 * mirror test alone missed a chirpy fist (live-caught 2026-07-16: EA1EYL's
 * drift makes his sidebands ASYMMETRIC — a slot camped on the lower pair at
 * −28 Hz and decoded skirt junk for minutes). A second periodogram
 * accumulates only over windows where the primary's own bin is dark; any
 * non-primary candidate must keep ≥ TS_OFF_RATIO of its power there. Until
 * enough dark windows exist the candidate is unverifiable: it gets NO slot
 * but does flag CONTESTED (conservative — candidates hold, spots wait).
 * Primaries with no keying dynamics (steady carrier, digi blob) cannot be
 * gated — after warm-up they fall back to the ungated rules. */
#define TS_OFF_RATIO 0.25
#define TS_LO_MIN    8                      /* dark windows before verdicts  */
#define TS_WARM_HOPS 40                     /* hops before "steady" verdict  */
#define TS_DYN       4.0                    /* pa_hi/pa_lo ≥ this = keying   */
/* One separability bar everywhere: carriers closer than TS_ENGAGE_HZ are
 * CONTESTED — never split, never slotted. The first build had two bars
 * (contested < 20, engage ≥ 22) and a pair sitting in the 20–22 Hz gap was
 * neither: its beat garbage flowed to the extractor unflagged (live-caught
 * 2026-07-16, EA1EYL + a carrier 20 Hz up mutating on 14014.4). */
#define TS_ENGAGE_HZ 22.0
#define TS_HOLD      2                      /* consecutive evals to engage   */
#define TS_STABLE_HZ 10.0                   /* eval-to-eval carrier drift    */
#define TS_TTL_S     90.0                   /* carrier unseen → slot dies    */
#define TS_TRACK_HZ  12.0                   /* peak-to-slot claim radius     */
#define TS_SAME_HZ   6.0                    /* peaks this close = one line   */
#define TS_MIRROR    0.30                   /* mirror power ratio = sideband */
#define TS_CUT_MIN   10.0                   /* slot FIR cutoff clamp (Hz)    */
#define TS_CUT_MAX   32.0
#define TS_RETAP_HZ  2.0                    /* cutoff moved this far → retap */
/* FOCUS guards (live-caught 2026-07-19, F5IN @ 26 dB / 25 WPM): a 25 Hz
 * cutoff sits UNDER the keying's 3rd harmonic (~1.25×WPM Hz) — the softened
 * edges filled inter-element gaps and dit·gap·dit fused into a dah (F5IN →
 * G5IN, TEST → TENT), while the strong station never needed the filter in
 * the first place. Focus now engages only on carriers ≤ TS_FOCUS_MAX_DB
 * over the median floor (the weak ones the wide envelope actually loses,
 * ≈ 15 dB in-channel), and the cutoff rides the decoded speed:
 * 1.3 × WPM Hz, floored by the configured minimum, capped near the channel
 * edge. Bootstrap before any decode assumes 25 WPM (32.5 Hz).
 * The floor for THIS bar is the lower quartile of the PASSBAND bins (the
 * full-band median is dragged down by the channelizer stopband, and a
 * strong carrier's sidebands pollute anything above the quartile). The
 * ratio COMPRESSES at the top — sidebands lift the floor with the carrier
 * — so the measured table is what sets the bar (gate, ratio dump):
 * SNR 6→15-18, 8→15-19, 10→9-21, 12→18-24, 25-26→23-28 dB. 21 keeps the
 * weak side solidly under (2 consecutive evals must clear it) and the
 * strong side out. */
#define TS_FOCUS_MAX_DB 21.0                /* carrier over passband quartile */
#define TS_FOCUS_FC_MAX 55.0                /* never wider than the channel  */
#define TS_FOCUS_WPM0   25.0                /* cutoff bootstrap speed        */

typedef struct {
  gboolean active;
  guint    gen;
  double   mix_hz;                          /* carrier offset being mixed    */
  double   phi, dphi;                       /* NCO phase / increment         */
  double   fc_hz;                           /* current FIR cutoff            */
  float    taps[TS_TAPS];
  float    dl[2 * TS_TAPS];                 /* complex FIR delay line        */
  guint    dpos;
  double   unseen_s;                        /* carrier absent this long      */
  gboolean contested;
  guint    clean;                           /* contested-free evals in a row */
  double   wpm_hint;                        /* decoded speed; 0 = none yet   */
  float    ring[2 * TS_RING];
  guint    rw, rr;                          /* free-running frame counters   */
} TsSlot;

struct _SkimToneSplit {
  double         rate;
  double         alpha;                     /* periodogram EMA coefficient   */
  fftwf_complex *fin, *fout;
  fftwf_plan     plan;
  float          win[TS_FFT];               /* Hann                          */
  float          wbuf[2 * TS_FFT];          /* sliding input window          */
  guint          wfill;
  double         psd[TS_FFT];               /* averaged periodogram          */
  gboolean       primed;
  double         psd_lo[TS_FFT];            /* … over primary-dark windows   */
  gboolean       lo_primed;
  guint8         lo_n;                      /* dark windows seen (saturates) */
  guint8         hops;                      /* windows seen (saturates)      */
  gint           prim_bin;                  /* primary's bin; −1 = none yet  */
  double         pa_hi, pa_lo;              /* primary-bin power trackers    */
  double         eval_due;                  /* samples until next eval       */
  guint          hold;
  double         hold_hz[2];
  double         focus_fc;                  /* single-carrier cutoff; 0 = off */
  guint          fhold;                     /* focus stability counter       */
  double         fhold_hz;
  TsSlot         slot[SKIM_TONE_SPLIT_MAX];
  guint          nslots;
  gboolean       split;                     /* narrow slots engaged          */
  guint          genseq;
  gboolean       debug;
};

/* ---- construction ------------------------------------------------------------ */

SkimToneSplit *skim_tone_split_new(double sample_rate) {
  SkimToneSplit *ts = g_new0(SkimToneSplit, 1);
  ts->rate  = sample_rate;
  ts->alpha = 1.0 - exp(-((double)TS_HOP / sample_rate) / TS_AVG_TC_S);
  ts->fin   = fftwf_alloc_complex(TS_FFT);
  ts->fout  = fftwf_alloc_complex(TS_FFT);
  ts->plan  = fftwf_plan_dft_1d(TS_FFT, ts->fin, ts->fout, FFTW_FORWARD,
                                FFTW_ESTIMATE);
  for (guint i = 0; i < TS_FFT; i++) {
    ts->win[i] = 0.5f - 0.5f * cosf(2.0f * (float)G_PI * i / (TS_FFT - 1));
  }
  ts->nslots = 1;
  ts->slot[0].active = TRUE;
  ts->slot[0].gen = ++ts->genseq;
  ts->prim_bin = -1;
  ts->eval_due = TS_EVAL_S * sample_rate;
  ts->debug = g_getenv("SKIM_TS_DEBUG") != NULL;
  return ts;
}

void skim_tone_split_set_focus(SkimToneSplit *ts, double fc_hz) {
  ts->focus_fc = fc_hz;
}

void skim_tone_split_slot_hint_wpm(SkimToneSplit *ts, guint slot, double wpm) {
  if (slot < ts->nslots && wpm > 0) { ts->slot[slot].wpm_hint = wpm; }
}

void skim_tone_split_free(SkimToneSplit *ts) {
  if (!ts)
    return;
  fftwf_destroy_plan(ts->plan);
  fftwf_free(ts->fin);
  fftwf_free(ts->fout);
  g_free(ts);
}

/* ---- slot ring ---------------------------------------------------------------- */

static inline void ring_put(TsSlot *sl, float i, float q) {
  if (sl->rw - sl->rr >= TS_RING) {         /* owner stalled: drop oldest    */
    sl->rr = sl->rw - TS_RING + 1;
  }
  const guint at = sl->rw & (TS_RING - 1);
  sl->ring[2 * at]     = i;
  sl->ring[2 * at + 1] = q;
  sl->rw++;
}

guint skim_tone_split_read(SkimToneSplit *ts, guint slot, float *iq,
                           guint max_frames) {
  if (slot >= ts->nslots)
    return 0;
  TsSlot *sl = &ts->slot[slot];
  const guint n = MIN(sl->rw - sl->rr, max_frames);
  for (guint i = 0; i < n; i++) {
    const guint at = (sl->rr + i) & (TS_RING - 1);
    iq[2 * i]     = sl->ring[2 * at];
    iq[2 * i + 1] = sl->ring[2 * at + 1];
  }
  sl->rr += n;
  return n;
}

/* ---- slot setup ---------------------------------------------------------------- */

/* Windowed-sinc lowpass (Hamming), unity DC gain — the carrier amplitude
 * survives the slot, so decoder levels stay comparable across channels. */
static void slot_design_fir(TsSlot *sl, double rate, double fc) {
  const int M = (TS_TAPS - 1) / 2;
  double sum = 0.0;
  for (int k = 0; k < TS_TAPS; k++) {
    const int d = k - M;
    const double x = 2.0 * fc / rate * d;
    double v = (d == 0) ? 1.0 : sin(G_PI * x) / (G_PI * x);
    v *= 0.54 + 0.46 * cos(G_PI * (double)d / M);
    sl->taps[k] = (float)v;
    sum += v;
  }
  for (int k = 0; k < TS_TAPS; k++) { sl->taps[k] = (float)(sl->taps[k] / sum); }
  sl->fc_hz = fc;
}

static void slot_start(SkimToneSplit *ts, TsSlot *sl, double hz) {
  sl->active    = TRUE;
  sl->gen       = ++ts->genseq;
  sl->mix_hz    = hz;
  sl->phi       = 0.0;
  sl->dphi      = -2.0 * G_PI * hz / ts->rate;
  sl->unseen_s  = 0.0;
  sl->contested = FALSE;
  sl->clean     = 0;
  sl->wpm_hint  = 0.0;
  sl->dpos      = 0;
  sl->fc_hz     = 0.0;                      /* forces the first retap        */
  memset(sl->dl, 0, sizeof(sl->dl));
  sl->rr = sl->rw;                          /* discard transition samples    */
}

/* Cutoffs ride the CURRENT spacing: two slots drifting toward each other
 * narrow their filters, drifting apart widens them back. A single engaged
 * slot has no neighbour — that is FOCUS mode, and it gets the focus cutoff. */
static void retap_all(SkimToneSplit *ts) {
  if (!ts->split)
    return;
  for (guint s = 0; s < ts->nslots; s++) {
    double dmin = ts->rate;                 /* > any real spacing            */
    for (guint o = 0; o < ts->nslots; o++) {
      if (o == s)
        continue;
      dmin = MIN(dmin, fabs(ts->slot[o].mix_hz - ts->slot[s].mix_hz));
    }
    double fc;
    if (ts->nslots == 1) {                  /* FOCUS: ride the decoded speed */
      const double wpm = ts->slot[0].wpm_hint > 0 ? ts->slot[0].wpm_hint
                                                  : TS_FOCUS_WPM0;
      fc = CLAMP(1.3 * wpm, ts->focus_fc, TS_FOCUS_FC_MAX);
    } else {
      fc = CLAMP(0.55 * dmin, TS_CUT_MIN, TS_CUT_MAX);
    }
    if (fabs(fc - ts->slot[s].fc_hz) > TS_RETAP_HZ) {
      slot_design_fir(&ts->slot[s], ts->rate, fc);
    }
  }
}

/* ---- detection ----------------------------------------------------------------- */

static void accumulate_psd(SkimToneSplit *ts) {
  for (guint i = 0; i < TS_FFT; i++) {
    ts->fin[i][0] = ts->wbuf[2 * i] * ts->win[i];
    ts->fin[i][1] = ts->wbuf[2 * i + 1] * ts->win[i];
  }
  fftwf_execute(ts->plan);
  double bin[TS_FFT];
  for (guint i = 0; i < TS_FFT; i++) {
    bin[i] = (double)ts->fout[i][0] * ts->fout[i][0] +
             (double)ts->fout[i][1] * ts->fout[i][1];
    ts->psd[i] = ts->primed ? ts->psd[i] + ts->alpha * (bin[i] - ts->psd[i])
                            : bin[i];
  }
  ts->primed = TRUE;
  if (ts->hops < 255) { ts->hops++; }

  /* OFF-gated periodogram (see TS_OFF_RATIO): windows where the primary's
   * own line is dark teach psd_lo — the view with the primary blanked. */
  if (ts->prim_bin < 0)
    return;
  double pa = 0.0;
  for (gint d = -1; d <= 1; d++) {
    pa = MAX(pa, bin[(ts->prim_bin + d + TS_FFT) % TS_FFT]);
  }
  if (pa > ts->pa_hi) {
    ts->pa_hi = pa;
  } else {
    ts->pa_hi += 0.05 * (pa - ts->pa_hi);
  }
  if (pa < ts->pa_lo || ts->pa_lo <= 0.0) {
    ts->pa_lo = pa;
  } else {
    ts->pa_lo += 0.05 * (pa - ts->pa_lo);
  }
  if (ts->pa_hi > TS_DYN * ts->pa_lo && pa < sqrt(ts->pa_hi * ts->pa_lo)) {
    for (guint i = 0; i < TS_FFT; i++) {
      ts->psd_lo[i] = ts->lo_primed
                          ? ts->psd_lo[i] + ts->alpha * (bin[i] - ts->psd_lo[i])
                          : bin[i];
    }
    ts->lo_primed = TRUE;
    if (ts->lo_n < 255) { ts->lo_n++; }
  }
}

static double bin_hz(const SkimToneSplit *ts, gint i) {
  const gint s = (i < TS_FFT / 2) ? i : i - TS_FFT;
  return s * ts->rate / TS_FFT;
}

/* Max averaged power within ±win_hz of hz, in the given periodogram. */
static double arr_near(const SkimToneSplit *ts, const double *arr, double hz,
                       double win_hz) {
  double best = 0.0;
  for (gint i = 0; i < TS_FFT; i++) {
    if (fabs(bin_hz(ts, i) - hz) <= win_hz && arr[i] > best) { best = arr[i]; }
  }
  return best;
}

typedef struct {
  double hz;
  double pw;
} Peak;

static int peak_cmp(const void *a, const void *b) {
  const double d = ((const Peak *)b)->pw - ((const Peak *)a)->pw;
  return d > 0 ? 1 : d < 0 ? -1 : 0;
}
static int dbl_cmp(const void *a, const void *b) {
  const double d = *(const double *)a - *(const double *)b;
  return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/* The peak list, sidebands and smear removed — see the file header.
 * *pending is set when a non-primary candidate had to be dropped ONLY
 * because the OFF-gate has no data yet: unverifiable, so the caller should
 * treat the channel as contested rather than clean. */
static guint find_carriers(const SkimToneSplit *ts, Peak *out, guint max,
                           gboolean *pending, double *floor_out) {
  double tmp[TS_FFT];
  memcpy(tmp, ts->psd, sizeof(tmp));
  qsort(tmp, TS_FFT, sizeof(double), dbl_cmp);
  /* Focus-bar floor: lower quartile of the PASSBAND bins only. The channel
   * is 2× oversampled — the outer bins are the channelizer's stopband, and
   * a full-band quantile reads that filtered near-nothing as "the noise",
   * inflating every carrier ratio past any bar (gate-caught on the 48 kHz
   * integration). The quartile also dodges the keying sidebands a strong
   * carrier lifts around itself. */
  if (floor_out) {
    double pb[TS_FFT];
    guint  npb = 0;
    for (gint i = 0; i < TS_FFT; i++) {
      if (fabs(bin_hz(ts, i)) <= TS_EDGE_HZ) { pb[npb++] = ts->psd[i]; }
    }
    qsort(pb, npb, sizeof(double), dbl_cmp);
    *floor_out = pb[npb / 4];
  }
  const double thr = tmp[TS_FFT / 2] * pow(10.0, TS_FLOOR_DB / 10.0);

  Peak cand[16];
  guint nc = 0;
  for (gint i = 0; i < TS_FFT && nc < G_N_ELEMENTS(cand); i++) {
    const double f = bin_hz(ts, i);
    if (fabs(f) > TS_EDGE_HZ)
      continue;
    const double p  = ts->psd[i];
    const double pl = ts->psd[(i + TS_FFT - 1) % TS_FFT];
    const double pr = ts->psd[(i + 1) % TS_FFT];
    if (p < thr || p < pl || p <= pr)       /* strict on one side: plateaus  */
      continue;
    const double lp = log(MAX(p, 1e-30)), ll = log(MAX(pl, 1e-30)),
                 lr = log(MAX(pr, 1e-30));
    const double den   = ll - 2.0 * lp + lr;
    double       delta = (den < -1e-9) ? 0.5 * (ll - lr) / den : 0.0;
    delta        = CLAMP(delta, -0.5, 0.5);
    cand[nc].hz  = f + delta * ts->rate / TS_FFT;
    cand[nc].pw  = p;
    nc++;
  }
  qsort(cand, nc, sizeof(Peak), peak_cmp);

  guint na = 0;
  for (guint i = 0; i < nc && na < max; i++) {
    if (na && cand[i].pw < out[0].pw * pow(10.0, -TS_REL_DB / 10.0))
      break;                                /* sorted: the rest is weaker    */
    gboolean drop = FALSE;
    for (guint a = 0; a < na && !drop; a++) {
      if (fabs(cand[i].hz - out[a].hz) < TS_SAME_HZ) {
        drop = TRUE;                        /* same line, straddle smear     */
      } else {
        const double mirror = 2.0 * out[a].hz - cand[i].hz;
        if (arr_near(ts, ts->psd, mirror, TS_SAME_HZ) >=
            TS_MIRROR * cand[i].pw) {
          drop = TRUE;                      /* keying sideband pair member   */
        }
      }
    }
    /* OFF-gate (see TS_OFF_RATIO). Order matters: mirror-dropped peaks are
     * PROVEN sidebands and never reach here — they must not raise pending. */
    if (!drop && na > 0) {
      if (ts->lo_n >= TS_LO_MIN) {
        const double off = arr_near(ts, ts->psd_lo, cand[i].hz, TS_SAME_HZ);
        const double tot = arr_near(ts, ts->psd, cand[i].hz, TS_SAME_HZ);
        if (off < TS_OFF_RATIO * tot) {
          drop = TRUE;                      /* dark with the primary: skirt  */
        }
      } else if (!(ts->hops >= TS_WARM_HOPS &&
                   ts->pa_hi < TS_DYN * ts->pa_lo)) {
        drop = TRUE;                        /* unverifiable yet — hold       */
        if (pending) { *pending = TRUE; }
      }
    }
    if (!drop) { out[na++] = cand[i]; }
  }
  return na;
}

/* ---- topology ------------------------------------------------------------------ */

static void engage(SkimToneSplit *ts, const Peak *c, guint nc) {
  double hz[SKIM_TONE_SPLIT_MAX];
  guint  n = 0;
  for (guint k = 0; k < nc && n < SKIM_TONE_SPLIT_MAX; k++) {
    gboolean ok = TRUE;
    for (guint j = 0; j < n; j++) {
      if (fabs(c[k].hz - hz[j]) < TS_ENGAGE_HZ) { ok = FALSE; }
    }
    if (ok) { hz[n++] = c[k].hz; }
  }
  if (n < 2)
    return;
  ts->split  = TRUE;
  ts->nslots = n;
  ts->hold   = 0;
  ts->fhold  = 0;
  for (guint s = 0; s < n; s++) { slot_start(ts, &ts->slot[s], hz[s]); }
  retap_all(ts);
  if (ts->debug) {
    g_printerr("tone-split %p: ENGAGE %u slots (%+.1f / %+.1f%s) Hz\n",
               (void *)ts, n, hz[0], hz[1], n > 2 ? " / +1" : "");
  }
}

/* Focus: the narrow path with ONE slot — same machinery, no second carrier.
 * From here the split-mode eval logic runs: the slot tracks its carrier, a
 * genuinely new second line spawns a real split, the TTL releases back to
 * passthrough. Entering costs a slot generation (decoder reset) — the wide
 * state a weak station accumulated was noise-fed anyway. */
static void focus_engage(SkimToneSplit *ts, double hz) {
  ts->split  = TRUE;
  ts->nslots = 1;
  ts->hold   = 0;
  ts->fhold  = 0;
  slot_start(ts, &ts->slot[0], hz);
  retap_all(ts);
  if (ts->debug) {
    g_printerr("tone-split %p: FOCUS %+.1f Hz (fc %.0f)\n", (void *)ts, hz,
               ts->focus_fc);
  }
}

static void collapse(SkimToneSplit *ts) {
  ts->split  = FALSE;
  ts->nslots = 1;
  TsSlot *s0 = &ts->slot[0];
  s0->active    = TRUE;
  s0->gen       = ++ts->genseq;
  s0->mix_hz    = 0.0;
  s0->contested = FALSE;
  s0->clean     = 0;
  s0->unseen_s  = 0.0;
  s0->rr = s0->rw;
  for (guint s = 1; s < SKIM_TONE_SPLIT_MAX; s++) { ts->slot[s].active = FALSE; }
  if (ts->debug) { g_printerr("tone-split %p: COLLAPSE\n", (void *)ts); }
}

/* Contested is set on sight and cleared only after 2 clean evals — the flag
 * gates the callsign path, so flapping costs candidates, not correctness. */
static void slot_set_contested(TsSlot *sl, gboolean evidence) {
  if (evidence) {
    sl->contested = TRUE;
    sl->clean     = 0;
  } else if (sl->contested && ++sl->clean >= 2) {
    sl->contested = FALSE;
  }
}

static void eval_topology(SkimToneSplit *ts) {
  if (!ts->primed)
    return;
  Peak     c[SKIM_TONE_SPLIT_MAX + 1];
  gboolean pending = FALSE;
  double   floor_med = 0.0;
  guint    nc = find_carriers(ts, c, SKIM_TONE_SPLIT_MAX + 1, &pending,
                              &floor_med);

  /* The strongest accepted line gates the OFF periodogram from now on. */
  if (nc > 0) {
    const gint b = (gint)lround(c[0].hz / (ts->rate / TS_FFT));
    ts->prim_bin = ((b % TS_FFT) + TS_FFT) % TS_FFT;
  }

  /* SKIM_TS_RATIO=1 dumps the focus-bar measurements — this is how
   * TS_FOCUS_MAX_DB gets calibrated (offline table in the header comment;
   * live recalibration will want the same dump). VERY verbose in the app. */
  if (g_getenv("SKIM_TS_RATIO") && nc > 0) {
    g_printerr("ts-ratio %p: nc=%u c0=%.1f Hz ratio=%.1f dB split=%d\n",
               (void *)ts, nc, c[0].hz,
               10.0 * log10(c[0].pw / MAX(floor_med, 1e-30)), ts->split);
  }
  if (!ts->split) {
    /* Two lines below the separable spacing — or an unverifiable second
     * line (OFF-gate still warming): flag, don't split. */
    slot_set_contested(&ts->slot[0],
                       pending ||
                       (nc >= 2 && fabs(c[0].hz - c[1].hz) < TS_ENGAGE_HZ));
    if (nc >= 2 && fabs(c[0].hz - c[1].hz) >= TS_ENGAGE_HZ) {
      const double lo = MIN(c[0].hz, c[1].hz), hi = MAX(c[0].hz, c[1].hz);
      if (ts->hold > 0 && fabs(lo - ts->hold_hz[0]) <= TS_STABLE_HZ &&
          fabs(hi - ts->hold_hz[1]) <= TS_STABLE_HZ) {
        ts->hold++;
      } else {
        ts->hold = 1;
      }
      ts->hold_hz[0] = lo;
      ts->hold_hz[1] = hi;
      if (ts->hold >= TS_HOLD) { engage(ts, c, nc); }
      ts->fhold = 0;
    } else if (ts->focus_fc > 0 && nc == 1 && !ts->slot[0].contested &&
               c[0].pw < floor_med * pow(10.0, TS_FOCUS_MAX_DB / 10.0)) {
      /* Exactly one clean, stable, WEAK carrier: tighten onto it. Strong
       * stations decode fine wide and only stand to lose (the F5IN fusion).
       * Contested (or pending-unverifiable) channels keep the wide
       * passthrough — focus cannot separate what the splitter refused to. */
      ts->hold = 0;
      if (ts->fhold > 0 && fabs(c[0].hz - ts->fhold_hz) <= TS_STABLE_HZ) {
        ts->fhold++;
      } else {
        ts->fhold = 1;
      }
      ts->fhold_hz = c[0].hz;
      if (ts->fhold >= TS_HOLD) { focus_engage(ts, c[0].hz); }
    } else {
      ts->hold  = 0;
      ts->fhold = 0;
    }
    return;
  }

  /* Split mode: every slot claims the nearest peak within the track radius —
   * carriers sit ≥20 Hz apart while the radius is 12, so claims can't
   * collide. A claimed slot follows its carrier (EMA); an unclaimed one
   * ages toward the TTL (the station is between overs, or gone). */
  gboolean claimed[SKIM_TONE_SPLIT_MAX + 1] = { FALSE };
  for (guint s = 0; s < ts->nslots; s++) {
    TsSlot *sl   = &ts->slot[s];
    gint    best = -1;
    double  bd   = TS_TRACK_HZ;
    for (guint k = 0; k < nc; k++) {
      const double d = fabs(c[k].hz - sl->mix_hz);
      if (!claimed[k] && d < bd) {
        bd   = d;
        best = (gint)k;
      }
    }
    if (best >= 0) {
      claimed[best] = TRUE;
      sl->unseen_s  = 0.0;
      sl->mix_hz += 0.25 * (c[best].hz - sl->mix_hz);
      sl->dphi = -2.0 * G_PI * sl->mix_hz / ts->rate;
    } else {
      sl->unseen_s += TS_EVAL_S;
    }
  }

  /* Unclaimed peaks: far from every slot → a NEW station gets a slot;
   * close to a slot → that slot's band holds two lines → contested. In
   * FOCUS (one slot) a pending-unverifiable candidate counts as evidence
   * too: its position is unknown, and the narrow filter would happily pass
   * a beat the wide passthrough at least flagged. */
  for (guint s = 0; s < ts->nslots; s++) {
    gboolean crowd = (ts->nslots == 1) && pending;
    for (guint k = 0; k < nc; k++) {
      if (!claimed[k] &&
          fabs(c[k].hz - ts->slot[s].mix_hz) < TS_ENGAGE_HZ) {
        crowd = TRUE;
      }
    }
    slot_set_contested(&ts->slot[s], crowd);
  }
  for (guint k = 0; k < nc; k++) {
    if (claimed[k] || ts->nslots >= SKIM_TONE_SPLIT_MAX)
      continue;
    double dmin = ts->rate;
    for (guint s = 0; s < ts->nslots; s++) {
      dmin = MIN(dmin, fabs(c[k].hz - ts->slot[s].mix_hz));
    }
    if (dmin >= TS_ENGAGE_HZ) {
      slot_start(ts, &ts->slot[ts->nslots], c[k].hz);
      ts->nslots++;
      if (ts->debug) {
        g_printerr("tone-split %p: SPAWN slot %u at %+.1f Hz\n", (void *)ts,
                   ts->nslots - 1, c[k].hz);
      }
    }
  }

  /* TTL kills — compact downward; a moved slot keeps its gen (≠ whatever
   * the owner cached at that index, so the owner resets, as it must). */
  for (gint s = (gint)ts->nslots - 1; s >= 0; s--) {
    if (ts->slot[s].unseen_s <= TS_TTL_S)
      continue;
    if (ts->debug) {
      g_printerr("tone-split %p: KILL slot %d (%+.1f Hz)\n", (void *)ts, s,
                 ts->slot[s].mix_hz);
    }
    for (guint m = (guint)s; m + 1 < ts->nslots; m++) {
      ts->slot[m] = ts->slot[m + 1];
    }
    ts->nslots--;
    ts->slot[ts->nslots].active = FALSE;
  }
  /* One slot left with focus armed = focus mode: stay narrow (retap gives it
   * the focus cutoff). Without focus, one slot = the old collapse. Zero
   * slots (all carriers dead — possible when both TTLs expire in the same
   * eval, which used to leave the channel DARK forever) always revives the
   * passthrough. */
  if (ts->nslots == 0 || (ts->nslots == 1 && ts->focus_fc <= 0)) {
    collapse(ts);
    return;
  }
  retap_all(ts);
}

/* ---- the sample path ------------------------------------------------------------ */

void skim_tone_split_push(SkimToneSplit *ts, const float *iq, guint nframes) {
  for (guint i = 0; i < nframes; i++) {
    const float I = iq[2 * i], Q = iq[2 * i + 1];

    ts->wbuf[2 * ts->wfill]     = I;
    ts->wbuf[2 * ts->wfill + 1] = Q;
    if (++ts->wfill == TS_FFT) {
      accumulate_psd(ts);
      memmove(ts->wbuf, ts->wbuf + 2 * TS_HOP,
              2 * (TS_FFT - TS_HOP) * sizeof(float));
      ts->wfill = TS_FFT - TS_HOP;
    }

    if (!ts->split) {
      ring_put(&ts->slot[0], I, Q);         /* passthrough: sample-exact     */
    } else {
      for (guint s = 0; s < ts->nslots; s++) {
        TsSlot *sl = &ts->slot[s];
        const float cp = (float)cos(sl->phi), sp = (float)sin(sl->phi);
        const float mr = I * cp - Q * sp;   /* ×e^{jφ}, φ runs at −mix_hz    */
        const float mi = I * sp + Q * cp;
        sl->phi += sl->dphi;
        if (sl->phi > G_PI) { sl->phi -= 2.0 * G_PI; }
        if (sl->phi < -G_PI) { sl->phi += 2.0 * G_PI; }

        sl->dl[2 * sl->dpos]     = mr;
        sl->dl[2 * sl->dpos + 1] = mi;
        float or_ = 0.0f, oi = 0.0f;
        guint at = sl->dpos;
        for (guint k = 0; k < TS_TAPS; k++) {
          or_ += sl->taps[k] * sl->dl[2 * at];
          oi  += sl->taps[k] * sl->dl[2 * at + 1];
          at = (at == 0) ? TS_TAPS - 1 : at - 1;
        }
        sl->dpos = (sl->dpos + 1) % TS_TAPS;
        ring_put(sl, or_, oi);
      }
    }
  }

  ts->eval_due -= nframes;
  if (ts->eval_due <= 0) {
    eval_topology(ts);
    ts->eval_due += TS_EVAL_S * ts->rate;
  }
}

/* ---- queries -------------------------------------------------------------------- */

guint skim_tone_split_slots(const SkimToneSplit *ts) { return ts->nslots; }

double skim_tone_split_slot_hz(const SkimToneSplit *ts, guint slot) {
  return slot < ts->nslots ? ts->slot[slot].mix_hz : 0.0;
}

guint skim_tone_split_slot_gen(const SkimToneSplit *ts, guint slot) {
  return slot < ts->nslots ? ts->slot[slot].gen : 0;
}

gboolean skim_tone_split_slot_contested(const SkimToneSplit *ts, guint slot) {
  return slot < ts->nslots ? ts->slot[slot].contested : FALSE;
}
