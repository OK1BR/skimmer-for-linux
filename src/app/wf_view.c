/*
 * wf_view.c — see wf_view.h.
 *
 * Rendering follows sdr-for-linux's gui.c: the composed pixels go up as a
 * GdkMemoryTexture (NEAREST — keying streaks stay crisp), the scale, the
 * marker and the labels are drawn with Cairo on top. The texture is rebuilt
 * only when the pixels changed: new rows shift the picture left and compose
 * just the new columns; a pan, a zoom, a resize or a drifted noise floor
 * recompose everything.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wf_view.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "spot_out.h"
#include "wf_compose.h"

#define SCALE_W        46     /* kHz scale strip                            */
#define COLUMN_W      180     /* callsign column                            */
#define COL_RAIL_X      9     /* the column's vertical rail (dots ride it)  */
#define COL_TEXT_X     26     /* label text starts here                     */
#define LABEL_FONT   "Sans 9"
#define CLICK_SLOP_PX   4     /* press→release travel that still clicks     */
#define MIN_SPAN_HZ  2000.0
#define DEF_SPAN_HZ 20000.0
#define REFLOOR_DB    1.0     /* floor drift that forces a full recompose   */

struct _SkimWfView {
  GtkWidget      parent_instance;
  SkimWfHistory *hist;
  SkimWfDelay   *delay;                        /* label lag → true centre    */
  double         centre_hz;                    /* visible window centre      */
  double         span_hz;
  gboolean       have_centre;
  double         vfo_hz;
  guint          rows_per_px;
  gboolean       dragging;                     /* scale strip grabbed        */
  double         drag_centre0;                 /* centre when the grab began */

  SkimWfStation *st;                           /* callsign column snapshot   */
  guint          nst;
  SkimWfLabel   *lab;                          /* nst entries, index-aligned */
  gboolean       labels_stale;
  int            lab_h;                        /* height the layout is for   */
  double         lab_pitch;                    /* label pitch of that layout */
  int            hover;                        /* label under the pointer/−1 */
  double         press_x, press_y;             /* where the primary went down*/
  SkimWfViewClickCb click_cb;
  gpointer       click_user;

  guint32       *pix;                          /* wf_w × h                   */
  int            pix_w, pix_h;
  GdkTexture    *tex;
  gboolean       tex_stale;
  gboolean       need_full;
  guint          pending_rows;                 /* rows since the last compose */
  double         floor_at_full;
};

G_DEFINE_FINAL_TYPE(SkimWfView, skim_wf_view, GTK_TYPE_WIDGET)

/* --- geometry ------------------------------------------------------------------- */

static void clamp_window(SkimWfView *v) {
  if (skim_wf_history_rows(v->hist) == 0) { return; }
  const double lo = skim_wf_history_lo_hz(v->hist);   /* the CURRENT band   */
  const double hi = skim_wf_history_hi_hz(v->hist);
  v->span_hz = CLAMP(v->span_hz, MIN_SPAN_HZ, hi - lo);
  /* Only the window's CENTRE must stay inside the band: a retune that
   * drags the band edge past a standing window must not drag the window
   * along with it (the half outside the band simply shows floor). */
  v->centre_hz = CLAMP(v->centre_hz, lo, hi);
  v->labels_stale = TRUE;                      /* the column follows the
                                                * window                     */
}

static void window_of(const SkimWfView *v, SkimWfWindow *win) {
  win->f_top_hz    = v->centre_hz + v->span_hz / 2.0;
  win->f_bot_hz    = v->centre_hz - v->span_hz / 2.0;
  win->rows_per_px = v->rows_per_px;
}

static int wf_width(const SkimWfView *v) {
  const int w = gtk_widget_get_width(GTK_WIDGET(v)) - SCALE_W - COLUMN_W;
  return MAX(w, 8);
}

/* --- data in ---------------------------------------------------------------------- */

