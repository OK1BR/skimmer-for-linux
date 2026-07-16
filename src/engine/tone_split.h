/* tone_split.h — per-channel carrier splitter (multi-signal channels).
 *
 * Two CW stations closer than ~60 Hz share ONE channelizer channel; the
 * |IQ| envelope then carries the SUM of both keyings (a beat at Δf) and the
 * decoder mutates both calls (live-caught 2026-07-15, the 14036 slot). The
 * splitter watches each channel's Welch-averaged spectrum; when it resolves
 * two or three distinct carriers ≥ ~20 Hz apart it opens one SLOT per
 * carrier — a phase-continuous NCO mix to ~0 Hz plus a narrow FIR whose
 * cutoff follows the carrier spacing — and the owner runs a separate
 * decoder per slot. Carriers closer than the keying bandwidth cannot be
 * separated linearly: the affected slot is flagged CONTESTED so the owner
 * keeps its beat-garbled text away from the callsign candidates.
 *
 * A single-carrier channel stays on a verbatim passthrough (slot 0 == the
 * channel, sample-exact legacy behaviour) — until a second carrier shows,
 * the splitter spends CPU only on the small detection FFT.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_TONE_SPLIT_H
#define SKIMMER_TONE_SPLIT_H

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_TONE_SPLIT_MAX 3               /* slots per channel */

typedef struct _SkimToneSplit SkimToneSplit;

SkimToneSplit *skim_tone_split_new(double sample_rate);
void           skim_tone_split_free(SkimToneSplit *ts);

/* Feed one block of channel IQ (interleaved I/Q floats). Runs detection and
 * routes the samples into the slot rings. */
void skim_tone_split_push(SkimToneSplit *ts, const float *iq, guint nframes);

/* Active slots, ≥1. 1 = passthrough (slot 0 is the channel verbatim). */
guint skim_tone_split_slots(const SkimToneSplit *ts);

/* Slot's mixer offset inside the channel (0 in passthrough). Add the
 * decoder's freq_offset_hz to it for the tone's true in-channel offset. */
double skim_tone_split_slot_hz(const SkimToneSplit *ts, guint slot);

/* Bumped every time the slot's identity changes (split engages/collapses,
 * a slot respawns onto a new carrier, a compaction moves a slot to this
 * index) — the owner must reset the decoder and extractor behind the slot
 * whenever the value moves. Never 0 for a live slot. */
guint skim_tone_split_slot_gen(const SkimToneSplit *ts, guint slot);

/* TRUE while the slot's band holds a second unresolvable carrier — its
 * decode text is beat-garbled; keep it out of the callsign candidates. */
gboolean skim_tone_split_slot_contested(const SkimToneSplit *ts, guint slot);

/* Drain up to max_frames of the slot's output (interleaved I/Q). */
guint skim_tone_split_read(SkimToneSplit *ts, guint slot, float *iq,
                           guint max_frames);

G_END_DECLS

#endif /* SKIMMER_TONE_SPLIT_H */
