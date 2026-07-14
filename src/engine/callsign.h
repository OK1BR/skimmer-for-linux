/* callsign.h — callsign extraction + validation (RBN-grade).
 *
 * Pulls candidate callsigns out of decoded text and scores their plausibility
 * (prefix/suffix regex against the ITU allocation + a known-call dictionary).
 * The RBN feed must never emit unvalidated calls, so this gates spotting.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_CALLSIGN_H
#define SKIMMER_CALLSIGN_H

#include <glib.h>

G_BEGIN_DECLS

/* TRUE if s is a structurally valid amateur callsign. M4. */
gboolean skim_callsign_is_valid(const char *s);

/* Extract the best callsign candidate from decoded text into out (size cap),
 * returning a confidence 0..1, or 0.0 if none. M4. */
double   skim_callsign_extract(const char *text, char *out, gsize out_size);

G_END_DECLS

#endif /* SKIMMER_CALLSIGN_H */
