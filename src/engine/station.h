/* station.h — per-frequency station tracker.
 *
 * Aggregates decode events into stations: a callsign heard at a frequency,
 * with mode, speed, SNR and first/last-heard times. Merges GHOSTS — the same
 * call decoded on neighbouring channels (splatter of a strong signal) folds
 * into the strongest report. Feeds the UI list, the spot feeder and the RBN
 * feed. Mode-agnostic. Engine-thread only.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_STATION_H
#define SKIMMER_STATION_H

#include <glib.h>

G_BEGIN_DECLS

/* Reports of one call within this many Hz are the same station. */
#define SKIM_STATION_MERGE_HZ 300.0

typedef struct {
  char    call[16];
  char    mode[8];      /* "CW", "RTTY", "PSK31" ... */
  double  freq_hz;
  double  speed;        /* WPM or baud */
  double  snr_db;
  double  score;        /* best callsign plausibility seen */
  gint64  first_heard;  /* g_get_monotonic_time() */
  gint64  last_heard;
  guint   reports;      /* decode events folded into this record */
} SkimStation;

typedef struct _SkimStationTable SkimStationTable;

SkimStationTable *skim_station_table_new(void);
void              skim_station_table_free(SkimStationTable *t);

/* Insert or update. Ghost rule: an existing record with the same call within
 * SKIM_STATION_MERGE_HZ absorbs the report; the frequency/SNR of the STRONGER
 * report wins. Returns the merged record (owned by the table). */
const SkimStation *skim_station_table_report(SkimStationTable *t,
                                             const SkimStation *st);

guint  skim_station_table_size(const SkimStationTable *t);

/* Find a station by call (any frequency); NULL when unknown. */
const SkimStation *skim_station_table_lookup(const SkimStationTable *t,
                                             const char *call);

typedef void (*SkimStationFunc)(const SkimStation *st, gpointer user_data);
void   skim_station_table_foreach(SkimStationTable *t, SkimStationFunc f,
                                  gpointer user_data);

/* Drop stations not heard for max_age_us; returns how many were removed. */
guint  skim_station_table_prune(SkimStationTable *t, gint64 max_age_us);

G_END_DECLS

#endif /* SKIMMER_STATION_H */
