/* cw_words.c — see cw_words.h.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "cw_words.h"

#include <string.h>

/* R, E and T stay out on purpose: a lone dit or dah is the band's commonest
 * noise fragment, and R is a legal callsign prefix besides. ENN is the cut
 * 5NN — 347 of them in one SAC CW day against 7582 full ones. */
SkimCwWordRole skim_cw_word_role(const char *tok) {
  static const struct { const char *w; SkimCwWordRole role; } WORDS[] = {
    { "CQ",  SKIM_CW_WORD_CALLING }, { "TEST", SKIM_CW_WORD_CALLING },
    { "QRZ", SKIM_CW_WORD_CALLING },
    { "DE",  SKIM_CW_WORD_DE },
    { "5NN", SKIM_CW_WORD_REPORT },  { "599",  SKIM_CW_WORD_REPORT },
    { "ENN", SKIM_CW_WORD_REPORT },
    { "TU",  SKIM_CW_WORD_CLOSING }, { "73",   SKIM_CW_WORD_CLOSING },
    { "K",   SKIM_CW_WORD_CLOSING }, { "BK",   SKIM_CW_WORD_CLOSING },
    { "KN",  SKIM_CW_WORD_CLOSING }, { "SK",   SKIM_CW_WORD_CLOSING },
    { "AR",  SKIM_CW_WORD_CLOSING },
  };
  if (!tok || !tok[0] || strlen(tok) > 4) { return SKIM_CW_WORD_NONE; }
  for (guint i = 0; i < G_N_ELEMENTS(WORDS); i++) {
    if (strcmp(tok, WORDS[i].w) == 0) { return WORDS[i].role; }
  }
  return SKIM_CW_WORD_NONE;
}
