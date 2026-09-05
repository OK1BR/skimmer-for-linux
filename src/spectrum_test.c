/*
 * spectrum_test.c — offline gate for the M8 spectrum tap (skimmer-spectrum-test).
 *
 * The one trap this tap can fall into is the mirror: a +12 kHz tone drawn
 * BELOW the centre (the 2026-07-15 live catch, in a new coat). So the first
 * checks are orientation at every rate the radio can announce, then the
 * bin/row bookkeeping, the dynamic range of the byte encoding, and the
 * pipeline plumbing (rows only while enabled, rows during TX hold, the right
 * centre on every row).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/pipeline.h"
#include "engine/spectrum.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

/* --- synthesis ------------------------------------------------------------------ */

typedef struct { double hz, amp; double ph; } Tone;

static guint32 rng = 0x2545F491u;
static float frand(void) {                    /* uniform [-1, 1), deterministic */
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return (float)(rng & 0xFFFFFF) / 8388608.0f - 1.0f;
}

/* nframes of interleaved I/Q at `rate`: tones (true orientation — +hz is a
 * counter-clockwise phasor) over white noise of amplitude `noise`. */
static float *synth(double rate, guint nframes, Tone *t, guint nt, double noise) {
  float *iq = g_new(float, 2 * nframes);
  for (guint n = 0; n < nframes; n++) {
    double re = noise * frand(), im = noise * frand();
    for (guint k = 0; k < nt; k++) {
      t[k].ph += 2.0 * G_PI * t[k].hz / rate;
      if (t[k].ph > G_PI) { t[k].ph -= 2.0 * G_PI; }
      re += t[k].amp * cos(t[k].ph);
      im += t[k].amp * sin(t[k].ph);
    }
    iq[2 * n] = (float)re;
    iq[2 * n + 1] = (float)im;
  }
  return iq;
}

/* --- row capture ---------------------------------------------------------------- */

typedef struct {
  guint   rows;
  guint   nbins;
  guint8 *last;                                /* copy of the last row       */
  double  center_hz, bin_hz;                   /* pipeline path only         */
} Cap;

static void cap_row(const guint8 *row, guint nbins, gpointer user) {
  Cap *c = user;
  c->rows++;
  if (c->nbins != nbins) {
    g_free(c->last);
    c->last  = g_new(guint8, nbins);
    c->nbins = nbins;
  }
  memcpy(c->last, row, nbins);
}

static void cap_pipe_row(const guint8 *row, guint nbins, double center_hz,
                         double bin_hz, gpointer user) {
  Cap *c = user;
  c->center_hz = center_hz;
  c->bin_hz    = bin_hz;
  cap_row(row, nbins, user);
}

static guint argmax(const guint8 *row, guint n) {
  guint best = 0;
  for (guint i = 1; i < n; i++) { if (row[i] > row[best]) { best = i; } }
  return best;
}

/* Offset from the centre represented by row index i (fftshifted). */
static double idx_hz(guint i, guint n, double bin_hz) {
  return ((double)i - (double)n / 2.0) * bin_hz;
}

static guint8 floor_byte(const guint8 *row, guint n) {   /* 20th percentile */
  guint hist[256] = { 0 };
  for (guint i = 0; i < n; i++) { hist[row[i]]++; }
  guint cum = 0, target = n / 5;
  for (guint b = 0; b < 256; b++) {
    cum += hist[b];
    if (cum >= target) { return (guint8)b; }
  }
  return 255;
}

/* --- 1. orientation + bookkeeping at every rate --------------------------------- */

static void rate_section(double rate) {
  printf("-- %.0f Hz\n", rate);
  SkimSpectrum *s = skim_spectrum_new(rate);
  Cap cap = { 0 };
  skim_spectrum_set_row_cb(s, cap_row, &cap);
  const guint n = skim_spectrum_bins(s);
  const double bin = skim_spectrum_bin_hz(s);
  char what[160];

  g_snprintf(what, sizeof(what), "bin width %.4f Hz == %.4f (N = %u)",
             bin, SKIM_SPECTRUM_BIN_HZ, n);
  check(what, fabs(bin - SKIM_SPECTRUM_BIN_HZ) < 1e-9 && (n & (n - 1)) == 0);

  /* +12 kHz above the centre (true orientation), −n/4·bin below. */
  const double below = -(double)(n / 4) * bin + 3.0;   /* 3 Hz off a bin edge */
  Tone t[2] = { { 12000.0, 0.5, 0 }, { below, 0.25, 0 } };
  const guint nframes = (guint)rate;                  /* one second           */
  float *iq = synth(rate, nframes, t, 2, 1e-3);
  skim_spectrum_push(s, iq, nframes);
  g_free(iq);

  const guint hop = skim_spectrum_hop(s);
  const guint expect_rows = (nframes - n) / hop + 1;
  g_snprintf(what, sizeof(what), "1 s of IQ → %u rows (hop %u frames = %.1f ms)",
             cap.rows, hop, 1000.0 * hop / rate);
  check(what, cap.rows == expect_rows && cap.nbins == n);

  const guint pk = argmax(cap.last, n);
  const double pk_hz = idx_hz(pk, n, bin);
  g_snprintf(what, sizeof(what), "strongest tone at %+.1f Hz (want +12000, ±bin/2) — ABOVE centre",
             pk_hz);
  check(what, fabs(pk_hz - 12000.0) <= bin / 2.0 + 1e-9 && pk > n / 2);

  /* Second tone: blank the first peak's neighbourhood, find the next. */
  guint8 *tmp = g_memdup2(cap.last, n);
  for (guint i = (pk > 4 ? pk - 4 : 0); i < MIN(pk + 5, n); i++) { tmp[i] = 0; }
  const guint pk2 = argmax(tmp, n);
  const double pk2_hz = idx_hz(pk2, n, bin);
  g_snprintf(what, sizeof(what), "weaker tone at %+.1f Hz (want %+.1f, ±bin/2) — BELOW centre",
             pk2_hz, below);
  check(what, fabs(pk2_hz - below) <= bin / 2.0 + 1e-9 && pk2 < n / 2);
  g_free(tmp);

  /* Dynamic range of the byte encoding: the −6 dBFS tone against a −60 dBFS
   * noise floor must read ≥ 55 bytes (dB) over the row's floor byte. */
  const guint8 fl = floor_byte(cap.last, n);
  g_snprintf(what, sizeof(what), "tone byte %u − floor byte %u ≥ 55 dB", cap.last[pk], fl);
  check(what, cap.last[pk] >= fl + 55);
  g_snprintf(what, sizeof(what), "full-scale-ish tone near the encoding's 0 dBFS mark (byte %u in 185..200)",
             cap.last[pk]);
  check(what, cap.last[pk] >= 185 && cap.last[pk] <= 200);

  /* Reset forgets the buffered tail: no row before a full window again. */
  cap.rows = 0;
  skim_spectrum_reset(s);
  float *quiet = g_new0(float, 2 * (n - 1));
  skim_spectrum_push(s, quiet, n - 1);
  check("reset: n−1 frames after reset produce no row", cap.rows == 0);
  skim_spectrum_push(s, quiet, 1);
  check("reset: the n-th frame produces exactly one row", cap.rows == 1);
  g_free(quiet);

  g_free(cap.last);
  skim_spectrum_free(s);
}

