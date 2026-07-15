/* decode_cw.c — CW decode backend (M3, docs/SCOPE.md).
 *
 * Per channel: |IQ| envelope (3-tap MA) → dual-rate level trackers (instant
 * attack / slow release peak, slow-rise noise floor) → Schmitt-trigger keying
 * (on/off thresholds at 50 %/30 % of the tracked span) → run-length element
 * timing with an adaptive dit clock (EMA over classified dots and dashes,
 * bootstrapped by clustering the first marks) → binary-coded Morse table.
 * Characters are emitted LIVE as the inter-element space crosses 2.2 dits
 * (word space at 5.5 dits), so the decoder needs no explicit flush.
 *
 * A squelch gates everything: no character ever leaves a channel whose peak
 * tracker is not well clear of its noise floor — the skimmer must stay silent
 * on empty channels (RBN feeds garbage otherwise; M4 validates on top).
 *
 * The tone's offset inside the channel is estimated from the phase slope
 * during marks (EMA) — M5 refines spot frequencies with it.
 *
 * v1 is deliberately classical; the planned HMM/Bayes layer for ragged fists
 * replaces the run-length classifier, not this plumbing. Known v1 limit: a
 * deep in-dash dropout can split a dash into two dots (mitigated by the MA +
 * hysteresis, properly solved by the HMM layer).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_cw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CW_DBG(...) \
  do { if (g_getenv("SKIM_CW_DEBUG")) { fprintf(stderr, __VA_ARGS__); } } while (0)

/* --- Morse table: code built as 1-prefixed bits, dot=0 dash=1 -------------- */

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

/* --- per-channel state ------------------------------------------------------ */

#define BOOT_MARKS 8            /* marks buffered while the dit is unlearned  */

/* A space this many dits long ends the TRANSMISSION (over), not just a word
 * (~2× the nominal 7-dit word space): the decoder emits "· " (UTF-8 middle
 * dot) as a compact over separator ("F6HKA · CQ CWT F6HKA…" — Richard's
 * pick, matching the header's separator style; 2026-07-15). */
#define BREAK_DITS 16.0
#define BREAK_MARK "\xC2\xB7 "  /* "· " */

typedef struct {
  double rate;                  /* baseband sample rate (channelizer out)     */

  /* envelope + level trackers */
  float  ma[3];                 /* 3-tap moving average ring                  */
  guint  ma_n;
  double env_hi, env_lo;
  double k_hi_rel, k_lo, k_ph;
  gboolean gate_open;           /* hysteresis squelch state                   */
  double   p_high;              /* fraction of samples near the peak (EMA)    */
  gboolean keying;              /* p_high says "this is keying, not noise"    */

  /* keying state machine */
  gboolean key;
  double   run;                 /* samples in the current key state           */
  gboolean pend_valid;          /* a completed run awaits proof it was real   */
  gboolean pend_key;
  double   pend_len;
  double   last_space;          /* space before a probationary mark — restored
                                 * if that mark is discarded as a noise ping  */

  /* element clock */
  double dit;                   /* samples per dit; 0 = unlearned             */
  double boot[BOOT_MARKS];
  guint  nboot;

  /* character assembly */
  guint    code;                /* 1-prefixed element bits                    */
  guint    nelem;
  guint    marks_ok;            /* post-bootstrap marks since assembly reset  */
  gboolean brk_out;             /* over-break separator already emitted       */
  gboolean paused;              /* quiet, but the learned fist is kept        */
  double   quiet;               /* samples spent paused                       */
  gboolean char_out;            /* char already emitted in this space run     */
  gboolean word_out;            /* word space already emitted                 */
  gboolean overlong;            /* current mark is a carrier, not keying      */

  guint64 nsamp;                /* debug: absolute sample counter             */

  /* estimates */
  double prev_i, prev_q;
  double foff_hz;               /* EMA of the in-channel tone offset          */
  double elem_err;              /* EMA of |run − ideal|/ideal                 */
} CwState;

static gpointer cw_channel_new(double sample_rate) {
  morse_init();
  CwState *st = g_new0(CwState, 1);
  st->rate      = sample_rate > 0 ? sample_rate : 250.0;
  st->k_hi_rel  = 1.0 - exp(-1.0 / (st->rate * 0.8));   /* peak release 0.8 s */
  st->k_lo      = 1.0 - exp(-1.0 / (st->rate * 0.3));   /* floor EMA 0.3 s    */
  st->k_ph      = 1.0 - exp(-1.0 / (st->rate * 0.7));   /* duty EMA 0.7 s     */
  st->env_hi = st->env_lo = -1.0;                       /* seed on 1st sample */
  st->elem_err = 0.2;
  return st;
}

