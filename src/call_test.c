/*
 * skimmer-call-test — offline gate for callsign extraction/validation (M4).
 *
 * Three layers:
 *   - validity: structural + ITU allocation on hand-picked calls (rare
 *     prefixes must pass, decode-garbage shapes must die — T1BR, 5NN, Q…),
 *   - a labelled corpus of realistic decoder output lines (with the error
 *     patterns decode_cw actually makes) → PRECISION MUST BE 1.0, recall
 *     ≥ 0.9 — the RBN rule is "never spot garbage",
 *   - fuzz: E/T-biased noise-decode babble and random alnum tokens must
 *     never reach the spot threshold (seeded, deterministic).
 * Plus mechanics: token continuity across fragmented feeds, DE-marker
 * behaviour, dictionary boost, candidate eviction.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/callsign.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

int main(void) {
  printf("=== callsign gate (offline) ===\n");

  /* -- validity: must accept ------------------------------------------------ */
  static const char *GOOD[] = {
    "OK1BR", "W1AW", "K1A", "DL1ABC", "LZ2PP", "UA9CDC", "F5IN", "G3XYZ",
    "B1HQ", "9A1AA", "2E0ABC", "3DA0RS", "E73ABC", "T77XX", "C6AGU",
    "VK6ABC", "ZL4AA", "5B4AH", "9V1AB", "OK1BR/P", "OK1BR/4", "F/OK1BR",
    "HB9CV", "OL2025X", "SP9XYZ", "PY2ABC",
  };
  int good_ok = 0;
  for (guint i = 0; i < G_N_ELEMENTS(GOOD); i++) {
    if (skim_callsign_is_valid(GOOD[i])) {
      good_ok++;
    } else {
      printf("       rejected valid: %s\n", GOOD[i]);
    }
  }
  check("accepts real callsigns incl. rare prefixes (26/26)",
        good_ok == (int)G_N_ELEMENTS(GOOD));

  /* -- validity: must reject ------------------------------------------------- */
  static const char *BAD[] = {
    "T1BR",                      /* the decoder's classic O→T truncation     */
    "QRL", "QRZ1X", "Q1AB",      /* Q is Q-codes, never a call               */
    "5NN", "599", "73",          /* exchange babble                          */
    "TEST", "TU", "K", "AGN",    /* letters-only / too short                 */
    "E", "EEE", "TTT",           /* noise decodes                            */
    "OK1", "OKABC", "1ABC",      /* no suffix / no digit / 0-1 first         */
    "0K1BR", "OK1BR2",           /* zero-first, suffix ends in a digit       */
    "OK1BRXYZQ",                 /* too long                                 */
    "OK1BR/P/M",                 /* two designators                          */
  };
  int bad_ok = 0;
  for (guint i = 0; i < G_N_ELEMENTS(BAD); i++) {
    if (!skim_callsign_is_valid(BAD[i])) {
      bad_ok++;
    } else {
      printf("       accepted garbage: %s\n", BAD[i]);
    }
  }
  check("rejects garbage and decode-error shapes (18/18)",
        bad_ok == (int)G_N_ELEMENTS(BAD));

  /* -- labelled corpus: realistic decoder lines ------------------------------ */
  static const struct { const char *text, *expect; } CORPUS[] = {
    { "CQ TEST DE OK1BR OK1BR K",            "OK1BR"  },
    { "UV CQ TEST DE OK1BR OK1BR K",         "OK1BR"  },  /* warmup garble   */
    { "CQ CQ DE DL1ABC DL1ABC PSE K",        "DL1ABC" },
    { "CQ DE W1AW K",                        "W1AW"   },  /* single, DE      */
    { "TEST OK5Z OK5Z TEST",                 "OK5Z"   },  /* repeat, no DE   */
    { "DE R OK1BR OK1BR",                    "OK1BR"  },  /* garble after DE */
    { "T1BR OK1BR OK1BR",                    "OK1BR"  },  /* err then good   */
    { "CQ DE 9A1AA 9A1AA",                   "9A1AA"  },  /* digit-first     */
    { "QRL QSY DE F5IN K",                   "F5IN"   },  /* q-codes around  */
    { "CQ DE E73ABC E73ABC K",               "E73ABC" },  /* letter+digit px */
    { "TU 5NN 73 GL",                        NULL     },  /* exchange only   */
    { "E EEE T TTT EE",                      NULL     },  /* noise babble    */
    { "VVV VVV VVV",                         NULL     },  /* tune-up         */
    { "OK1XX",                               NULL     },  /* lone, no ctx    */
    { "CQ TEST DE OK1BR/P OK1BR/P",          "OK1BR/P"},  /* portable        */
  };
  int tp = 0, fp = 0, fn = 0;
  for (guint i = 0; i < G_N_ELEMENTS(CORPUS); i++) {
    char got[32];
    double s = skim_callsign_extract(CORPUS[i].text, got, sizeof(got));
    if (CORPUS[i].expect) {
      if (s > 0 && strcmp(got, CORPUS[i].expect) == 0) {
        tp++;
      } else if (s > 0) {
        fp++;
        printf("       [%u] wrong call: \"%s\" -> %s (want %s)\n",
               i, CORPUS[i].text, got, CORPUS[i].expect);
      } else {
        fn++;
        printf("       [%u] missed: \"%s\" (want %s)\n",
               i, CORPUS[i].text, CORPUS[i].expect);
      }
    } else if (s > 0) {
      fp++;
      printf("       [%u] false spot: \"%s\" -> %s (%.2f)\n",
             i, CORPUS[i].text, got, s);
    }
  }
  int npos = 0;
  for (guint i = 0; i < G_N_ELEMENTS(CORPUS); i++) {
    if (CORPUS[i].expect) { npos++; }
  }
  printf("       corpus: %d TP, %d FP, %d FN over %d positives\n",
         tp, fp, fn, npos);
  check("corpus precision = 1.0 (not one false spot)", fp == 0);
  check("corpus recall ≥ 0.9", tp >= (int)ceil(0.9 * npos));

  /* -- token continuity across fragmented feeds ------------------------------ */
  {
    SkimCallsignExtractor *x = skim_callsign_extractor_new();
    skim_callsign_extractor_feed(x, "CQ TE");
    skim_callsign_extractor_feed(x, "ST DE OK1B");
    skim_callsign_extractor_feed(x, "R OK1BR K ");
    char got[32];
    double s = skim_callsign_extractor_best(x, got, sizeof(got));
    check("token fragments survive across feed() calls",
          s >= SKIM_CALLSIGN_SPOT_THRESHOLD && strcmp(got, "OK1BR") == 0);
    skim_callsign_extractor_reset(x);
    check("reset clears candidates",
          skim_callsign_extractor_best(x, got, sizeof(got)) == 0.0);
    skim_callsign_extractor_free(x);
  }

  /* -- dictionary boost -------------------------------------------------------- */
  {
    char got[32];
    double lone = skim_callsign_extract("HB9CV", got, sizeof(got));
    check("a lone call without context is NOT spottable", lone == 0.0);

    char *dict = g_build_filename(g_get_tmp_dir(), "skimmer-call-dict.txt", NULL);
    g_file_set_contents(dict, "# test dict\nHB9CV\nOK1BR\n", -1, NULL);
    GError *err = NULL;
    check("dictionary loads (MASTER.SCP style)",
          skim_callsign_dict_load(dict, &err) && skim_callsign_dict_size() == 2);
    g_clear_error(&err);
    double boosted = skim_callsign_extract("HB9CV", got, sizeof(got));
    check("dictionary hit lifts a lone call over the threshold",
          boosted >= SKIM_CALLSIGN_SPOT_THRESHOLD && strcmp(got, "HB9CV") == 0);
    g_remove(dict);
    g_free(dict);
  }

  /* -- fuzz: E/T-biased noise babble ------------------------------------------- */
  {
    GRand *rng = g_rand_new_with_seed(4711);
    static const char *NOISE = "EEEETTTISNAHM";   /* CW noise letter bias    */
    SkimCallsignExtractor *x = skim_callsign_extractor_new();
    char got[32];
    int spots = 0;
    for (int t = 0; t < 4000; t++) {
      char tok[8];
      int len = g_rand_int_range(rng, 1, 6);
      for (int i = 0; i < len; i++) {
        tok[i] = NOISE[g_rand_int_range(rng, 0, (gint)strlen(NOISE))];
      }
      tok[len] = ' ';
      tok[len + 1] = '\0';
      skim_callsign_extractor_feed(x, tok);
      if (skim_callsign_extractor_best(x, got, sizeof(got)) > 0) { spots++; }
    }
    check("4000 tokens of E/T noise babble: zero spots", spots == 0);
    skim_callsign_extractor_free(x);

    /* Random alnum tokens: a SINGLE mention of even structurally valid junk
     * must stay under the threshold (no DE, no repeats, no dict) — reset per
     * token, repetition of one call is legitimately spottable by design. */
    x = skim_callsign_extractor_new();
    static const char *AL = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    spots = 0;
    for (int t = 0; t < 4000; t++) {
      char tok[10];
      int len = g_rand_int_range(rng, 2, 8);
      for (int i = 0; i < len; i++) {
        tok[i] = AL[g_rand_int_range(rng, 0, 36)];
      }
      tok[len] = ' ';
      tok[len + 1] = '\0';
      skim_callsign_extractor_feed(x, tok);
      if (skim_callsign_extractor_best(x, got, sizeof(got)) > 0) { spots++; }
      skim_callsign_extractor_reset(x);
    }
    check("4000 random alnum tokens, single mentions: zero spots", spots == 0);
    skim_callsign_extractor_free(x);
    g_rand_free(rng);
  }

  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — precision holds, garbage dies, calls "
                          "get through.");
  return fails ? 1 : 0;
}
