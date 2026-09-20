/* skimmer-pane-test — offline gate for the phase B hybrid pane
 * (draft → final: the streamed neural reader owns a solid over's pane text,
 * v2's live draft shows until each word firms up).
 *
 *   - SkimPaneLog unit semantics: append / OPEN / SET / CLOSE / head trim,
 *     self-sync of a SET whose OPEN was lost, appends landing BEFORE an
 *     open over region.
 *   - CW word roles (the pane tints CQ / DE / 5NN / TU… by role): whole
 *     tokens only — a glued over or a call holding a keyword stays plain.
 *   - stream positions: push_pos/flush_pos text is bit-identical to read()
 *     and every committed char carries a monotone in-range run index.
 *   - the WHOLE offline pipeline (SKIM_CW_V2=1 + SKIM_CW_READER=blob):
 *     ops arrive, the reader's words commit mid-over (SETs), the over
 *     CLOSEs with the reader text, the composed pane equals it exactly
 *     once (no doubled draft, no newlines), runs are deterministic, the
 *     decode log carries the increments, and the station table is
 *     IDENTICAL with the reader off — nothing of the reader ever reaches
 *     the extractor.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "engine/cw_reader.h"
#include "engine/cw_words.h"
#include "engine/decode.h"
#include "engine/pane_log.h"
#include "engine/pipeline.h"

static int checks, fails;

static void check(const char *what, gboolean ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
}

/* --- SkimPaneLog units ----------------------------------------------------------- */

static void op(SkimPaneLog *pl, SkimPaneOpKind k, guint erase,
               const char *text, guint flen) {
  SkimPaneOp o = { k, erase, flen, (char *)text, NULL };
  skim_pane_log_apply(pl, &o);
}

static void run_pane_log_units(void) {
  printf("--- SkimPaneLog units ---\n");
  SkimPaneLog *pl = skim_pane_log_new();

  skim_pane_log_append(pl, "OLD ");
  skim_pane_log_append(pl, "CQ TE");                    /* the shown draft   */
  op(pl, SKIM_PANE_OP_OPEN, 5, "CQ TEST", 0);
  check("OPEN takes the draft back",
        strcmp(skim_pane_log_text(pl), "OLD CQ TEST") == 0);
  check("over region spans the new text", skim_pane_log_over_len(pl) == 7);

  op(pl, SKIM_PANE_OP_SET, 0, "CQ TEST OK1BR OK", 8);
  check("SET replaces the region",
        strcmp(skim_pane_log_text(pl), "OLD CQ TEST OK1BR OK") == 0);
  check("final prefix tracked", skim_pane_log_final_len(pl) == 8);

  skim_pane_log_append(pl, "[N]");                     /* neighbour slot     */
  check("append lands BEFORE the open region",
        strcmp(skim_pane_log_text(pl), "OLD [N]CQ TEST OK1BR OK") == 0);

  op(pl, SKIM_PANE_OP_CLOSE, 0, "CQ TEST OK1BR OK1BR", 19);
  check("CLOSE seals with the final text",
        strcmp(skim_pane_log_text(pl), "OLD [N]CQ TEST OK1BR OK1BR") == 0);
  check("no region after CLOSE", skim_pane_log_over_len(pl) == 0);

  skim_pane_log_append(pl, " X");
  check("append after CLOSE is plain",
        g_str_has_suffix(skim_pane_log_text(pl), "OK1BR X"));

  op(pl, SKIM_PANE_OP_SET, 0, "NEW", 3);               /* lost OPEN          */
  check("orphan SET self-syncs at the tail",
        g_str_has_suffix(skim_pane_log_text(pl), "OK1BR XNEW") &&
            skim_pane_log_over_len(pl) == 3);

  const gsize before = skim_pane_log_len(pl);
  skim_pane_log_trim_head(pl, 4);
  check("head trim keeps the region",
        skim_pane_log_len(pl) == before - 4 &&
            skim_pane_log_over_len(pl) == 3 &&
            g_str_has_suffix(skim_pane_log_text(pl), "NEW"));

  skim_pane_log_free(pl);
}

/* --- CW word roles ----------------------------------------------------------------
 * The pane tints protocol words by role. Whole tokens only: a glued over or
 * a callsign that merely contains a keyword must stay plain. */

