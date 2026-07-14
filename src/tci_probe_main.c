/*
 * skimmer-tci-probe — M1 live gate against a running sdr-for-linux TCI server.
 *
 *   skimmer-tci-probe [host] [port] [rate] [seconds]
 *     host     TCI server (default 127.0.0.1)
 *     port     WebSocket port (default 40001)
 *     rate     IQ rate in Hz or kHz: 48/96/192/384[000] (default 192000)
 *     seconds  capture length (default 10)
 *
 * Connects, prints the handshake (protocol, device, dds centre, granted IQ
 * rate), ingests IQ for N seconds and reports: block/frame counts, effective
 * vs. nominal sample rate, RMS/peak/DC, and an 8192-point spectrum (top peaks
 * + a one-line ASCII panorama) in TRUE orientation — the client conjugates the
 * ExpertSDR wire convention on ingest, so a station ABOVE the DDC centre must
 * show at a POSITIVE offset here. Compare peaks against the panadapter: that
 * eyeball check *is* the M1 orientation gate (we cannot key a reference tone —
 * the skimmer is read-only by design).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/tci_client.h"

#define FFT_N     8192
#define SKIP_BLKS 2               /* let the stream settle before capturing   */
#define ASCII_W   96

/* ---- capture state (IQ callback runs on the client's LWS thread) ------------ */

static GMutex        p_lock;
static guint64       p_frames;
static guint         p_blocks;
static gint64        p_t_first, p_t_last;   /* monotonic µs at block arrival   */
static double        p_hdr_rate;            /* per-block header rate           */
static double        p_sum_i, p_sum_q, p_sum_p2;  /* DC + power accumulators   */
static float         p_peak;
static float         p_fft_buf[FFT_N * 2];
static guint         p_fft_fill;            /* frames captured into p_fft_buf  */

static void iq_cb(const float *iq, guint nframes, double rate, double center,
                  gpointer user) {
  (void)center; (void)user;
  gint64 now = g_get_monotonic_time();
  g_mutex_lock(&p_lock);
  if (!p_blocks) { p_t_first = now; }
  p_t_last   = now;
  p_hdr_rate = rate;
  p_blocks++;
  p_frames += nframes;
  for (guint i = 0; i < nframes; i++) {
    float vi = iq[2 * i], vq = iq[2 * i + 1];
    p_sum_i  += vi;
    p_sum_q  += vq;
    p_sum_p2 += (double)vi * vi + (double)vq * vq;
    float a = fabsf(vi) > fabsf(vq) ? fabsf(vi) : fabsf(vq);
    if (a > p_peak) { p_peak = a; }
  }
  if (p_blocks > SKIP_BLKS && p_fft_fill < FFT_N) {
    guint take = MIN(nframes, FFT_N - p_fft_fill);
    memcpy(p_fft_buf + 2 * p_fft_fill, iq, take * 2 * sizeof(float));
    p_fft_fill += take;
  }
  g_mutex_unlock(&p_lock);
}

/* ---- small in-place radix-2 complex FFT (probe-only; M2 brings WDSP/fftw) --- */

