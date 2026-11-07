/* feed_twins.c — the RBN feed's memory of the calls read and sent nearby
 * (see feed_twins.h). After OK1HRA's fork, written anew.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "feed_twins.h"

#include "callsign.h"

#include <math.h>
#include <string.h>

/* A dictionary call read twice or more yields to a look-alike only when
 * that one was read at least this often AND this many times as often: the
 * short contest calls the dictionary is full of (E2M, M5E, T6A) are exactly
 * what a misread of a neighbour lands on, and one or two copies of a
 * neighbour must not silence a station the dictionary vouches for. */
#define NEAR_DICT_FLOOR 3u
#define NEAR_DICT_RATIO 2u
/* A near-call rival is within this many edits; the fold also takes a call
 * that is the other's head or tail (a boundary gone wrong). */
#define TWIN_EDITS 2

typedef struct {
  double   hz;                 /* where it was last reported                */
  gint64   read_us;            /* clock of the last copy read; 0 = never    */
  guint    read_n;             /* most hearings a report carried            */
  double   sent_hz;            /* where its last line went out              */
  gint64   sent_us;            /* that line's clock; 0 = never sent         */
  guint    sent_n;             /* most hearings a line of this run carried  */
  gboolean dict;               /* MASTER.SCP's verdict, as of the last note */
} Twin;

struct _SkimFeedTwins {
  GHashTable *by_call;         /* call (owned) → Twin* (owned)              */
  double      radius_hz;
  gint64      near_us, fold_us, run_us;
};

SkimFeedTwins *skim_feed_twins_new(double radius_hz, gint64 near_us,
                                   gint64 fold_us, gint64 run_us) {
  SkimFeedTwins *t = g_new0(SkimFeedTwins, 1);
  t->by_call   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  t->radius_hz = radius_hz;
  t->near_us   = MAX(near_us, 0);
  t->fold_us   = MAX(fold_us, 0);
  t->run_us    = MAX(run_us, t->fold_us);
  return t;
}

void skim_feed_twins_free(SkimFeedTwins *t) {
  if (!t)
    return;
  g_hash_table_destroy(t->by_call);
  g_free(t);
}

static Twin *twin_get(SkimFeedTwins *t, const char *call) {
  Twin *w = g_hash_table_lookup(t->by_call, call);
  if (!w) {
    w = g_new0(Twin, 1);
    g_hash_table_insert(t->by_call, g_strdup(call), w);
  }
  return w;
}

void skim_feed_twins_read(SkimFeedTwins *t, const char *call, double hz,
                          gint64 read_us, guint hearings, gboolean dict) {
  if (!t || t->near_us <= 0 || !call || !call[0] || read_us <= 0)
    return;
  Twin *w = twin_get(t, call);
  w->hz      = hz;
  w->read_us = MAX(w->read_us, read_us);
  w->read_n  = MAX(w->read_n, hearings);
  w->dict    = dict;
}

void skim_feed_twins_sent(SkimFeedTwins *t, const char *call, double hz,
                          gint64 now_us, guint hearings, gboolean dict) {
  if (!t || t->fold_us <= 0 || !call || !call[0])
    return;
  Twin *w = twin_get(t, call);
  if (w->sent_us > 0 && now_us - w->sent_us > t->run_us) {
    w->sent_n = 0;                             /* a new run, a new count     */
  }
  w->sent_hz = hz;
  w->sent_us = now_us;
  w->sent_n  = MAX(w->sent_n, hearings);
  w->dict    = dict;
}

const char *skim_feed_twins_near(const SkimFeedTwins *t, const char *call,
                                 double hz, gint64 now_us, guint hearings,
                                 gboolean dict) {
  if (!t || t->near_us <= 0 || !call || !call[0])
    return NULL;
  /* a dictionary call read twice or more stands on its own unless a
   * look-alike was read much more often; read once it passed the readings
   * gate on the dictionary's word alone and yields like an unknown call */
  const gboolean firm = dict && hearings >= 2;
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const Twin *w = val;
    if (w->read_us <= 0 || now_us - w->read_us > t->near_us ||
        fabs(w->hz - hz) > t->radius_hz || strcmp(key, call) == 0)
      continue;
    const gboolean rival =
        firm ? w->read_n >= NEAR_DICT_FLOOR &&
                   w->read_n >= NEAR_DICT_RATIO * hearings
             : w->dict || w->read_n > MAX(hearings, 1u);
    if (rival && skim_callsign_edits(key, call) <= TWIN_EDITS)
      return key;
  }
  return NULL;
}

const char *skim_feed_twins_fold(const SkimFeedTwins *t, const char *call,
                                 double hz, gint64 now_us, guint hearings,
                                 gboolean dict) {
  if (!t || t->fold_us <= 0 || !call || !call[0])
    return NULL;
  const Twin *own = g_hash_table_lookup(t->by_call, call);
  if (own && own->sent_us > 0 && now_us - own->sent_us <= t->run_us)
    return NULL;                               /* its own run goes on        */
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const Twin *w = val;
    if (w->sent_us <= 0 || now_us - w->sent_us > t->fold_us ||
        fabs(w->sent_hz - hz) > t->radius_hz || strcmp(key, call) == 0)
      continue;
    if (skim_callsign_edits(key, call) > TWIN_EDITS &&
        !skim_callsign_is_edge(key, call))
      continue;
    /* the dictionary speaks only for a call heard at least as often: a cut
     * twin that happens to be a call too (SP2R of SP2RCL, SM5D of SM5DL)
     * must not silence the station keyed over and over */
    if ((w->dict && !dict && w->sent_n >= hearings) ||
        (w->dict == dict && w->sent_n > hearings))
      return key;
  }
  return NULL;
}

void skim_feed_twins_sweep(SkimFeedTwins *t, gint64 now_us) {
  if (!t)
    return;
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const Twin *w = val;
    const gboolean read_live = t->near_us > 0 && w->read_us > 0 &&
                               now_us - w->read_us <= t->near_us;
    const gboolean sent_live = t->fold_us > 0 && w->sent_us > 0 &&
                               now_us - w->sent_us <= t->run_us;
    if (!read_live && !sent_live) { g_hash_table_iter_remove(&it); }
  }
}

guint skim_feed_twins_size(const SkimFeedTwins *t) {
  return t ? g_hash_table_size(t->by_call) : 0;
}