static void cw_channel_free(gpointer state) { g_free(state); }

/* Reset the assembly when the channel goes quiet or turns out to be carrier. */
static void assembly_reset(CwState *st) {
  st->code = 0;
  st->nelem = 0;
  st->marks_ok = 0;
  st->dit = 0;
  st->nboot = 0;
  st->char_out = st->word_out = TRUE;   /* no leading space on next signal    */
  st->overlong = FALSE;
}

/* Pause: the signal went quiet, but the fist was learned — KEEP the dit
 * clock so the first character after the pause decodes whole. A full reset
 * here sent 4-8 marks back into the bootstrap and ate the leading letter of
 * every over ("Z31RM" logged as "31RM" all through the 2026-07-15 sample). */
static void assembly_pause(CwState *st) {
  st->code = 0;
  st->nelem = 0;
  st->char_out = st->word_out = TRUE;
  st->overlong = FALSE;
}

static void emit(SkimDecode *out, guint *pos, char c) {
  if (*pos + 1 < SKIM_DECODE_TEXT_MAX) {
    out->text[(*pos)++] = c;
    out->text[*pos] = '\0';
  }
}

/* Whole string or nothing — a multi-byte UTF-8 mark must never be split. */
static void emit_str(SkimDecode *out, guint *pos, const char *s) {
  gsize n = strlen(s);
  if (*pos + n < SKIM_DECODE_TEXT_MAX) {
    memcpy(out->text + *pos, s, n + 1);
    *pos += (guint)n;
  }
}

static void emit_code(CwState *st, SkimDecode *out, guint *pos) {
  if (st->nelem && st->code < G_N_ELEMENTS(morse_lut)) {
    char c = morse_lut[st->code];
    /* Single-element characters (E/T) are what random noise degenerates
     * into — an isolated mark only counts once the channel has shown a
     * consistent fist (several classified marks, stable element timing).
     * Real E/T inside running text always passes; lone noise pings do not
     * (2026-07-15 contest sample: E+T were 33 % of all decoded chars). */
    gboolean lone_et = st->nelem == 1 &&
                       (st->marks_ok < 4 || st->elem_err > 0.45);
    if (c && !lone_et) { emit(out, pos, c); }
  }
  st->code  = 0;
  st->nelem = 0;
}

/* A mark run ended: classify it as dot/dash and refine the dit clock. */
static void classify_mark(CwState *st, double run) {
  if (st->dit <= 0) {
    /* Bootstrap: buffer marks until dots AND dashes are both on record. */
    if (st->nboot < BOOT_MARKS) { st->boot[st->nboot++] = run; }
    double lo = st->boot[0], hi = st->boot[0];
    for (guint i = 1; i < st->nboot; i++) {
      lo = MIN(lo, st->boot[i]);
      hi = MAX(hi, st->boot[i]);
    }
    if (st->nboot >= 4 && hi >= 2.0 * lo) {
      double mid = (lo + hi) / 2.0, sum = 0.0;
      guint  n = 0;
      for (guint i = 0; i < st->nboot; i++) {
        if (st->boot[i] < mid) { sum += st->boot[i]; n++; }
      }
      st->dit = sum / n;
    } else if (st->nboot == BOOT_MARKS) {
      st->dit = lo;                      /* monotone fist — assume dots       */
    }
    return;                              /* bootstrap marks are not decoded   */
  }

  gboolean dash  = run > 2.0 * st->dit;
  CW_DBG("[mark %5.1f dit %4.1f -> %s]\n", run, st->dit, dash ? "DASH" : "dot");
  double   ideal = dash ? 3.0 * st->dit : st->dit;
  st->elem_err += 0.1 * (fabs(run - ideal) / ideal - st->elem_err);
  st->dit      += 0.15 * ((dash ? run / 3.0 : run) - st->dit);
  /* Lower dit bound = 55 WPM. Nobody CQs faster; every "80 WPM" trace on
   * the 2026-07-15 contest sample was noise riding the old 0.015 clamp. */
  st->dit       = CLAMP(st->dit, st->rate * 0.022, st->rate * 0.30);
  st->marks_ok++;
  if (st->nelem < 7) {
    st->code  = (st->nelem ? st->code : 1u) << 1 | (dash ? 1u : 0u);
    st->nelem++;
  } else {
    st->code = 0;                        /* > 7 elements is not a character   */
    st->nelem = 0;
  }
}

