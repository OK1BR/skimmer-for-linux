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

/* Traffic within this many Hz belongs to one frequency slot (UI text
 * routing, tuned-pane matching). */
#define SKIM_STATION_MERGE_HZ 300.0

/* Reports of one call within this many Hz are the SAME station (one
 * transmitter): a strong signal ghost-decodes several channels away, so the
 * same-call fold must reach far beyond one channel. The strongest (most
 * readable) report positions the station on its peak. */
#define SKIM_STATION_SAMECALL_HZ 2500.0

/* One call = ONE record per band: a transmitter is only ever in one place.
 * A report beyond SAMECALL_HZ is a QSY and MOVES the record — but only at
 * this score or better (repeated / dictionary-backed), so a one-off garble
 * of somebody else's signal cannot teleport a station's marker.
 * (Live-caught 2026-07-15: EA2BTN S&P listed at 14044 AND 14050.) */
#define SKIM_STATION_QSY_SCORE 0.85

typedef struct {
  char    call[16];
  char    mode[8];      /* "CW", "RTTY", "PSK31" ... */
  double  freq_hz;
  double  speed;        /* WPM or baud */
  double  snr_db;
  double  score;        /* best callsign plausibility seen */
  gboolean cq;          /* heard CALLING (CQ/TEST/QRZ context) — owns the
                         * frequency; S&P answers report cq=FALSE */
  gint64  first_heard;  /* g_get_monotonic_time() */
  gint64  last_heard;
  guint   reports;      /* decode events folded into this record */
} SkimStation;

typedef struct _SkimStationTable SkimStationTable;

SkimStationTable *skim_station_table_new(void);
void              skim_station_table_free(SkimStationTable *t);

/* A record left the table (TTL prune, or its frequency was taken over by a
 * different call). st is valid only for the duration of the callback. */
typedef void (*SkimStationGoneFunc)(const SkimStation *st, gpointer user_data);
void skim_station_table_set_gone_cb(SkimStationTable *t, SkimStationGoneFunc cb,
                                    gpointer user_data);

/* A different call reported on the same frequency evicts a record that has
 * been silent this long — the frequency changed hands. A fresher record
 * survives (co-channel pileup is real). */
#define SKIM_STATION_TAKEOVER_US (30 * G_USEC_PER_SEC)

/* Insert or update. Ghost rule: an existing record with the same call within
 * SKIM_STATION_SAMECALL_HZ absorbs the report; the frequency/SNR of the
 * STRONGER report wins. Returns the merged record (owned by the table). */
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
