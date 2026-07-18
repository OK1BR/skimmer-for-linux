/* station.c — per-frequency station tracker (M5, docs/SCOPE.md).
 *
 * Storage: call → GSList of records. ONE record per call per band — a
 * transmitter is only ever in one place: a report within
 * SKIM_STATION_SAMECALL_HZ merges (the stronger report's frequency and SNR
 * win — that folds adjacent-channel ghosts of a strong station back into
 * one spot on the peak of the most readable decode; live-tuned 2026-07-15:
 * 300 Hz was too narrow, one call showed up 5× across the splatter), and a
 * confident report beyond it is a QSY that MOVES the record (an S&P station
 * hops around the band — its marker follows; live-caught 2026-07-15:
 * EA2BTN listed on 14044 and 14050 at once).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "station.h"

#include <math.h>
#include <string.h>

struct _SkimStationTable {
  GHashTable *by_call;        /* call (owned str) → GSList of SkimStation*   */
  guint       count;
  SkimStationGoneFunc gone_cb;
  gpointer            gone_user;
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

void skim_station_table_set_gone_cb(SkimStationTable *t, SkimStationGoneFunc cb,
                                    gpointer user_data) {
  t->gone_cb   = cb;
  t->gone_user = user_data;
}

/* TRUE when `shorter` reads as a clipped decode of `longer` — a leading or
 * trailing fragment ("M0K" of "M0KKB", "31RM" of "Z31RM"). */
static gboolean call_is_clip(const char *shorter, const char *longer) {
  gsize ls = strlen(shorter), ll = strlen(longer);
  if (ls < 3 || ls >= ll)
    return FALSE;
  return strncmp(longer, shorter, ls) == 0 ||
         strcmp(longer + (ll - ls), shorter) == 0;
}

/* A confident report of a DIFFERENT call on the same frequency evicts records
 * that have been silent past the takeover age — someone else settled there;
 * without this the old call stayed listed (and kept being re-reported by its
 * channel's extractor) for the whole 10-minute TTL (Richard, 2026-07-15).
 * A record whose call is a CLIP of the incoming one goes immediately — it
 * was never a station, just a torn decode of this very signal. */
static void takeover_sweep(SkimStationTable *t, const SkimStation *st) {
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    if (strcmp(key, st->call) == 0)
      continue;
    GSList *list = val, *l = list;
    gboolean changed = FALSE;
    while (l) {
      SkimStation *e = l->data;
      GSList *next = l->next;
      if (fabs(e->freq_hz - st->freq_hz) <= SKIM_STATION_MERGE_HZ &&
          (st->last_heard - e->last_heard > SKIM_STATION_TAKEOVER_US ||
           call_is_clip(e->call, st->call))) {
        if (t->gone_cb) { t->gone_cb(e, t->gone_user); }
        list = g_slist_remove(list, e);
        g_free(e);
        t->count--;
        changed = TRUE;
      }
      l = next;
    }
    if (changed) {
      if (!list) {
        g_hash_table_iter_remove(&it);
      } else {
        g_hash_table_iter_replace(&it, list);
      }
    }
  }
}

/* The reverse clip direction: a report of "M0K" while a fresh "M0KKB" sits on
 * the frequency is a torn decode of that station — fold it into the longer
 * record instead of creating a phantom. Returns the record it fed, or NULL. */
static SkimStation *clip_fold(SkimStationTable *t, const SkimStation *st) {
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    if (!call_is_clip(st->call, key))
      continue;
    for (GSList *l = val; l; l = l->next) {
      SkimStation *e = l->data;
      if (fabs(e->freq_hz - st->freq_hz) <= SKIM_STATION_MERGE_HZ &&
          st->last_heard - e->last_heard <= SKIM_STATION_TAKEOVER_US) {
        e->last_heard = st->last_heard;
        e->reports++;
        return e;
      }
    }
  }
  return NULL;
}

const SkimStation *skim_station_table_report(SkimStationTable *t,
                                             const SkimStation *st) {
  SkimStation *folded = clip_fold(t, st);
  if (folded) { return folded; }
  takeover_sweep(t, st);
  GSList *list = g_hash_table_lookup(t->by_call, st->call);
  for (GSList *l = list; l; l = l->next) {
    SkimStation *e = l->data;
    if (fabs(e->freq_hz - st->freq_hz) <= SKIM_STATION_SAMECALL_HZ) {
      /* Merge: the stronger report positions the station — as an EMA, not a
       * replacement: single estimates scatter ±10-20 Hz around the carrier,
       * averaging converges onto it (stations sit on round frequencies;
       * 14048.99 for a 14049.00 station bugged Richard, 2026-07-15). */
      if (st->snr_db >= e->snr_db) {
        e->freq_hz += 0.3 * (st->freq_hz - e->freq_hz);
        e->snr_db  = st->snr_db;
        e->speed   = st->speed;
      }
      e->score      = MAX(e->score, st->score);
      if (st->cq) { e->cq = TRUE; }            /* calling here — owns it     */
      e->last_heard = st->last_heard;
      e->reports++;
      return e;
    }
  }
  if (list) {
    /* Same call far away — the transmitter MOVED (S&P hop / QSY). A
     * confident report relocates the record; a one-off garble does not. */
    SkimStation *e = list->data;
    if (st->score >= SKIM_STATION_QSY_SCORE) {
      e->freq_hz    = st->freq_hz;
      e->snr_db     = st->snr_db;
      e->speed      = st->speed;
      e->score      = MAX(e->score, st->score);
      e->cq         = st->cq;                  /* new place, new context     */
      e->last_heard = st->last_heard;
      e->reports++;
    }
    return e;
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

guint skim_station_table_prune(SkimStationTable *t, gint64 now_us,
                               gint64 max_age_us) {
  const gint64 cutoff = now_us - max_age_us;
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
        if (t->gone_cb) { t->gone_cb(e, t->gone_user); }
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
