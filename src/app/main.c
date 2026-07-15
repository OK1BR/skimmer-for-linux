/* main.c — skimmer-for-linux GTK4/libadwaita front-end (M5).
 *
 * A light station list + decode log over the headless engine pipeline:
 * header bar with host + connect toggle, a sorted GtkColumnView of stations
 * (call / freq / WPM / SNR / heard count) and a tailing decode log. Engine
 * callbacks fire on the engine thread and are marshalled here with
 * g_idle_add — the engine stays GLib-only (src/engine/ has no GTK).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <adwaita.h>

#include "pipeline.h"

#ifndef SKIMMER_VERSION
#define SKIMMER_VERSION "0.0.0"
#endif

#define LOG_MAX_LINES 2000

/* --- station row object --------------------------------------------------------- */

#define SKIM_TYPE_ROW (skim_row_get_type())
G_DECLARE_FINAL_TYPE(SkimRow, skim_row, SKIM, ROW, GObject)

struct _SkimRow {
  GObject parent_instance;
  SkimStation st;
};
G_DEFINE_TYPE(SkimRow, skim_row, G_TYPE_OBJECT)
static void skim_row_class_init(SkimRowClass *k) { (void)k; }
static void skim_row_init(SkimRow *r) { (void)r; }

static SkimRow *skim_row_new(const SkimStation *st) {
  SkimRow *r = g_object_new(SKIM_TYPE_ROW, NULL);
  r->st = *st;
  return r;
}

/* --- app state -------------------------------------------------------------------- */

typedef struct {
  SkimPipeline   *pipeline;
  GListStore     *stations;      /* of SkimRow                                */
  GtkSortListModel *sorted;
  GtkTextBuffer  *log;
  GtkTextView    *log_view;
  GtkEntry       *host_entry;
  GtkToggleButton *connect_btn;
  AdwWindowTitle *title;
  GtkLabel       *status;
  GPtrArray      *monitors;      /* open per-frequency traffic windows        */
} App;

/* --- per-frequency traffic monitor -------------------------------------------------- */

/* Traffic within this many Hz of the monitor belongs to it (matches the
 * station tracker's merge radius, so a monitor follows "its" station). */
#define MONITOR_WINDOW_HZ SKIM_STATION_MERGE_HZ

typedef struct {
  App            *app;
  double          freq_hz;
  char            call[16];
  GtkWindow      *win;
  GtkTextBuffer  *buf;
  GtkTextView    *view;
  AdwWindowTitle *title;
} Monitor;

static gboolean monitor_closed(GtkWindow *win, gpointer user) {
  Monitor *m = user;
  g_ptr_array_remove(m->app->monitors, m);   /* frees m via the free func    */
  (void)win;
  return FALSE;                              /* proceed with the close       */
}

static void monitor_subtitle(Monitor *m, const SkimStation *st) {
  char s[96];
  if (st) {
    g_snprintf(s, sizeof(s), "%.1f kHz · %.0f WPM · %.0f dB · %u×",
               m->freq_hz / 1000.0, st->speed, st->snr_db, st->reports);
  } else {
    g_snprintf(s, sizeof(s), "%.1f kHz", m->freq_hz / 1000.0);
  }
  adw_window_title_set_subtitle(m->title, s);
}

static void monitor_open(App *app, const SkimStation *st) {
  /* One monitor per station — re-activate raises the existing window. */
  for (guint i = 0; i < app->monitors->len; i++) {
    Monitor *m = g_ptr_array_index(app->monitors, i);
    if (g_strcmp0(m->call, st->call) == 0 &&
        ABS(m->freq_hz - st->freq_hz) <= MONITOR_WINDOW_HZ) {
      gtk_window_present(m->win);
      return;
    }
  }
  Monitor *m = g_new0(Monitor, 1);
  m->app     = app;
  m->freq_hz = st->freq_hz;
  g_strlcpy(m->call, st->call, sizeof(m->call));

  GtkWidget *win = adw_window_new();
  m->win = GTK_WINDOW(win);
  gtk_window_set_default_size(m->win, 520, 300);
  m->title = ADW_WINDOW_TITLE(adw_window_title_new(st->call, NULL));
  monitor_subtitle(m, st);
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(m->title));

  m->buf = gtk_text_buffer_new(NULL);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(m->buf, &end);
  gtk_text_buffer_create_mark(m->buf, "tail", &end, FALSE);
  GtkWidget *view = gtk_text_view_new_with_buffer(m->buf);
  m->view = GTK_TEXT_VIEW(view);
  gtk_text_view_set_editable(m->view, FALSE);
  gtk_text_view_set_monospace(m->view, TRUE);
  gtk_text_view_set_cursor_visible(m->view, FALSE);
  gtk_text_view_set_wrap_mode(m->view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(m->view, 8);
  gtk_text_view_set_top_margin(m->view, 8);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
  gtk_widget_set_vexpand(scroll, TRUE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), scroll);
  adw_window_set_content(ADW_WINDOW(win), box);

  g_signal_connect(win, "close-request", G_CALLBACK(monitor_closed), m);
  g_ptr_array_add(app->monitors, m);
  gtk_window_present(m->win);
}