/* --- 2. through the pipeline (offline) ------------------------------------------ */

static void pipeline_section(void) {
  printf("-- pipeline (offline, 48 k, centre 14 020 000)\n");
  SkimPipelineConfig cfg = { .host = NULL, .port = 0, .iq_rate = 0,
                             .mode = SKIM_PIPELINE_MODE_CW, .chan_bw_hz = 0,
                             .dict_path = NULL, .decode_log_path = NULL };
  SkimPipeline *p = skim_pipeline_new(&cfg);
  Cap cap = { 0 };
  skim_pipeline_set_spectrum_cb(p, cap_pipe_row, &cap);
  GError *err = NULL;
  check("offline pipeline starts", skim_pipeline_start_offline(p, &err));
  g_clear_error(&err);

  const double rate = 48000.0, center = 14020000.0;
  Tone t[1] = { { 12000.0, 0.5, 0 } };
  const guint blk = 1024;
  float *iq = synth(rate, (guint)rate, t, 1, 1e-3);

  /* Disabled (the default): half a second of IQ, not one row. */
  for (guint off = 0; off + blk <= (guint)rate / 2; off += blk) {
    skim_pipeline_feed(p, iq + 2 * off, blk, rate, center);
  }
  check("spectrum disabled by default: no rows", cap.rows == 0 &&
        !skim_pipeline_spectrum_enabled(p));

  skim_pipeline_set_spectrum_enabled(p, TRUE);
  for (guint off = 0; off + blk <= (guint)rate; off += blk) {
    skim_pipeline_feed(p, iq + 2 * off, blk, rate, center);
  }
  char what[160];
  g_snprintf(what, sizeof(what), "enabled: %u rows over 1 s", cap.rows);
  check(what, cap.rows > 50);
  g_snprintf(what, sizeof(what), "row carries the stream centre %.0f and bin %.4f",
             cap.center_hz, cap.bin_hz);
  check(what, cap.center_hz == center && fabs(cap.bin_hz - SKIM_SPECTRUM_BIN_HZ) < 1e-9);
  const guint pk = argmax(cap.last, cap.nbins);
  const double abs_hz = center + idx_hz(pk, cap.nbins, cap.bin_hz);
  g_snprintf(what, sizeof(what), "peak at %.1f Hz absolute (want 14 032 000 ±bin/2)", abs_hz);
  check(what, fabs(abs_hz - 14032000.0) <= cap.bin_hz / 2.0 + 1e-9);

  /* TX hold swallows decode blocks; the picture must keep flowing. */
  const guint before = cap.rows;
  skim_pipeline_set_tx_hold(p, TRUE);
  for (guint off = 0; off + blk <= (guint)rate / 2; off += blk) {
    skim_pipeline_feed(p, iq + 2 * off, blk, rate, center);
  }
  skim_pipeline_set_tx_hold(p, FALSE);
  g_snprintf(what, sizeof(what), "rows keep coming during TX hold (+%u)", cap.rows - before);
  check(what, cap.rows > before + 20);

  /* Off again: silence. */
  skim_pipeline_set_spectrum_enabled(p, FALSE);
  const guint at_off = cap.rows;
  for (guint off = 0; off + blk <= (guint)rate / 2; off += blk) {
    skim_pipeline_feed(p, iq + 2 * off, blk, rate, center);
  }
  check("disabled again: no further rows", cap.rows == at_off);

  g_free(iq);
  g_free(cap.last);
  skim_pipeline_stop(p);
  skim_pipeline_free(p);
}

int main(void) {
  printf("=== skimmer-spectrum-test — M8 spectrum tap ===\n");
  const double rates[] = { 48000.0, 96000.0, 192000.0, 384000.0 };
  for (guint i = 0; i < G_N_ELEMENTS(rates); i++) { rate_section(rates[i]); }
  pipeline_section();
  printf("=== %d checks, %d failed ===\n", checks, fails);
  return fails ? 1 : 0;
}
