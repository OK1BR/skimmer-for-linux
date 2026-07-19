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
 * A ~2 s pre-roll ring replays the head of an over the gate delay and the
 * bootstrap consumed (preroll_replay — the squelch attack fix).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_cw.h"

#include "cw_reader.h"

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
#define CLK_RING 6                      /* re-lock ring: raw mark durations   */
#define CLK_HOLD 3                      /* marks between clock jumps          */
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
  guint  dah_streak;                    /* committed dahs since the last dit  */
  guint  dit_streak;                    /* committed dits since the last dah  */
  float  clk[CLK_RING];                 /* last raw mark durations (samples)  */
  guint  clk_n, clk_head, clk_hold;     /* re-lock ring (clock_push)          */
  float  clk_sp[CLK_RING];              /* last raw space durations (witness) */
  guint  clk_sp_n, clk_sp_head;

  /* --- soft discriminator ---------------------------------------------------- */
  double mu_m, mu_s;                    /* mark / space level trackers        */
  double k_m_dn, k_s;
  guint  cont_mark;                     /* samples continuously above mid     */
  guint  cont_space;                    /* samples continuously below mid     */
  gboolean carrier;                     /* long solid mark — not keying       */

  /* --- pre-roll ring (squelch attack fix, 2026-07-19) -------------------------
   * The last ~2 s of post-MA envelope samples, written on EVERY sample —
   * dead air included. When the channel wakes with a usable dit clock
   * (reopen from pause, or the moment the bootstrap learns the clock) the
   * ring is replayed through the soft path, so the head the gate delay and
   * the bootstrap consumed still decodes. pre_from fences the replayable
   * window: audio older than the last pause entry (or a watchdog reboot)
   * was already decoded or squelched — replaying it would duplicate text. */
  float  *pre;
  guint   pre_size;                     /* power of two                       */
  guint   pre_head;                     /* total writes; index = & (size-1)   */
  guint   pre_from;                     /* replay window fence (write count)  */

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

  /* --- run dump (SKIM_CW_DUMP_RUNS — offline ML corpus) ------------------------
   * An independent v1-style Schmitt keyer with the bootstrap's 3-sample
   * glitch fold, running on every sample regardless of gate/pause state.
   * Diagnostics only: the decode path never reads any of this. */
  gboolean rd_on;
  double   freq_hz;                     /* absolute label from the pipeline   */
  gboolean rd_key, rd_pend_key, rd_pend_valid;
  double   rd_run, rd_pend_len;

  /* --- neural reader (SKIM_CW_READER / cw-reader.bin) --------------------------
   * The same Schmitt run stream. A v3 blob STREAMS (phase A): once the
   * channel proves solid the buffered backlog is fed and every further run
   * pushes through the bounded-lookahead net — text commits ~2-4 s behind
   * the key. A v2 blob keeps the old shape: buffer the over, re-read the
   * whole of it at the break (aux line). Pane only either way — the
   * extractor/spot path never sees reader text (hallucination guard). */
  gboolean rr_arm;
  gboolean rr_gate;                     /* SKIM_CW_READER_GATE: no dead-air
                                         * runs into the ring/stream (A/B)    */
  GArray  *rr_runs;                     /* RrRun ring of the current over     */
  SkimCwReaderStream *rr_stream;        /* v3 blobs only (lazy)               */
  gboolean rr_stream_no;                /* stream_new refused (v2 blob)       */
  gboolean rr_live;                     /* streaming armed for this over      */
  guint    rr_base;                     /* rr_runs index of stream input 0    */

  /* --- phase B: draft → final pane composition (streamed reader only) ----------
   * The pane view of a streamed over is COMPOSED here: reader-final words +
   * v2's live draft tail, shipped as full-state pane ops (decode.h). The
   * seam sits on run END TIMES: each draft char remembers when its audio
   * ended, each reader char reports the run it fired at. d.text itself is
   * untouched — extractor, station and spot paths stay classical. */
  GString *ov_txt;                      /* this over's draft chars (pane)     */
  GArray  *ov_t;                        /* guint64 end time per draft byte    */
  guint    ov_sent;                     /* draft bytes already SHOWN (pane)   */
  guint    ov_covered;                  /* draft bytes superseded by commits  */
  GString *hy_word;                     /* reader chars short of a word gap   */
  GArray  *hy_pos;                      /* run index per hy_word byte         */
  GArray  *hy_marg;                     /* logit margin per hy_word byte      */
  GString *hy_final;                    /* over's reader-final pane text      */
  GString *hy_fresh;                    /* newly final chars (decode log)     */
  gboolean hy_open;                     /* the pane has a live over region    */
  gboolean hy_dirty;                    /* op due at the end of this call     */
  gboolean pane_own_call;               /* this call's d.text is pane-owned   */
  GArray  *ops;                         /* queued SkimPaneOp                  */
} Cw2State;

typedef struct { guint8 key; float dur_ms; guint64 t_end; } RrRun;

static SkimCwReader *rr_reader(void);

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
  st->rd_on    = g_getenv("SKIM_CW_DUMP_RUNS") != NULL;
  /* Arming follows the env var at CHANNEL BIRTH (a pipeline restart), not
   * just the first process-wide load — the app's Preferences switch sets
   * and clears SKIM_CW_READER between connects. Weights stay cached in
   * rr_reader() once loaded; only the arming toggles. */
  st->rr_arm   = g_getenv("SKIM_CW_READER") != NULL && rr_reader() != NULL;
  /* Dead-air gating is the DEFAULT reader diet (run5 A/B 2026-07-18: same
   * or better on every regression, half the candidate words, 2.8× faster).
   * SKIM_CW_READER_RAW=1 restores the raw feed for A/B analyses. */
  st->rr_gate  = g_getenv("SKIM_CW_READER_RAW") == NULL;
  /* Pre-roll ring ~2 s: long enough for the bootstrap span of a slow fist
   * (4 marks of a 15 WPM "Y" ≈ 1.2 s) plus its quiet lead-in. */
  guint n = 1;
  while (n < (guint)(2.0 * st->rate)) { n <<= 1; }
  st->pre_size = MIN(n, 4096u);
  st->pre      = g_new0(float, st->pre_size);
  return st;
}

