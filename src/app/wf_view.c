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
#include <string.h>

#include "wf_compose.h"

#define SCALE_W        46     /* kHz scale strip                            */
#define COLUMN_W      180     /* callsign column                            */
#define MIN_SPAN_HZ  2000.0
#define DEF_SPAN_HZ 20000.0
#define REFLOOR_DB    1.0     /* floor drift that forces a full recompose   */

struct _SkimWfView {
  GtkWidget      parent_instance;
  SkimWfHistory *hist;
  double         centre_hz;                    /* visible window centre      */
  double         span_hz;
  gboolean       have_centre;
  double         vfo_hz;
  guint          rows_per_px;
  gboolean       dragging;                     /* scale strip grabbed        */
  double         drag_centre0;                 /* centre when the grab began */

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
  const double lo = skim_wf_history_lo_hz(v->hist);
  const double hi = skim_wf_history_hi_hz(v->hist);
  v->span_hz = CLAMP(v->span_hz, MIN_SPAN_HZ, hi - lo);
  v->centre_hz = CLAMP(v->centre_hz, lo + v->span_hz / 2.0, hi - v->span_hz / 2.0);
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

void skim_wf_view_push(SkimWfView *v, const guint8 *row, guint nbins,
                       double center_hz, double bin_hz) {
  const gboolean moved = skim_wf_history_rows(v->hist) == 0 ||
                         fabs(center_hz - skim_wf_history_center_hz(v->hist)) > 0.5 ||
                         nbins != skim_wf_history_bins(v->hist);
  skim_wf_history_push(v->hist, row, nbins, center_hz, bin_hz);
  if (moved) {
    /* First row, or the whole band moved (the radio's DDS centre changed):
     * the old window means nothing now — start on the VFO if we know it,
     * else on the band centre, and repaint from scratch. This is the ONLY
     * time the window moves by itself. */
    v->centre_hz   = v->vfo_hz > 0 ? v->vfo_hz : center_hz;
    v->have_centre = TRUE;
    clamp_window(v);
    v->need_full = TRUE;
  }
  v->pending_rows++;
  v->tex_stale = TRUE;
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
  (void)m; (void)y;
  SkimWfView *v = user;
  gtk_widget_set_cursor_from_name(GTK_WIDGET(v), in_scale(v, x) ? "grab" : NULL);
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
  /* Callsign column background (labels come with the station model). */
  cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.04);
  cairo_rectangle(cr, wf_w + SCALE_W, 0, COLUMN_W, H);
  cairo_fill(cr);
  cairo_destroy(cr);
}

/* --- GObject ---------------------------------------------------------------------- */

static void skim_wf_view_dispose(GObject *obj) {
  SkimWfView *v = SKIM_WF_VIEW(obj);
  g_clear_object(&v->tex);
  g_clear_pointer(&v->hist, skim_wf_history_free);
  g_clear_pointer(&v->pix, g_free);
  G_OBJECT_CLASS(skim_wf_view_parent_class)->dispose(obj);
}

static void skim_wf_view_class_init(SkimWfViewClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  wc->snapshot = skim_wf_view_snapshot;
  G_OBJECT_CLASS(klass)->dispose = skim_wf_view_dispose;
  gtk_widget_class_set_css_name(wc, "skimwaterfall");
}

static void skim_wf_view_init(SkimWfView *v) {
  v->hist        = skim_wf_history_new(SKIM_WF_HISTORY_ROWS);
  v->span_hz     = DEF_SPAN_HZ;
  v->rows_per_px = SKIM_WF_ROWS_PER_PX;
  v->need_full   = TRUE;
  v->tex_stale   = TRUE;
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
  gtk_widget_add_controller(GTK_WIDGET(v), mo);
}

GtkWidget *skim_wf_view_new(void) {
  return g_object_new(SKIM_TYPE_WF_VIEW, NULL);
}
