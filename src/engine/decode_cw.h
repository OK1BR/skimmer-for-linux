/* decode_cw.h — CW (Morse) decode backend. Phase 1.
 *
 * Envelope detection → adaptive threshold → dot/dash element timing → adaptive
 * WPM → Morse table, with tolerance for a ragged fist (HMM/Bayes planned).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_DECODE_CW_H
#define SKIMMER_DECODE_CW_H

#include "decode.h"

G_BEGIN_DECLS

/* Returns the CW backend singleton (static vtable). */
const SkimDecodeBackend *skim_decode_cw(void);

G_END_DECLS

#endif /* SKIMMER_DECODE_CW_H */
