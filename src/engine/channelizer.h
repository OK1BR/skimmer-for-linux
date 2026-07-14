/* channelizer.h — wideband IQ → N narrow COMPLEX baseband channels.
 *
 * A 2×-oversampled polyphase filter bank splits the wideband TCI IQ stream
 * into M = in_rate/chan_bw_hz uniformly spaced, decimated complex channels
 * (output rate = 2·chan_bw_hz). Complex (phase-preserving) from day one
 * because RTTY/PSK backends need phase, not just magnitude (docs/SCOPE.md).
 * Prototype filter by WDSP's fir_bandpass; the M-point FFT is fftw3f.
 *
 * Threading contract: push() and read() are single-threaded (the engine
 * pipeline thread). No internal locking.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_CHANNELIZER_H
#define SKIMMER_CHANNELIZER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimChannelizer SkimChannelizer;

/* Create a channelizer for input at in_rate Hz with chan_bw_hz channel
 * spacing. in_rate/chan_bw_hz must be an even integer (the hop is M/2).
 * NULL on invalid geometry. */
SkimChannelizer *skim_channelizer_new(double in_rate, double chan_bw_hz);
void             skim_channelizer_free(SkimChannelizer *ch);

/* Number of output channels (= M). Channel 0 is the stream centre; channels
 * 1..M/2 sit above it, M/2+1..M-1 below (negative offsets). */
guint  skim_channelizer_count(const SkimChannelizer *ch);

/* Per-channel output sample rate (= 2·chan_bw_hz). */
double skim_channelizer_out_rate(const SkimChannelizer *ch);

/* Signed offset of channel chan's centre from the stream centre, in Hz. */
double skim_channelizer_offset_hz(const SkimChannelizer *ch, guint chan);

/* Push one block of interleaved input IQ (nframes I/Q pairs, true
 * orientation as delivered by the TCI client). */
void   skim_channelizer_push(SkimChannelizer *ch, const float *iq, guint nframes);

/* Drain up to max_frames of channel chan's complex baseband into iq
 * (interleaved I/Q). Returns frames copied. Each channel buffers ~8 s;
 * unread output beyond that overwrites oldest (see _dropped). */
guint  skim_channelizer_read(SkimChannelizer *ch, guint chan, float *iq,
                             guint max_frames);

/* Total frames lost to ring overwrites since creation (all channels). */
guint64 skim_channelizer_dropped(const SkimChannelizer *ch);

G_END_DECLS

#endif /* SKIMMER_CHANNELIZER_H */