static gboolean cw_process(gpointer state, const float *iq, guint nframes,
                           SkimDecode *out) {
  CwState *st = state;
  guint    pos = 0;
  out->text[0] = '\0';

  for (guint n = 0; n < nframes; n++) {
    const float i = iq[2 * n], q = iq[2 * n + 1];
    const float mag = sqrtf(i * i + q * q);

    /* 3-tap moving average. */
    st->ma[st->ma_n % 3] = mag;
    st->ma_n++;
    float m = (st->ma[0] + st->ma[1] + st->ma[2]) / (float)MIN(st->ma_n, 3);

    /* Level trackers. The floor is an EMA of the below-midpoint samples —
     * the MEAN of the quiet state, not its minimum. A min-follower reads
     * Rayleigh noise as "floor ≪ peaks" and lets pure noise through the
     * squelch; the mean keeps noise-only channels at peak/floor ≈ 2. */
    if (st->env_hi < 0) { st->env_hi = st->env_lo = m; }
    st->env_hi = m > st->env_hi
                     ? m
                     : st->env_hi + st->k_hi_rel * (m - st->env_hi);
    if (m < 0.5 * (st->env_lo + st->env_hi)) {
      st->env_lo += st->k_lo * (m - st->env_lo);
    }

    /* Squelch with hysteresis: open at peak > 4×floor, close only when the
     * peak sinks under 2.6×floor. Without the gap a marginal signal's peak
     * tracker decays through a single threshold DURING word gaps — the gate
     * flaps, the dit clock resets, and the bootstrap eats the next character
     * (the systematic every-other-char loss the 12 dB gate caught). */
    if (!st->gate_open) {
      st->gate_open = st->env_hi > 4.0 * st->env_lo + 1e-6;
    } else if (st->env_hi < 2.6 * st->env_lo + 1e-6) {
      st->gate_open = FALSE;
    }

    /* Keying-likeness: CW spends its mark duty cycle (~40 %) NEAR THE PEAK
     * of the envelope; noise magnitude hardly ever reaches it (~4 %). The
     * peak/floor ratio alone cannot tell channel noise from a weak signal —
     * this can, and it is what keeps noise-only channels mute. */
    const double span0 = st->env_hi - st->env_lo;
    st->p_high += st->k_ph *
                  ((m > st->env_lo + 0.7 * span0 ? 1.0 : 0.0) - st->p_high);
    if (!st->keying) {
      st->keying = st->p_high > 0.15;
    } else if (st->p_high < 0.07) {
      st->keying = FALSE;
    }

    /* A channel with a PROVEN fist is gated by the envelope squelch alone:
     * the keying-duty test exists to keep unknown/noise channels mute, but
     * its EMA dips during inter-over pauses and used to hard-reset the dit
     * clock (and truncate the first mark back) — the clipped-first-letter
     * bug. Proven = enough consistently timed marks; a bogus dit learned
     * from noise (few marks, ragged timing) still faces the duty test.
     * Quiet longer than 8 s forgets the fist for real. */
    const gboolean solid =
        st->dit > 0 && st->marks_ok >= 8 && st->elem_err < 0.35;
    if (!st->gate_open || (!st->keying && !solid)) {
      if (solid || st->nboot) {
        if (!st->paused) {
          /* The signal vanished — that ends the over too. */
          if (st->dit > 0 && !st->brk_out) {
            st->brk_out = TRUE;
            emit_str(out, &pos, BREAK_MARK);
          }
          assembly_pause(st);
          st->paused = TRUE;
          st->quiet  = 0.0;
        }
        st->quiet += 1.0;
        if (st->quiet > 8.0 * st->rate) {
          assembly_reset(st);
          st->paused = FALSE;
        }
      }
      st->key = FALSE;
      st->run = 0;
      st->pend_valid = FALSE;
      continue;
    }
    st->paused = FALSE;

    /* In-channel tone offset from the phase slope, marks only. */
    if (st->key && mag > st->env_lo * 3.0) {
      const double dr = (double)i * st->prev_i + (double)q * st->prev_q;
      const double di = (double)q * st->prev_i - (double)i * st->prev_q;
      if (dr != 0.0 || di != 0.0) {
        const double f = atan2(di, dr) * st->rate / (2.0 * G_PI);
        st->foff_hz += 0.01 * (f - st->foff_hz);
      }
    }
    st->prev_i = i;
    st->prev_q = q;

    /* Schmitt keying. The ON threshold sits high (0.55 of the span) so noise
     * pings rarely key at all; once keyed, OFF at 0.30 rides dips out. */
    const double span = st->env_hi - st->env_lo;
    const double thr  = st->env_lo + (st->key ? 0.30 : 0.55) * span;
    const gboolean want = m > thr;
    const double glitch = st->dit > 0 ? MAX(2.0, st->dit / 3.0) : 3.0;

    /* A completed run is only CLASSIFIED once the run that follows outgrows
     * the glitch length — a shorter follower is a blip (noise ping in a
     * space, dropout inside a dash) and the interrupted run resumes as if
     * nothing happened. This is what keeps dashes whole at low SNR. */
    st->nsamp++;
    if (want != st->key) {
      if (st->pend_valid) {
        CW_DBG("%6lu fold blip %s(%.0f) back into %s(%.0f)\n",
               (unsigned long)st->nsamp, st->key ? "M" : "S", st->run,
               st->pend_key ? "M" : "S", st->pend_len);
        st->run += st->pend_len;      /* blip: fold and resume (want ==     */
        st->key  = want;              /* pend_key by construction)          */
        st->pend_valid = FALSE;
      } else {
        CW_DBG("%6lu end %s(%.0f)\n", (unsigned long)st->nsamp,
               st->key ? "M" : "S", st->run);
        st->pend_key   = st->key;
        st->pend_len   = st->run;
        st->pend_valid = TRUE;
        st->key = want;
        st->run = 0.0;
      }
    }
    st->run += 1.0;

    if (st->pend_valid && st->run > glitch) {
      if (st->pend_key) {             /* a mark, now proven by a real space */
        /* Anything much shorter than a dot is a noise ping, not an element —
         * classifying it would insert phantom dots into the character. */
        const double mark_min = st->dit > 0 ? 0.55 * st->dit : glitch;
        if (!st->overlong && st->pend_len >= mark_min) {
          classify_mark(st, st->pend_len);
          st->char_out = FALSE;
          st->word_out = FALSE;
          st->brk_out  = FALSE;
          st->last_space = 0.0;
        } else if (!st->overlong) {
          /* Discarded ping: it split one space in two — bridge them, or the
           * character gap never reaches 2.2 dits and two characters merge
           * into an invalid code that silently disappears. */
          CW_DBG("[ping %4.1f discarded, space resumes %.1f]\n",
                 st->pend_len, st->run + st->pend_len + st->last_space);
          st->run += st->pend_len + st->last_space;
          st->last_space = 0.0;
        }
        st->overlong = FALSE;
      } else {
        st->last_space = st->pend_len; /* the space before the current mark  */
      }
      st->pend_valid = FALSE;
    }

    if (st->key) {
      /* A mark far beyond a dash is a carrier — drop the assembly. */
      if (st->dit > 0 && !st->overlong && st->run > 8.0 * st->dit) {
        st->overlong = TRUE;
        st->code = 0;
        st->nelem = 0;
      }
    } else if (st->dit > 0 && !st->pend_valid) {
      /* Live space emission: char at 2.2 dits, word space at 5.5. */
      if (!st->char_out && st->run > 2.2 * st->dit) {
        st->char_out = TRUE;
        CW_DBG("[char gap, code %u/%u elems]\n", st->code, st->nelem);
        emit_code(st, out, &pos);
      }
      if (!st->word_out && st->char_out && st->run > 5.5 * st->dit) {
        st->word_out = TRUE;
        emit(out, &pos, ' ');
      }
      if (!st->brk_out && st->word_out && st->run > BREAK_DITS * st->dit) {
        st->brk_out = TRUE;                    /* over ended — separator     */
        emit_str(out, &pos, BREAK_MARK);
      }
    }
  }

  if (pos == 0)
    return FALSE;

  out->confidence     = CLAMP(1.0 - 1.5 * st->elem_err, 0.0, 1.0);
  out->freq_offset_hz = st->foff_hz;
  out->speed          = st->dit > 0 ? 1.2 * st->rate / st->dit : 0.0;
  out->snr_db         = 20.0 * log10(st->env_hi / MAX(st->env_lo, 1e-9));
  return TRUE;
}

static double cw_level(gpointer state) {
  return ((CwState *)state)->env_hi;
}

static double cw_tone_offset_hz(gpointer state) {
  return ((CwState *)state)->foff_hz;
}

const SkimDecodeBackend *skim_decode_cw(void) {
  static const SkimDecodeBackend backend = {
    .name           = "cw",
    .channel_new    = cw_channel_new,
    .channel_free   = cw_channel_free,
    .process        = cw_process,
    .level          = cw_level,
    .tone_offset_hz = cw_tone_offset_hz,
  };
  return &backend;
}
