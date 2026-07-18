/* spot_out.h — spot output policy: dedup, rate limit, sinks.
 *
 * Takes validated (callsign, frequency, mode) spots and emits them to its
 * sinks: the sdr-for-linux panadapter over TCI (M5) and/or a callback (the
 * RBN telnet feed of M6, gates). De-duplicated (per call: re-spot only
 * after an interval or a real QSY) and globally rate-limited. Only validated
 * calls may ever reach this module — callsign.c gates it (M4 gates M6).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_SPOT_OUT_H
#define SKIMMER_SPOT_OUT_H

#include <glib.h>
#include "tci_client.h"

G_BEGIN_DECLS

typedef struct _SkimSpotOut SkimSpotOut;

/* Spot sink policy instance. tci may be NULL (no radio feed). */
SkimSpotOut *skim_spot_out_new(SkimTciClient *tci);
void         skim_spot_out_free(SkimSpotOut *s);

/* Dedup/rate policy (defaults: re-spot 180 s, QSY 30 Hz, ≤5 spots/s). */
void  skim_spot_out_set_policy(SkimSpotOut *s, guint respot_s,
                               double qsy_hz, guint max_per_s);

/* Clock source for the dedup/rate windows. Default: monotonic wall time.
 * An offline replay injects STREAM time here — policy windows must depend
 * on the recording, not on how fast the box chews it. Resets the token
 * bucket to the new clock's now. */
typedef gint64 (*SkimNowFn)(gpointer user);
void  skim_spot_out_set_clock(SkimSpotOut *s, SkimNowFn now_fn, gpointer user);

/* Snap OUTGOING spot frequencies to a grid (Hz); 0/1 = exact (default).
 * SDC-style "spot accuracy" — dedup/QSY policy always runs on the raw
 * measured frequency, only what leaves gets rounded. Thread-safe. */
void  skim_spot_out_set_round_hz(SkimSpotOut *s, guint hz);

/* Optional extra sink (M6 RBN feed, gates): called for every emitted spot.
 * speed is WPM for CW, baud for the digital modes. */
typedef void (*SkimSpotSink)(const char *call, const char *mode,
                             double freq_hz, double snr_db, double speed,
                             gpointer user);
void  skim_spot_out_set_sink(SkimSpotOut *s, SkimSpotSink sink, gpointer user);

/* Offer one validated spot; the policy decides whether it goes out.
 * Returns TRUE when actually emitted. */
gboolean skim_spot_out_emit(SkimSpotOut *s, const char *call, const char *mode,
                            double freq_hz, double snr_db, double speed);

/* The station is gone (TTL / takeover): pull its label off the panadapter
 * (SPOT_DELETE) and forget its dedup memo, so a comeback re-spots at once. */
void skim_spot_out_delete(SkimSpotOut *s, const char *call);

/* Spots actually emitted since creation. */
guint64 skim_spot_out_count(const SkimSpotOut *s);

G_END_DECLS

#endif /* SKIMMER_SPOT_OUT_H */
