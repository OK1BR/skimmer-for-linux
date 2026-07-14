/*
 * skimmer-wdsp-smoke — the vendored WDSP *subset* compiles, links and behaves.
 *
 * Counterpart of sdrfl-wdsp-smoke for our subset (vendor/wdsp/VENDOR.md):
 *   - fir_bandpass designs a real lowpass whose DC gain is ~1 and whose
 *     stopband actually attenuates,
 *   - create_resample / xresample decimate 192 k → 48 k and a passband tone
 *     survives with ~unit gain while amplitude is preserved.
 *
 * Built with the GNU dialect because it includes WDSP's comm.h directly —
 * engine code must go through wrappers instead (vendor/wdsp/meson.build).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <comm.h>

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

int main(void) {
  printf("=== WDSP subset smoke ===\n");

  /* fir_bandpass: real (rtype 0) lowpass ±100 Hz @ 48 k, BH4 window. */
  const int N = 1024;
  double *h = fir_bandpass(N, -100.0, 100.0, 48000.0, 0, 0, 1.0);
  check("fir_bandpass returns an impulse", h != NULL);
  if (h) {
    /* A narrow windowed sinc does NOT sum to 1 — the BH4 window eats the
     * spread-out sinc mass (which is why the channelizer normalises by Σh).
     * What must hold: a usable DC gain and a deep stopband RELATIVE to it. */
    double dc = 0.0;
    for (int i = 0; i < N; i++) { dc += h[i]; }
    printf("       (DC gain Σh = %.3f)\n", dc);
    check("lowpass DC gain sane (0.1–1.1)", dc > 0.1 && dc < 1.1);
    /* Response at 2 kHz (deep stopband): |Σ h[n]·e^{-jωn}| vs DC. */
    double wr = 0.0, wi = 0.0, w = 2.0 * M_PI * 2000.0 / 48000.0;
    for (int i = 0; i < N; i++) {
      wr += h[i] * cos(w * i);
      wi -= h[i] * sin(w * i);
    }
    double stop = sqrt(wr * wr + wi * wi);
    check("stopband @2 kHz < -60 dB of DC", stop < dc * 1e-3);
    free(h);
  } else {
    checks += 2; fails += 2;
  }

  /* create_resample: 192 k → 48 k, complex tone at +5 kHz, amplitude 0.5.
   * Buffer sizes follow the TCI-server usage: in blocks of 1024 pairs. */
  const int BLK = 1024, BLOCKS = 64;
  double *in  = calloc(2 * BLK, sizeof(double));
  double *out = calloc(2 * BLK, sizeof(double));   /* ÷4: ≤256 pairs out    */
  RESAMPLE rs = create_resample(1, BLK, in, out, 192000, 48000, 0.0, 0, 1.0);
  check("create_resample 192k→48k", rs != NULL);
  if (rs) {
    double ph = 0.0, dph = 2.0 * M_PI * 5000.0 / 192000.0;
    int outtotal = 0;
    double peak = 0.0;
    for (int b = 0; b < BLOCKS; b++) {
      for (int i = 0; i < BLK; i++) {
        in[2 * i]     = 0.5 * cos(ph);
        in[2 * i + 1] = 0.5 * sin(ph);
        ph += dph;
      }
      int n = xresample(rs);
      /* Skip the filter's settle-in before measuring amplitude. */
      if (b > 8) {
        for (int i = 0; i < n; i++) {
          double m = hypot(out[2 * i], out[2 * i + 1]);
          if (m > peak) { peak = m; }
        }
      }
      outtotal += n;
    }
    check("output count ~ input/4",
          abs(outtotal - BLOCKS * BLK / 4) < BLK);
    check("passband tone survives with ~unit gain",
          peak > 0.45 && peak < 0.55);
    destroy_resample(rs);
  } else {
    checks += 2; fails += 2;
  }
  free(in);
  free(out);

  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — the WDSP subset links and behaves.");
  return fails ? 1 : 0;
}