static void monitor_append(Monitor *m, const char *text) {
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(m->buf, &end);
  gtk_text_buffer_insert(m->buf, &end, text, -1);
  if (gtk_text_buffer_get_char_count(m->buf) > 20000) {
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(m->buf, &s);
    gtk_text_buffer_get_iter_at_offset(m->buf, &e, 2000);
    gtk_text_buffer_delete(m->buf, &s, &e);
  }
  gtk_text_buffer_get_end_iter(m->buf, &end);
  GtkTextMark *mark = gtk_text_buffer_get_mark(m->buf, "tail");
  gtk_text_buffer_move_mark(m->buf, mark, &end);
  gtk_text_view_scroll_mark_onscreen(m->view, mark);
}

/* --- marshalled engine events ------------------------------------------------------ */

typedef struct {
  App        *app;
  SkimStation st;
} StationEvent;

static gboolean on_station_idle(gpointer data) {
  StationEvent *ev = data;
  App *app = ev->app;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
  for (guint i = 0; i < n; i++) {
    SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->stations), i);
    gboolean same = g_strcmp0(r->st.call, ev->st.call) == 0 &&
                    ABS(r->st.freq_hz - ev->st.freq_hz) <= SKIM_STATION_MERGE_HZ;
    g_object_unref(r);
    if (same) {
      /* Rows carry no notify — replace to refresh the bound labels. */
      g_list_store_remove(app->stations, i);
      break;
    }
  }
  g_list_store_append(app->stations, skim_row_new(&ev->st));
  /* Keep any open traffic monitor's header stats fresh. */
  for (guint i = 0; i < app->monitors->len; i++) {
    Monitor *m = g_ptr_array_index(app->monitors, i);
    if (g_strcmp0(m->call, ev->st.call) == 0 &&
        ABS(m->freq_hz - ev->st.freq_hz) <= MONITOR_WINDOW_HZ) {
      m->freq_hz = ev->st.freq_hz;             /* follow small drift         */
      monitor_subtitle(m, &ev->st);
    }
  }
  g_free(ev);
  return G_SOURCE_REMOVE;
}

typedef struct {
  App   *app;
  double freq_hz;
  char  *text;
} TextEvent;

static gboolean on_text_idle(gpointer data) {
  TextEvent *ev = data;
  /* Route the channel's text into any monitor watching that frequency. */
  for (guint i = 0; i < ev->app->monitors->len; i++) {
    Monitor *m = g_ptr_array_index(ev->app->monitors, i);
    if (ABS(m->freq_hz - ev->freq_hz) <= MONITOR_WINDOW_HZ) {
      monitor_append(m, ev->text);
    }
  }
  GtkTextBuffer *buf = ev->app->log;
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buf, &end);
  char line[160];
  g_snprintf(line, sizeof(line), "%10.1f  %s\n", ev->freq_hz / 1000.0, ev->text);
  gtk_text_buffer_insert(buf, &end, line, -1);
  int over = gtk_text_buffer_get_line_count(buf) - LOG_MAX_LINES;
  if (over > 0) {
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(buf, &s);
    gtk_text_buffer_get_iter_at_line(buf, &e, over);
    gtk_text_buffer_delete(buf, &s, &e);
  }
  gtk_text_buffer_get_end_iter(buf, &end);
  GtkTextMark *mark = gtk_text_buffer_get_mark(buf, "tail");
  gtk_text_buffer_move_mark(buf, mark, &end);
  gtk_text_view_scroll_mark_onscreen(ev->app->log_view, mark);
  g_free(ev->text);
  g_free(ev);
  return G_SOURCE_REMOVE;
}

typedef struct {
  App     *app;
  gboolean connected;
  char    *detail;
} StateEvent;

static gboolean on_state_idle(gpointer data) {
  StateEvent *ev = data;
  adw_window_title_set_subtitle(ev->app->title, ev->detail);
  g_free(ev->detail);
  g_free(ev);
  return G_SOURCE_REMOVE;
}

