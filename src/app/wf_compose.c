/*
 * wf_compose.c — see wf_compose.h.
 *
 * The palette tables, the interpolation and the percentile noise-floor idea
 * are copied from sdr-for-linux's src/waterfall.c (OK1BR, GPL-3.0-or-later)
 * so the two apps colour a band identically. The history/compose model is
 * new: that renderer keeps a fixed 256-row bitmap scaled to fit, this one
 * keeps full-resolution rows and composes a pannable, zoomable window.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wf_compose.h"

#include <math.h>
#include <string.h>

/* -------- palette (sdr-for-linux waterfall.c) --------------------------------- */

typedef struct { double t, r, g, b; } PalStop;
typedef struct { const char *name; const PalStop *stop; int n; } Palette;

/* Classic: black-blue -> cyan -> green -> yellow -> orange -> red. */
static const PalStop pal_classic[] = {
  {0.00,   0,   0,  12}, {0.20,   0,  18, 110}, {0.40,   0, 130, 180},
  {0.58,   0, 200, 120}, {0.74, 180, 220,   0}, {0.88, 255, 165,   0},
  {1.00, 255,  40,  30},
};
/* Mono white: neutral greyscale, black -> white. */
static const PalStop pal_mono_white[] = {
  {0.00, 0, 0, 0}, {0.30, 6, 6, 6}, {0.58, 90, 90, 90}, {0.80, 210, 210, 210}, {1.00, 255, 255, 255},
};
/* Mono green: retro phosphor — near-black floor, bright green signals. */
static const PalStop pal_mono_green[] = {
  {0.00, 0, 2, 0}, {0.30, 0, 34, 6}, {0.60, 0, 150, 40}, {0.82, 130, 240, 95}, {1.00, 225, 255, 205},
};
/* Mono amber: warm monochrome — near-black floor, bright amber signals. */
static const PalStop pal_mono_amber[] = {
  {0.00, 2, 1, 0}, {0.30, 40, 18, 0}, {0.60, 165, 90, 4}, {0.82, 248, 190, 55}, {1.00, 255, 245, 215},
};
/* Inferno: perceptually-uniform black -> purple -> red -> orange -> yellow. */
static const PalStop pal_inferno[] = {
  {0.00, 0, 0, 4}, {0.15, 22, 11, 57}, {0.30, 66, 10, 104}, {0.45, 114, 31, 107},
  {0.60, 168, 46, 86}, {0.74, 216, 83, 45}, {0.87, 245, 140, 20}, {1.00, 252, 255, 164},
};
/* Turbo: high-detail improved rainbow. */
static const PalStop pal_turbo[] = {
  {0.00, 48, 18, 59}, {0.13, 62, 74, 211}, {0.25, 40, 150, 240}, {0.38, 43, 210, 180},
  {0.50, 130, 235, 80}, {0.62, 220, 220, 40}, {0.75, 250, 150, 30}, {0.88, 230, 60, 20},
  {1.00, 122, 4, 3},
};

static const Palette g_palettes[] = {
  { "Classic",    pal_classic,    G_N_ELEMENTS(pal_classic)    },
  { "Mono white", pal_mono_white, G_N_ELEMENTS(pal_mono_white) },
  { "Mono green", pal_mono_green, G_N_ELEMENTS(pal_mono_green) },
  { "Mono amber", pal_mono_amber, G_N_ELEMENTS(pal_mono_amber) },
  { "Inferno",    pal_inferno,    G_N_ELEMENTS(pal_inferno)    },
  { "Turbo",      pal_turbo,      G_N_ELEMENTS(pal_turbo)      },
};
static int      g_sel = 0;
static guint32  g_lut[256];
static gboolean g_lut_ok = FALSE;

int skim_wf_palette_count(void) { return (int)G_N_ELEMENTS(g_palettes); }
const char *skim_wf_palette_name(int idx) {
  return (idx < 0 || idx >= skim_wf_palette_count()) ? "" : g_palettes[idx].name;
}
int skim_wf_get_palette(void) { return g_sel; }