/* --- run dump --------------------------------------------------------------------
 * One line per completed run: "freq_hz t_ms key dur_ms" (C locale). Opened
 * lazily on first use; engine-thread only like the rest of the backend. */
static FILE *rd_file(void) {
  static FILE *f;
  static gsize once;
  if (g_once_init_enter(&once)) {
    const char *path = g_getenv("SKIM_CW_DUMP_RUNS");
    if (path) { f = fopen(path, "w"); }
    g_once_init_leave(&once, TRUE);
  }
  return f;
}

/* Shared reader instance — weights are read-only, the forward is stateless,
 * and every decoder lives on the one engine thread. Armed ONLY by an
 * explicit SKIM_CW_READER=<blob> — the operator checks the pane against his
 * EAR, and injected re-read lines break that flow (Richard, 2026-07-16), so
 * a plain app launch must never arm it. The offline replay analyses set the
 * env var. */
static SkimCwReader *rr_reader(void) {
  static SkimCwReader *r;
  static gsize once;
  if (g_once_init_enter(&once)) {
    const char *path = g_getenv("SKIM_CW_READER");
    if (path) {
      GError *err = NULL;
      r = skim_cw_reader_load(path, &err);
      if (r) {
        g_message("cw2: neural reader armed (%s)", path);
      } else {
        g_warning("cw2: reader blob %s: %s", path, err->message);
        g_clear_error(&err);
      }
    }
    g_once_init_leave(&once, TRUE);
  }
  return r;
}

/* Queue one pane op (owns text/fresh). Queued ops make this call pane-owned:
 * the pipeline suppresses the direct d.text append, the ops carry the view. */
static void hy_queue(Cw2State *st, SkimPaneOpKind kind, guint erase,
                     char *text, guint final_len, char *fresh) {
  if (!st->ops) { st->ops = g_array_new(FALSE, FALSE, sizeof(SkimPaneOp)); }
  SkimPaneOp op = { kind, erase, final_len, text, fresh };
  g_array_append_val(st->ops, op);
  st->pane_own_call = TRUE;
}

/* Fold freshly committed stream text into the over's reader-final prefix,
 * WHOLE WORDS at a time (force=TRUE at the over break folds the rest too),
 * and advance the model/draft seam: the last folded char's run maps to an
 * end time, and every draft char whose audio ended by then is superseded. */
static void rr_commit_stream(Cw2State *st, char *txt, guint *pos,
                             float *marg, gboolean force) {
  if (txt) {
    const gsize n = strlen(txt);
    if (!st->hy_word) {
      st->hy_word = g_string_new(NULL);
      st->hy_pos  = g_array_new(FALSE, FALSE, sizeof(guint));
      st->hy_marg = g_array_new(FALSE, FALSE, sizeof(float));
    }
    g_string_append(st->hy_word, txt);
    if (n) {
      g_array_append_vals(st->hy_pos, pos, (guint)n);
      g_array_append_vals(st->hy_marg, marg, (guint)n);
    }
  }
  g_free(txt);
  g_free(pos);
  g_free(marg);
  if (!st->hy_word || !st->hy_word->len)
    return;
  const char *sp = force ? NULL : strrchr(st->hy_word->str, ' ');
  gsize keep = force ? st->hy_word->len
                     : (sp ? (gsize)(sp - st->hy_word->str) + 1 : 0);
  if (!keep && st->hy_word->len >= 24) {   /* spaceless torrent — cap it     */
    keep = st->hy_word->len;
  }
  if (!keep)
    return;
  if (!st->hy_final) {
    st->hy_final = g_string_new(NULL);
    st->hy_fresh = g_string_new(NULL);
  }
  /* Per-word confidence gate (logit margins). Calibrated for run5 with
   * ml/margin_sweep.py (labeled 3-class sweep against v2's .chars decode,
   * phaseB live capture, 2026-07-18): 6/3 keeps 219 v2-exact words per
   * 600 s against 93 conflicts (half of those are the reader out-reading
   * a v2 fragment); higher bars halve the recall while barely moving the
   * conflict share, and the same-shape guard below removes the worst of
   * what remains. A word only replaces the draft when the net is SURE of
   * it, mean and worst char both; a rejected word's span keeps v2's draft
   * — the pane loses nothing, it just stays classical there. Rejected
   * babble whose span v2's squelch kept silent vanishes entirely. A new
   * blob recalibrates these (rerun the sweep). */
#define RR_MARG_MEAN 6.0f
#define RR_MARG_MIN  3.0f
  gsize at = 0;
  while (at < keep) {
    gsize we = at;
    while (we < keep && st->hy_word->str[we] != ' ') { we++; }
    if (we < keep) { we++; }                   /* the trailing space rides  */
    const gsize wn = we - at;
    double sum = 0.0;
    float  lo = 1e9f;
    for (gsize i = at; i < we; i++) {
      const float m = g_array_index(st->hy_marg, float, i);
      sum += m;
      lo = MIN(lo, m);
    }
    gsize solid_chars = 0;
    for (gsize i = at; i < we; i++) {
      if (st->hy_word->str[i] != ' ') { solid_chars++; }
    }
    /* A single char corrects nothing v2 does not already show — E/T chop
     * fragments read "confidently" and repainted panes one letter at a
     * time. */
    const gboolean sure = solid_chars >= 2 &&
                          sum / (double)wn >= RR_MARG_MEAN &&
                          lo >= RR_MARG_MIN;
    const guint seam_run =
        st->rr_base + g_array_index(st->hy_pos, guint, we - 1);
    guint64 T = G_MAXUINT64;                   /* runs wiped → cover all    */
    if (seam_run < st->rr_runs->len) {
      T = g_array_index(st->rr_runs, RrRun, seam_run).t_end;
    }
    const guint dv0 = st->ov_covered;
    if (st->ov_txt) {
      while (st->ov_covered < st->ov_txt->len &&
             g_array_index(st->ov_t, guint64, st->ov_covered) <= T) {
        st->ov_covered++;
      }
    }
    /* Same-shape guard (run5 margin sweep, phaseB capture): the mutation
     * that SURVIVES the margin gate is a same-length substitution of a
     * solid draft word ("EA2KC" over v2's EA2CC, min margin 5.4) — while a
     * genuine fist correction changes length (torn gaps split chars, fused
     * gaps merge them). A word that rewrites its whole draft span 1:1 with
     * only 1-2 chars changed is a confident wrong call, not a fix — the
     * draft stays. */
    gboolean mut = FALSE;
    if (sure && st->ov_txt && st->ov_covered > dv0) {
      char  wb[64], db[64];
      guint wl = 0, dl = 0;
      for (gsize i = at; i < we && wl < sizeof wb; i++) {
        if (st->hy_word->str[i] != ' ') { wb[wl++] = st->hy_word->str[i]; }
      }
      for (guint i = dv0; i < st->ov_covered && dl < sizeof db; i++) {
        if (st->ov_txt->str[i] != ' ') { db[dl++] = st->ov_txt->str[i]; }
      }
      if (wl && wl == dl && wl < sizeof wb) {
        guint diff = 0;
        for (guint i = 0; i < wl; i++) { diff += wb[i] != db[i]; }
        mut = diff >= 1 && diff <= 2;
      }
    }
    if (g_getenv("SKIM_RR_DBG")) {
      /* t_ms = end of the word's audio, sample clock — aligns an offline
       * margin sweep against the .chars label dump. */
      char *w = g_strndup(st->hy_word->str + at, wn);
      fprintf(stderr, "RRWORD %.1f t=%.1f mean=%.2f min=%.2f %s |%s|\n",
              st->freq_hz,
              T == G_MAXUINT64 ? -1.0 : (double)T * 1000.0 / st->rate,
              sum / (double)wn, lo, mut ? "MUT" : sure ? "OK " : "REJ", w);
      g_free(w);
    }
    if (sure && !mut) {
      g_string_append_len(st->hy_final, st->hy_word->str + at, (gssize)wn);
      g_string_append_len(st->hy_fresh, st->hy_word->str + at, (gssize)wn);
    } else if (st->ov_txt && st->ov_covered > dv0) {
      g_string_append_len(st->hy_final, st->ov_txt->str + dv0,
                          (gssize)(st->ov_covered - dv0));
    }
    at = we;
  }
  g_string_erase(st->hy_word, 0, (gssize)keep);
  g_array_remove_range(st->hy_pos, 0, (guint)keep);
  g_array_remove_range(st->hy_marg, 0, (guint)keep);
  st->hy_dirty      = TRUE;
  st->pane_own_call = TRUE;
}