/* A row placed on its true centre (the label delay line's output). */
static void view_commit_row(SkimWfView *v, const guint8 *row, guint nbins,
                            double center_hz, double bin_hz) {
  const gboolean fresh = skim_wf_history_rows(v->hist) == 0 ||
                         nbins != skim_wf_history_bins(v->hist);
  const double old_c = skim_wf_history_center_hz(v->hist);
  skim_wf_history_push(v->hist, row, nbins, center_hz, bin_hz);
  const double lo = skim_wf_history_lo_hz(v->hist);
  const double hi = skim_wf_history_hi_hz(v->hist);
  if (fresh || !v->have_centre) {
    /* First row (or a rate change emptied the history): start on the VFO
     * if we know it, else on the band centre. */
    v->centre_hz   = v->vfo_hz > 0 ? v->vfo_hz : center_hz;
    v->have_centre = TRUE;
    clamp_window(v);
    v->need_full = TRUE;
  } else if (fabs(center_hz - old_c) > 0.5) {
    /* The band moved under a standing window (sdr-for-linux has no CTUN —
     * every retune moves the IQ centre). The window STAYS in absolute
     * frequency; old rows keep their place, new ones land shifted. Only a
     * band change that leaves the window with no overlap at all recentres
     * it on the VFO — there is nothing left to look at otherwise. */
    const double top = v->centre_hz + v->span_hz / 2.0;
    const double bot = v->centre_hz - v->span_hz / 2.0;
    if (bot > hi || top < lo) {
      v->centre_hz = v->vfo_hz > 0 ? v->vfo_hz : center_hz;
      clamp_window(v);
      v->need_full = TRUE;                     /* the window moved            */
    }
    /* Otherwise NOTHING already drawn changes: every stored row keeps its
     * own shift and the window stands still, so the new rows simply land
     * shifted. (A full recompose here used to ride the settle drop — once
     * per retune; without the drop it would run per row while the knob
     * turns, 94 × 5 ms a second on the main thread.) */
  }
  v->pending_rows++;
  v->tex_stale = TRUE;
}

/* SKIM_WF_DEBUG: measure how many rows the centre LABEL leads the DATA by.
 * A retune episode opens on the first label change and keeps the last row
 * BEFORE it as the reference. For every following row the probe correlates
 * the row against the reference at the shift each candidate lag L implies
 * (the label current L rows earlier minus the reference label, in bins) and
 * reports the best L — the residual against the configured lag is what a
 * tooth would be. It also tracks the plain flip: the first row that matches
 * the reference better at the full label step than at zero shift, i.e. the
 * row the data arrived on (the midpoint of the 4-row window transition). The
 * episode closes once the label has stood still for LMAX + 4 rows or after
 * 300 rows, with one summary line. Any step size works — the old ±128-bin
 * cross-correlation clipped on every step that mattered (192…785 bins). A
 * fixed station sits at bin (f − centre)/bin + N/2, so a centre step of +S
 * bins moves it S bins DOWN: row[i] ≈ ref[i + S]. */
#define DBG_LMAX  16
#define DBG_RING  (DBG_LMAX + 1)
static double dbg_corr(const guint8 *a, const guint8 *b, guint n, int lag) {
  const int i0 = MAX(0, -lag), i1 = MIN((int)n, (int)n - lag);
  if (i1 - i0 < 64) { return -2.0; }
  double ma = 0, mb = 0;
  for (int i = i0; i < i1; i++) { ma += a[i]; mb += b[i + lag]; }
  const double cnt = (double)(i1 - i0);
  ma /= cnt; mb /= cnt;
  double sab = 0, saa = 0, sbb = 0;
  for (int i = i0; i < i1; i++) {
    const double da = (double)a[i] - ma, db = (double)b[i + lag] - mb;
    sab += da * db; saa += da * da; sbb += db * db;
  }
  return (saa > 0 && sbb > 0) ? sab / sqrt(saa * sbb) : -2.0;
}

