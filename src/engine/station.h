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

/* A record's dB follows its reports as an EMA with this gain (gh#27): the
 * reports come several a second while a station keys, so this is a few
 * seconds of keying — the station's level, not the peak of its QSB. */
#define SKIM_STATION_SNR_GAIN 0.2

typedef struct {
  char    call[16];
  char    mode[8];      /* "CW", "RTTY", "PSK31" ... */
  double  freq_hz;
  double  speed;        /* WPM or baud */
  double  snr_db;       /* SNR in 500 Hz for the CW backends (decode.h): an
                         * EMA of the record's reports (gh#27)             */
  double  snr_peak;     /* the strongest report's dB — positions the
                         * station on its peak against ghost reports; the
                         * table keeps it, a report leaves it 0           */
  double  score;        /* best callsign plausibility seen */
  gboolean cq;          /* heard CALLING (CQ/TEST/QRZ context) — owns the
                         * frequency; S&P answers report cq=FALSE */
  gboolean addressed;   /* heard being ANSWERED — a report right after the
                         * call, or DE after the call TU blessed — with no
                         * clean calling window of its own (callsign.h,
                         * gh#31): the other side of a QSO, not calling.
                         * Once set it clears cq for the record's life,
                         * whatever a ghost channel that never read the
                         * report keeps saying */
  gint64  first_heard;  /* g_get_monotonic_time() */
  gint64  last_heard;
  guint   reports;      /* decode events folded into this record */
  guint   hearings;     /* the most copies of the call ONE extractor counted
                         * — keyed and read, not reported (callsign.h); the
                         * RBN feed's readings gate (gh#24), and what
                         * decides between a call and a longer reading of it
                         * the dictionary knows nothing about (gh#34) */
  gint64  last_read;    /* when a copy of the call was last actually READ
                         * (the extractor's clock) — last_heard moves with
                         * every report, and a quiet channel's noise
                         * re-reports a candidate long gone; the RBN
                         * feed's freshness gate (gh#26) */
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

/* A call MASTER.SCP does not know, read once, that is within this many
 * edits of a better attested call on its frequency is a misread of that
 * station, not a station (gh#21, station.c) — while that call has a record
 * there, and for this long after the record left: a station in a long QSO
 * gives its call every few minutes, the record lives two. The radius is
 * SKIM_STATION_MERGE_HZ. Two edits, 300 Hz and an hour are what the RBN
 * feed's near-call rule looks within (feed_twins.h). The same memo serves
 * the clip pair that comes after the fold window (station.c). */
#define SKIM_STATION_TWIN_EDITS   2
#define SKIM_STATION_TWIN_MEMO_US ((gint64)3600 * G_USEC_PER_SEC)

/* Insert or update. Ghost rule: an existing record with the same call within
 * SKIM_STATION_SAMECALL_HZ absorbs the report; the frequency/SNR of the
 * STRONGER report wins. Returns the merged record (owned by the table), or
 * NULL when the report was dropped as a twin of a better attested call or
 * as a torn or glued reading of one (gh#21) — then nothing in the table
 * changed. */
const SkimStation *skim_station_table_report(SkimStationTable *t,
                                             const SkimStation *st);

guint  skim_station_table_size(const SkimStationTable *t);

/* Find a station by call (any frequency); NULL when unknown. */
const SkimStation *skim_station_table_lookup(const SkimStationTable *t,
                                             const char *call);

typedef void (*SkimStationFunc)(const SkimStation *st, gpointer user_data);
void   skim_station_table_foreach(SkimStationTable *t, SkimStationFunc f,
                                  gpointer user_data);

/* Drop stations not heard for max_age_us as of now_us (the caller's clock —
 * monotonic live, stream time in an offline replay); returns how many were
 * removed. */
guint  skim_station_table_prune(SkimStationTable *t, gint64 now_us,
                                gint64 max_age_us);

G_END_DECLS

#endif /* SKIMMER_STATION_H */