/* The streamed over ends: flush the stream's tail, seal the pane region with
 * the reader's text (it owns a solid over; an empty read keeps the draft),
 * follow with the classical over separator, and reset for the next over. */
static void rr_close_over(Cw2State *st) {
  guint *pos  = NULL;
  float *marg = NULL;
  char  *tail = skim_cw_reader_stream_flush_pos(st->rr_stream, &pos, &marg);
  rr_commit_stream(st, tail, pos, marg, TRUE);
  const char *fin =
      (st->hy_final && st->hy_final->len) ? st->hy_final->str
      : (st->ov_txt ? st->ov_txt->str : "");
  if (st->hy_open || fin[0]) {
    const guint flen = (guint)strlen(fin);
    if (!st->hy_open) {                  /* never shown as a region yet      */
      hy_queue(st, SKIM_PANE_OP_OPEN, st->ov_sent, g_strdup(fin), flen, NULL);
      st->hy_open = TRUE;
    }
    char *fresh = (st->hy_fresh && st->hy_fresh->len)
                      ? g_strndup(st->hy_fresh->str, st->hy_fresh->len) : NULL;
    hy_queue(st, SKIM_PANE_OP_CLOSE, 0, g_strdup(fin), flen, fresh);
    hy_queue(st, SKIM_PANE_OP_APPEND, 0, g_strdup(BREAK_MARK), 0, NULL);
  }
  st->rr_live = FALSE;
  st->hy_open = st->hy_dirty = FALSE;
  if (st->hy_word) { g_string_truncate(st->hy_word, 0); }
  if (st->hy_pos) { g_array_set_size(st->hy_pos, 0); }
  if (st->hy_marg) { g_array_set_size(st->hy_marg, 0); }
  if (st->hy_final) { g_string_truncate(st->hy_final, 0); }
  if (st->hy_fresh) { g_string_truncate(st->hy_fresh, 0); }
}

/* Draft/over bookkeeping resets whenever the reader's over ends (streamed
 * or not) — the next over starts a fresh draft. */
static void ov_reset(Cw2State *st) {
  if (st->ov_txt) { g_string_truncate(st->ov_txt, 0); }
  if (st->ov_t) { g_array_set_size(st->ov_t, 0); }
  st->ov_sent = st->ov_covered = 0;
}

/* End of one process() call: at most ONE composed op ships the over's
 * current view (reader-final prefix + live draft tail); the first such op
 * OPENs the region by taking back the draft the pane already shows. When
 * an over closed mid-call, any draft a NEW over emitted after it must still
 * reach the (suppressed) pane — as a plain append. Finally the call's
 * pane ownership lands in out->pane_own and the sent counter catches up. */
static void hy_call_end(Cw2State *st, SkimDecode *out) {
  out->pane_own = FALSE;
  if (!st->rr_arm)
    return;
  if (st->rr_live && st->hy_dirty) {
    GString *view = g_string_new(st->hy_final ? st->hy_final->str : "");
    const guint flen = (guint)view->len;
    if (st->ov_txt && st->ov_covered < st->ov_txt->len) {
      g_string_append_len(view, st->ov_txt->str + st->ov_covered,
                          (gssize)(st->ov_txt->len - st->ov_covered));
    }
    char *fresh = (st->hy_fresh && st->hy_fresh->len)
                      ? g_strndup(st->hy_fresh->str, st->hy_fresh->len) : NULL;
    if (st->hy_fresh) { g_string_truncate(st->hy_fresh, 0); }
    hy_queue(st, st->hy_open ? SKIM_PANE_OP_SET : SKIM_PANE_OP_OPEN,
             st->hy_open ? 0 : st->ov_sent, g_string_free(view, FALSE), flen,
             fresh);
    st->hy_open  = TRUE;
    st->hy_dirty = FALSE;
  }
  if (st->pane_own_call && !st->rr_live && st->ov_txt &&
      st->ov_txt->len > st->ov_sent) {
    hy_queue(st, SKIM_PANE_OP_APPEND, 0,
             g_strndup(st->ov_txt->str + st->ov_sent,
                       st->ov_txt->len - st->ov_sent),
             0, NULL);
    st->ov_sent = (guint)st->ov_txt->len;
  }
  out->pane_own = st->pane_own_call;
  if (!st->pane_own_call) {
    st->ov_sent = st->ov_txt ? (guint)st->ov_txt->len : 0;
  }
  st->pane_own_call = FALSE;
}