static void debug_lag(const guint8 *row, guint nbins, double center_hz, double bin_hz,
                      guint cfg_lag) {
  static guint8 *ref, *prev; static guint pn;
  static double labels[DBG_RING]; static guint64 seq; static double prev_label;
  static gboolean active; static guint64 ep_start, ep_last_change;
  static double ref_label; static int flip;
  if (pn != nbins) {
    g_free(ref); g_free(prev);
    ref = g_malloc(nbins); prev = g_malloc(nbins);
    pn = nbins; seq = 0; active = FALSE;
  }
  labels[seq % DBG_RING] = center_hz;
  const gboolean changed = seq > 0 && fabs(center_hz - prev_label) > 0.5;
  if (!active && changed) {
    active = TRUE; ep_start = ep_last_change = seq; ref_label = prev_label; flip = -1;
    memcpy(ref, prev, nbins);
  }
  if (active) {
    if (changed) { ep_last_change = seq; }
    const int step = (int)lrint((center_hz - ref_label) / bin_hz);
    int lags[DBG_LMAX + 1]; double corr[DBG_LMAX + 1];
    int best = 0; double best_c = -3.0;
    for (int L = 0; L <= DBG_LMAX; L++) {
      const double lab = (seq >= ep_start + (guint64)L) ? labels[(seq - (guint64)L) % DBG_RING] : ref_label;
      lags[L] = (int)lrint((lab - ref_label) / bin_hz);
      corr[L] = -3.0;
      for (int k = 0; k < L; k++) { if (lags[k] == lags[L]) { corr[L] = corr[k]; break; } }
      if (corr[L] <= -3.0) { corr[L] = dbg_corr(row, ref, nbins, lags[L]); }
      if (corr[L] > best_c) { best_c = corr[L]; best = L; }   /* ties → smallest L */
    }
    const double c0 = dbg_corr(row, ref, nbins, 0);
    const double cS = step ? dbg_corr(row, ref, nbins, step) : c0;
    if (flip < 0 && step != 0 && cS > c0) { flip = (int)(seq - ep_start); }
    g_message("wf: row %" G_GUINT64_FORMAT " +%d: label step %+d bins, best lag L=%d (shift %+d, r %.2f), "
              "cfg L=%u (shift %+d, r %.2f), r0 %.2f rS %.2f",
              seq, (int)(seq - ep_start), step, best, lags[best], best_c,
              cfg_lag, lags[MIN(cfg_lag, (guint)DBG_LMAX)], corr[MIN(cfg_lag, (guint)DBG_LMAX)], c0, cS);
    if (seq - ep_last_change > DBG_LMAX + 4 || seq - ep_start > 300) {
      g_message("wf: retune episode rows %" G_GUINT64_FORMAT "..%" G_GUINT64_FORMAT ": label step %+d bins, "
                "data flipped %d rows after the first label change (cfg lag %u)",
                ep_start, seq, step, flip, cfg_lag);
      active = FALSE;
    }
  }
  prev_label = center_hz;
  memcpy(prev, row, nbins);
  seq++;
}

void skim_wf_view_push(SkimWfView *v, const guint8 *row, guint nbins,
                       double center_hz, double bin_hz) {
  static int dbg = -1;
  if (dbg < 0) { dbg = g_getenv("SKIM_WF_DEBUG") != NULL; }
  if (dbg) { debug_lag(row, nbins, center_hz, bin_hz, skim_wf_delay_lag(v->delay)); }
  const double placed = skim_wf_delay_push(v->delay, center_hz, bin_hz);
  view_commit_row(v, row, nbins, placed, bin_hz);
}

void skim_wf_view_set_vfo(SkimWfView *v, double hz) {
  if (hz == v->vfo_hz) { return; }
  v->vfo_hz = hz;                              /* marker only — the window
                                                * stays where the operator
                                                * put it                     */
  gtk_widget_queue_draw(GTK_WIDGET(v));
}

void skim_wf_view_set_span(SkimWfView *v, double span_hz) {
  v->span_hz = span_hz;
  clamp_window(v);
  v->need_full = TRUE;
  v->tex_stale = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(v));
}

double skim_wf_view_span(const SkimWfView *v) { return v->span_hz; }

void skim_wf_view_set_palette(SkimWfView *v, int idx) {
  skim_wf_set_palette(idx);                    /* process-wide LUT           */
  v->need_full = TRUE;                         /* recolour the whole history */
  v->tex_stale = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(v));
}

