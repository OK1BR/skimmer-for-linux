/* station.c — per-frequency station tracker (M5, docs/SCOPE.md).
 *
 * Storage: call → GSList of records (a call on two truly different
 * frequencies stays two stations). A report merges into the first record of
 * the same call within SKIM_STATION_SAMECALL_HZ; the stronger report's
 * frequency and SNR win — that is what folds the adjacent-channel ghosts of
 * a strong station (its splatter decodes several channels away with a much
 * weaker SNR) back into one spot at the right frequency, on the peak of the
 * most readable decode (live-tuned 2026-07-15: 300 Hz was too narrow, one
 * call showed up 5× across the splatter).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "station.h"

#include <math.h>
#include <string.h>

struct _SkimStationTable {
  GHashTable *by_call;        /* call (owned str) → GSList of SkimStation*   */
  guint       count;
};

SkimStationTable *skim_station_table_new(void) {
  SkimStationTable *t = g_new0(SkimStationTable, 1);
  t->by_call = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  return t;
}

static void free_list(gpointer list) {
  g_slist_free_full(list, g_free);
}

void skim_station_table_free(SkimStationTable *t) {
  if (!t)
    return;
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) { free_list(val); }
  g_hash_table_destroy(t->by_call);
  g_free(t);
}

const SkimStation *skim_station_table_report(SkimStationTable *t,
                                             const SkimStation *st) {
  GSList *list = g_hash_table_lookup(t->by_call, st->call);
  for (GSList *l = list; l; l = l->next) {
    SkimStation *e = l->data;
    if (fabs(e->freq_hz - st->freq_hz) <= SKIM_STATION_SAMECALL_HZ) {
      /* Merge: the stronger report positions the station. */
      if (st->snr_db >= e->snr_db) {
        e->freq_hz = st->freq_hz;
        e->snr_db  = st->snr_db;
        e->speed   = st->speed;
      }
      e->score      = MAX(e->score, st->score);
      e->last_heard = st->last_heard;
      e->reports++;
      return e;
    }
  }
  SkimStation *e = g_memdup2(st, sizeof(*st));
  e->reports = 1;
  if (list) {
    list = g_slist_append(list, e);           /* head unchanged, no reinsert */
  } else {
    g_hash_table_insert(t->by_call, g_strdup(st->call),
                        g_slist_append(NULL, e));
  }
  t->count++;
  return e;
}

guint skim_station_table_size(const SkimStationTable *t) {
  return t ? t->count : 0;
}

const SkimStation *skim_station_table_lookup(const SkimStationTable *t,
                                             const char *call) {
  GSList *list = g_hash_table_lookup(t->by_call, call);
  return list ? list->data : NULL;
}

void skim_station_table_foreach(SkimStationTable *t, SkimStationFunc f,
                                gpointer user_data) {
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    for (GSList *l = val; l; l = l->next) { f(l->data, user_data); }
  }
}

guint skim_station_table_prune(SkimStationTable *t, gint64 max_age_us) {
  const gint64 cutoff = g_get_monotonic_time() - max_age_us;
  guint removed = 0;
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    GSList *list = val, *l = list;
    while (l) {
      SkimStation *e = l->data;
      GSList *next = l->next;
      if (e->last_heard < cutoff) {
        list = g_slist_remove(list, e);
        g_free(e);
        removed++;
        t->count--;
      }
      l = next;
    }
    if (!list) {
      g_hash_table_iter_remove(&it);
    } else {
      g_hash_table_iter_replace(&it, list);
    }
  }
  return removed;
}
