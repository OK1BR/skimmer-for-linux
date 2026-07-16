/* decode_cw_v2.c — CW decode backend v2: soft-decision semi-Markov Viterbi.
 *
 * v1's plumbing survives verbatim — |IQ| envelope (3-tap MA), dual-rate
 * level trackers, hysteresis squelch, keying-duty test, phase-slope tone
 * offset, pause/reset semantics. What v2 replaces is the decision layer:
 * where v1 runs a Schmitt trigger and classifies hard run lengths, v2 keeps
 * every sample SOFT and lets a Viterbi search pick the element sequence.
 *
 *   sample → mark/space log-likelihood ratio      (fast µ_mark/µ_space
 *            discriminator, ~0.4 s time constants  — it FOLLOWS QSB)
 *   LLR    → semi-Markov Viterbi over segments {DIT, DAH} × {ESP, CSP, WSP}
 *            with log-normal duration priors tied to the adaptive dit
 *   path   → lag-committed traceback → Morse code assembly (v1's table,
 *            lone-E/T rule, break separator)
 *
 * Why: v1's binary threshold LOSES elements in deep QSB — a faded trailing
 * dit never crosses the Schmitt threshold and "7" (--...) mutates into "G"
 * (--.), a dropout splits a dash (live-caught 9A170NT, 2026-07-15). In v2 a
 * faded dit still scores *less space-like* than its surroundings, and the
 * duration priors carry it: the element is weak evidence, not no evidence.
 *
 * The dit clock bootstraps exactly like v1 (Schmitt run clustering) — the
 * lattice needs a timing anchor before its duration priors mean anything.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_cw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CW2_DBG(...) \
  do { if (g_getenv("SKIM_CW2_DEBUG")) { fprintf(stderr, __VA_ARGS__); } } while (0)

/* --- Morse table (v1's, duplicated so v1 stays untouched) ------------------- */