/* --- callsign column: data + layout ----------------------------------------------- */

void skim_wf_view_set_stations(SkimWfView *v, const SkimWfStation *st, guint n) {
  g_free(v->st);
  v->st  = n ? g_memdup2(st, (gsize)n * sizeof(*st)) : NULL;
  v->nst = n;
  v->labels_stale = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(v));
}

void skim_wf_view_set_click_cb(SkimWfView *v, SkimWfViewClickCb cb, gpointer user) {
  v->click_cb   = cb;
  v->click_user = user;
}

static PangoFontDescription *label_font(gboolean bold) {
  PangoFontDescription *fd = pango_font_description_from_string(LABEL_FONT);
  if (bold) { pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD); }
  return fd;
}

/* One line of label text plus a pixel of air — the seat pitch. Measured
 * off the widget's own Pango context, so a theme font change re-seats. */
static double label_pitch(SkimWfView *v) {
  PangoLayout *lay = gtk_widget_create_pango_layout(GTK_WIDGET(v), "CQ OK1BR/P");
  PangoFontDescription *fd = label_font(TRUE);
  pango_layout_set_font_description(lay, fd);
  pango_font_description_free(fd);
  int tw, th;
  pango_layout_get_pixel_size(lay, &tw, &th);
  g_object_unref(lay);
  return th + 1.0;
}

/* Seat the labels for the current window and height (wf_compose.c does the
 * geometry: frequency order kept, crowds fanned apart, edges respected,
 * the weakest hidden when the column is over capacity). Only when the
 * snapshot or the window changed — never per frame. */
static void layout_labels(SkimWfView *v, int H) {
  if (!v->labels_stale && v->lab_h == H && v->lab) { return; }
  g_free(v->lab);
  v->lab = g_new0(SkimWfLabel, MAX(v->nst, 1u));
  SkimWfWindow win;
  window_of(v, &win);
  for (guint i = 0; i < v->nst; i++) {
    const SkimWfStation *s = &v->st[i];
    v->lab[i].anchor_y = skim_wf_y_of_hz(&win, H, s->hz);
    /* Seat priority: the fixed station always, then callers, then SNR. */
    v->lab[i].prio = (s->tuned ? 100000 : 0) + (s->cq ? 1000 : 0) +
                     (int)lrint(CLAMP(s->snr_db, -99.0, 999.0));
  }
  v->lab_pitch = label_pitch(v);
  skim_wf_layout_labels(v->lab, v->nst, v->lab_pitch, (double)H);
  v->lab_h = H;
  v->labels_stale = FALSE;
}

static gboolean in_column(const SkimWfView *v, double x) {
  return x >= wf_width(v) + SCALE_W;
}

/* The label under a widget coordinate, or −1. */
static int label_hit(SkimWfView *v, double x, double y) {
  if (!in_column(v, x) || v->nst == 0 || !v->have_centre ||
      skim_wf_history_rows(v->hist) == 0) {
    return -1;
  }
  layout_labels(v, gtk_widget_get_height(GTK_WIDGET(v)));
  return skim_wf_label_at(v->lab, v->nst, v->lab_pitch, y);
}

/* --- wheel: pan / zoom ------------------------------------------------------------ */

static gboolean on_scroll(GtkEventControllerScroll *ctl, double dx, double dy,
                          gpointer user) {
  (void)dx;
  SkimWfView *v = user;
  const GdkModifierType st = gtk_event_controller_get_current_event_state(
      GTK_EVENT_CONTROLLER(ctl));
  if (st & GDK_CONTROL_MASK) {
    v->span_hz *= pow(1.25, dy);               /* wheel down = zoom out      */
  } else {
    v->centre_hz -= dy * v->span_hz * 0.1;     /* wheel down = lower freqs   */
  }
  clamp_window(v);
  v->need_full = TRUE;
  v->tex_stale = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(v));
  return TRUE;
}

/* --- grab the scale strip and drag it ----------------------------------------------- */

