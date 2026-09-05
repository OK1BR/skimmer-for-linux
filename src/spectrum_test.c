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

/* Retune-guard capture: rows out, order (byte 0 = sequence), last centre. */
typedef struct { guint n; double last_c; gboolean in_order; guint8 prev; } GuardGot;
static void guard_commit(const guint8 *r, guint nb, double c, double b, gpointer u) {
  (void)nb; (void)b;
  GuardGot *gg = u;
  if (gg->n && r[0] <= gg->prev) { gg->in_order = FALSE; }
  gg->prev = r[0]; gg->n++; gg->last_c = c;
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

  /* Retune guard: the G rows waiting before a centre change and every row
   * after it until the centre has stood still for SETTLE rows are dropped;
   * the rest is committed in order with its own centre. */
  {
    const guint G = 3, S = 10;
    GuardGot got = { 0, 0, TRUE, 0 };
    SkimWfGuard *gd = skim_wf_guard_new(G, S);
    skim_wf_guard_set_commit_cb(gd, guard_commit, &got);
    guint8 r1[4];
    /* 20 rows at centre A, then 30 at centre B; row byte 0 = its sequence. */
    for (guint i = 1; i <= 50; i++) {
      r1[0] = (guint8)i; r1[1] = r1[2] = r1[3] = 0;
      skim_wf_guard_push(gd, r1, 4, i <= 20 ? 1000.0 : 2000.0, 1.0);
    }
    g_snprintf(what, sizeof(what), "guard: 50 rows in → %u committed, %u dropped (3 before + 10 settle), 3 waiting",
               got.n, skim_wf_guard_dropped(gd));
    check(what, got.n == 50 - G - S - G && skim_wf_guard_dropped(gd) == G + S);
    check("guard: committed rows stay in order", got.in_order);
    check("guard: the last committed row carries the NEW centre", got.last_c == 2000.0);
    /* A label that keeps changing (polling server re-labels mid-turn) never
     * settles: NOTHING from the turn gets out, the first rows after it do. */
    GuardGot turn = { 0, 0, TRUE, 0 };
    SkimWfGuard *gt = skim_wf_guard_new(G, S);
    skim_wf_guard_set_commit_cb(gt, guard_commit, &turn);
    for (guint i = 1; i <= 80; i++) {
      r1[0] = (guint8)i;
      /* rows 21..50: a new centre every 6 rows (< S) — the knob turning */
      const double c = i <= 20 ? 1000.0 : i <= 50 ? 1000.0 + 100.0 * ((i - 21) / 6 + 1) : 1600.0;
      skim_wf_guard_push(gt, r1, 4, c, 1.0);
    }
    /* out: rows 1..17 (3 dropped before the first change), then 61..77 */
    g_snprintf(what, sizeof(what), "guard: a turning knob commits nothing mid-turn (%u out, %u dropped)",
               turn.n, skim_wf_guard_dropped(gt));
    check(what, turn.n == 17 + 17 && skim_wf_guard_dropped(gt) == 80 - 34 - G && turn.in_order &&
          turn.last_c == 1600.0);
    /* No retune → nothing dropped, everything (bar the waiting tail) out. */
    GuardGot quiet = { 0, 0, TRUE, 0 };
    SkimWfGuard *gq = skim_wf_guard_new(G, S);
    skim_wf_guard_set_commit_cb(gq, guard_commit, &quiet);
    for (guint i = 1; i <= 30; i++) { r1[0] = (guint8)i; skim_wf_guard_push(gq, r1, 4, 1000.0, 1.0); }
    check("guard: steady centre drops nothing", quiet.n == 30 - G && skim_wf_guard_dropped(gq) == 0);
    skim_wf_guard_free(gd);
    skim_wf_guard_free(gt);
    skim_wf_guard_free(gq);
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

int main(void) {
  printf("=== skimmer-spectrum-test — M8 spectrum tap ===\n");
  const double rates[] = { 48000.0, 96000.0, 192000.0, 384000.0 };
  for (guint i = 0; i < G_N_ELEMENTS(rates); i++) { rate_section(rates[i]); }
  pipeline_section();
  compose_section();
  printf("=== %d checks, %d failed ===\n", checks, fails);
  return fails ? 1 : 0;
}