/* The moment a channel proves solid mid-over, arm the stream and feed it
 * the buffered backlog (trimmed to start on a mark, ≥ 40 runs ≈ 8 chars —
 * the same blip filter the batch path applies). Noise channels never
 * qualify, so no forward passes burn on them. */
static void rr_try_arm(Cw2State *st) {
  if (st->rr_live || st->rr_stream_no || !st->rr_runs ||
      st->rr_runs->len < 40 || st->dit <= 0 || st->marks_ok < 8 ||
      st->snr_latch < 12.0)
    return;
  if (!st->rr_stream) {
    st->rr_stream = skim_cw_reader_stream_new(rr_reader());
    if (!st->rr_stream) {                /* v2 blob — batch at the break     */
      st->rr_stream_no = TRUE;
      return;
    }
  }
  st->rr_live = TRUE;
  const RrRun *v = (const RrRun *)st->rr_runs->data;
  guint a = 0;
  while (a < st->rr_runs->len && !v[a].key) { a++; }
  st->rr_base = a;
  for (guint i = a; i < st->rr_runs->len; i++) {
    guint *pos  = NULL;
    float *marg = NULL;
    char  *txt = skim_cw_reader_stream_push_pos(st->rr_stream, v[i].key,
                                                v[i].dur_ms, &pos, &marg);
    rr_commit_stream(st, txt, pos, marg, FALSE);
  }
}

/* Over break: land the reader text for the pane — STREAMED overs only.
 * The streaming flush commits the tail through the same per-word margin
 * gate every mid-over word passed. There is NO batch fallback into the
 * pane: a one-shot skim_cw_reader_read() carries no margins, and letting
 * it seal short overs ungated repainted the pane with chop garbage the
 * word gate exists to stop (live-caught 2026-07-18, second session —
 * "EE ILE=3ISHRIEN5SFDXEUE" whole-over rewrites). Short overs and v2
 * blobs keep v2's text; the hybrid pane needs a v3 blob. */
static void rr_flush_over(Cw2State *st) {
  if (!st->rr_runs)
    return;
  if (st->rr_live) {
    rr_close_over(st);
  }
  g_array_set_size(st->rr_runs, 0);
  ov_reset(st);
}