static gboolean in_scale(const SkimWfView *v, double x) {
  const int wf_w = wf_width(v);
  return x >= wf_w && x < wf_w + SCALE_W;
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer user) {
  (void)g; (void)y;
  SkimWfView *v = user;
  v->dragging = in_scale(v, x) && skim_wf_history_rows(v->hist) > 0;
  v->drag_centre0 = v->centre_hz;
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer user) {
  (void)g; (void)dx;
  SkimWfView *v = user;
  if (!v->dragging) { return; }
  /* The label under the pointer travels WITH the pointer: dragging the axis
   * down by dy px raises the frequency at any fixed pixel by dy · span / h,
   * i.e. the centre goes up by that much. */
  const int h = gtk_widget_get_height(GTK_WIDGET(v));
  if (h <= 0) { return; }
  v->centre_hz = v->drag_centre0 + dy * v->span_hz / (double)h;
  clamp_window(v);
  v->need_full = TRUE;
  v->tex_stale = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(v));
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer user) {
  (void)g; (void)dx; (void)dy;
  SkimWfView *v = user;
  v->dragging = FALSE;
}

static void on_motion(GtkEventControllerMotion *m, double x, double y, gpointer user) {
  (void)m;
  SkimWfView *v = user;
  const int hit = label_hit(v, x, y);
  if (hit != v->hover) {
    v->hover = hit;
    gtk_widget_queue_draw(GTK_WIDGET(v));
  }
  gtk_widget_set_cursor_from_name(GTK_WIDGET(v),
                                  hit >= 0 ? "pointer" : in_scale(v, x) ? "grab" : NULL);
}

static void on_leave(GtkEventControllerMotion *m, gpointer user) {
  (void)m;
  SkimWfView *v = user;
  if (v->hover >= 0) {
    v->hover = -1;
    gtk_widget_queue_draw(GTK_WIDGET(v));
  }
}

/* --- click a callsign ------------------------------------------------------------- */

static void on_press(GtkGestureClick *g, gint n_press, double x, double y, gpointer user) {
  (void)g; (void)n_press;
  SkimWfView *v = user;
  v->press_x = x;
  v->press_y = y;
}

/* Fires on release like the decode pane's click: a press that travelled
 * (a drag through the column) is not a click. */
static void on_release(GtkGestureClick *g, gint n_press, double x, double y, gpointer user) {
  (void)g;
  SkimWfView *v = user;
  if (n_press != 1 || !v->click_cb) { return; }
  if (fabs(x - v->press_x) > CLICK_SLOP_PX || fabs(y - v->press_y) > CLICK_SLOP_PX) { return; }
  const int hit = label_hit(v, x, y);
  if (hit < 0) { return; }
  v->click_cb(v->st[hit].call, v->st[hit].hz, v->click_user);
}

/* --- rendering -------------------------------------------------------------------- */

static void ensure_pixels(SkimWfView *v, int w, int h) {
  if (v->pix && v->pix_w == w && v->pix_h == h) { return; }
  g_free(v->pix);
  v->pix   = g_new(guint32, (gsize)w * h);
  v->pix_w = w;
  v->pix_h = h;
  v->need_full = TRUE;
}

static void compose(SkimWfView *v) {
  SkimWfWindow win;
  window_of(v, &win);
  const int w = v->pix_w, h = v->pix_h;
  const double floor_db = skim_wf_history_floor_db(v->hist);
  if (!v->need_full && fabs(floor_db - v->floor_at_full) > REFLOOR_DB) {
    v->need_full = TRUE;                       /* colours drifted — repaint  */
  }
  if (v->need_full) {
    skim_wf_compose(v->hist, &win, v->pix, w, h, w);
    v->need_full     = FALSE;
    v->pending_rows  = 0;
    v->floor_at_full = floor_db;
    return;
  }
  const guint rpp  = MAX(v->rows_per_px, 1u);
  const int   cols = (int)MIN(v->pending_rows / rpp, (guint)w);
  if (cols <= 0) { return; }
  /* Scroll left by `cols`, then compose the fresh columns on the right. */
  for (int y = 0; y < h; y++) {
    guint32 *line = v->pix + (gsize)y * w;
    memmove(line, line + cols, (gsize)(w - cols) * sizeof(guint32));
  }
  skim_wf_compose(v->hist, &win, v->pix, w, h, cols);
  v->pending_rows -= (guint)cols * rpp;
}

