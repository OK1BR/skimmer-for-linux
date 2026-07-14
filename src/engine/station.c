/* station.c — station tracker. M0: skeleton store (no dedup/aging yet).
 * Dedup, aging and UI/spot wiring land in M5 (docs/SCOPE.md).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "station.h"

struct _SkimStationTable {
  GHashTable *by_key; /* key: "call@freqbucket" → SkimStation* */
};

SkimStationTable *skim_station_table_new(void) {
  SkimStationTable *t = g_new0(SkimStationTable, 1);
  t->by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  return t;
}

void skim_station_table_free(SkimStationTable *t) {
  if (!t)
    return;
  g_hash_table_destroy(t->by_key);
  g_free(t);
}

void skim_station_table_report(SkimStationTable *t, const SkimStation *st) {
  (void)t; (void)st;
  /* M5: bucket by callsign+frequency, insert/update, age out stale entries. */
}

guint skim_station_table_size(const SkimStationTable *t) {
  return t ? g_hash_table_size(t->by_key) : 0;
}
