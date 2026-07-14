/* channelizer.c — 2×-oversampled polyphase filter bank (M2, docs/SCOPE.md).
 *
 * Geometry: M = in_rate/chan_bw channels, prototype lowpass of K·M taps
 * (WDSP fir_bandpass, Blackman-Harris 4-term, cutoff ±chan_bw/2, DC gain
 * normalised to 1), hop H = M/2 → output rate 2·chan_bw per channel.
 *
 * Per hop: the newest K·M input frames are folded branch-wise,
 *   acc[m] = Σ_k hist[P−1−(kM+m)] · h[kM+m],      m = 0..M−1,
 * then an unnormalised BACKWARD M-point FFT (fftw3f) evaluates all channels
 * at once: channel c ⇔ +c·chan_bw (c ≤ M/2), c−M below centre — matching the
 * true-orientation IQ the TCI client delivers. A centred unit tone comes out
 * with unit amplitude (Σh = 1 lands whole in its bin).
 *
 * Oversampling phase fix: hopping by H = M/2 rotates channel c by e^{jπc}
 * between hops, so on odd hops odd channels are negated — without it every
 * odd channel's baseband would sit at ±chan_bw instead of near DC.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "channelizer.h"

#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* WDSP subset (vendor/wdsp): its headers need the GNU dialect, so the one
 * call we use is declared here instead of pulling comm.h into c11 code. */
extern double *fir_bandpass(int N, double f_low, double f_high,
                            double samplerate, int wintype, int rtype,
                            double scale);

#define TAPS_PER_BRANCH 8
#define RING_CAP        4096      /* frames/channel ≈ 8 s at 2×250 Hz        */

struct _SkimChannelizer {
  guint  M;                 /* channels                                       */
  guint  H;                 /* hop = M/2                                      */
  guint  P;                 /* prototype taps = TAPS_PER_BRANCH·M             */
  double in_rate;
  double chan_bw;

  float *proto;             /* P real taps, Σ = 1                             */
  float *hist;              /* P complex frames, newest at index P−1          */
  guint  staged;            /* frames accumulated toward the next hop         */
  guint  hop_odd;           /* parity of the running hop counter              */

  fftwf_complex *fft_in;
  fftwf_complex *fft_out;
  fftwf_plan     plan;

  float   *ring;            /* M · RING_CAP complex frames                    */
  guint   *wr;              /* per-channel write index                        */
  guint   *cnt;              /* per-channel valid frames (≤ RING_CAP)         */
  guint64  dropped;
};

SkimChannelizer *skim_channelizer_new(double in_rate, double chan_bw_hz) {
  if (in_rate <= 0 || chan_bw_hz <= 0)
    return NULL;
  double md = in_rate / chan_bw_hz;
  guint  M  = (guint)(md + 0.5);
  if (fabs(md - (double)M) > 1e-6 || M < 8 || M > 16384 || (M & 1))
    return NULL;

  SkimChannelizer *ch = g_new0(SkimChannelizer, 1);
  ch->M       = M;
  ch->H       = M / 2;
  ch->P       = TAPS_PER_BRANCH * M;
  ch->in_rate = in_rate;
  ch->chan_bw = chan_bw_hz;

  /* Prototype lowpass ±chan_bw/2, real taps (rtype 0), BH4 (wintype 0). */
  double *hd = fir_bandpass((int)ch->P, -chan_bw_hz / 2.0, chan_bw_hz / 2.0,
                            in_rate, 0, 0, 1.0);
  if (!hd) {
    g_free(ch);
    return NULL;
  }
  double sum = 0.0;
  for (guint i = 0; i < ch->P; i++) { sum += hd[i]; }
  ch->proto = g_new(float, ch->P);
  for (guint i = 0; i < ch->P; i++) { ch->proto[i] = (float)(hd[i] / sum); }
  free(hd);

  ch->hist    = g_new0(float, 2 * ch->P);
  ch->fft_in  = fftwf_alloc_complex(M);
  ch->fft_out = fftwf_alloc_complex(M);
  ch->plan    = fftwf_plan_dft_1d((int)M, ch->fft_in, ch->fft_out,
                                  FFTW_BACKWARD, FFTW_ESTIMATE);
  ch->ring = g_new0(float, (gsize)M * RING_CAP * 2);
  ch->wr   = g_new0(guint, M);
  ch->cnt  = g_new0(guint, M);
  return ch;
}