static void draw_scale(SkimWfView *v, cairo_t *cr, int x, int h, const GdkRGBA *fg) {
  SkimWfWindow win;
  window_of(v, &win);
  cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.08);
  cairo_rectangle(cr, x, 0, SCALE_W, h);
  cairo_fill(cr);

  /* Tick step: the smallest of the ladder with ≥ 26 px between ticks. */
  static const double steps[] = { 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000 };
  double step = steps[G_N_ELEMENTS(steps) - 1];
  for (guint i = 0; i < G_N_ELEMENTS(steps); i++) {
    if (steps[i] / v->span_hz * h >= 26.0) { step = steps[i]; break; }
  }
  PangoLayout *lay = gtk_widget_create_pango_layout(GTK_WIDGET(v), NULL);
  PangoFontDescription *fd = pango_font_description_from_string("Sans 9");
  pango_layout_set_font_description(lay, fd);
  pango_font_description_free(fd);

  cairo_set_line_width(cr, 1.0);
  const double first = ceil(win.f_bot_hz / step) * step;
  for (double f = first; f <= win.f_top_hz; f += step) {
    const double y = floor(skim_wf_y_of_hz(&win, h, f)) + 0.5;
    const gboolean major = fmod(f, 1000.0) < 1e-6 || fmod(f, 1000.0) > 1000.0 - 1e-6;
    cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, major ? 0.9 : 0.5);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + (major ? 8 : 5), y);
    cairo_stroke(cr);
    char txt[16];
    if (step >= 1000.0) {
      g_snprintf(txt, sizeof(txt), "%03d", (int)lrint(f / 1000.0) % 1000);
    } else {
      const double khz = f / 1000.0;
      g_snprintf(txt, sizeof(txt), "%03d.%d", (int)floor(khz) % 1000,
                 (int)lrint((khz - floor(khz)) * 10.0) % 10);
    }
    pango_layout_set_text(lay, txt, -1);
    int tw, th;
    pango_layout_get_pixel_size(lay, &tw, &th);
    cairo_move_to(cr, x + 10, y - th / 2.0);
    pango_cairo_show_layout(cr, lay);
  }
  g_object_unref(lay);
}

static void draw_marker(SkimWfView *v, cairo_t *cr, int wf_w, int h) {
  if (v->vfo_hz <= 0) { return; }
  SkimWfWindow win;
  window_of(v, &win);
  const double y = floor(skim_wf_y_of_hz(&win, h, v->vfo_hz)) + 0.5;
  /* Spot green (SKIM_SPOT_ARGB family colour). */
  cairo_set_source_rgba(cr, 0.19, 0.75, 0.38, 0.9);
  if (y < 0 || y > h) {
    /* The radio sits outside the window: an arrow on the scale strip says
     * which way — the window itself never chases the VFO. */
    const double ax = wf_w + SCALE_W / 2.0;
    if (y < 0) {
      cairo_move_to(cr, ax, 3); cairo_line_to(cr, ax - 6, 12); cairo_line_to(cr, ax + 6, 12);
    } else {
      cairo_move_to(cr, ax, h - 3); cairo_line_to(cr, ax - 6, h - 12); cairo_line_to(cr, ax + 6, h - 12);
    }
    cairo_close_path(cr);
    cairo_fill(cr);
    return;
  }
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, 0, y);
  cairo_line_to(cr, wf_w, y);
  cairo_stroke(cr);
  cairo_move_to(cr, wf_w, y);
  cairo_line_to(cr, wf_w + 7, y - 5);
  cairo_line_to(cr, wf_w + 7, y + 5);
  cairo_close_path(cr);
  cairo_fill(cr);
}