static void rd_emit(const Cw2State *st, gboolean key, double len) {
  FILE *f = rd_file();
  if (!f)
    return;
  char hz[G_ASCII_DTOSTR_BUF_SIZE];
  char tms[G_ASCII_DTOSTR_BUF_SIZE];
  char dur[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(hz, sizeof(hz), "%.1f", st->freq_hz);
  g_ascii_formatd(tms, sizeof(tms), "%.1f", (double)st->t * 1000.0 / st->rate);
  g_ascii_formatd(dur, sizeof(dur), "%.1f", len * 1000.0 / st->rate);
  fprintf(f, "%s %s %d %s\n", hz, tms, key ? 1 : 0, dur);
}

/* The decode's own text, char by char, into <SKIM_CW_DUMP_RUNS>.chars:
 * "freq_hz t_ms ascii elem_err solid". Time is the end of the char's AUDIO
 * (same sample clock as rd_emit), so a char aligns to the run stream
 * exactly; elem_err/solid carry v2's own quality judgment at the commit.
 * The offline harvest labels its chunks with these instead of a trivial
 * re-decode — v2's squelch and lattice are the label's defenses. */
static FILE *rd_char_file(void) {
  static FILE *f;
  static gsize once;
  if (g_once_init_enter(&once)) {
    const char *path = g_getenv("SKIM_CW_DUMP_RUNS");
    if (path) {
      char *p = g_strconcat(path, ".chars", NULL);
      f = fopen(p, "w");
      g_free(p);
    }
    g_once_init_leave(&once, TRUE);
  }
  return f;
}

static void rd_emit_char(const Cw2State *st, char c, guint64 t_end) {
  FILE *f = rd_char_file();
  if (!f)
    return;
  char hz[G_ASCII_DTOSTR_BUF_SIZE];
  char tms[G_ASCII_DTOSTR_BUF_SIZE];
  char er[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(hz, sizeof(hz), "%.1f", st->freq_hz);
  g_ascii_formatd(tms, sizeof(tms), "%.1f", (double)t_end * 1000.0 / st->rate);
  g_ascii_formatd(er, sizeof(er), "%.3f", st->elem_err);
  const int solid = st->dit > 0 && st->marks_ok >= 8 && st->elem_err < 0.35;
  fprintf(f, "%s %s %d %s %d\n", hz, tms, (int)(unsigned char)c, er, solid);
}

static void cw2_channel_free(gpointer state) {
  Cw2State *st = state;
  if (!st)
    return;
  if (st->rr_runs) { g_array_free(st->rr_runs, TRUE); }
  if (st->rr_stream) { skim_cw_reader_stream_free(st->rr_stream); }
  if (st->ov_txt) { g_string_free(st->ov_txt, TRUE); }
  if (st->ov_t) { g_array_free(st->ov_t, TRUE); }
  if (st->hy_word) { g_string_free(st->hy_word, TRUE); }
  if (st->hy_pos) { g_array_free(st->hy_pos, TRUE); }
  if (st->hy_marg) { g_array_free(st->hy_marg, TRUE); }
  if (st->hy_final) { g_string_free(st->hy_final, TRUE); }
  if (st->hy_fresh) { g_string_free(st->hy_fresh, TRUE); }
  if (st->ops) {
    for (guint i = 0; i < st->ops->len; i++) {
      SkimPaneOp *op = &g_array_index(st->ops, SkimPaneOp, i);
      g_free(op->text);
      g_free(op->fresh);
    }
    g_array_free(st->ops, TRUE);
  }
  g_free(st->pre);
  g_free(st->lat);
  g_free(st);
}

/* --- assembly helpers (v1 semantics) ------------------------------------------ */

static void emit_str(SkimDecode *out, guint *pos, const char *s) {
  gsize n = strlen(s);
  if (*pos + n < SKIM_DECODE_TEXT_MAX) {
    memcpy(out->text + *pos, s, n + 1);
    *pos += (guint)n;
  }
}

/* Emit one decoded char and, with the reader armed, remember it as this
 * over's pane DRAFT together with the time its audio ended — the reader's
 * word commits supersede draft chars by exactly that time (the seam).
 * Only chars that really reached out->text are tracked (the 64-char cap
 * must not desync the pane's erase counts). The over separator goes via
 * emit_str, untracked — it is not part of any over. */
static void emit(Cw2State *st, SkimDecode *out, guint *pos, char c,
                 guint64 t_end) {
  if (*pos + 1 >= SKIM_DECODE_TEXT_MAX)
    return;
  out->text[(*pos)++] = c;
  out->text[*pos] = '\0';
  if (st->rd_on) { rd_emit_char(st, c, t_end); }
  if (!st->rr_arm)
    return;
  if (!st->ov_txt) {
    st->ov_txt = g_string_new(NULL);
    st->ov_t   = g_array_new(FALSE, FALSE, sizeof(guint64));
  }
  g_string_append_c(st->ov_txt, c);
  g_array_append_val(st->ov_t, t_end);
  if (st->rr_live) {
    st->hy_dirty      = TRUE;
    st->pane_own_call = TRUE;
  }
}

static void emit_code(Cw2State *st, SkimDecode *out, guint *pos,
                      guint64 t_end) {
  if (st->nelem && st->code < G_N_ELEMENTS(morse_lut)) {
    char c = morse_lut[st->code];
    gboolean lone_et = st->nelem == 1 &&
                       (st->marks_ok < 4 || st->elem_err > 0.45);
    if (c && !lone_et) { emit(st, out, pos, c, t_end); }
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

/* --- clock re-lock ---------------------------------------------------------------
 * The per-mark EMA glides — a QSO turnaround does not (live 40 m,
 * 2026-07-18: the OTHER op comes back inside the 8 s fist-forget window at
 * his own speed, and the whole first word rides a stale clock; worse, past
 * the 2-dit class boundary the misread marks pull the EMA the WRONG way,
 * self-consistently). So keep a ring of the last raw mark durations and
 * re-run the bootstrap's own clustering over it on every commit: a BIMODAL
 * ring whose low cluster disagrees with the clock by >30 % is a new speed,
 * proven structurally — JUMP, do not glide. A unimodal ring is ambiguous
 * (dit-only text reads clean) and may only jump while the clock visibly
 * fails (elem_err), interpreted as dits — the dah-streak watchdog remains
 * the backstop for the all-dah read. Gate: cw-test "QSO turnaround". */


/* Robust cluster mean: tight (max ≤ 1.5×min) = trustworthy speed evidence;
 * one outlier (a torn-dah fragment) may be shed and the rest retried —
 * looser than that is a FIST, not a speed (σ0.2 jitter false-jumped the
 * pane gate), and yields no estimate. */
static gboolean clk_est(const float *v, guint n, double *est) {
  float s[CLK_RING];
  memcpy(s, v, n * sizeof(float));
  for (guint i = 1; i < n; i++) {
    float x = s[i];
    guint j = i;
    while (j > 0 && s[j - 1] > x) { s[j] = s[j - 1]; j--; }
    s[j] = x;
  }
  guint m = n;
  if (s[m - 1] > 1.5f * s[0]) {
    if (n < 4 || s[n - 2] > 1.5f * s[0])
      return FALSE;
    m = n - 1;                           /* shed the lone outlier            */
  }
  double sum = 0.0;
  for (guint i = 0; i < m; i++) { sum += s[i]; }
  *est = sum / m;
  return TRUE;
}

static void clock_space(Cw2State *st, double d) {
  st->clk_sp[st->clk_sp_head++ % CLK_RING] = (float)d;
  if (st->clk_sp_n < CLK_RING) { st->clk_sp_n++; }
}

static void clock_push(Cw2State *st, double d) {
  st->clk[st->clk_head++ % CLK_RING] = (float)d;
  if (st->clk_n < CLK_RING) { st->clk_n++; }
  if (st->clk_hold) { st->clk_hold--; }
  if (st->clk_n < 4 || st->clk_hold || st->dit <= 0)
    return;                     /* 4 marks + the space witness already decide */
  float lo = st->clk[0], hi = st->clk[0];
  for (guint i = 1; i < st->clk_n; i++) {
    lo = MIN(lo, st->clk[i]);
    hi = MAX(hi, st->clk[i]);
  }
  double est;
  if (hi >= 2.0f * lo) {                    /* dits AND dahs in the ring     */
    const float mid = 0.5f * (lo + hi);
    float cl[CLK_RING];
    guint cnt = 0;
    for (guint i = 0; i < st->clk_n; i++) {
      if (st->clk[i] < mid) { cl[cnt++] = st->clk[i]; }
    }
    if (cnt < 3 || !clk_est(cl, cnt, &est))
      return;
  } else {
    if (!clk_est(st->clk, st->clk_n, &est))
      return;
    /* A unimodal ring is dits-or-dahs ambiguous, and the nasty turnaround
     * reads the new op's dits as PERFECT dahs (elem_err never rises — the
     * live 40 m trap). The SPACES break the tie: element gaps run 1:1 with
     * dits but 1:3 with dahs, so the smallest recent space is the witness. */
    if (st->clk_sp_n >= 3) {
      /* The ELEMENT-GAP class of the space ring: sort, then the smallest
       * class with ≥3 members within 1.5× of its floor. Not the min (a
       * torn dah drops glitch pairs under the real gaps) and not the
       * median (a dah-heavy stretch — "MM DE UB7M" — holds more char gaps
       * than element gaps, the median flips class and the witness jumped a
       * clean 24 WPM clock 3× up; live-replay-caught). */
      float sp[CLK_RING];
      memcpy(sp, st->clk_sp, st->clk_sp_n * sizeof(float));
      for (guint i = 1; i < st->clk_sp_n; i++) {
        float v = sp[i];
        guint j = i;
        while (j > 0 && sp[j - 1] > v) { sp[j] = sp[j - 1]; j--; }
        sp[j] = v;
      }
      float egap = 0.0f;
      for (guint i = 0; i + 2 < st->clk_sp_n; i++) {
        if (sp[i + 2] <= 1.5f * sp[i]) {   /* three members = a real class  */
          double s = 0.0;
          guint  c = 0;
          for (guint j = i; j < st->clk_sp_n && sp[j] <= 1.5f * sp[i]; j++) {
            s += sp[j];
            c++;
          }
          egap = (float)(s / c);
          break;
        }
      }
      if (egap <= 0.0f)
        return;
      const double r = est / egap;
      if (r > 2.2) {
        est /= 3.0;                         /* dahs riding element gaps      */
      } else if (r >= 1.6) {
        return;                             /* between classes — no verdict  */
      }
    } else if (st->elem_err <= 0.30) {
      return;
    }
  }
  if (est > 0.80 * st->dit && est < 1.25 * st->dit)
    return;                                 /* the clock is already right    */
  CW2_DBG("[clock relock %.1f -> %.1f (err %.2f)]\n", st->dit, est,
          st->elem_err);
  st->dit      = CLAMP(est, st->rate * 0.022, st->rate * 0.30);
  st->elem_err = 0.2;                       /* neutral — re-measure          */
  st->clk_hold = CLK_HOLD;
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
      clock_push(st, d);
      st->dah_streak = ty == SEG_DAH ? st->dah_streak + 1 : 0;
      st->dit_streak = ty == SEG_DIT ? st->dit_streak + 1 : 0;
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
      clock_space(st, d);
      break;
    case SEG_CSP:
      fist_learn(st, d / st->dit);
      clock_space(st, d);
      if (!lived) { emit_code(st, out, pos, end - segs_d[i - 1]); }
      st->char_live = st->word_live = FALSE;
      break;
    case SEG_WSP:
      fist_learn(st, d / st->dit);
      if (!lived) {
        emit_code(st, out, pos, end - segs_d[i - 1]);
        emit(st, out, pos, ' ', end);
      } else if (!st->word_live && st->live_had_char) {
        emit(st, out, pos, ' ', end);          /* live char, space grew      */
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
  emit_code(st, out, pos, st->t);              /* the char in flight         */
  st->active = FALSE;
}

/* --- soft path: discriminator → llr ring → Viterbi ------------------------------
 * One sample through the soft path: µ trackers → carrier watch → LLR →
 * lattice step → lagged commit + live emission. Factored out of the live
 * loop VERBATIM so the pre-roll replay pushes ring samples through the
 * exact same float ops a live sample takes. */
static void soft_step(Cw2State *st, float m, SkimDecode *out, guint *pos) {
  /* µ_mark is a FAST decaying peak (instant attack, ~0.35 s release): a
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
    return;
  }
  if (st->cont_mark > (guint)MAX(8.0 * st->dit, 0.8 * st->rate)) {
    /* 8 dits alone is NOT carrier-proof against a stale clock: an
     * operator dropping from ~35 to ~12 WPM makes every dah ~8× the old
     * dit — the whole slow transmission read as "carrier" and, worse,
     * dahs never committed so the clock had nothing to relearn from
     * (live-caught 2026-07-16, 7028.9). A real carrier holds for
     * seconds; even a 5 WPM dah is 720 ms, so the 0.8 s floor keeps
     * SLOW keying committing — and one committed slow dah snaps the
     * elem_err-boosted clock onto the new speed. */
    st->carrier = TRUE;
    st->active  = FALSE;                       /* drop the code in flight    */
    st->code    = 0;
    st->nelem   = 0;
    st->t++;
    return;
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

  if ((st->t & 7) == 0) {                      /* lagged commit, every 8     */
    /* Healthy signal (discriminator riding near the envelope peak, clock
     * trusted) → short lag; a fade or a re-lock keeps the full window. */
    const double lag =
        (st->mu_m > 0.6 * st->env_hi && st->elem_err < 0.20)
            ? LAG_DITS_FAST
            : LAG_DITS;
    lattice_commit(st, lag, out, pos);

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
        emit_code(st, out, pos, tau);
      }
      if (st->char_live && st->live_had_char && !st->word_live &&
          sp > 1.2 * sqrt(st->fist_csp * st->fist_wsp) * st->dit) {
        st->word_live = TRUE;
        emit(st, out, pos, ' ', tau);
      }
      if (sp > BREAK_DITS * st->dit) {         /* the over is done           */
        CW2_DBG("[break at t=%lu, sp=%.0f]\n", (unsigned long)st->t, sp);
        lattice_flush(st, out, pos);
        if (!st->brk_out) {
          st->brk_out = TRUE;
          emit_str(out, pos, BREAK_MARK);
        }
        rr_flush_over(st);
        lattice_start(st);
        /* The next over may be the OTHER op of a QSO — the re-lock rings
         * must weigh HIS marks alone (a solid channel skips the pause on
         * a short turnaround gap, so the pause-side clear never fires). */
        st->clk_n = st->clk_sp_n = st->clk_head = st->clk_sp_head =
            st->clk_hold = 0;
      }
    }
  }
}

/* --- pre-roll replay (squelch attack fix, 2026-07-19) ---------------------------
 * The dead-air gate and the dit bootstrap both consume signal from the HEAD
 * of an over: the keying-duty EMA needs ~0.3 s to concede a cold channel is
 * keying, and after the 8 s fist-forget the first 4-8 marks are spent
 * LEARNING the clock, not decoding — "YOTA" logged as "OTA" (Y is exactly
 * the 4 marks the bootstrap eats; 32 % of the R3OM reference channel's
 * overs, live 2026-07-19). Every sample lands in the pre ring regardless of
 * gate state, so when the channel wakes with a usable clock the ring still
 * holds the head: find the over's onset (first ≥0.55-dit mark run preceded
 * by ≥2 dits of quiet, inside the pre_from fence) and push the span through
 * the same soft path live samples take. No onset, or quiet ≥ 14 dits
 * embedded in the span (that is QRM + a real break, not one over's head) →
 * no replay — exactly the old behaviour, never worse. */
static gboolean preroll_replay(Cw2State *st, SkimDecode *out, guint *pos) {
  if (!st->pre || st->dit <= 0 || !st->active)
    return FALSE;
  const guint size = st->pre_size;
  guint have = MIN(st->pre_head, size);
  have = MIN(have, st->pre_head - st->pre_from);
  const float thr =
      (float)(st->env_lo + 0.45 * (st->env_hi - st->env_lo));
  const guint qmin    = (guint)MAX(2.0 * st->dit, 0.10 * st->rate);
  const guint marklen = (guint)MAX(3.0, 0.55 * st->dit);
  if (have < qmin + marklen)
    return FALSE;
  /* Onset: the first above-threshold run ≥ marklen (a real mark, not a
   * noise pop) whose start follows ≥ qmin of quiet. qmin ≥ 2 dits keeps a
   * mid-character element gap from ever qualifying — a replay may only
   * start at a char/word boundary or dead air, so no partial character can
   * be minted. */
  guint quiet = 0, run = 0, run_at = 0, onset = 0;
  gboolean found = FALSE;
  for (guint k = 0; k < have && !found; k++) {
    const float v = st->pre[(st->pre_head - have + k) & (size - 1)];
    if (v > thr) {
      if (run == 0) { run_at = k; }
      run++;
      if (run >= marklen && quiet >= qmin) {
        onset = run_at;
        found = TRUE;
      }
    } else {
      if (run) { quiet = 0; }                  /* a pop ended — quiet anew   */
      quiet++;
      run = 0;
    }
  }
  if (!found)
    return FALSE;
  /* Weak heads keep the old behaviour. The seeded µ midpoint reflects the
   * WAKE moment; the head of a fading-in over sits under it and reads TORN
   * (YOTA fixture: 12 dB RV6HV re-read as "ST AEV6HV" where the old code
   * stayed quiet — and the mutation cost the station its table entry). A
   * mutated head near the extractor is dearer than a missing one, and the
   * clip class this fix hunts was measured on strong runners (R3OM 33 dB).
   * The bar is measured on the RING, onset-run level against the quiet
   * level before it — the envelope ratio at wake time is useless here: the
   * gate opens ON the rising edge, so it reads ~4-5× by construction
   * whatever the station's strength (fixture-measured). 5.6× on the ring
   * means keeps every strong-channel gain and blocks the 12 dB tear. */
  {
    double qsum = 0.0, msum = 0.0;
    guint  qn = 0, mn = 0;
    for (guint k = onset > qmin ? onset - qmin : 0; k < onset; k++) {
      const float v = st->pre[(st->pre_head - have + k) & (size - 1)];
      if (v <= thr) {
        qsum += v;
        qn++;
      }
    }
    for (guint k = onset; k < have && mn < 2 * marklen; k++) {
      const float v = st->pre[(st->pre_head - have + k) & (size - 1)];
      if (v > thr) {
        msum += v;
        mn++;
      }
    }
    if (!qn || !mn || msum * (double)qn < 5.6 * qsum * (double)mn)
      return FALSE;
  }
  const guint brk = (guint)(14.0 * st->dit);
  quiet = 0;
  for (guint k = onset; k < have; k++) {
    const float v = st->pre[(st->pre_head - have + k) & (size - 1)];
    quiet = v > thr ? 0 : quiet + 1;
    if (quiet >= brk)
      return FALSE;
  }
  const guint lead = MIN((guint)(2.0 * st->dit), onset);
  CW2_DBG("[preroll replay %u samples (onset -%u) at t=%lu]\n",
          have - (onset - lead), have - onset, (unsigned long)st->t);
  for (guint k = onset - lead; k < have; k++) {
    soft_step(st, st->pre[(st->pre_head - have + k) & (size - 1)], out, pos);
  }
  return TRUE;
}

/* --- the backend --------------------------------------------------------------- */

static gboolean cw2_process(gpointer state, const float *iq, guint nframes,
                            SkimDecode *out) {
  Cw2State *st = state;
  guint pos = 0;
  out->text[0]      = '\0';
  out->pane_own     = FALSE;
  st->pane_own_call = FALSE;

  /* Clock-lost watchdog: a run of committed dahs with not one dit means the
   * dit clock sits a class too fast and the labeling went SELF-CONSISTENT —
   * slow dits read as dahs at ratio ~3, elem_err stays low, the EMA never
   * moves (a ~3× speed drop; the carrier floor alone let the marks commit
   * but not the clock recover, gate-caught 2026-07-16). No real text is
   * dah-only for 16 marks ("0 0 0" is 15), so forget the clock and let the
   * bootstrap relearn it from the very keying that is arriving. */
  /* The mirror trap: a clock stuck a class too SLOW reads everything as
   * dits (a wrong up-jump lands here — live-replay-caught on UB7M: 24 WPM
   * jumped to 9 and sat in "IISSESS" for 20 s). Real dit-only text runs
   * longer than real dah-only text ("5 IS HIS" happens), so the bar sits
   * higher than the dah watchdog's. */
  if ((st->dah_streak >= 16 || st->dit_streak >= 24) && st->dit > 0) {
    CW2_DBG("[clock lost at t=%lu — %u dahs/%u dits one-class; rebootstrap]\n",
            (unsigned long)st->t, st->dah_streak, st->dit_streak);
    lattice_flush(st, out, &pos);
    st->dit = 0;
    st->nboot = 0;
    st->marks_ok = 0;
    st->dah_streak = 0;
    st->dit_streak = 0;
    st->clk_n = st->clk_sp_n = st->clk_head = st->clk_sp_head =
        st->clk_hold = 0;
    st->fist_csp = FIST_CSP0;
    st->fist_wsp = FIST_WSP0;
    st->ngaps = st->gap_head = st->gap_fresh = 0;
    st->carrier = FALSE;
    st->paused = FALSE;
    /* The mis-clocked span WAS emitted (badly) — replaying it after the
     * rebootstrap would duplicate text. Only post-watchdog audio may. */
    st->pre_from = st->pre_head;
  }

  for (guint n = 0; n < nframes; n++) {
    const float i = iq[2 * n], q = iq[2 * n + 1];
    const float mag = sqrtf(i * i + q * q);

    st->ma[st->ma_n % 3] = mag;
    st->ma_n++;
    float m = (st->ma[0] + st->ma[1] + st->ma[2]) / (float)MIN(st->ma_n, 3);
    st->pre[st->pre_head++ & (st->pre_size - 1)] = m;

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

    /* Dead air by v2's own judgment (the pause predicate below, hoisted):
     * envelope gate shut with no solid discriminator to ride a dip, or no
     * keying duty on a channel that never proved itself. */
    const gboolean solid =
        st->dit > 0 && st->marks_ok >= 8 && st->elem_err < 0.35;
    const gboolean mu_alive =
        st->active && st->mu_m > 3.0 * st->mu_s;
    const gboolean dead =
        (!st->gate_open && !(solid && mu_alive)) || (!st->keying && !solid);

    if (st->rd_on || st->rr_arm) {
      /* Run keyer: sees EVERY sample (before gate/pause early-exits), so the
       * corpus/reader keeps what the decode path may throw away. */
      const double rspan = st->env_hi - st->env_lo;
      const gboolean want =
          m > st->env_lo + (st->rd_key ? 0.30 : 0.55) * rspan;
      if (want != st->rd_key) {
        if (st->rd_pend_valid) {
          st->rd_run += st->rd_pend_len;         /* glitch — fold it back    */
          st->rd_key  = want;
          st->rd_pend_valid = FALSE;
        } else {
          st->rd_pend_key   = st->rd_key;
          st->rd_pend_len   = st->rd_run;
          st->rd_pend_valid = TRUE;
          st->rd_key = want;
          st->rd_run = 0.0;
        }
      }
      st->rd_run += 1.0;
      if (st->rd_pend_valid && st->rd_run > 3.0) {
        if (st->rd_on) { rd_emit(st, st->rd_pend_key, st->rd_pend_len); }
        /* Gated feed: the reader eats only what v2 itself would — dead-air
         * runs (noise before the squelch opens, splatter shadows) stay out
         * of the ring and the stream. The corpus dump above stays raw. */
        if (st->rr_arm && !(st->rr_gate && dead)) {
          if (!st->rr_runs) {
            st->rr_runs = g_array_new(FALSE, FALSE, sizeof(RrRun));
          }
          if (st->rr_runs->len >= 4096) {        /* runaway — start over     */
            if (st->rr_live) { rr_close_over(st); }
            g_array_set_size(st->rr_runs, 0);
            ov_reset(st);
          }
          const RrRun rr = { st->rd_pend_key ? 1 : 0,
                             (float)(st->rd_pend_len * 1000.0 / st->rate),
                             st->t };
          g_array_append_val(st->rr_runs, rr);
          if (st->rr_live) {                     /* stream keeps pace        */
            guint *rp = NULL;
            float *rm = NULL;
            char  *rt = skim_cw_reader_stream_push_pos(st->rr_stream,
                                                       rr.key != 0, rr.dur_ms,
                                                       &rp, &rm);
            rr_commit_stream(st, rt, rp, rm, FALSE);
          } else {
            rr_try_arm(st);
          }
        }
        st->rd_pend_valid = FALSE;
      }
    }

    /* Deep QSB pulls faded marks under the envelope midpoint, feeds them
     * to env_lo and slams the envelope gate shut mid-callsign (v1 tears
     * "9A170NT" apart exactly this way). The µ discriminator knows better:
     * in a trough µ_m/µ_s stays ≫ the 1.5 floor it collapses to on noise —
     * a SOLID channel with a live discriminator rides the dip out. */
    if (dead) {
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
          rr_flush_over(st);
          st->code = 0;
          st->nelem = 0;
          st->clk_n = st->clk_sp_n = st->clk_head = st->clk_sp_head =
      st->clk_hold = 0;
          st->paused = TRUE;
          st->quiet  = 0.0;
          /* Everything before this pause was decoded or squelched — the
           * pre-roll replay may only reach back to here. */
          st->pre_from = st->pre_head;
        }
        st->quiet += 1.0;
        if (st->quiet > 8.0 * st->rate) {      /* forget the fist            */
          st->dit = 0;
          st->nboot = 0;
          st->marks_ok = 0;
          st->clk_n = st->clk_sp_n = st->clk_head = st->clk_sp_head =
              st->clk_hold = 0;
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
      if (st->dit > 0) {
        lattice_start(st);
        /* The wake lagged the true onset (in a fade re-entry by a lot) —
         * replay it from the ring; the current sample rides along. */
        if (preroll_replay(st, out, &pos)) { continue; }
      }
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
            /* The bootstrap consumed the over's head to LEARN the clock —
             * the ring still holds that audio; decode it now. */
            if (preroll_replay(st, out, &pos)) {
              st->pend_valid = FALSE;
              continue;                        /* replay ran through NOW     */
            }
          }
        }
        st->pend_valid = FALSE;
      }
      st->t++;
      continue;
    }

    soft_step(st, m, out, &pos);
  }

  hy_call_end(st, out);
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

static void cw2_set_freq(gpointer state, double freq_hz) {
  ((Cw2State *)state)->freq_hz = freq_hz;
}

static gboolean cw2_take_pane_op(gpointer state, SkimPaneOp *op) {
  Cw2State *st = state;
  if (!st->ops || !st->ops->len)
    return FALSE;
  *op = g_array_index(st->ops, SkimPaneOp, 0);
  g_array_remove_index(st->ops, 0);
  return TRUE;
}

const SkimDecodeBackend *skim_decode_cw_v2(void) {
  static const SkimDecodeBackend backend = {
    .name           = "cw2",
    .channel_new    = cw2_channel_new,
    .set_freq       = cw2_set_freq,
    .take_pane_op   = cw2_take_pane_op,
    .channel_free   = cw2_channel_free,
    .process        = cw2_process,
    .level          = cw2_level,
    .tone_offset_hz = cw2_tone_offset_hz,
  };
  return &backend;
}
