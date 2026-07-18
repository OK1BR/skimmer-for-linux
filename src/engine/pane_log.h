/* pane_log.h — one frequency slot's pane text with an optional live "over"
 * region at its tail (phase B: draft → final hybrid pane).
 *
 * The app's per-frequency decode history and the offline gate both apply
 * SkimPaneOp streams through this, so the protocol semantics live (and are
 * tested) in exactly one place. Plain appends and head trims are here too —
 * the over offset must survive both. GLib-only.
 *
 * Ops are full-state: SET/CLOSE carry the over's entire text, and a SET on a
 * log whose over region was never opened (a dropped OPEN, an app restart)
 * self-syncs by opening an empty region at the tail. The over region is
 * always the TAIL of the log; a plain append while an over is open lands
 * BEFORE the region (an interleaving neighbour slot must not be eaten by
 * the next SET).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_PANE_LOG_H
#define SKIMMER_PANE_LOG_H

#include <glib.h>
#include "decode.h"

G_BEGIN_DECLS

typedef struct _SkimPaneLog SkimPaneLog;

SkimPaneLog *skim_pane_log_new(void);
void         skim_pane_log_free(SkimPaneLog *pl);

const char *skim_pane_log_text(const SkimPaneLog *pl);
gsize       skim_pane_log_len(const SkimPaneLog *pl);

/* The live over region: bytes at the tail (0 = none open), and how many of
 * them are reader-final (the rest is live draft — dim in the UI). */
gsize skim_pane_log_over_len(const SkimPaneLog *pl);
gsize skim_pane_log_final_len(const SkimPaneLog *pl);

/* Plain append: an open over region stays the log's tail — the new text is
 * inserted just before it. */
void skim_pane_log_append(SkimPaneLog *pl, const char *text);

/* Apply one op (APPEND routes to skim_pane_log_append). */
void skim_pane_log_apply(SkimPaneLog *pl, const SkimPaneOp *op);

/* Drop bytes from the head (history cap); clamps into an open over region
 * only when it must. */
void skim_pane_log_trim_head(SkimPaneLog *pl, gsize bytes);

G_END_DECLS

#endif /* SKIMMER_PANE_LOG_H */
