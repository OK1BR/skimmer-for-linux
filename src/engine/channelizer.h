/* channelizer.h — wideband IQ → N narrow COMPLEX baseband channels.
 *
 * A 2×-oversampled polyphase filter bank splits the wideband TCI IQ stream
 * into M = in_rate/chan_bw_hz uniformly spaced, decimated complex channels
 * (output rate = 2·chan_bw_hz). Complex (phase-preserving) from day one
 * because RTTY/PSK backends need phase, not just magnitude.
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

/* Extended geometry: passband_hz = one-sided prototype cutoff (0 = the
 * default chan_bw/2), taps_per_branch = prototype length in branches
 * (0 = the default 8). A mode whose signal is WIDER than its channel
 * spacing (RTTY: ±85 Hz tones + keying sidebands) opens the passband up
 * to the output Nyquist (chan_bw_hz) so a station anywhere between two
 * channel centres still lands with BOTH tones in one channel; the longer
 * prototype buys back the alias rejection the wide cutoff spends. */
SkimChannelizer *skim_channelizer_new_ex(double in_rate, double chan_bw_hz,
                                         double passband_hz,
                                         guint taps_per_branch);
void             skim_channelizer_free(SkimChannelizer *ch);

/* Number of output channels (= M). Channel 0 is the stream centre; channels
 * 1..M/2 sit above it, M/2+1..M-1 below (negative offsets). */
guint  skim_channelizer_count(const SkimChannelizer *ch);

/* Per-channel output sample rate (= 2·chan_bw_hz). */
double skim_channelizer_out_rate(const SkimChannelizer *ch);

/* Equivalent noise bandwidth of one channel, in Hz: ∫|H(f)|² df of the
 * prototype = in_rate · Σh² for the unity-DC-gain taps. White input noise
 * of density N0 leaves N0·ENBW of power in a channel, so a decoder that
 * measures the noise of its whole input can state SNR in a reference
 * bandwidth (500 Hz, the dB of RBN spots — gh#27). The CW bank's default
 * geometry (±62.5 Hz cutoff, BH4, 8 taps per branch) integrates to about
 * 0.85 of the channel spacing; the number here is from the taps, not a
 * constant. */
double skim_channelizer_enbw_hz(const SkimChannelizer *ch);

/* The prototype's power response at off_hz from a channel's centre, in dB
 * (≤ 0; 0 at the centre, −6 at the default ±chan_bw/2 cutoff). A tone off
 * the centre reaches the decoder this much weaker while the band's noise
 * density does not — the pipeline adds it back to a spot's SNR (gh#27).
 * Tabulated at 1 Hz from the taps on first use. */
double skim_channelizer_response_db(const SkimChannelizer *ch, double off_hz);

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
