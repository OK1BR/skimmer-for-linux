/* decode_rtty.h — RTTY (Baudot/ITA2 FSK) decode backend.
 *
 * 45.45 Bd, 170 Hz shift: periodogram pair finder → per-tone matched filters
 * with ATC (selective-fade proof) → start-bit-anchored UART with automatic
 * polarity → ITA2 with unshift-on-space. Layered squelch: FSK pair above the
 * noise floor AND anti-correlated tone envelopes AND a sustained valid-frame
 * rate — a lone carrier, keyed CW or two unrelated stations 170 Hz apart
 * must never emit a character (RBN-grade rule, same as CW).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_DECODE_RTTY_H
#define SKIMMER_DECODE_RTTY_H

#include "decode.h"

G_BEGIN_DECLS

/* Returns the RTTY backend singleton (static vtable). */
const SkimDecodeBackend *skim_decode_rtty(void);

G_END_DECLS

#endif /* SKIMMER_DECODE_RTTY_H */
