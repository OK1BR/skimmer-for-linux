/* station.h — per-frequency station tracker.
 *
 * Aggregates decode events into stations: a callsign heard at a frequency, with
 * mode, speed, SNR and first/last-heard times. Feeds the UI list, the spot
 * feeder and the RBN feed. Mode-agnostic.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_STATION_H
#define SKIMMER_STATION_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char    call[16];
  char    mode[8];      /* "CW", "RTTY", "PSK31" ... */
  double  freq_hz;
  double  speed;        /* WPM or baud */
  double  snr_db;
  gint64  first_heard;  /* g_get_monotonic_time() */
  gint64  last_heard;
} SkimStation;

typedef struct _SkimStationTable SkimStationTable;

SkimStationTable *skim_station_table_new(void);
void              skim_station_table_free(SkimStationTable *t);

/* Insert or update a station (dedup by callsign+freq bucket). M5. */
void   skim_station_table_report(SkimStationTable *t, const SkimStation *st);

/* Number of currently tracked stations. */
guint  skim_station_table_size(const SkimStationTable *t);

G_END_DECLS

#endif /* SKIMMER_STATION_H */
