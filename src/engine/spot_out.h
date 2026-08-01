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
#include "dup_query.h"
#include "tci_client.h"

G_BEGIN_DECLS

/* The colour of our spots on the radio panadapter — sent as the ARGB field
 * of every TCI SPOT. The app's decode-pane callsign highlight uses the SAME
 * constant, so a highlighted call and its panadapter label always match
 * (Richard, 2026-08-01). Calls the logbook already has (DUP in the active
 * contest, or B4 = worked some earlier time) dim to gray — the operator
 * skips them at a glance; the 180 s re-announce recolours a label whose
 * verdict changed (dedup by callsign on the radio side). */
#define SKIM_SPOT_ARGB     0xFF30C060u
#define SKIM_SPOT_ARGB_DUP 0xFF808080u

/* One mapping from a logbook verdict to the spot colour — shared by the TCI
 * spot path and the app's pane highlight so they can never disagree. */
static inline guint32 skim_spot_argb_for_dup(SkimDupVerdict v) {
  return (v == SKIM_DUP_DUP || v == SKIM_DUP_B4) ? SKIM_SPOT_ARGB_DUP
                                                 : SKIM_SPOT_ARGB;
}

typedef struct _SkimSpotOut SkimSpotOut;

/* Spot sink policy instance. tci may be NULL (no radio feed). */
SkimSpotOut *skim_spot_out_new(SkimTciClient *tci);
void         skim_spot_out_free(SkimSpotOut *s);

/* Colour spots by the logbook's dup verdict (borrowed; NULL = default
 * colour for everything). Emit asks with a ~5 ms budget — localhost answers
 * in µs, an absent logbook costs one poll and stays UNKNOWN. */
void  skim_spot_out_set_dup_query(SkimSpotOut *s, SkimDupQuery *q);

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

/* Repaint a live label in the verdict's colour RIGHT NOW (the logbook says
 * "just logged him" — the operator must not wait out the re-announce).
 * A resend of the last emission with only the ARGB changed; dedup memo and
 * re-announce schedule stay untouched. No-op for calls without a fresh
 * label (≤10 min, the radio's own spot TTL). */
void skim_spot_out_recolour(SkimSpotOut *s, const char *call,
                            SkimDupVerdict verdict);

/* The station is gone (TTL / takeover): pull its label off the panadapter
 * (SPOT_DELETE) and forget its dedup memo, so a comeback re-spots at once. */
void skim_spot_out_delete(SkimSpotOut *s, const char *call);

/* Spots actually emitted since creation. */
guint64 skim_spot_out_count(const SkimSpotOut *s);

G_END_DECLS

#endif /* SKIMMER_SPOT_OUT_H */