/* The callsign column (CW Skimmer's): a rail down the column's left edge, a
 * dot on it at every station's frequency, the callsign beside it — "CQ "
 * prefixed for callers — and, where the seat had to move off the frequency,
 * a connector from the dot to the text. Label colours are the spot colours
 * (bright = call it, gray = the logbook says no), the fixed station bold,
 * the label under the pointer on a faint backdrop. */
static void draw_labels(SkimWfView *v, cairo_t *cr, int x0, int H, const GdkRGBA *fg) {
  layout_labels(v, H);
  const double rail = x0 + COL_RAIL_X + 0.5;
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.22);
  cairo_move_to(cr, rail, 0);
  cairo_line_to(cr, rail, H);
  cairo_stroke(cr);
  if (v->nst == 0) { return; }

  PangoLayout *lay = gtk_widget_create_pango_layout(GTK_WIDGET(v), NULL);
  PangoFontDescription *fd_reg = label_font(FALSE), *fd_bold = label_font(TRUE);
  const guint32 argb_ok = SKIM_SPOT_ARGB, argb_gray = SKIM_SPOT_ARGB_DUP;
  for (guint i = 0; i < v->nst; i++) {
    const SkimWfStation *s = &v->st[i];
    const SkimWfLabel   *l = &v->lab[i];
    const double ay = l->anchor_y;
    if (ay < -4 || ay > H + 4) { continue; }
    /* The dot: CW Skimmer's yellow, outlined so it reads on a light theme. */
    cairo_arc(cr, rail, ay, 3.0, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 0.96, 0.82, 0.18);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_stroke(cr);
    if (l->y < 0) { continue; }                /* over capacity: dot only    */

    const guint32 argb = s->gray ? argb_gray : argb_ok;
    const double r = ((argb >> 16) & 255) / 255.0, g = ((argb >> 8) & 255) / 255.0,
                 b = (argb & 255) / 255.0;
    pango_layout_set_font_description(lay, s->tuned ? fd_bold : fd_reg);
    char txt[24];
    g_snprintf(txt, sizeof(txt), "%s%s", s->cq ? "CQ " : "", s->call);
    pango_layout_set_text(lay, txt, -1);
    int tw, th;
    pango_layout_get_pixel_size(lay, &tw, &th);
    const double tx = x0 + COL_TEXT_X, ty = l->y - th / 2.0;
    if ((int)i == v->hover) {
      cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.12);
      cairo_rectangle(cr, tx - 3, floor(ty) - 1, tw + 6, th + 2);
      cairo_fill(cr);
    }
    /* Connector dot → text: flat on the frequency, slanted when seated
     * elsewhere. */
    cairo_set_source_rgba(cr, r, g, b, 0.75);
    cairo_move_to(cr, rail + 4, floor(ay) + 0.5);
    cairo_line_to(cr, tx - 4, floor(l->y) + 0.5);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, lay);
  }
  pango_font_description_free(fd_reg);
  pango_font_description_free(fd_bold);
  g_object_unref(lay);
}

static void skim_wf_view_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  SkimWfView *v = SKIM_WF_VIEW(widget);
  const int W = gtk_widget_get_width(widget), H = gtk_widget_get_height(widget);
  if (W <= 0 || H <= 0) { return; }
  const int wf_w = wf_width(v);

  ensure_pixels(v, wf_w, H);
  if (v->tex_stale || !v->tex) {
    compose(v);
    g_clear_object(&v->tex);
    GBytes *bytes = g_bytes_new(v->pix, (gsize)wf_w * H * sizeof(guint32));
    v->tex = gdk_memory_texture_new(wf_w, H, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                    bytes, (gsize)wf_w * sizeof(guint32));
    g_bytes_unref(bytes);
    v->tex_stale = FALSE;
  }
  const graphene_rect_t wf_rect = GRAPHENE_RECT_INIT(0, 0, (float)wf_w, (float)H);
  gtk_snapshot_append_scaled_texture(snapshot, v->tex, GSK_SCALING_FILTER_NEAREST,
                                     &wf_rect);

  GdkRGBA fg;
  gtk_widget_get_color(widget, &fg);
  const graphene_rect_t all = GRAPHENE_RECT_INIT(0, 0, (float)W, (float)H);
  cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &all);
  if (skim_wf_history_rows(v->hist) > 0) {
    draw_scale(v, cr, wf_w, H, &fg);
    draw_marker(v, cr, wf_w, H);
  } else {
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, "waiting for IQ…");
    int tw, th;
    pango_layout_get_pixel_size(lay, &tw, &th);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.6);
    cairo_move_to(cr, (wf_w - tw) / 2.0, (H - th) / 2.0);
    pango_cairo_show_layout(cr, lay);
    g_object_unref(lay);
  }
  /* Callsign column. */
  cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.04);
  cairo_rectangle(cr, wf_w + SCALE_W, 0, COLUMN_W, H);
  cairo_fill(cr);
  if (skim_wf_history_rows(v->hist) > 0 && v->have_centre) {
    draw_labels(v, cr, wf_w + SCALE_W, H, &fg);
  }
  cairo_destroy(cr);
}

