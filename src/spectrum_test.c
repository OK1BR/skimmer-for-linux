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
#include <stdlib.h>
#include <string.h>

#include "app/wf_compose.h"
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
  double  row_hz;                              /* tap path: the row's centre */
} Cap;

static void cap_row(const guint8 *row, guint nbins, double center_hz, gpointer user) {
  Cap *c = user;
  c->row_hz = center_hz;
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
  cap_row(row, nbins, center_hz, user);
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
  skim_spectrum_push(s, iq, nframes, 14020000.0);
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
  skim_spectrum_push(s, quiet, n - 1, 14020000.0);
  check("reset: n−1 frames after reset produce no row", cap.rows == 0);
  skim_spectrum_push(s, quiet, 1, 14020000.0);
  check("reset: the n-th frame produces exactly one row", cap.rows == 1);
  g_free(quiet);

  /* Window-centre labelling: a centre change at frame index T labels a row
   * with the NEW centre exactly when the middle of its window (total − n/2)
   * reaches T — two hops after the change at hop = n/4, whatever the rate.
   * Feed one whole window at A, then hop-sized pieces at B. */
  skim_spectrum_reset(s);
  float *z = g_new0(float, 2 * n);
  skim_spectrum_push(s, z, n, 1000.0);                 /* row 1: window [0,n) */
  check("label: the first row carries its own centre", cap.row_hz == 1000.0);
  double got[5] = { 0 };
  for (guint k = 1; k <= 4; k++) {
    skim_spectrum_push(s, z, hop, 2000.0);             /* change at frame n   */
    got[k] = cap.row_hz;
  }
  g_snprintf(what, sizeof(what), "label: after a change the rows read %.0f %.0f %.0f %.0f — B from the row whose middle passes it",
             got[1], got[2], got[3], got[4]);
  check(what, got[1] == 1000.0 && got[2] == 2000.0 && got[3] == 2000.0 && got[4] == 2000.0);
  /* a change INSIDE a push (sub-block): pushing hop/2 at B then hop/2 at C
   * places the C transition at the right sample, not at the push start */
  const guint half = hop / 2;
  skim_spectrum_push(s, z, half, 2000.0);
  skim_spectrum_push(s, z, hop - half, 3000.0);        /* change at n+4·hop+half */
  /* total = n+5·hop → mid = n/2+5·hop = n+3·hop < change (n+4.5·hop) → B */
  double g5 = cap.row_hz;
  skim_spectrum_push(s, z, hop, 3000.0); double g6 = cap.row_hz;   /* mid n+4·hop   → B */
  skim_spectrum_push(s, z, hop, 3000.0); double g7 = cap.row_hz;   /* mid n+5·hop   → C */
  skim_spectrum_push(s, z, hop, 3000.0); double g8 = cap.row_hz;   /* mid n+6·hop   → C */
  skim_spectrum_push(s, z, hop, 3000.0); double g9 = cap.row_hz;   /* mid n+7·hop   → C */
  g_snprintf(what, sizeof(what), "label: a change inside a push lands on its sample (%.0f %.0f %.0f %.0f %.0f)", g5, g6, g7, g8, g9);
  check(what, g5 == 2000.0 && g6 == 2000.0 && g7 == 3000.0 && g8 == 3000.0 && g9 == 3000.0);
  /* a change every hop for longer than the ring: no stale label, no crash.
   * The window middle (total − n/2) is the boundary between its 2nd and
   * 3rd hop; the centre of the 3rd hop starts exactly there and wins the
   * tie (≤), so a row reads the centre pushed ONE hop before it. */
  /* the first two rows still hold a larger C segment (3 and 2 hops); from
   * the third on all four segments are equal and the newest wins */
  gboolean mono = TRUE;
  for (guint k = 0; k < 40; k++) {
    skim_spectrum_push(s, z, hop, 10000.0 + 100.0 * k);
    const double want = k >= 2 ? 10000.0 + 100.0 * k : 3000.0;
    if (cap.row_hz != want) { mono = FALSE; }
  }
  check("label: a change per hop for 40 hops — C while it is the largest segment, then the NEWEST of four equals", mono);
  g_free(z);

  /* Straddle cut: a tone at a fixed ABSOLUTE frequency, the centre stepping
   * +5 kHz mid-stream (the tone's baseband drops by 5 kHz at the same
   * sample). Every row — including the ones whose window straddles the step
   * — must show ONE peak, at the tone's absolute frequency through the
   * row's own label, and no ghost at the other position (≥ 30 dB down). */
  {
    skim_spectrum_reset(s);
    const double cA = 14020000.0, cB = 14025000.0, f_abs = cA + 12000.0;
    Tone ta = { f_abs - cA, 0.5, 0 }, tb = { f_abs - cB, 0.5, 0 };
    float *pa = synth(rate, n, &ta, 1, 1e-4);
    float *pb = synth(rate, 8 * hop, &tb, 1, 1e-4);
    skim_spectrum_push(s, pa, n, cA);
    gboolean placed = TRUE, no_ghost = TRUE; double worst = 0;
    for (guint k = 0; k < 8; k++) {
      skim_spectrum_push(s, pb + 2 * k * hop, hop, cB);
      const guint pk = argmax(cap.last, n);
      const double abs_hz = cap.row_hz + idx_hz(pk, n, bin);
      if (fabs(abs_hz - f_abs) > 2.5 * bin) { placed = FALSE; }
      /* ghost: the strongest byte more than 16 bins from the peak */
      guint8 ghost = 0;
      for (guint i = 0; i < n; i++) {
        if ((i > pk ? i - pk : pk - i) > 16 && cap.last[i] > ghost) { ghost = cap.last[i]; }
      }
      const double down = (double)cap.last[pk] - (double)ghost;
      if (down < worst || k == 0) { worst = down; }
      if (down < 30.0) { no_ghost = FALSE; }
    }
    check("straddle cut: every row across a +5 kHz step puts the tone on its absolute frequency", placed);
    g_snprintf(what, sizeof(what), "straddle cut: no ghost at the other position (worst %.0f dB down ≥ 30)", worst);
    check(what, no_ghost);
    /* the same step with the LABEL n/32 frames late against the data (the
     * stamp's residual error): the fresh Hann's taper at the boundary
     * leaves the mislabelled frames next to no weight — no ghost either
     * (58 dB down; an explicit n/16 guard band was built, measured to add
     * nothing here, and removed). */
    skim_spectrum_reset(s);
    const guint late = n / 32;
    float *pa2 = synth(rate, n + late, &ta, 1, 1e-4);   /* data: old baseband */
    float *pb2 = synth(rate, 8 * hop, &tb, 1, 1e-4);    /* data: new baseband */
    skim_spectrum_push(s, pa2, n, cA);
    /* data steps at frame n; the label says cA for `late` more frames */
    skim_spectrum_push(s, pb2, late, cA);
    gboolean placed2 = TRUE, no_ghost2 = TRUE; double worst2 = 0;
    for (guint k = 0; k < 8; k++) {
      const guint from = late + k * hop, to = MIN(from + hop, 8 * hop);
      if (to <= from) { break; }
      skim_spectrum_push(s, pb2 + 2 * from, to - from, cB);
      const guint pk = argmax(cap.last, n);
      const double abs_hz = cap.row_hz + idx_hz(pk, n, bin);
      if (fabs(abs_hz - f_abs) > 2.5 * bin) { placed2 = FALSE; }
      guint8 ghost = 0;
      for (guint i = 0; i < n; i++) {
        if ((i > pk ? i - pk : pk - i) > 16 && cap.last[i] > ghost) { ghost = cap.last[i]; }
      }
      const double down = (double)cap.last[pk] - (double)ghost;
      if (down < worst2 || k == 0) { worst2 = down; }
      if (down < 30.0) { no_ghost2 = FALSE; }
    }
    check("late label: a label n/32 frames late still puts the tone on its absolute frequency", placed2);
    g_snprintf(what, sizeof(what), "late label: …and paints no ghost — the Hann taper (worst %.0f dB down ≥ 30)", worst2);
    check(what, no_ghost2);
    g_free(pa); g_free(pb); g_free(pa2); g_free(pb2);
  }

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

/* --- 3. the composer: frequency vertical, newest right, max-pooled ------------- */

/* Rows of the newest column that show the palette TOP (a tone well above
 * the floor saturates the map) — returns count, first and last y. */
static guint bright_rows(const guint32 *pix, int w, int hgt, int x, int *y0, int *y1) {
  const guint32 top = skim_wf_palette_argb(255);
  guint n = 0;
  *y0 = -1; *y1 = -1;
  for (int y = 0; y < hgt; y++) {
    if (pix[(gsize)y * w + x] == top) {
      n++;
      if (*y0 < 0) { *y0 = y; }
      *y1 = y;
    }
  }
  return n;
}

/* Same, restricted to rows within ±radius of yc (a second tone elsewhere in
 * the column must not confuse a check about THIS one). */
static guint bright_near(const guint32 *pix, int w, int hgt, int x, int yc, int radius,
                         int *y0, int *y1) {
  const guint32 top = skim_wf_palette_argb(255);
  guint n = 0;
  *y0 = -1; *y1 = -1;
  for (int y = MAX(yc - radius, 0); y <= MIN(yc + radius, hgt - 1); y++) {
    if (pix[(gsize)y * w + x] == top) {
      n++;
      if (*y0 < 0) { *y0 = y; }
      *y1 = y;
    }
  }
  return n;
}

static void compose_section(void) {
  printf("-- composer\n");
  const guint  nbins  = 2048;                        /* the 48 k geometry    */
  const double center = 14020000.0, bin = SKIM_SPECTRUM_BIN_HZ;
  const double tone_hz = center + 12000.0;
  const guint  tone_bin = (guint)lrint((tone_hz - center) / bin + nbins / 2.0);
  SkimWfHistory *h = skim_wf_history_new(128);
  guint8 *row = g_new(guint8, nbins);
  /* 40 rows of floor (byte 90 = −110 dBFS), then 20 rows with the tone
   * (byte 185 = −15 dBFS: 95 dB over the floor → palette top). A second
   * tone sits 13 bins above the band's LOWER edge throughout — the retune
   * check below pushes it out of the band. */
  const guint edge_bin = 13;
  for (int r = 0; r < 60; r++) {
    memset(row, 90, nbins);
    if (r >= 40) { row[tone_bin] = 185; }
    row[edge_bin] = 185;
    skim_wf_history_push(h, row, nbins, center, bin);
  }
  const double edge_tone_hz = skim_wf_history_lo_hz(h) + ((double)edge_bin) * bin;
  char what[200];
  check("history stores every pushed row", skim_wf_history_rows(h) == 60);
  g_snprintf(what, sizeof(what), "tracked floor %.1f dBFS (rows are −110)",
             skim_wf_history_floor_db(h));
  check(what, fabs(skim_wf_history_floor_db(h) + 110.0) < 0.5);

  /* Orientation of the mapping itself: higher frequency → smaller y. */
  SkimWfWindow win = { .f_top_hz = tone_hz + 2000.0, .f_bot_hz = tone_hz - 2000.0,
                       .rows_per_px = 1 };
  const int w = 100, hgt = 400;                     /* 10 Hz per pixel row  */
  check("higher frequency maps to a SMALLER y (top of the picture)",
        skim_wf_y_of_hz(&win, hgt, tone_hz + 1000.0) < skim_wf_y_of_hz(&win, hgt, tone_hz));
  check("hz_of_y(y_of_hz(f)) round-trips",
        fabs(skim_wf_hz_of_y(&win, hgt, skim_wf_y_of_hz(&win, hgt, tone_hz)) - tone_hz) < 1e-6);

  /* Zoomed IN (several pixels per bin): the tone is a short bright run of
   * rows centred on y_of_hz(tone), in the newest column. */
  guint32 *pix = g_new(guint32, (gsize)w * hgt);
  skim_wf_compose(h, &win, pix, w, hgt, w);
  const int y_exp = (int)floor(skim_wf_y_of_hz(&win, hgt, tone_hz));
  int y0, y1;
  guint n = bright_rows(pix, w, hgt, w - 1, &y0, &y1);
  g_snprintf(what, sizeof(what), "zoomed in: tone = bright rows %d..%d CENTRED on y %d (bin = %.1f px)",
             y0, y1, y_exp, bin / 10.0);
  check(what, n >= 2 && n <= 4 && y0 <= y_exp && y1 >= y_exp &&
        abs((y0 + y1) - 2 * y_exp) <= 1);
  /* Time: the tone rode the newest 20 rows → columns w−20 … w−1 bright,
   * w−21 dark, and the oldest columns beyond the history are floor. */
  check("time: column w−20 still shows the tone",
        bright_rows(pix, w, hgt, w - 20, &y0, &y1) >= 1);
  check("time: column w−21 (before the tone started) is floor",
        bright_rows(pix, w, hgt, w - 21, &y0, &y1) == 0);
  check("time: column 0 (beyond the 60-row history) is the floor colour",
        pix[(gsize)y_exp * w + 0] == skim_wf_palette_argb(0));

  /* Zoomed OUT (whole 48 kHz band on 300 rows ≈ 6.8 bins per pixel): the
   * lone tone bin must survive max-pooling as a bright pixel at its y. */
  SkimWfWindow wide = { .f_top_hz = skim_wf_history_hi_hz(h),
                        .f_bot_hz = skim_wf_history_lo_hz(h), .rows_per_px = 2 };
  const int hgt2 = 300;
  guint32 *pix2 = g_new(guint32, (gsize)w * hgt2);
  skim_wf_compose(h, &wide, pix2, w, hgt2, w);
  const int y_exp2 = (int)floor(skim_wf_y_of_hz(&wide, hgt2, tone_hz));
  n = bright_near(pix2, w, hgt2, w - 1, y_exp2, 4, &y0, &y1);
  g_snprintf(what, sizeof(what), "zoomed out: tone kept by max-pooling at y %d (bright %d..%d; a bin on a row edge may show in both)",
             y_exp2, y0, y1);
  check(what, n >= 1 && n <= 2 && y0 <= y_exp2 && y1 >= y_exp2);
  check("zoomed out + rows_per_px 2: column w−10 (rows 18..19) still bright, w−11 dark",
        bright_near(pix2, w, hgt2, w - 10, y_exp2, 4, &y0, &y1) >= 1 &&
        bright_near(pix2, w, hgt2, w - 11, y_exp2, 4, &y0, &y1) == 0);
  /* The edge tone rides every row: visible at its own y in the wide view. */
  const int y_edge2 = (int)floor(skim_wf_y_of_hz(&wide, hgt2, edge_tone_hz));
  check("zoomed out: the second (edge) tone shows at its own y",
        bright_near(pix2, w, hgt2, w - 1, y_edge2, 4, &y0, &y1) >= 1);

  /* A window reaching past the band edge paints floor there. */
  SkimWfWindow over = { .f_top_hz = skim_wf_history_hi_hz(h) + 5000.0,
                        .f_bot_hz = skim_wf_history_hi_hz(h) - 5000.0, .rows_per_px = 1 };
  skim_wf_compose(h, &over, pix, w, hgt, w);
  check("outside the band: floor colour", pix[0] == skim_wf_palette_argb(0));

  /* A retune (sdr-for-linux has no CTUN: the IQ centre moves with the VFO)
   * must NOT touch the history: old rows stay on their absolute frequencies,
   * new rows land shifted. Move the band up by 40 bins (937.5 Hz) and keep
   * the tone on the SAME absolute frequency. */
  const double c2 = center + 40.0 * bin;
  const guint tone_bin2 = (guint)lrint((tone_hz - c2) / bin + nbins / 2.0);
  for (int r = 0; r < 10; r++) {
    memset(row, 90, nbins);
    row[tone_bin2] = 185;
    skim_wf_history_push(h, row, nbins, c2, bin);
  }
  check("a retune keeps the history (70 rows)", skim_wf_history_rows(h) == 70);
  g_snprintf(what, sizeof(what), "current band follows the newest row: centre %.1f", skim_wf_history_center_hz(h));
  check(what, fabs(skim_wf_history_center_hz(h) - c2) < 1e-6);
  skim_wf_compose(h, &win, pix, w, hgt, w);
  int ya0, ya1, yb0, yb1;
  const guint na = bright_rows(pix, w, hgt, w - 1, &ya0, &ya1);   /* after  */
  const guint nb = bright_rows(pix, w, hgt, w - 15, &yb0, &yb1);  /* before */
  g_snprintf(what, sizeof(what), "same absolute tone before (%d..%d) and after (%d..%d) the retune", yb0, yb1, ya0, ya1);
  check(what, na >= 2 && nb >= 2 && ya0 == yb0 && ya1 == yb1);
  /* The edge tone (13 bins above the OLD lower edge) is now 27 bins BELOW
   * the new band: the newest column cannot show it (floor), a column from
   * before the retune still does, on the same absolute frequency. */
  SkimWfWindow edge = { .f_top_hz = edge_tone_hz + 600.0,
                        .f_bot_hz = edge_tone_hz - 600.0, .rows_per_px = 1 };
  skim_wf_compose(h, &edge, pix, w, hgt, w);
  const int ye = (int)floor(skim_wf_y_of_hz(&edge, hgt, edge_tone_hz));
  check("a tone the retune pushed out of the band: newest column shows floor",
        bright_rows(pix, w, hgt, w - 1, &ya0, &ya1) == 0);
  g_snprintf(what, sizeof(what), "…and a pre-retune column still shows it at y %d", ye);
  check(what, bright_rows(pix, w, hgt, w - 15, &yb0, &yb1) >= 1 && yb0 <= ye && yb1 >= ye);

  /* Label delay line: every row goes out, none delayed, none dropped, each
   * placed on the label that was current LAG rows earlier (the data lags
   * the label by the DDC apply + IQ transport + half a window). The old
   * retune guard dropped 16 of these 50 rows and paused the picture for
   * 0.7 s after every retune — Richard: the waterfall must FLOW. */
  {
    const guint LAG = 3;
    SkimWfDelay *d = skim_wf_delay_new(LAG);
    /* 20 rows at centre A, then 30 at centre B: the placed label switches
     * exactly LAG rows after the input label did, everything in order. */
    guint nA = 0, nB = 0, first_b = 0; gboolean ordered = TRUE;
    for (guint i = 1; i <= 50; i++) {
      const double out = skim_wf_delay_push(d, i <= 20 ? 1000.0 : 2000.0, 1.0);
      if (out == 1000.0)      { nA++; if (nB) { ordered = FALSE; } }
      else if (out == 2000.0) { nB++; if (!first_b) { first_b = i; } }
      else                    { ordered = FALSE; }
    }
    g_snprintf(what, sizeof(what), "delay: a discrete step — 50 rows in, 50 out (%u on A, %u on B), B from row %u",
               nA, nB, first_b);
    check(what, nA == 20 + LAG && nB == 30 - LAG && first_b == 21 + LAG && ordered);
    /* A turning knob (a new centre every 6 rows): row i is placed on the
     * label of row i − LAG, exactly, and nothing is dropped. */
    skim_wf_delay_free(d);
    d = skim_wf_delay_new(LAG);
    double in[81]; gboolean exact = TRUE; guint n = 0;
    for (guint i = 1; i <= 80; i++) {
      in[i] = i <= 20 ? 1000.0 : i <= 50 ? 1000.0 + 100.0 * ((i - 21) / 6 + 1) : 1600.0;
      const double out = skim_wf_delay_push(d, in[i], 1.0);
      if (out != in[i > LAG ? i - LAG : 1]) { exact = FALSE; }
      n++;
    }
    check("delay: a turning knob — every row out, each on the label LAG rows back", exact && n == 80);
    /* A steady centre is a passthrough; lag 0 is the identity. */
    gboolean steady = TRUE;
    for (guint i = 0; i < 30; i++) { if (skim_wf_delay_push(d, 1600.0, 1.0) != 1600.0) { steady = FALSE; } }
    check("delay: a steady centre passes through unchanged", steady);
    skim_wf_delay_set_lag(d, 0);
    check("delay: lag 0 is the identity",
          skim_wf_delay_push(d, 1700.0, 1.0) == 1700.0 && skim_wf_delay_push(d, 1800.0, 1.0) == 1800.0 &&
          skim_wf_delay_lag(d) == 0);
    /* A rate change (new bin width) restarts on the new label at once —
     * no stale centre from the old grid — and a fresh line rides its own
     * label while it fills. */
    skim_wf_delay_set_lag(d, LAG);
    check("delay: a rate change restarts on the new label",
          skim_wf_delay_push(d, 5000.0, 2.0) == 5000.0 && skim_wf_delay_push(d, 5100.0, 2.0) == 5000.0 &&
          skim_wf_delay_push(d, 5200.0, 2.0) == 5000.0 && skim_wf_delay_push(d, 5300.0, 2.0) == 5000.0 &&
          skim_wf_delay_push(d, 5400.0, 2.0) == 5100.0);
    check("delay: the lag is capped at SKIM_WF_LABEL_LAG_MAX",
          (skim_wf_delay_set_lag(d, 1000), skim_wf_delay_lag(d) == SKIM_WF_LABEL_LAG_MAX));
    skim_wf_delay_free(d);
  }

  /* Cost of the worst case (informative, not a check): the whole 192 k band
   * on a 700×600 picture (≈ 14 bins per pixel row), full recompose vs the
   * incremental two columns a drain typically adds. */
  {
    SkimWfHistory *big = skim_wf_history_new(SKIM_WF_HISTORY_ROWS);
    const guint nb = 8192;
    guint8 *r8 = g_new(guint8, nb);
    for (int r = 0; r < 1400; r++) {
      for (guint i = 0; i < nb; i++) { r8[i] = (guint8)(84 + ((i * 7 + r) & 15)); }
      skim_wf_history_push(big, r8, nb, 14030000.0, bin);
    }
    SkimWfWindow full = { .f_top_hz = skim_wf_history_hi_hz(big),
                          .f_bot_hz = skim_wf_history_lo_hz(big), .rows_per_px = 2 };
    const int W = 700, Hh = 600;
    guint32 *px = g_new(guint32, (gsize)W * Hh);
    gint64 t0 = g_get_monotonic_time();
    for (int k = 0; k < 5; k++) { skim_wf_compose(big, &full, px, W, Hh, W); }
    const double full_ms = (double)(g_get_monotonic_time() - t0) / 5000.0;
    t0 = g_get_monotonic_time();
    for (int k = 0; k < 200; k++) { skim_wf_compose(big, &full, px, W, Hh, 2); }
    const double inc_us = (double)(g_get_monotonic_time() - t0) / 200.0;
    printf("  info full-band recompose 700×600: %.1f ms; two new columns: %.0f µs\n",
           full_ms, inc_us);
    g_free(px); g_free(r8); skim_wf_history_free(big);
  }

  /* Optional eyeball: SKIM_WF_PPM=<file> writes a synthetic scene. */
  const char *ppm = g_getenv("SKIM_WF_PPM");
  if (ppm) {
    const char *pe = g_getenv("SKIM_WF_PPM_PALETTE");
    if (pe) { skim_wf_set_palette(atoi(pe)); }
    SkimWfHistory *d = skim_wf_history_new(SKIM_WF_HISTORY_ROWS);
    const guint nb = 8192; const double c = 14030000.0;
    guint8 *r8 = g_new(guint8, nb);
    guint32 seed = 7;
    for (int r = 0; r < 700; r++) {
      for (guint i = 0; i < nb; i++) { seed = seed * 1664525u + 1013904223u; r8[i] = 84 + (seed >> 28); }
      const guint bA = (guint)lrint(5200.0 / bin + nb / 2.0);
      const guint bB = (guint)lrint(-3100.0 / bin + nb / 2.0);
      const guint bC = (guint)lrint(800.0 / bin + nb / 2.0);
      r8[bA] = 165; r8[bA - 1] = 140; r8[bA + 1] = 140;          /* strong, continuous */
      if ((r / 5) % 3 != 2) { r8[bB] = 150; r8[bB + 1] = 130; }  /* keyed              */
      r8[bC] = 104;                                              /* weak, +14 dB       */
      skim_wf_history_push(d, r8, nb, c, bin);
    }
    SkimWfWindow sw = { .f_top_hz = c + 10000.0, .f_bot_hz = c - 10000.0, .rows_per_px = SKIM_WF_ROWS_PER_PX };
    const int W = 640, Hh = 480;
    guint32 *px = g_new(guint32, (gsize)W * Hh);
    skim_wf_compose(d, &sw, px, W, Hh, W);
    FILE *f = fopen(ppm, "wb");
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", W, Hh);
      for (gsize i = 0; i < (gsize)W * Hh; i++) {
        const guint8 rgb[3] = { (px[i] >> 16) & 255, (px[i] >> 8) & 255, px[i] & 255 };
        fwrite(rgb, 1, 3, f);
      }
      fclose(f);
      printf("  (wrote %s)\n", ppm);
    }
    g_free(px); g_free(r8); skim_wf_history_free(d);
  }

  g_free(pix); g_free(pix2); g_free(row);
  skim_wf_history_free(h);
}

