/* cw_words.h — the role a whole CW token plays in a QSO (calling, DE,
 * report, closing), for the text layer: the decode pane tints by it, the
 * column snippet can.
 *
 * A closed vocabulary matched on WHOLE tokens only — never on a substring:
 * a glued over ("CQTESTSF6W") is no keyword, and half the band's fragments
 * would light up otherwise. The contest exchange has no role here: telling
 * a serial from a zone takes QSO state, not a word list. GLib-only.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_CW_WORDS_H
#define SKIMMER_CW_WORDS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  SKIM_CW_WORD_NONE = 0,
  SKIM_CW_WORD_CALLING,  /* CQ TEST QRZ                                     */
  SKIM_CW_WORD_DE,       /* DE                                              */
  SKIM_CW_WORD_REPORT,   /* 5NN 599 ENN                                     */
  SKIM_CW_WORD_CLOSING,  /* TU 73 K BK KN SK AR                             */
  SKIM_CW_WORD_N_ROLES
} SkimCwWordRole;

/* `tok` is one upper-case token, already cut at its boundaries. */
SkimCwWordRole skim_cw_word_role(const char *tok);

G_END_DECLS

#endif /* SKIMMER_CW_WORDS_H */