/* Engine-thread callbacks: copy + hop to the main loop. */
static void pipe_station_cb(const SkimStation *st, gpointer user) {
  StationEvent *ev = g_new0(StationEvent, 1);
  ev->app = user;
  ev->st  = *st;
  g_idle_add(on_station_idle, ev);
}
static void pipe_text_cb(double freq_hz, const char *text, gpointer user) {
  TextEvent *ev = g_new0(TextEvent, 1);
  ev->app     = user;
  ev->freq_hz = freq_hz;
  ev->text    = g_strdup(text);
  g_idle_add(on_text_idle, ev);
}
static void pipe_state_cb(gboolean connected, const char *detail, gpointer user) {
  StateEvent *ev = g_new0(StateEvent, 1);
  ev->app       = user;
  ev->connected = connected;
  ev->detail    = g_strdup(detail);
  g_idle_add(on_state_idle, ev);
}

/* --- status line -------------------------------------------------------------------- */

static gboolean status_tick(gpointer data) {
  App *app = data;
  if (app->pipeline) {
    char s[128];
    g_snprintf(s, sizeof(s),
               "%u stations · %" G_GUINT64_FORMAT " spots · %" G_GUINT64_FORMAT
               " Mframes",
               skim_pipeline_stations(app->pipeline),
               skim_pipeline_spots(app->pipeline),
               skim_pipeline_frames(app->pipeline) / 1000000);
    gtk_label_set_text(app->status, s);
  } else {
    gtk_label_set_text(app->status, "not connected");
  }
  return G_SOURCE_CONTINUE;
}

/* --- connect toggle ------------------------------------------------------------------- */

static void on_connect_toggled(GtkToggleButton *btn, gpointer user) {
  App *app = user;
  if (gtk_toggle_button_get_active(btn)) {
    const char *host = gtk_editable_get_text(GTK_EDITABLE(app->host_entry));
    char *dict = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                  "master.scp", NULL);
    SkimPipelineConfig cfg = {
      .host = (host && host[0]) ? host : "127.0.0.1",
      .port = 40001,
      .iq_rate = 192000,
      .chan_bw_hz = 125.0,
      .dict_path = g_file_test(dict, G_FILE_TEST_EXISTS) ? dict : NULL,
    };
    app->pipeline = skim_pipeline_new(&cfg);
    g_free(dict);
    skim_pipeline_set_station_cb(app->pipeline, pipe_station_cb, app);
    skim_pipeline_set_text_cb(app->pipeline, pipe_text_cb, app);
    skim_pipeline_set_state_cb(app->pipeline, pipe_state_cb, app);
    GError *err = NULL;
    if (!skim_pipeline_start(app->pipeline, &err)) {
      adw_window_title_set_subtitle(app->title,
                                    err ? err->message : "connect failed");
      g_clear_error(&err);
      g_clear_pointer(&app->pipeline, skim_pipeline_free);
      gtk_toggle_button_set_active(btn, FALSE);
      gtk_button_set_label(GTK_BUTTON(btn), "Connect");
      return;
    }
    gtk_button_set_label(GTK_BUTTON(btn), "Disconnect");
  } else {
    if (app->pipeline) {
      skim_pipeline_stop(app->pipeline);
      g_clear_pointer(&app->pipeline, skim_pipeline_free);
    }
    adw_window_title_set_subtitle(app->title, "disconnected");
    gtk_button_set_label(GTK_BUTTON(btn), "Connect");
  }
}

/* --- column helpers -------------------------------------------------------------------- */

typedef void (*RowToText)(const SkimStation *st, char *out, gsize n);

static void cell_setup(GtkSignalListItemFactory *f, GtkListItem *item, gpointer u) {
  (void)f; (void)u;
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_widget_add_css_class(label, "numeric");
  gtk_list_item_set_child(item, label);
}

static void cell_bind(GtkSignalListItemFactory *f, GtkListItem *item, gpointer u) {
  (void)f;
  RowToText fmt = (RowToText)u;
  SkimRow *r = gtk_list_item_get_item(item);
  char text[64];
  fmt(&r->st, text, sizeof(text));
  gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), text);
}

static void fmt_call(const SkimStation *st, char *o, gsize n) {
  g_strlcpy(o, st->call, n);
}
static void fmt_freq(const SkimStation *st, char *o, gsize n) {
  g_snprintf(o, n, "%.1f", st->freq_hz / 1000.0);
}
static void fmt_wpm(const SkimStation *st, char *o, gsize n) {
  g_snprintf(o, n, "%.0f", st->speed);
}
static void fmt_snr(const SkimStation *st, char *o, gsize n) {
  g_snprintf(o, n, "%.0f dB", st->snr_db);
}
static void fmt_heard(const SkimStation *st, char *o, gsize n) {
  g_snprintf(o, n, "%u×", st->reports);
}

