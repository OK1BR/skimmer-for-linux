/* feed_twins.h — the RBN feed's memory of the calls read and sent nearby:
 * a look-alike of a call better attested within a few hundred Hz is a
 * misread of it, not a station of its own (gh#29).
 *
 * The station table folds a clipped or glued reading into the full call
 * only while both records are alive, and its glue rule asks MASTER.SCP
 * alone (station.c). A misread keyed twice the same way passes the
 * readings gate (gh#24), and a cut or glued twin that comes back after the
 * right call's record was pruned is a new station to the feed. Dan, OK1HRA,
 * measured both shapes in his fork and added two rules, written anew here:
 *
 * - The NEAR-CALL conflict (ok1hra/skimmer-for-linux 2e21c5d, memo an hour
 *   since 3a3acf4). Every call the table merges is remembered with where
 *   and when it was last READ and how often. A call MASTER.SCP does not
 *   know is held while a look-alike (two edits or fewer) within the radius,
 *   read within the memo time, is known to MASTER.SCP or was read more
 *   often. A dictionary call read twice or more yields only to a look-alike
 *   read at least NEAR_DICT_FLOOR times and NEAR_DICT_RATIO times as often;
 *   a dictionary call read once passed on the dictionary's word alone and
 *   yields like an unknown one. On CQ WW CW 2012, 154 of 162 such busts had
 *   the right call read by the same pipeline within 300 Hz in the hour.
 * - The TWIN FOLD (2ffa797). Every line sent is remembered. A call about to
 *   start a new run of lines is held when a look-alike (two edits, or one
 *   call the head or tail of the other) went out within the fold window and
 *   the radius and is better attested: MASTER.SCP knows it and not this one
 *   and it was read at least as often, or both stand alike with the
 *   dictionary and it was read more. A twin read more often than the call
 *   it resembles goes out — the feed often sends the garble first. In his
 *   live taps 225 of 477 boundary busts (F6FXX → F6FXXK, RK3DJW → RK3D) had
 *   the right call already spotted on that frequency.
 *
 * Both memos outlive the station table's prune: the runner a caller's
 * misread copies may have been quiet for a while. The near memo is stamped
 * with the clock of the last copy actually read (gh#26: a report on a quiet
 * channel is not a reading), the sent memo with the line's clock. A held
 * call is judged again each time it qualifies. GLib only, engine thread.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_FEED_TWINS_H
#define SKIMMER_FEED_TWINS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimFeedTwins SkimFeedTwins;

/* radius_hz: how far both rules look. near_us: how long a call read counts
 * as a rival (0 = the near-call rule is off). fold_us: how long a call sent
 * folds its twins (0 = the fold is off). run_us: a call sent within this
 * long is in its own run of lines and is never folded (the fold window
 * plus one re-spot period: the feed repeats a station only that often). */
SkimFeedTwins *skim_feed_twins_new(double radius_hz, gint64 near_us,
                                   gint64 fold_us, gint64 run_us);
void           skim_feed_twins_free(SkimFeedTwins *t);

/* The station table merged a report of call at hz: read_us is the clock of
 * the last copy actually read, hearings the record's count, dict whether
 * MASTER.SCP knows the call. */
void skim_feed_twins_read(SkimFeedTwins *t, const char *call, double hz,
                          gint64 read_us, guint hearings, gboolean dict);

/* A line for call went out at hz. */
void skim_feed_twins_sent(SkimFeedTwins *t, const char *call, double hz,
                          gint64 now_us, guint hearings, gboolean dict);

/* The near-call rule: the remembered call that holds this one back, or
 * NULL when it may go. The string is borrowed and valid until the memo
 * changes. */
const char *skim_feed_twins_near(const SkimFeedTwins *t, const char *call,
                                 double hz, gint64 now_us, guint hearings,
                                 gboolean dict);

/* The twin fold: the call sent lately that folds this one, or NULL. */
const char *skim_feed_twins_fold(const SkimFeedTwins *t, const char *call,
                                 double hz, gint64 now_us, guint hearings,
                                 gboolean dict);

/* Forget what no rule can use any more (call it on the table's tick). */
void  skim_feed_twins_sweep(SkimFeedTwins *t, gint64 now_us);
guint skim_feed_twins_size(const SkimFeedTwins *t);

G_END_DECLS

#endif /* SKIMMER_FEED_TWINS_H */