static const struct { char c; const char *m; } MORSE[] = {
  {'A',".-"},   {'B',"-..."}, {'C',"-.-."}, {'D',"-.."},  {'E',"."},
  {'F',"..-."}, {'G',"--."},  {'H',"...."}, {'I',".."},   {'J',".---"},
  {'K',"-.-"},  {'L',".-.."}, {'M',"--"},   {'N',"-."},   {'O',"---"},
  {'P',".--."}, {'Q',"--.-"}, {'R',".-."},  {'S',"..."},  {'T',"-"},
  {'U',"..-"},  {'V',"...-"}, {'W',".--"},  {'X',"-..-"}, {'Y',"-.--"},
  {'Z',"--.."},
  {'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},
  {'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
  {'/',"-..-."},{'?',"..--.."},{'=',"-...-"},{'+',".-.-."},{'.',".-.-.-"},
  {',',"--..--"},{'-',"-....-"},{'@',".--.-."},
};

static char morse_lut[256];

static void morse_init(void) {
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    memset(morse_lut, 0, sizeof(morse_lut));
    for (guint i = 0; i < G_N_ELEMENTS(MORSE); i++) {
      guint code = 1;
      for (const char *p = MORSE[i].m; *p; p++) {
        code = (code << 1) | (*p == '-' ? 1u : 0u);
      }
      morse_lut[code] = MORSE[i].c;
    }
    g_once_init_leave(&once, 1);
  }
}

/* --- lattice geometry --------------------------------------------------------- */

#define W        2048                   /* ring size, power of two            */
#define WMASK    (W - 1)
#define NEG_INF  (-1e30f)

/* Segment types. Marks and spaces alternate by construction. */
enum { SEG_DIT, SEG_DAH, SEG_ESP, SEG_CSP, SEG_WSP };

/* Duration prior: −α·ln²(d/ideal). Ideals are 1/3/1/3/7 dits; α is tight
 * for elements (keying is the rigid part of a fist) and loose for word
 * spaces (operators breathe). */
static const double SEG_IDEAL[5] = { 1.0, 3.0, 1.0, 3.0, 7.0 };
static const double SEG_ALPHA[5] = { 10.0, 10.0, 10.0, 6.0, 2.5 };

/* What kind of boundary the committed prefix ends in. EITHER is the fresh
 * seed: both a mark and a space may open the lattice (a restart can land
 * mid-pause, and the first real mark may be many dits away). */
enum { K_MARK, K_SPACE, K_EITHER };

/* Per-segment fixed cost: an MDL-style anti-chatter term — a boundary must
 * pay for itself in evidence. */
#define SEG_COST     0.5
/* Observation model: llr = K·(m − mid)/(µm − µs), clamped. */
#define LLR_K        5.0
#define LLR_MAX      2.5
/* Commit lag, dits: boundaries older than this are stable. The full lag
 * matters in a fade — evidence that rescues a drowned element trickles in
 * late. On a healthy signal the lattice never revises that far back, so a
 * short lag cuts the text latency roughly in half (Richard, 2026-07-15:
 * "the decode trails the audio"). */
#define LAG_DITS       8.0
#define LAG_DITS_FAST  4.0
#define BOOT_MARKS   8
#define BREAK_DITS   16.0
#define BREAK_MARK   "\xC2\xB7 "        /* "· " over separator (v1)           */

/* Fist model: keying is the rigid part of a fist, SPACING is the sloppy
 * one — real operators run char gaps anywhere from 2.5 to 5+ dits and word
 * gaps 5–11, so any fixed char/word boundary misreads somebody (live
 * 2026-07-16: EA1EYL's stretched CQ filed as "C Q" — two words, the CQ
 * marker never matched, the spot never fired). The decoder learns the
 * operator's OWN two space centres from the last FIST_RING committed gaps:
 * 2-means in the log domain, seeded from the ring's QUANTILES — seeding
 * from the model's own labels would self-reinforce (a stretched char gap
 * filed as word space teaches the word centre DOWN, never the char centre
 * up; raw-duration quantiles are label-free). An accepted fit (both
 * clusters populated, separation ≥ FIST_SEP) blends into the centres,
 * which move the duration priors, the lattice search windows and the
 * live-emission clocks together. The centres reset with the dit clock —
 * fist forgotten, spacing forgotten. */
#define FIST_RING    24
#define FIST_MIN_D   1.6                /* below: element space, not a gap    */
#define FIST_REFIT   4                  /* refit every this many new gaps     */
#define FIST_MIN_FIT 10                 /* ring entries before the first fit  */
#define FIST_SEP     1.55               /* min wsp/csp ratio to accept a fit  */
#define FIST_CSP_LO  2.4
#define FIST_CSP_HI  4.8
#define FIST_WSP_LO  5.0
#define FIST_WSP_HI  11.0
#define FIST_GAIN    0.25               /* blend of an accepted fit           */
#define FIST_CSP0    3.0                /* seeds = the ITU ideals             */
#define FIST_WSP0    7.0

typedef struct {
  double rate;

  /* --- v1 plumbing: envelope, trackers, squelch ---------------------------- */
  float  ma[3];
  guint  ma_n;
  double env_hi, env_lo;
  double k_hi_rel, k_lo, k_ph;
  gboolean gate_open;
  double   p_high;
  gboolean keying;

  /* --- bootstrap Schmitt (until the dit clock is learned) ------------------ */
  gboolean key;
  double   run;
  gboolean pend_valid;
  gboolean pend_key;
  double   pend_len;
  double   boot[BOOT_MARKS];
  guint    nboot;

  /* --- element clock -------------------------------------------------------- */
  double dit;                           /* samples per dit; 0 = unlearned     */
  double esp;                           /* EMA of inter-element spaces. The
                                         * soft edges bias marks LONG and
                                         * spaces SHORT by the same amount;
                                         * (dit+esp)/2 is the unbiased dit
                                         * the reported WPM comes from      */
  double elem_err;                      /* EMA of |d − ideal|/ideal           */
  guint  marks_ok;

  /* --- soft discriminator ---------------------------------------------------- */
  double mu_m, mu_s;                    /* mark / space level trackers        */
  double k_m_dn, k_s;
  guint  cont_mark;                     /* samples continuously above mid     */
  guint  cont_space;                    /* samples continuously below mid     */
  gboolean carrier;                     /* long solid mark — not keying       */

  /* --- Viterbi lattice (allocated lazily — noise channels never pay) -------- */
  gboolean active;
  guint64  t;                           /* absolute sample index              */
  guint64  commit_t;                    /* last committed boundary            */
  guint8   commit_kind;                 /* boundary kind at commit_t          */
  struct Lattice {
    double   cum[W];                    /* cumulative llr, cum[t & WMASK]     */
    float    bm[W], bs[W];              /* best score, segment ends at t      */
    guint16  bd_m[W], bd_s[W];          /* traceback: duration                */
    guint8   bt_m[W], bt_s[W];          /* traceback: segment type            */
  } *lat;

  /* --- character assembly ----------------------------------------------------- */
  guint    code;
  guint    nelem;
  gboolean brk_out;
  gboolean paused;
  double   quiet;
  /* Live emission (v1's 2.2/5.5-dit rule): a space in progress on the best
   * path emits the finished char/word WITHOUT waiting for the closing mark
   * — the last char of an over has no closing mark. live_t remembers which
   * space boundary fired, so its eventual CSP/WSP commit does not repeat. */
  gboolean char_live, word_live, live_had_char;
  guint64  live_t;

  /* --- fist model (see FIST_* above) ------------------------------------------ */
  double fist_csp, fist_wsp;            /* learned char/word gap centres, dits */
  float  gaps[FIST_RING];               /* recent gaps ≥ FIST_MIN_D, ln(dits)  */
  guint  ngaps, gap_head, gap_fresh;

  /* --- estimates -------------------------------------------------------------- */
  double prev_i, prev_q;
  double foff_hz;
  double snr_latch;                     /* SNR at the last mark commit — the
                                         * peak tracker has decayed by the
                                         * time a trailing break emits      */
} Cw2State;

static gpointer cw2_channel_new(double sample_rate) {
  morse_init();
  Cw2State *st = g_new0(Cw2State, 1);
  st->rate     = sample_rate > 0 ? sample_rate : 250.0;
  st->k_hi_rel = 1.0 - exp(-1.0 / (st->rate * 0.8));
  st->k_lo     = 1.0 - exp(-1.0 / (st->rate * 0.3));
  st->k_ph     = 1.0 - exp(-1.0 / (st->rate * 0.7));
  /* µ_mark rides QSB down within one fade edge (0.35 s release); µ_space
   * follows the below-mid mean a touch faster than env_lo. */
  st->k_m_dn   = 1.0 - exp(-1.0 / (st->rate * 0.35));
  st->k_s      = 1.0 - exp(-1.0 / (st->rate * 0.25));
  st->env_hi = st->env_lo = -1.0;
  st->elem_err = 0.2;
  st->fist_csp = FIST_CSP0;
  st->fist_wsp = FIST_WSP0;
  return st;
}

static void cw2_channel_free(gpointer state) {
  Cw2State *st = state;
  if (!st)
    return;
  g_free(st->lat);
  g_free(st);
}

/* --- assembly helpers (v1 semantics) ------------------------------------------ */

static void emit(SkimDecode *out, guint *pos, char c) {
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

static void emit_code(Cw2State *st, SkimDecode *out, guint *pos) {
  if (st->nelem && st->code < G_N_ELEMENTS(morse_lut)) {
    char c = morse_lut[st->code];
    gboolean lone_et = st->nelem == 1 &&
                       (st->marks_ok < 4 || st->elem_err > 0.45);
    if (c && !lone_et) { emit(out, pos, c); }
  }
  st->code  = 0;
  st->nelem = 0;
}

/* --- lattice ------------------------------------------------------------------- */

static void lattice_start(Cw2State *st) {
  if (!st->lat) { st->lat = g_new0(struct Lattice, 1); }
  struct Lattice *L = st->lat;
  for (guint i = 0; i < W; i++) {
    L->bm[i] = NEG_INF;
    L->bs[i] = NEG_INF;
  }
  st->commit_t    = st->t;
  st->commit_kind = K_EITHER;           /* first segment: mark OR space       */
  L->bm[st->t & WMASK]  = 0.0f;
  L->bs[st->t & WMASK]  = 0.0f;
  L->bd_m[st->t & WMASK] = 0;           /* seed boundaries stop the traceback */
  L->bd_s[st->t & WMASK] = 0;
  L->cum[st->t & WMASK]  = 0.0;
  st->mu_m = st->env_hi;
  st->mu_s = st->env_lo;
  st->active = TRUE;
  st->cont_mark = 0;
  st->char_live = st->word_live = FALSE;
  st->live_had_char = FALSE;
}

/* Duration prior in the log domain. CSP/WSP ideals ride the fist model. */
static inline double dur_pen(const Cw2State *st, int type, double d) {
  const double ideal = type == SEG_CSP   ? st->fist_csp
                       : type == SEG_WSP ? st->fist_wsp
                                         : SEG_IDEAL[type];
  double r = log(d / (ideal * st->dit));
  return -SEG_ALPHA[type] * r * r - SEG_COST;
}

static int fist_cmp(const void *a, const void *b) {
  const float d = *(const float *)a - *(const float *)b;
  return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/* Feed one committed gap and, periodically, refit the two space centres —
 * see the FIST_* block for the why and the self-reinforcement trap. */
static void fist_learn(Cw2State *st, double d_dits) {
  if (d_dits < FIST_MIN_D)
    return;
  st->gaps[st->gap_head] = (float)log(d_dits);
  st->gap_head = (st->gap_head + 1) % FIST_RING;
  if (st->ngaps < FIST_RING) { st->ngaps++; }
  if (++st->gap_fresh < FIST_REFIT || st->ngaps < FIST_MIN_FIT)
    return;
  st->gap_fresh = 0;

  float s[FIST_RING];
  memcpy(s, st->gaps, st->ngaps * sizeof(float));
  qsort(s, st->ngaps, sizeof(float), fist_cmp);
  double c1 = s[st->ngaps / 4];         /* label-free quantile seeds          */
  double c2 = s[(3 * st->ngaps) / 4];
  if (c2 - c1 < 1e-6)
    return;                             /* unimodal pile — nothing to learn   */
  guint n1 = 0, n2 = 0;
  for (int it = 0; it < 3; it++) {
    double s1 = 0.0, s2 = 0.0;
    n1 = n2 = 0;
    for (guint i = 0; i < st->ngaps; i++) {
      if (fabs(s[i] - c1) <= fabs(s[i] - c2)) {
        s1 += s[i];
        n1++;
      } else {
        s2 += s[i];
        n2++;
      }
    }
    if (!n1 || !n2)
      return;
    c1 = s1 / n1;
    c2 = s2 / n2;
  }
  if (exp(c2 - c1) < FIST_SEP || n1 < 3 || n2 < 3)
    return;                             /* clusters not credibly two          */
  const double csp = CLAMP(exp(c1), FIST_CSP_LO, FIST_CSP_HI);
  st->fist_csp += FIST_GAIN * (csp - st->fist_csp);
  const double wlo = MAX(FIST_WSP_LO, 1.6 * st->fist_csp);
  const double wsp = CLAMP(exp(c2), wlo, FIST_WSP_HI);
  st->fist_wsp += FIST_GAIN * (wsp - st->fist_wsp);
  CW2_DBG("[fist csp=%.2f wsp=%.2f (fit %.2f/%.2f, %u+%u gaps)]\n",
          st->fist_csp, st->fist_wsp, exp(c1), exp(c2), n1, n2);
}

/* One Viterbi step for the sample at st->t (llr already in the ring). */
static void lattice_step(Cw2State *st) {
  struct Lattice *L = st->lat;
  const guint64 t   = st->t;
  const double  T   = st->dit;
  const double  ct  = L->cum[t & WMASK];
  const guint64 cmt = st->commit_t;

  /* Mark ends at t: preceded by a space (or seed) boundary at t−d. */
  float  best_m = NEG_INF;
  guint16 bd = 0;
  guint8  bt = 0;
  /* Ranges OVERLAP on purpose: a hard dit/dah cutoff at a stale dit clock
   * misfiles every element after a speed change (28 WPM dahs = 1.9× an
   * 18 WPM dit — the classifier lock-up the first build hit). Inside the
   * overlap the duration priors decide, and they move with the clock. */
  static const int MTYPES[2] = { SEG_DIT, SEG_DAH };
  static const double MLO[2] = { 0.60, 1.6 }, MHI[2] = { 2.6, 5.0 };
  for (int k = 0; k < 2; k++) {
    int dlo = MAX(2, (int)(MLO[k] * T + 0.5));
    int dhi = MIN((int)(MHI[k] * T + 0.5), W / 2);
    for (int d = dlo; d <= dhi; d++) {
      if (t < (guint64)d || t - d < cmt)
        break;                          /* d grows — only older from here     */
      if (t - d == cmt && st->commit_kind == K_MARK)
        continue;                       /* two marks may not touch            */
      float prev = L->bs[(t - d) & WMASK];
      if (prev <= NEG_INF)
        continue;
      double ev = ct - L->cum[(t - d) & WMASK];
      float  sc = prev + (float)(ev + dur_pen(st, MTYPES[k], d));
      if (sc > best_m) {
        best_m = sc;
        bd = (guint16)d;
        bt = (guint8)MTYPES[k];
      }
    }
  }
  L->bm[t & WMASK]   = best_m;
  L->bd_m[t & WMASK] = bd;
  L->bt_m[t & WMASK] = bt;

  /* Space ends at t: preceded by a mark (or seed) boundary at t−d. WSP's
   * ceiling reaches past the break length so a lattice seeded mid-pause can
   * always bridge the remaining quiet to the first real mark. */
  float best_s = NEG_INF;
  bd = 0;
  bt = 0;
  /* ESP's floor sits at 0.7 dits: a real inter-element space is never
   * shorter (0.85 at −15 % jitter), but a sub-dit flutter dropout inside a
   * dash is — without the floor the lattice happily reads the dropout as
   * an inter-element space and splits the dash (dot-dot for a dash).
   * EXCEPT while the element clock is visibly wrong (elem_err high — a
   * speed change in progress): a stale slow dit makes 0.7·T LONGER than
   * the new true spaces and the lattice cannot separate elements at all.
   * The loose floor lets it re-lock; steady copy restores the strict one. */
  const double esp_lo = st->elem_err > 0.20 ? 0.50 : 0.70;
  static const int STYPES[3] = { SEG_ESP, SEG_CSP, SEG_WSP };
  /* CSP/WSP windows ride the fist centres (seeds reproduce the old fixed
   * 1.6–5.5 / 4.0–17: 1.833·3 = 5.5, 0.571·7 = 4.0). The overlap between
   * CSP hi and WSP lo survives any centre pair — the priors decide there. */
  const double SLO[3] = { 0.70, 1.6, 0.571 * st->fist_wsp };
  const double SHI[3] = { 2.6, 1.833 * st->fist_csp, 17.0 };
  for (int k = 0; k < 3; k++) {
    int dlo = MAX(2, (int)((k == 0 ? esp_lo : SLO[k]) * T + 0.5));
    int dhi = MIN((int)(SHI[k] * T + 0.5), W / 2);
    for (int d = dlo; d <= dhi; d++) {
      if (t < (guint64)d || t - d < cmt)
        break;
      if (t - d == cmt && st->commit_kind == K_SPACE)
        continue;                       /* two spaces may not touch           */
      float prev = L->bm[(t - d) & WMASK];
      if (prev <= NEG_INF)
        continue;
      double ev = -(ct - L->cum[(t - d) & WMASK]);
      float  sc = prev + (float)(ev + dur_pen(st, STYPES[k], d));
      if (sc > best_s) {
        best_s = sc;
        bd = (guint16)d;
        bt = (guint8)STYPES[k];
      }
    }
  }
  L->bs[t & WMASK]   = best_s;
  L->bd_s[t & WMASK] = bd;
  L->bt_s[t & WMASK] = bt;
}

/* Best full-path hypothesis "now": a boundary at τ plus the in-progress
 * segment (τ, t] scored with optimistic (zero) duration penalty. */
static void lattice_head(const Cw2State *st, guint64 *out_tau,
                         gboolean *out_mark_ended) {
  const struct Lattice *L = st->lat;
  const guint64 t = st->t;
  const double  ct = L->cum[t & WMASK];
  const int back = MIN((int)(17.0 * st->dit), (int)(t - st->commit_t));
  double best = -1e30;
  guint64 tau = st->commit_t;
  gboolean mark_ended = st->commit_kind != K_SPACE;
  for (int d = 0; d <= back; d++) {
    guint64 at = t - d;
    double ev = ct - L->cum[at & WMASK];       /* llr sum over (τ, t]        */
    if (L->bs[at & WMASK] > NEG_INF) {         /* space ended → mark running */
      double sc = L->bs[at & WMASK] + ev;
      if (sc > best) { best = sc; tau = at; mark_ended = FALSE; }
    }
    if (L->bm[at & WMASK] > NEG_INF) {         /* mark ended → space running */
      double sc = L->bm[at & WMASK] - ev;
      if (sc > best) { best = sc; tau = at; mark_ended = TRUE; }
    }
  }
  *out_tau = tau;
  *out_mark_ended = mark_ended;
}

/* Commit the stable prefix of the best path (segments ending ≤ t − lag) and
 * feed it to the character assembly. With lag 0 everything commits (flush). */
static void lattice_commit(Cw2State *st, double lag_dits, SkimDecode *out,
                           guint *pos) {
  if (!st->active || st->t <= st->commit_t)
    return;
  struct Lattice *L = st->lat;
  guint64 tau;
  gboolean mark_ended;
  lattice_head(st, &tau, &mark_ended);

  /* Walk the backpointers down to commit_t, collecting segments. */
  guint16 segs_d[64];
  guint8  segs_t[64];
  guint   n = 0;
  guint64 at = tau;
  gboolean m = mark_ended;
  while (at > st->commit_t && n < 64) {
    guint16 d = m ? L->bd_m[at & WMASK] : L->bd_s[at & WMASK];
    guint8  ty = m ? L->bt_m[at & WMASK] : L->bt_s[at & WMASK];
    if (d == 0)
      break;                                   /* seed boundary reached      */
    segs_d[n] = d;
    segs_t[n] = ty;
    n++;
    at -= d;
    m = !m;                                    /* alternation                */
  }

  /* Oldest first, commit those whose END is out of the revision window. */
  const guint64 lag = (guint64)(lag_dits * st->dit);
  const guint64 stable = st->t > lag ? st->t - lag : 0;
  guint64 end = st->commit_t;
  gboolean committed_any = FALSE;
  for (guint i = n; i > 0; i--) {
    end += segs_d[i - 1];
    if (end > stable)
      break;
    const int ty = segs_t[i - 1];
    const double d = segs_d[i - 1];
    const double ideal = SEG_IDEAL[ty] * st->dit;
    /* Did the live rule already emit for the space starting here? */
    const gboolean lived = st->char_live && (end - segs_d[i - 1]) == st->live_t;
    CW2_DBG("[commit %s d=%.0f dit=%.1f err=%.2f]\n",
            (const char *[]){"DIT","DAH","esp","CSP","WSP"}[ty], d, st->dit,
            st->elem_err);
    switch (ty) {
    case SEG_DIT:
    case SEG_DAH: {
      st->elem_err += 0.1 * (fabs(d - ideal) / ideal - st->elem_err);
      /* Track the clock harder while it is visibly wrong (speed change) —
       * commits arrive a lag behind, so the leisurely steady-state gain
       * would still be converging when the over ends. */
      const double a = st->elem_err > 0.20 ? 0.30 : 0.15;
      st->dit += a * ((ty == SEG_DAH ? d / 3.0 : d) - st->dit);
      st->dit  = CLAMP(st->dit, st->rate * 0.022, st->rate * 0.30);
    }
      st->marks_ok++;
      if (st->nelem < 7) {
        st->code = (st->nelem ? st->code : 1u) << 1 |
                   (ty == SEG_DAH ? 1u : 0u);
        st->nelem++;
      } else {
        st->code = 0;
        st->nelem = 0;
      }
      st->brk_out = FALSE;
      st->char_live = st->word_live = FALSE;   /* a new space will come      */
      st->snr_latch = 20.0 * log10(st->env_hi / MAX(st->env_lo, 1e-9));
      break;
    case SEG_ESP:
      st->esp = st->esp > 0 ? st->esp + 0.2 * (d - st->esp) : d;
      break;
    case SEG_CSP:
      fist_learn(st, d / st->dit);
      if (!lived) { emit_code(st, out, pos); }
      st->char_live = st->word_live = FALSE;
      break;
    case SEG_WSP:
      fist_learn(st, d / st->dit);
      if (!lived) {
        emit_code(st, out, pos);
        emit(out, pos, ' ');
      } else if (!st->word_live && st->live_had_char) {
        emit(out, pos, ' ');                   /* live char, space grew      */
      }
      st->char_live = st->word_live = FALSE;
      break;
    }
    st->commit_t    = end;
    st->commit_kind = (ty == SEG_DIT || ty == SEG_DAH) ? K_MARK : K_SPACE;
    committed_any   = TRUE;
  }

  /* Renormalize scores once they grow large (float precision). */
  if (committed_any) {
    float base = st->commit_kind == K_MARK ? L->bm[st->commit_t & WMASK]
                                           : L->bs[st->commit_t & WMASK];
    if (fabsf(base) > 1e5f) {
      for (guint i = 0; i < W; i++) {
        if (L->bm[i] > NEG_INF) { L->bm[i] -= base; }
        if (L->bs[i] > NEG_INF) { L->bs[i] -= base; }
      }
    }
  }
}

/* Flush everything decodable and stop the lattice (signal gone / carrier). */
static void lattice_flush(Cw2State *st, SkimDecode *out, guint *pos) {
  if (!st->active)
    return;
  lattice_commit(st, 0.0, out, pos);
  emit_code(st, out, pos);                     /* the char in flight         */
  st->active = FALSE;
}

/* --- the backend --------------------------------------------------------------- */

static gboolean cw2_process(gpointer state, const float *iq, guint nframes,
                            SkimDecode *out) {
  Cw2State *st = state;
  guint pos = 0;
  out->text[0] = '\0';

  for (guint n = 0; n < nframes; n++) {
    const float i = iq[2 * n], q = iq[2 * n + 1];
    const float mag = sqrtf(i * i + q * q);

    st->ma[st->ma_n % 3] = mag;
    st->ma_n++;
    float m = (st->ma[0] + st->ma[1] + st->ma[2]) / (float)MIN(st->ma_n, 3);

    /* Level trackers + squelch + keying-duty — v1 verbatim. */
    if (st->env_hi < 0) { st->env_hi = st->env_lo = m; }
    st->env_hi = m > st->env_hi
                     ? m
                     : st->env_hi + st->k_hi_rel * (m - st->env_hi);
    if (m < 0.5 * (st->env_lo + st->env_hi)) {
      st->env_lo += st->k_lo * (m - st->env_lo);
    }
    if (!st->gate_open) {
      st->gate_open = st->env_hi > 4.0 * st->env_lo + 1e-6;
    } else if (st->env_hi < 2.6 * st->env_lo + 1e-6) {
      st->gate_open = FALSE;
    }
    const double span0 = st->env_hi - st->env_lo;
    st->p_high += st->k_ph *
                  ((m > st->env_lo + 0.7 * span0 ? 1.0 : 0.0) - st->p_high);
    if (!st->keying) {
      st->keying = st->p_high > 0.15;
    } else if (st->p_high < 0.07) {
      st->keying = FALSE;
    }

    const gboolean solid =
        st->dit > 0 && st->marks_ok >= 8 && st->elem_err < 0.35;
    /* Deep QSB pulls faded marks under the envelope midpoint, feeds them
     * to env_lo and slams the envelope gate shut mid-callsign (v1 tears
     * "9A170NT" apart exactly this way). The µ discriminator knows better:
     * in a trough µ_m/µ_s stays ≫ the 1.5 floor it collapses to on noise —
     * a SOLID channel with a live discriminator rides the dip out. */
    const gboolean mu_alive =
        st->active && st->mu_m > 3.0 * st->mu_s;
    if ((!st->gate_open && !(solid && mu_alive)) ||
        (!st->keying && !solid)) {
      if (solid || st->nboot) {
        if (!st->paused) {
          CW2_DBG("[pause t=%lu gate=%d keying=%d solid=%d marks=%u err=%.2f "
                  "hi=%.3f lo=%.3f]\n",
                  (unsigned long)st->t, st->gate_open, st->keying, solid,
                  st->marks_ok, st->elem_err, st->env_hi, st->env_lo);
          lattice_flush(st, out, &pos);
          if (st->dit > 0 && !st->brk_out) {
            st->brk_out = TRUE;
            emit_str(out, &pos, BREAK_MARK);
          }
          st->code = 0;
          st->nelem = 0;
          st->paused = TRUE;
          st->quiet  = 0.0;
        }
        st->quiet += 1.0;
        if (st->quiet > 8.0 * st->rate) {      /* forget the fist            */
          st->dit = 0;
          st->nboot = 0;
          st->marks_ok = 0;
          st->paused = FALSE;
          st->fist_csp = FIST_CSP0;            /* spacing style goes with it */
          st->fist_wsp = FIST_WSP0;
          st->ngaps = st->gap_head = st->gap_fresh = 0;
        }
      }
      st->key = FALSE;
      st->run = 0;
      st->pend_valid = FALSE;
      st->t++;
      continue;
    }
    if (st->paused) {
      st->paused = FALSE;                      /* signal is back             */
      st->carrier = FALSE;
      if (st->dit > 0) { lattice_start(st); }
    }

    /* Tone offset from the phase slope, SOLID mark samples only — the 0.55
     * bar matches v1's Schmitt ON level. A softer bar (the discriminator
     * midpoint) let borderline samples in, and their noisy phase slope
     * dragged the estimate toward the channel centre — the reported
     * frequency kept sliding with signal strength (live 2026-07-15). */
    const double mid_now =
        st->active || st->carrier
            ? st->mu_s + 0.55 * (st->mu_m - st->mu_s)
            : st->env_lo + 0.55 * (st->env_hi - st->env_lo);
    if (m > mid_now && mag > st->env_lo * 3.0) {
      const double dr = (double)i * st->prev_i + (double)q * st->prev_q;
      const double di = (double)q * st->prev_i - (double)i * st->prev_q;
      if (dr != 0.0 || di != 0.0) {
        const double f = atan2(di, dr) * st->rate / (2.0 * G_PI);
        st->foff_hz += 0.01 * (f - st->foff_hz);
      }
    }
    st->prev_i = i;
    st->prev_q = q;

    if (!st->active && !st->carrier) {
      /* Bootstrap: v1's Schmitt + first-marks clustering learns the dit. */
      const double span = st->env_hi - st->env_lo;
      const double thr  = st->env_lo + (st->key ? 0.30 : 0.55) * span;
      const gboolean want = m > thr;
      const double glitch = 3.0;
      if (want != st->key) {
        if (st->pend_valid) {
          st->run += st->pend_len;
          st->key  = want;
          st->pend_valid = FALSE;
        } else {
          st->pend_key   = st->key;
          st->pend_len   = st->run;
          st->pend_valid = TRUE;
          st->key = want;
          st->run = 0.0;
        }
      }
      st->run += 1.0;
      if (st->pend_valid && st->run > glitch) {
        if (st->pend_key && st->pend_len >= glitch) {
          if (st->nboot < BOOT_MARKS) { st->boot[st->nboot++] = st->pend_len; }
          double lo = st->boot[0], hi = st->boot[0];
          for (guint b = 1; b < st->nboot; b++) {
            lo = MIN(lo, st->boot[b]);
            hi = MAX(hi, st->boot[b]);
          }
          if (st->nboot >= 4 && hi >= 2.0 * lo) {
            double mid = (lo + hi) / 2.0, sum = 0.0;
            guint  cnt = 0;
            for (guint b = 0; b < st->nboot; b++) {
              if (st->boot[b] < mid) { sum += st->boot[b]; cnt++; }
            }
            st->dit = CLAMP(sum / cnt, st->rate * 0.022, st->rate * 0.30);
          } else if (st->nboot == BOOT_MARKS) {
            st->dit = CLAMP(lo, st->rate * 0.022, st->rate * 0.30);
          }
          if (st->dit > 0) {
            CW2_DBG("[boot dit %.1f from %u marks]\n", st->dit, st->nboot);
            lattice_start(st);                 /* hand over to the Viterbi   */
          }
        }
        st->pend_valid = FALSE;
      }
      st->t++;
      continue;
    }

    /* --- soft path: discriminator → llr ring → Viterbi -----------------------
     * µ_mark is a FAST decaying peak (instant attack, ~0.35 s release): a
     * QSB trough pulls it down within one fade edge, so faded marks stay
     * above the midpoint and keep their positive evidence — the fix for
     * "the fade ate my dits". The µ_s·1.5 floor stops a long pause from
     * collapsing the midpoint into the noise. µ_space is the below-mid
     * mean. (An early conditional-update µ_mark got STUCK whenever a fade
     * dropped marks below the old midpoint — nothing above mid ever
     * updated it again; the decaying peak cannot stick.) */
    st->mu_m = m > st->mu_m ? (double)m
                            : st->mu_m + st->k_m_dn * (m - st->mu_m);
    st->mu_m = MAX(st->mu_m, MAX(1.5 * st->mu_s, 1e-12));
    const double mid = 0.5 * (st->mu_m + st->mu_s);
    if (m > mid) {
      st->cont_mark++;
    } else {
      /* Only CLEARLY space-like samples may teach µ_space. A fade's edge
       * throws mark samples just under the midpoint — letting them in
       * drags µ_s (and the midpoint) up under the whole trough until every
       * faded mark reads as space (the mid-word break cascade). */
      if (m < st->mu_s + 0.25 * (st->mu_m - st->mu_s)) {
        st->mu_s += st->k_s * (m - st->mu_s);
      }
      st->cont_mark = 0;
    }

    /* A solid mark far beyond a dash is a carrier, not keying (v1 rule):
     * drop the assembly and idle until the key finally opens. */
    if (st->carrier) {
      if (st->cont_mark == 0) {
        st->carrier = FALSE;
        lattice_start(st);
      }
      st->t++;
      continue;
    }
    if (st->cont_mark > (guint)(8.0 * st->dit)) {
      st->carrier = TRUE;
      st->active  = FALSE;                     /* drop the code in flight    */
      st->code    = 0;
      st->nelem   = 0;
      st->t++;
      continue;
    }

    /* Two llr views, averaged:
     *  - span: where does m sit between µ_space and µ_mark? Follows QSB,
     *    but cannot tell a −18 dB notch inside a dash from a real space.
     *  - Rayleigh: how plausible is m as pure noise? A notch bottoms out
     *    4–5× ABOVE the noise floor — "not noise" — while a real space
     *    sits ON it. Anchored to µ_s, blind to fading.
     * The average lets a sub-dit notch pass with ~zero evidence (duration
     * priors then carry the dash through), yet keeps real spaces at full
     * negative weight and low-level QRM in spaces near zero. */
    const double x = (m - mid) / (st->mu_m - st->mu_s);
    const double span_llr = CLAMP(LLR_K * x, -LLR_MAX, LLR_MAX);
    /* Zero crossing at z ≈ 2.8: Rayleigh noise clears that only ~2 % of
     * samples (no sustained runs — no phantom dits minted from the tail
     * after an over), while a −18 dB notch (z ≈ 5) and QSB-trough marks
     * (z ≈ 7) sit far in the positive. */
    const double z = m / MAX(st->mu_s / 1.2533, 1e-12);
    const double ray_llr =
        z <= 1.0 ? -LLR_MAX
                 : CLAMP(3.3 * (0.5 * z * z - log(z) - 2.89),
                         -LLR_MAX, LLR_MAX);
    const double llr = 0.5 * (span_llr + ray_llr);
    if (g_getenv("SKIM_CW2_TRACE")) {
      fprintf(stderr, "T %lu %.4f %.4f %.4f %.2f\n",
              (unsigned long)st->t, m, st->mu_m, st->mu_s, llr);
    }
    st->t++;
    st->lat->cum[st->t & WMASK] = st->lat->cum[(st->t - 1) & WMASK] + llr;
    lattice_step(st);

    if ((st->t & 7) == 0) {                    /* lagged commit, every 8     */
      /* Healthy signal (discriminator riding near the envelope peak, clock
       * trusted) → short lag; a fade or a re-lock keeps the full window. */
      const double lag =
          (st->mu_m > 0.6 * st->env_hi && st->elem_err < 0.20)
              ? LAG_DITS_FAST
              : LAG_DITS;
      lattice_commit(st, lag, out, &pos);

      /* Live emission (v1's rule, model-driven): once the best path says
       * "a space has been running since commit_t" — everything before it
       * is committed — the char in flight is DONE: emit it without waiting
       * for a closing mark (the last char of an over never gets one).
       * The word space and the over break follow the same clock. */
      guint64 tau;
      gboolean space_running;
      lattice_head(st, &tau, &space_running);
      if (space_running && tau == st->commit_t) {
        const double sp = (double)(st->t - tau);
        /* Live clocks ride the fist centres: char fires at 0.867·csp (the
         * old 2.6 dits at the 3.0 seed), word at 1.2·√(csp·wsp) (the old
         * 5.5 at the 3/7 seeds) — a stretched fist gets slower live text
         * instead of premature word splits. */
        if (!st->char_live && sp > 0.867 * st->fist_csp * st->dit) {
          st->char_live     = TRUE;
          st->live_t        = tau;
          st->live_had_char = st->nelem > 0;
          emit_code(st, out, &pos);
        }
        if (st->char_live && st->live_had_char && !st->word_live &&
            sp > 1.2 * sqrt(st->fist_csp * st->fist_wsp) * st->dit) {
          st->word_live = TRUE;
          emit(out, &pos, ' ');
        }
        if (sp > BREAK_DITS * st->dit) {       /* the over is done           */
          CW2_DBG("[break at t=%lu, sp=%.0f]\n", (unsigned long)st->t, sp);
          lattice_flush(st, out, &pos);
          if (!st->brk_out) {
            st->brk_out = TRUE;
            emit_str(out, &pos, BREAK_MARK);
          }
          lattice_start(st);
        }
      }
    }
  }

  if (pos == 0)
    return FALSE;

  out->confidence     = CLAMP(1.0 - 1.5 * st->elem_err, 0.0, 1.0);
  out->freq_offset_hz = st->foff_hz;
  const double dit_ub = st->esp > 0 ? 0.5 * (st->dit + st->esp) : st->dit;
  out->speed          = st->dit > 0 ? 1.2 * st->rate / dit_ub : 0.0;
  out->snr_db         = st->snr_latch != 0.0
                            ? st->snr_latch
                            : 20.0 * log10(st->env_hi / MAX(st->env_lo, 1e-9));
  return TRUE;
}

static double cw2_level(gpointer state) {
  return ((Cw2State *)state)->env_hi;
}

static double cw2_tone_offset_hz(gpointer state) {
  return ((Cw2State *)state)->foff_hz;
}

const SkimDecodeBackend *skim_decode_cw_v2(void) {
  static const SkimDecodeBackend backend = {
    .name           = "cw2",
    .channel_new    = cw2_channel_new,
    .channel_free   = cw2_channel_free,
    .process        = cw2_process,
    .level          = cw2_level,
    .tone_offset_hz = cw2_tone_offset_hz,
  };
  return &backend;
}
