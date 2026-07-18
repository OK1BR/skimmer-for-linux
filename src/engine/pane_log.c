/* pane_log.c — see pane_log.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "pane_log.h"

#include <string.h>

struct _SkimPaneLog {
  GString *text;
  gssize   over_off;                   /* byte offset of the over region;
                                        * -1 = no over open                  */
  gsize    final_len;                  /* reader-final prefix of the region  */
};

SkimPaneLog *skim_pane_log_new(void) {
  SkimPaneLog *pl = g_new0(SkimPaneLog, 1);
  pl->text     = g_string_new(NULL);
  pl->over_off = -1;
  return pl;
}

void skim_pane_log_free(SkimPaneLog *pl) {
  if (!pl)
    return;
  g_string_free(pl->text, TRUE);
  g_free(pl);
}

const char *skim_pane_log_text(const SkimPaneLog *pl) { return pl->text->str; }
gsize skim_pane_log_len(const SkimPaneLog *pl) { return pl->text->len; }

gsize skim_pane_log_over_len(const SkimPaneLog *pl) {
  return pl->over_off < 0 ? 0 : pl->text->len - (gsize)pl->over_off;
}

gsize skim_pane_log_final_len(const SkimPaneLog *pl) {
  return pl->over_off < 0 ? 0 : MIN(pl->final_len, skim_pane_log_over_len(pl));
}

void skim_pane_log_append(SkimPaneLog *pl, const char *text) {
  if (!text || !text[0])
    return;
  if (pl->over_off < 0) {
    g_string_append(pl->text, text);
  } else {
    /* keep the over region the tail — a neighbour slot's text (the ±25 Hz
     * routing window can interleave two carriers into one log) must not be
     * swallowed by the region's next full-state SET */
    g_string_insert(pl->text, pl->over_off, text);
    pl->over_off += (gssize)strlen(text);
  }
}

static void over_replace(SkimPaneLog *pl, const SkimPaneOp *op) {
  if (pl->over_off < 0 || (gsize)pl->over_off > pl->text->len) {
    pl->over_off = (gssize)pl->text->len;      /* self-sync: open at tail    */
  }
  g_string_truncate(pl->text, (gsize)pl->over_off);
  if (op->text) { g_string_append(pl->text, op->text); }
  pl->final_len = MIN(op->final_len, skim_pane_log_over_len(pl));
}

void skim_pane_log_apply(SkimPaneLog *pl, const SkimPaneOp *op) {
  switch (op->kind) {
  case SKIM_PANE_OP_APPEND:
    skim_pane_log_append(pl, op->text);
    break;
  case SKIM_PANE_OP_OPEN: {
    /* eat the already-shown draft of this over, then open at the tail */
    const gsize erase = MIN((gsize)op->erase, pl->text->len);
    g_string_truncate(pl->text, pl->text->len - erase);
    pl->over_off = (gssize)pl->text->len;
    if (op->text) { g_string_append(pl->text, op->text); }
    pl->final_len = MIN(op->final_len, skim_pane_log_over_len(pl));
    break;
  }
  case SKIM_PANE_OP_SET:
    over_replace(pl, op);
    break;
  case SKIM_PANE_OP_CLOSE:
    over_replace(pl, op);
    pl->over_off  = -1;
    pl->final_len = 0;
    break;
  }
}

void skim_pane_log_trim_head(SkimPaneLog *pl, gsize bytes) {
  bytes = MIN(bytes, pl->text->len);
  if (!bytes)
    return;
  g_string_erase(pl->text, 0, (gssize)bytes);
  if (pl->over_off >= 0) {
    pl->over_off = MAX(pl->over_off - (gssize)bytes, 0);
  }
}
