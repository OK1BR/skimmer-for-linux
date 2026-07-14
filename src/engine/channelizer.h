/* channelizer.h — wideband IQ → N narrow COMPLEX baseband channels.
 *
 * A polyphase filter bank splits the wideband TCI IQ stream into many narrow,
 * decimated complex channels. Complex (phase-preserving) from day one because
 * RTTY/PSK backends need phase, not just magnitude (docs/SCOPE.md). Built on the
 * vendored WDSP FFT + resampler in M2.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_CHANNELIZER_H
#define SKIMMER_CHANNELIZER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimChannelizer SkimChannelizer;

/* Create a channelizer for input at in_rate Hz, producing channels of about
 * chan_bw_hz each. NULL on failure. */
SkimChannelizer *skim_channelizer_new(double in_rate, double chan_bw_hz);
void             skim_channelizer_free(SkimChannelizer *ch);

/* Number of output channels. */
guint  skim_channelizer_count(const SkimChannelizer *ch);

/* Push one block of interleaved input IQ (nframes I/Q pairs). Decoded output is
 * pulled per channel by the engine; wiring lands in M2. */
void   skim_channelizer_push(SkimChannelizer *ch, const float *iq, guint nframes);

G_END_DECLS

#endif /* SKIMMER_CHANNELIZER_H */