static void fft(double *re, double *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {          /* bit-reversal permutation    */
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) { j ^= bit; }
    j |= bit;
    if (i < j) {
      double t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    double ang = -2.0 * G_PI / len;
    double wr = cos(ang), wi = sin(ang);
    for (int i = 0; i < n; i += len) {
      double cr = 1.0, ci = 0.0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = i + k + len / 2;
        double tr = re[b] * cr - im[b] * ci;
        double ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr;        im[a] += ti;
        double ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

/* dB bin k (0..N-1) → signed offset Hz, DC-centred spectrum. */
static double bin_hz(int k, double rate) {
  int s = (k <= FFT_N / 2) ? k : k - FFT_N;
  return (double)s * rate / FFT_N;
}

int main(int argc, char **argv) {
  const char *host = argc > 1 ? argv[1] : "127.0.0.1";
  guint16     port = argc > 2 ? (guint16)atoi(argv[2]) : 40001;
  guint       rate = argc > 3 ? (guint)atoi(argv[3]) : 192000;
  int         secs = argc > 4 ? atoi(argv[4]) : 10;
  if (rate < 1000) { rate *= 1000; }              /* 192 → 192000              */

  printf("=== skimmer-tci-probe — ws://%s:%u, %u Hz IQ, %d s ===\n",
         host, port, rate, secs);

  SkimTciClient *c = skim_tci_client_new(host, port);
  skim_tci_client_set_iq_cb(c, iq_cb, NULL);

  GError *err = NULL;
  if (!skim_tci_client_start(c, rate, &err)) {
    printf("FAIL — %s\n", err ? err->message : "?");
    g_clear_error(&err);
    skim_tci_client_free(c);
    return 1;
  }
  printf("handshake: protocol %s, device \"%s\"\n",
         skim_tci_client_protocol(c), skim_tci_client_device(c));
  printf("           dds centre %.0f Hz, device iq rate at connect %u\n",
         skim_tci_client_center_hz(c), skim_tci_client_iq_rate(c));

  for (int s = 0; s < secs; s++) {
    g_usleep(G_USEC_PER_SEC);
    g_mutex_lock(&p_lock);
    guint64 fr = p_frames;
    g_mutex_unlock(&p_lock);
    printf("  %2d s  %" G_GUINT64_FORMAT " frames\r", s + 1, fr);
    fflush(stdout);
  }
  printf("\n");
  double center  = skim_tci_client_center_hz(c);
  guint  granted = skim_tci_client_iq_rate(c);   /* echoed by now (or default) */
  skim_tci_client_stop(c);

  g_mutex_lock(&p_lock);
  guint    blocks  = p_blocks;
  guint64  frames  = p_frames;
  double   span_s  = (double)(p_t_last - p_t_first) / G_USEC_PER_SEC;
  double   hdrrate = p_hdr_rate;
  double   dc_i    = frames ? p_sum_i / (double)frames : 0;
  double   dc_q    = frames ? p_sum_q / (double)frames : 0;
  double   rms     = frames ? sqrt(p_sum_p2 / (double)frames) : 0;
  float    peak    = p_peak;
  guint    ffill   = p_fft_fill;
  g_mutex_unlock(&p_lock);

  if (!blocks) {
    printf("FAIL — connected but no IQ block arrived\n");
    skim_tci_client_free(c);
    return 1;
  }

  /* First→last block span covers frames of blocks 2..N — effective rate uses
   * (frames − first block's share); with thousands of blocks the bias of one
   * block is < 0.1 %, so fold it in via blocks/(blocks−1) correction-free. */
  double eff = span_s > 0 ? (double)(frames - frames / blocks) / span_s : 0;
  double dev = hdrrate > 0 ? (eff - hdrrate) / hdrrate * 100.0 : 0;

  printf("\nIQ stats:\n");
  printf("  blocks %u, frames %" G_GUINT64_FORMAT ", header rate %.0f Hz, "
         "granted iq_samplerate %u\n", blocks, frames, hdrrate, granted);
  printf("  effective rate %.1f Hz (%+.3f %% vs header) — %s\n",
         eff, dev, fabs(dev) < 2.0 ? "ok" : "OUT OF TOLERANCE");
  printf("  RMS %.1f dBFS, peak %.3f, DC offset I %+.5f Q %+.5f\n",
         rms > 0 ? 20.0 * log10(rms) : -999.0, peak, dc_i, dc_q);

  if (ffill == FFT_N) {
    static double re[FFT_N], im[FFT_N], db[FFT_N];
    for (int i = 0; i < FFT_N; i++) {           /* Hann window                */
      double w = 0.5 * (1.0 - cos(2.0 * G_PI * i / (FFT_N - 1)));
      re[i] = p_fft_buf[2 * i] * w;
      im[i] = p_fft_buf[2 * i + 1] * w;
    }
    fft(re, im, FFT_N);
    double maxdb = -999.0;
    for (int k = 0; k < FFT_N; k++) {
      double p2 = re[k] * re[k] + im[k] * im[k];
      db[k] = 10.0 * log10(p2 + 1e-30);
      if (db[k] > maxdb) { maxdb = db[k]; }
    }
    for (int k = 0; k < FFT_N; k++) { db[k] -= maxdb; }

    /* Top peaks: strongest bins, ≥ 8 bins apart, within 60 dB of the max. */
    printf("\nspectrum peaks (TRUE orientation — wire conjugated on ingest):\n");
    printf("  %-12s %-14s %s\n", "offset", "absolute", "rel");
    gboolean used[FFT_N] = { FALSE };
    for (int p = 0; p < 8; p++) {
      int best = -1;
      for (int k = 0; k < FFT_N; k++) {
        if (!used[k] && (best < 0 || db[k] > db[best])) { best = k; }
      }
      if (best < 0 || db[best] < -60.0) { break; }
      for (int k = best - 8; k <= best + 8; k++) {
        used[(k + FFT_N) % FFT_N] = TRUE;
      }
      double off = bin_hz(best, hdrrate);
      printf("  %+9.0f Hz %12.0f Hz %6.1f dB\n", off, center + off, db[best]);
    }

    /* One-line ASCII panorama, DC-centred: −rate/2 … +rate/2. */
    static const char shade[] = " .:-=+*#%@";
    char line[ASCII_W + 1];
    for (int x = 0; x < ASCII_W; x++) {
      int k0 = x * FFT_N / ASCII_W;             /* columns over shifted bins   */
      int k1 = (x + 1) * FFT_N / ASCII_W;
      double m = -999.0;
      for (int k = k0; k < k1; k++) {
        int bin = (k - FFT_N / 2 + FFT_N) % FFT_N;   /* left edge = −rate/2    */
        if (db[bin] > m) { m = db[bin]; }
      }
      int idx = (int)((m + 80.0) / 80.0 * 9.0);      /* −80..0 dB → 0..9       */
      line[x] = shade[CLAMP(idx, 0, 9)];
    }
    line[ASCII_W] = '\0';
    printf("\n  [%s]\n", line);
    printf("  %*s^ centre %.0f Hz (left −%u kHz … right +%u kHz)\n",
           ASCII_W / 2, "", center, rate / 2000, rate / 2000);
    printf("\nEYEBALL GATE: a station ABOVE the panadapter centre must sit at a "
           "POSITIVE offset above.\nMirrored peaks = orientation bug.\n");
  } else {
    printf("  (spectrum skipped — only %u/%u frames captured)\n", ffill, FFT_N);
  }

  skim_tci_client_free(c);
  gboolean pass = fabs(dev) < 2.0;
  printf("\n%s\n", pass ? "PASS (plus the eyeball orientation check above)"
                        : "FAIL — sample-rate deviation out of tolerance");
  return pass ? 0 : 1;
}