void skim_wf_palette_rgb(double t, double *r, double *g, double *b) {
  const Palette *p = &g_palettes[g_sel];
  const PalStop *stop = p->stop;
  const int nstops = p->n;
  t = CLAMP(t, 0.0, 1.0);
  int s = 0;
  while (s < nstops - 2 && t > stop[s + 1].t) { s++; }
  const double t0 = stop[s].t, t1 = stop[s + 1].t;
  const double f = CLAMP((t1 > t0) ? (t - t0) / (t1 - t0) : 0.0, 0.0, 1.0);
  *r = (stop[s].r + f * (stop[s + 1].r - stop[s].r)) / 255.0;
  *g = (stop[s].g + f * (stop[s + 1].g - stop[s].g)) / 255.0;
  *b = (stop[s].b + f * (stop[s + 1].b - stop[s].b)) / 255.0;
}

static void build_lut(void) {
  for (int i = 0; i < 256; i++) {
    double r, g, b;
    skim_wf_palette_rgb(i / 255.0, &r, &g, &b);
    g_lut[i] = 0xFFu << 24 | (guint32)(r * 255) << 16 |
               (guint32)(g * 255) << 8 | (guint32)(b * 255);
  }
  g_lut_ok = TRUE;
}

void skim_wf_set_palette(int idx) {
  if (idx < 0 || idx >= skim_wf_palette_count()) { return; }
  g_sel = idx;
  build_lut();
}

guint32 skim_wf_palette_argb(guint8 idx) {
  if (!g_lut_ok) { build_lut(); }
  return g_lut[idx];
}

/* -------- history --------------------------------------------------------------- */

struct _SkimWfHistory {
  guint8  *data;                               /* max_rows × nbins           */
  guint    max_rows, nbins;
  guint    head;                               /* next slot to write         */
  guint    count;                              /* rows stored (≤ max_rows)   */
  double   center_hz, bin_hz;
  double   floor_db;                           /* tracked noise floor        */
  gboolean floor_init;
};

SkimWfHistory *skim_wf_history_new(guint max_rows) {
  SkimWfHistory *h = g_new0(SkimWfHistory, 1);
  h->max_rows = MAX(max_rows, 1u);
  return h;
}

void skim_wf_history_free(SkimWfHistory *h) {
  if (!h) { return; }
  g_free(h->data);
  g_free(h);
}

void skim_wf_history_clear(SkimWfHistory *h) {
  h->head = h->count = 0;
  h->floor_init = FALSE;
}

/* Slot of row r, r = 0 newest. */
static inline const guint8 *row_ptr(const SkimWfHistory *h, guint r) {
  const guint slot = (h->head + h->max_rows - 1 - r) % h->max_rows;
  return h->data + (gsize)slot * h->nbins;
}

void skim_wf_history_push(SkimWfHistory *h, const guint8 *row, guint nbins,
                          double center_hz, double bin_hz) {
  if (nbins == 0) { return; }
  if (nbins != h->nbins || fabs(bin_hz - h->bin_hz) > 1e-9 ||
      fabs(center_hz - h->center_hz) > 0.5) {
    /* The band moved (retune, rate change): old rows sit on other
     * frequencies — drop them rather than draw them in the wrong place. */
    skim_wf_history_clear(h);
    if (nbins != h->nbins) {
      g_free(h->data);
      h->data  = g_malloc((gsize)h->max_rows * nbins);
      h->nbins = nbins;
    }
    h->center_hz = center_hz;
    h->bin_hz    = bin_hz;
  }
  memcpy(h->data + (gsize)h->head * nbins, row, nbins);
  h->head = (h->head + 1) % h->max_rows;
  if (h->count < h->max_rows) { h->count++; }

  /* Noise floor: the SKIM_WF_FLOOR_PCT-th percentile of this row (bytes are
   * already 1 dB steps, so the histogram IS the byte histogram), tracked
   * slowly so the colour mapping does not breathe with every burst. */
  guint hist[256] = { 0 };
  for (guint i = 0; i < nbins; i++) { hist[row[i]]++; }
  const guint target = nbins * SKIM_WF_FLOOR_PCT / 100;
  guint cum = 0, fb = 0;
  for (guint b = 0; b < 256; b++) {
    cum += hist[b];
    if (cum >= target) { fb = b; break; }
  }
  const double noise = (double)fb - SKIM_WF_DB_OFFSET;
  if (!h->floor_init) {
    h->floor_db   = noise;
    h->floor_init = TRUE;
  } else {
    h->floor_db += SKIM_WF_FLOOR_SMOOTH * (noise - h->floor_db);
  }
}