/* --- callsign column layout (wf_compose.c) ---------------------------------------- */

static void labels_section(void) {
  printf("--- callsign column layout ---\n");
  const double P = 14.0, H = 400.0;

  /* Far apart: nobody moves. */
  SkimWfLabel a[3] = { { 100, 0, 0 }, { 200, 0, 0 }, { 300, 0, 0 } };
  guint shown = skim_wf_layout_labels(a, 3, P, H);
  check("far apart: all shown, none moved",
        shown == 3 && a[0].y == 100 && a[1].y == 200 && a[2].y == 300);

  /* Two labels 3 px apart, given out of order: pushed to ±P/2 about their
   * mean, the higher-frequency (smaller y) one stays above. */
  SkimWfLabel b[2] = { { 203, 0, 0 }, { 200, 0, 0 } };
  skim_wf_layout_labels(b, 2, P, H);
  check("pair 3 px apart: spread symmetrically about the mean anchor",
        fabs(b[1].y - (201.5 - P / 2)) < 1e-9 && fabs(b[0].y - (201.5 + P / 2)) < 1e-9);
  check("pair: frequency order kept", b[1].y < b[0].y);

  /* Five on one frequency: spaced ≥ pitch, the run centred on the anchor. */
  SkimWfLabel c[5];
  for (int i = 0; i < 5; i++) { c[i] = (SkimWfLabel){ 150, 0, 0 }; }
  skim_wf_layout_labels(c, 5, P, H);
  double lo = 1e9, hi = -1e9;
  gboolean spaced = TRUE;
  for (int i = 0; i < 5; i++) { lo = MIN(lo, c[i].y); hi = MAX(hi, c[i].y); }
  for (int i = 0; i < 5; i++) {
    for (int j = i + 1; j < 5; j++) { if (fabs(c[i].y - c[j].y) < P - 1e-9) { spaced = FALSE; } }
  }
  check("cluster of five: spaced ≥ pitch, centred on the anchor",
        spaced && fabs((lo + hi) / 2 - 150) < 1e-9 && fabs(hi - lo - 4 * P) < 1e-9);

  /* A neighbour 20 px under a triple is nudged down, never overrun. */
  SkimWfLabel d[4] = { { 150, 0, 0 }, { 150, 0, 0 }, { 150, 0, 0 }, { 170, 0, 0 } };
  skim_wf_layout_labels(d, 4, P, H);
  check("near neighbour: pushed down by the cluster, order kept",
        d[3].y >= d[2].y + P - 1e-9 && d[3].y > 170);

  /* Edges: a run at the top slides down until its first band starts at 0,
   * one at the bottom slides up until its last band ends at hgt. */
  SkimWfLabel e[3] = { { 2, 0, 0 }, { 2, 0, 0 }, { 2, 0, 0 } };
  skim_wf_layout_labels(e, 3, P, H);
  const double emin = MIN(e[0].y, MIN(e[1].y, e[2].y));
  check("top edge: first label's band starts at 0", fabs(emin - P / 2) < 1e-9);
  SkimWfLabel f[3] = { { 398, 0, 0 }, { 398, 0, 0 }, { 398, 0, 0 } };
  skim_wf_layout_labels(f, 3, P, H);
  const double fmax = MAX(f[0].y, MAX(f[1].y, f[2].y));
  check("bottom edge: last label's band ends at hgt", fabs(fmax - (H - P / 2)) < 1e-9);

  /* Anchors outside the column are hidden, not pulled in. */
  SkimWfLabel g[2] = { { -5, 0, 0 }, { H + 1, 0, 0 } };
  check("outside the column: hidden",
        skim_wf_layout_labels(g, 2, P, H) == 0 && g[0].y < 0 && g[1].y < 0);

  /* Over capacity: 40 labels on 400 px at 14 px pitch seat 28 — the 12
   * lowest-priority ones go, the rest stay ordered and spaced. */
  SkimWfLabel o[40];
  for (int i = 0; i < 40; i++) {
    o[i] = (SkimWfLabel){ 5 + i * 9.7, (i % 3 == 0) ? 1 : 100 + i, 0 };
  }
  shown = skim_wf_layout_labels(o, 40, P, H);
  guint hidden_low = 0, hidden_high = 0;
  for (int i = 0; i < 40; i++) {
    if (o[i].y < 0) { if (o[i].prio == 1) { hidden_low++; } else { hidden_high++; } }
  }
  check("over capacity: exactly floor(H/P) labels shown", shown == 28);
  check("over capacity: only the lowest-priority labels are hidden",
        hidden_low == 12 && hidden_high == 0);
  gboolean ordered = TRUE, sp = TRUE;
  double prev = -1e9;
  for (int i = 0; i < 40; i++) {
    if (o[i].y < 0) { continue; }
    if (o[i].y < prev + P - 1e-9) { sp = FALSE; }
    if (o[i].y < prev) { ordered = FALSE; }
    prev = o[i].y;
  }
  check("over capacity: the shown labels stay ordered and spaced", ordered && sp);

  /* Hit test: the band of a shown label, nothing between labels, and a
   * hidden label's anchor never resolves to it. */
  check("hit: y inside a label's band finds it", skim_wf_label_at(a, 3, P, 205.0) == 1);
  check("hit: y between labels finds nothing", skim_wf_label_at(a, 3, P, 150.0) == -1);
  check("hit: a hidden label is never hit", o[0].y < 0 && skim_wf_label_at(o, 40, P, 5.0) != 0);
}

int main(void) {
  printf("=== skimmer-spectrum-test — M8 spectrum tap ===\n");
  const double rates[] = { 48000.0, 96000.0, 192000.0, 384000.0 };
  for (guint i = 0; i < G_N_ELEMENTS(rates); i++) { rate_section(rates[i]); }
  pipeline_section();
  compose_section();
  labels_section();
  printf("=== %d checks, %d failed ===\n", checks, fails);
  return fails ? 1 : 0;
}