static void add_column(GtkColumnView *view, const char *title, RowToText fmt,
                       gboolean expand) {
  GtkListItemFactory *f = gtk_signal_list_item_factory_new();
  g_signal_connect(f, "setup", G_CALLBACK(cell_setup), NULL);
  g_signal_connect(f, "bind", G_CALLBACK(cell_bind), (gpointer)fmt);
  GtkColumnViewColumn *col = gtk_column_view_column_new(title, f);
  gtk_column_view_column_set_expand(col, expand);
  gtk_column_view_append_column(view, col);
  g_object_unref(col);
}

static int freq_cmp(gconstpointer a, gconstpointer b, gpointer u) {
  (void)u;
  const SkimRow *ra = a, *rb = b;
  return (ra->st.freq_hz > rb->st.freq_hz) - (ra->st.freq_hz < rb->st.freq_hz);
}

/* Row activated (double-click / Enter) → open the traffic monitor. */
static void on_row_activated(GtkColumnView *view, guint position, gpointer user) {
  (void)view;
  App *app = user;
  SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->sorted), position);
  if (r) {
    monitor_open(app, &r->st);
    g_object_unref(r);
  }
}

/* --- activate ----------------------------------------------------------------------------- */

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
  (void)user_data;
  App *app = g_new0(App, 1);

  GtkWidget *window = adw_application_window_new(gtk_app);
  gtk_window_set_title(GTK_WINDOW(window), "Skimmer for Linux");
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 640);

  app->title = ADW_WINDOW_TITLE(adw_window_title_new("Skimmer for Linux",
                                                     "disconnected"));
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                  GTK_WIDGET(app->title));

  app->host_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(app->host_entry, "127.0.0.1");
  gtk_editable_set_width_chars(GTK_EDITABLE(app->host_entry), 14);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(app->host_entry));

  app->connect_btn = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Connect"));
  gtk_widget_add_css_class(GTK_WIDGET(app->connect_btn), "suggested-action");
  g_signal_connect(app->connect_btn, "toggled", G_CALLBACK(on_connect_toggled), app);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(app->connect_btn));

  /* Station list, sorted by frequency — a text band map. Activate a row
   * (double-click / Enter) to open its traffic monitor. */
  app->monitors = g_ptr_array_new_with_free_func(g_free);
  app->stations = g_list_store_new(SKIM_TYPE_ROW);
  GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(freq_cmp, NULL, NULL));
  app->sorted = gtk_sort_list_model_new(G_LIST_MODEL(app->stations), sorter);
  g_object_ref(app->sorted);
  GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(app->sorted));
  GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(sel));
  gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(view), FALSE);
  g_signal_connect(view, "activate", G_CALLBACK(on_row_activated), app);
  add_column(GTK_COLUMN_VIEW(view), "Call", fmt_call, TRUE);
  add_column(GTK_COLUMN_VIEW(view), "kHz", fmt_freq, FALSE);
  add_column(GTK_COLUMN_VIEW(view), "WPM", fmt_wpm, FALSE);
  add_column(GTK_COLUMN_VIEW(view), "SNR", fmt_snr, FALSE);
  add_column(GTK_COLUMN_VIEW(view), "Heard", fmt_heard, FALSE);
  GtkWidget *list_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll), view);
  gtk_widget_set_vexpand(list_scroll, TRUE);

  /* Decode log. */
  app->log = gtk_text_buffer_new(NULL);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(app->log, &end);
  gtk_text_buffer_create_mark(app->log, "tail", &end, FALSE);
  GtkWidget *log_view = gtk_text_view_new_with_buffer(app->log);
  app->log_view = GTK_TEXT_VIEW(log_view);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
  GtkWidget *log_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_scroll), log_view);
  gtk_widget_set_size_request(log_scroll, -1, 160);

  app->status = GTK_LABEL(gtk_label_new("not connected"));
  gtk_label_set_xalign(app->status, 0.0);
  gtk_widget_add_css_class(GTK_WIDGET(app->status), "dim-label");
  gtk_widget_set_margin_start(GTK_WIDGET(app->status), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(app->status), 4);
  gtk_widget_set_margin_bottom(GTK_WIDGET(app->status), 4);
  g_timeout_add_seconds(1, status_tick, app);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), list_scroll);
  gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(box), log_scroll);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->status));

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
  g_set_application_name("Skimmer for Linux");

  AdwApplication *app =
      adw_application_new("cz.ok1br.SkimmerForLinux", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
