/* spot_out.h — spot output: TCI SPOT feed + RBN telnet feed.
 *
 * Takes validated (callsign, frequency, mode) spots and emits them back to the
 * sdr-for-linux panadapter over TCI (M5) and, as a goal, to the Reverse Beacon
 * Network over telnet (M6). Rate-limited and de-duplicated.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_SPOT_OUT_H
#define SKIMMER_SPOT_OUT_H

#include <glib.h>
#include "tci_client.h"

G_BEGIN_DECLS

typedef struct _SkimSpotOut SkimSpotOut;

/* Spot sink. tci may be NULL (no radio feed); rbn_host NULL disables RBN. */
SkimSpotOut *skim_spot_out_new(SkimTciClient *tci, const char *rbn_host, guint16 rbn_port);
void         skim_spot_out_free(SkimSpotOut *s);

/* Emit one validated spot (deduped + rate-limited downstream). M5/M6. */
void  skim_spot_out_emit(SkimSpotOut *s, const char *call, const char *mode,
                        double freq_hz, double snr_db);

G_END_DECLS

#endif /* SKIMMER_SPOT_OUT_H */
