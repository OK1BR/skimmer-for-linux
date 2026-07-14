/* channelizer.c — polyphase filter bank. M0: skeleton (no DSP yet).
 * Real polyphase FFT / WDSP resampler wiring lands in M2 (docs/SCOPE.md).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "channelizer.h"

struct _SkimChannelizer {
  double in_rate;
  double chan_bw_hz;
  guint  count;
  /* M2: polyphase prototype filter, FFT plan, per-channel decimators. */
};

SkimChannelizer *skim_channelizer_new(double in_rate, double chan_bw_hz) {
  if (in_rate <= 0.0 || chan_bw_hz <= 0.0)
    return NULL;
  SkimChannelizer *ch = g_new0(SkimChannelizer, 1);
  ch->in_rate    = in_rate;
  ch->chan_bw_hz = chan_bw_hz;
  ch->count      = 0; /* M2: (guint)(in_rate / chan_bw_hz) */
  return ch;
}

void skim_channelizer_free(SkimChannelizer *ch) {
  g_free(ch);
}

guint skim_channelizer_count(const SkimChannelizer *ch) {
  return ch ? ch->count : 0;
}

void skim_channelizer_push(SkimChannelizer *ch, const float *iq, guint nframes) {
  (void)ch; (void)iq; (void)nframes;
  /* M2: run the filter bank, decimate, fan out complex channels. */
}