void skim_channelizer_free(SkimChannelizer *ch) {
  if (!ch)
    return;
  fftwf_destroy_plan(ch->plan);
  fftwf_free(ch->fft_in);
  fftwf_free(ch->fft_out);
  g_free(ch->proto);
  g_free(ch->hist);
  g_free(ch->ring);
  g_free(ch->wr);
  g_free(ch->cnt);
  g_free(ch);
}

guint skim_channelizer_count(const SkimChannelizer *ch) { return ch->M; }

double skim_channelizer_out_rate(const SkimChannelizer *ch) {
  return 2.0 * ch->chan_bw;
}

double skim_channelizer_offset_hz(const SkimChannelizer *ch, guint chan) {
  gint c = (gint)chan;
  if (chan > ch->M / 2) { c -= (gint)ch->M; }
  return (double)c * ch->chan_bw;
}

/* One hop over the newest P frames of history. */
static void hop(SkimChannelizer *ch) {
  const guint M = ch->M, P = ch->P;
  for (guint m = 0; m < M; m++) {
    float ar = 0.0f, ai = 0.0f;
    for (guint k = 0; k < TAPS_PER_BRANCH; k++) {
      const guint tap = k * M + m;
      const guint idx = P - 1 - tap;         /* x[s − tap]                   */
      const float w   = ch->proto[tap];
      ar += w * ch->hist[2 * idx];
      ai += w * ch->hist[2 * idx + 1];
    }
    ch->fft_in[m][0] = ar;
    ch->fft_in[m][1] = ai;
  }
  fftwf_execute(ch->plan);

  const guint odd = ch->hop_odd;
  ch->hop_odd ^= 1;
  for (guint c = 0; c < M; c++) {
    float yr = ch->fft_out[c][0], yi = ch->fft_out[c][1];
    if (odd && (c & 1)) { yr = -yr; yi = -yi; }
    float *slot = ch->ring + ((gsize)c * RING_CAP + ch->wr[c]) * 2;
    slot[0] = yr;
    slot[1] = yi;
    ch->wr[c] = (ch->wr[c] + 1) % RING_CAP;
    if (ch->cnt[c] < RING_CAP) {
      ch->cnt[c]++;
    } else {
      ch->dropped++;                          /* overwrote the oldest frame  */
    }
  }
}

void skim_channelizer_push(SkimChannelizer *ch, const float *iq, guint nframes) {
  const guint H = ch->H, P = ch->P;
  while (nframes) {
    guint take = MIN(nframes, H - ch->staged);
    /* Slide history left as the new frames stage in at the tail. */
    memmove(ch->hist, ch->hist + 2 * take,
            (2 * (gsize)(P - take)) * sizeof(float));
    memcpy(ch->hist + 2 * (P - take), iq, 2 * (gsize)take * sizeof(float));
    iq         += 2 * take;
    nframes    -= take;
    ch->staged += take;
    if (ch->staged == H) {
      ch->staged = 0;
      hop(ch);
    }
  }
}

guint skim_channelizer_read(SkimChannelizer *ch, guint chan, float *iq,
                            guint max_frames) {
  if (chan >= ch->M)
    return 0;
  guint n = MIN(max_frames, ch->cnt[chan]);
  const float *base = ch->ring + (gsize)chan * RING_CAP * 2;
  guint rd = (ch->wr[chan] + RING_CAP - ch->cnt[chan]) % RING_CAP;
  for (guint i = 0; i < n; i++) {
    iq[2 * i]     = base[2 * rd];
    iq[2 * i + 1] = base[2 * rd + 1];
    rd = (rd + 1) % RING_CAP;
  }
  ch->cnt[chan] -= n;
  return n;
}

guint64 skim_channelizer_dropped(const SkimChannelizer *ch) {
  return ch->dropped;
}