guint  skim_wf_history_rows(const SkimWfHistory *h)      { return h->count; }
guint  skim_wf_history_bins(const SkimWfHistory *h)      { return h->nbins; }
double skim_wf_history_center_hz(const SkimWfHistory *h) { return h->center_hz; }
double skim_wf_history_bin_hz(const SkimWfHistory *h)    { return h->bin_hz; }
double skim_wf_history_lo_hz(const SkimWfHistory *h) {
  return h->center_hz - (double)h->nbins / 2.0 * h->bin_hz;
}
double skim_wf_history_hi_hz(const SkimWfHistory *h) {
  return h->center_hz + (double)h->nbins / 2.0 * h->bin_hz;
}
double skim_wf_history_floor_db(const SkimWfHistory *h) {
  return h->floor_init ? h->floor_db : -120.0;
}

/* -------- mapping ---------------------------------------------------------------- */

double skim_wf_y_of_hz(const SkimWfWindow *win, int hgt, double hz) {
  const double span = win->f_bot_hz - win->f_top_hz;         /* negative     */
  return (hz - win->f_top_hz) / span * (double)hgt;
}

double skim_wf_hz_of_y(const SkimWfWindow *win, int hgt, double y) {
  return win->f_top_hz + (win->f_bot_hz - win->f_top_hz) * (y / (double)hgt);
}

/* -------- compose ---------------------------------------------------------------- */

void skim_wf_compose(const SkimWfHistory *h, const SkimWfWindow *win,
                     guint32 *pix, int w, int hgt, int cols) {
  if (!g_lut_ok) { build_lut(); }
  const guint32 floor_argb = g_lut[0];
  cols = CLAMP(cols, 0, w);
  const int x0 = w - cols;

  if (!h || h->count == 0 || h->nbins == 0 || hgt <= 0) {
    for (int y = 0; y < hgt; y++) {
      for (int x = x0; x < w; x++) { pix[(gsize)y * w + x] = floor_argb; }
    }
    return;
  }

  /* Per pixel row: the bin range [b0, b1) it covers. Zoomed in past the
   * bin width the range collapses to the ONE nearest bin; zoomed out it
   * spans several and the pixel takes their maximum. Bin k is CENTRED on
   * offset (k − nbins/2) · bin_hz and extends half a bin either side — hence
   * the + 0.5 — otherwise every line lands half a bin high (gate-caught). */
  int *b0 = g_new(int, hgt), *b1 = g_new(int, hgt);
  const double half = (double)h->nbins / 2.0 + 0.5;
  for (int y = 0; y < hgt; y++) {
    const double hz_hi = skim_wf_hz_of_y(win, hgt, (double)y);
    const double hz_lo = skim_wf_hz_of_y(win, hgt, (double)y + 1.0);
    const double fb_lo = (hz_lo - h->center_hz) / h->bin_hz + half;
    const double fb_hi = (hz_hi - h->center_hz) / h->bin_hz + half;
    int lo = (int)floor(fb_lo), hi = (int)ceil(fb_hi);
    if (hi <= lo + 1) {                          /* < 1 bin per pixel        */
      lo = (int)floor((fb_lo + fb_hi) * 0.5);
      hi = lo + 1;
    }
    lo = MAX(lo, 0);
    hi = MIN(hi, (int)h->nbins);
    if (hi <= lo) { b0[y] = -1; b1[y] = -1; }    /* outside the band         */
    else          { b0[y] = lo; b1[y] = hi; }
  }

  const double low   = skim_wf_history_floor_db(h);
  const double scale = 255.0 / SKIM_WF_SPAN_DB;
  const guint  rpp   = MAX(win->rows_per_px, 1u);

  for (int x = x0; x < w; x++) {
    const guint col_from_right = (guint)(w - 1 - x);
    const guint r0 = col_from_right * rpp;
    if (r0 >= h->count) {                        /* older than the history   */
      for (int y = 0; y < hgt; y++) { pix[(gsize)y * w + x] = floor_argb; }
      continue;
    }
    const guint r1 = MIN(r0 + rpp, h->count);
    for (int y = 0; y < hgt; y++) {
      if (b0[y] < 0) { pix[(gsize)y * w + x] = floor_argb; continue; }
      guint8 v = 0;
      for (guint r = r0; r < r1; r++) {
        const guint8 *row = row_ptr(h, r);
        for (int b = b0[y]; b < b1[y]; b++) { if (row[b] > v) { v = row[b]; } }
      }
      int idx = (int)(((double)v - SKIM_WF_DB_OFFSET - low) * scale);
      idx = CLAMP(idx, 0, 255);
      pix[(gsize)y * w + x] = g_lut[idx];
    }
  }
  g_free(b0);
  g_free(b1);
}