/* --- GObject ---------------------------------------------------------------------- */

static void skim_wf_view_dispose(GObject *obj) {
  SkimWfView *v = SKIM_WF_VIEW(obj);
  g_clear_object(&v->tex);
  g_clear_pointer(&v->delay, skim_wf_delay_free);
  g_clear_pointer(&v->hist, skim_wf_history_free);
  g_clear_pointer(&v->pix, g_free);
  g_clear_pointer(&v->st, g_free);
  g_clear_pointer(&v->lab, g_free);
  G_OBJECT_CLASS(skim_wf_view_parent_class)->dispose(obj);
}

static void skim_wf_view_class_init(SkimWfViewClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  wc->snapshot = skim_wf_view_snapshot;
  G_OBJECT_CLASS(klass)->dispose = skim_wf_view_dispose;
  gtk_widget_class_set_css_name(wc, "skimwaterfall");
}

/* SKIM_WF_LAG_ROWS=<n> overrides the label lag for a measurement session. */
static guint label_lag_rows(void) {
  const char *e = g_getenv("SKIM_WF_LAG_ROWS");
  if (e && *e) {
    char *end = NULL;
    const long n = strtol(e, &end, 10);
    if (end && *end == '\0' && n >= 0) { return MIN((guint)n, (guint)SKIM_WF_LABEL_LAG_MAX); }
    g_warning("wf: SKIM_WF_LAG_ROWS=\"%s\" ignored (want 0..%d)", e, SKIM_WF_LABEL_LAG_MAX);
  }
  return SKIM_WF_LABEL_LAG;
}

static void skim_wf_view_init(SkimWfView *v) {
  v->hist        = skim_wf_history_new(SKIM_WF_HISTORY_ROWS);
  v->delay       = skim_wf_delay_new(label_lag_rows());
  v->span_hz     = DEF_SPAN_HZ;
  v->rows_per_px = SKIM_WF_ROWS_PER_PX;
  v->need_full   = TRUE;
  v->tex_stale   = TRUE;
  v->hover       = -1;
  v->labels_stale = TRUE;
  gtk_widget_set_hexpand(GTK_WIDGET(v), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(v), TRUE);
  gtk_widget_set_size_request(GTK_WIDGET(v), SCALE_W + COLUMN_W + 200, 160);
  GtkEventController *sc = gtk_event_controller_scroll_new(
      GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
  g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), v);
  gtk_widget_add_controller(GTK_WIDGET(v), sc);
  GtkGesture *drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), v);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), v);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), v);
  gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(drag));
  GtkEventController *mo = gtk_event_controller_motion_new();
  g_signal_connect(mo, "motion", G_CALLBACK(on_motion), v);
  g_signal_connect(mo, "leave", G_CALLBACK(on_leave), v);
  gtk_widget_add_controller(GTK_WIDGET(v), mo);
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_press), v);
  g_signal_connect(click, "released", G_CALLBACK(on_release), v);
  gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(click));
}

GtkWidget *skim_wf_view_new(void) {
  return g_object_new(SKIM_TYPE_WF_VIEW, NULL);
}