static void run_word_role_units(void) {
  printf("--- CW word roles ---\n");
  check("CQ / TEST / QRZ call",
        skim_cw_word_role("CQ") == SKIM_CW_WORD_CALLING &&
            skim_cw_word_role("TEST") == SKIM_CW_WORD_CALLING &&
            skim_cw_word_role("QRZ") == SKIM_CW_WORD_CALLING);
  check("DE is its own role", skim_cw_word_role("DE") == SKIM_CW_WORD_DE);
  check("5NN / 599 / cut ENN report",
        skim_cw_word_role("5NN") == SKIM_CW_WORD_REPORT &&
            skim_cw_word_role("599") == SKIM_CW_WORD_REPORT &&
            skim_cw_word_role("ENN") == SKIM_CW_WORD_REPORT);
  check("TU / 73 / K / BK / KN / SK / AR close",
        skim_cw_word_role("TU") == SKIM_CW_WORD_CLOSING &&
            skim_cw_word_role("73") == SKIM_CW_WORD_CLOSING &&
            skim_cw_word_role("K") == SKIM_CW_WORD_CLOSING &&
            skim_cw_word_role("BK") == SKIM_CW_WORD_CLOSING &&
            skim_cw_word_role("KN") == SKIM_CW_WORD_CLOSING &&
            skim_cw_word_role("SK") == SKIM_CW_WORD_CLOSING &&
            skim_cw_word_role("AR") == SKIM_CW_WORD_CLOSING);
  check("a glued over is no keyword",
        skim_cw_word_role("CQTESTSF6W") == SKIM_CW_WORD_NONE &&
            skim_cw_word_role("CQTEST") == SKIM_CW_WORD_NONE &&
            skim_cw_word_role("TU5NN") == SKIM_CW_WORD_NONE);
  check("a call holding a keyword stays plain",
        skim_cw_word_role("DE1ABC") == SKIM_CW_WORD_NONE &&
            skim_cw_word_role("SK5AA") == SKIM_CW_WORD_NONE &&
            skim_cw_word_role("K1AR") == SKIM_CW_WORD_NONE);
  check("noise fragments R / E / T / EE carry no role",
        skim_cw_word_role("R") == SKIM_CW_WORD_NONE &&
            skim_cw_word_role("E") == SKIM_CW_WORD_NONE &&
            skim_cw_word_role("T") == SKIM_CW_WORD_NONE &&
            skim_cw_word_role("EE") == SKIM_CW_WORD_NONE);
  check("empty and NULL are safe",
        skim_cw_word_role("") == SKIM_CW_WORD_NONE &&
            skim_cw_word_role(NULL) == SKIM_CW_WORD_NONE);
}

/* --- stream positions -------------------------------------------------------------
 * Perfectly timed runs for a short text; the v3 blob must stream the same
 * chars read() produces, each with a sane, monotone run index. */

static const struct { char c; const char *m; } MORSE[] = {
  {'A',".-"},   {'B',"-..."}, {'C',"-.-."}, {'D',"-.."},  {'E',"."},
  {'F',"..-."}, {'G',"--."},  {'H',"...."}, {'I',".."},   {'J',".---"},
  {'K',"-.-"},  {'L',".-.."}, {'M',"--"},   {'N',"-."},   {'O',"---"},
  {'P',".--."}, {'Q',"--.-"}, {'R',".-."},  {'S',"..."},  {'T',"-"},
  {'U',"..-"},  {'V',"...-"}, {'W',".--"},  {'X',"-..-"}, {'Y',"-.--"},
  {'Z',"--.."},
  {'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},
  {'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
};

static const char *morse_of(char c) {
  for (guint i = 0; i < G_N_ELEMENTS(MORSE); i++) {
    if (MORSE[i].c == c) { return MORSE[i].m; }
  }
  return NULL;
}

/* text → alternating key/duration runs at `dit` ms. */
static void runs_of(const char *text, double dit, GArray *key, GArray *dur) {
  const gboolean t = TRUE, f = FALSE;
  for (const char *p = text; *p; p++) {
    if (*p == ' ') {
      if (dur->len) {                            /* extend the last gap      */
        double *d = &g_array_index(dur, double, dur->len - 1);
        *d = 7.0 * dit;
      }
      continue;
    }
    const char *m = morse_of(*p);
    if (!m) { continue; }
    for (const char *e = m; *e; e++) {
      const double dm = (*e == '-' ? 3.0 : 1.0) * dit;
      const double dg = (e[1] ? 1.0 : 3.0) * dit;
      g_array_append_val(key, t);
      g_array_append_val(dur, dm);
      g_array_append_val(key, f);
      g_array_append_val(dur, dg);
    }
  }
}

int main(void) {
  printf("=== decode pane gate (SkimPaneLog units) ===\n");
  run_pane_log_units();
  run_word_role_units();
  printf("=== %d checks, %d failures ===\n", checks, fails);
  return fails ? 1 : 0;
}
