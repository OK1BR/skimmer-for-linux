/* main.c — skimmer-for-linux GTK4/libadwaita front-end (M5).
 *
 * A light station list over the headless engine pipeline: a sorted
 * GtkColumnView of stations (call / freq / WPM / SNR / heard count) and a
 * bottom pane showing the decode text at the radio's TUNED frequency (the
 * vfo:0,0 the server broadcasts). Decoded text is kept per frequency, so the
 * pane always shows one frequency's history — never a mixed log. Activating
 * a row TUNES the radio there (the only radio state the skimmer ever touches,
 * and only on the user's click). No connect button: a scanner probes
 * <host>:40001 (host set in Preferences, persisted) and the pipeline follows
 * the server up and down automatically. Engine callbacks fire on
 * engine/network threads and are marshalled here with g_idle_add — the
 * engine stays GLib-only (src/engine/ has no GTK).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <adwaita.h>

#include "pipeline.h"

#ifndef SKIMMER_VERSION
#define SKIMMER_VERSION "0.0.0"
#endif

/* Traffic within this many Hz belongs to one frequency slot (the station
 * tracker's merge radius, so a slot follows "its" station). */
#define TUNED_WINDOW_HZ   SKIM_STATION_MERGE_HZ
#define FREQLOG_MAX       256      /* LRU cap on remembered frequency slots  */
#define FREQLOG_CAP_CHARS 20000    /* per-slot history cap                   */

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
  SkimPipeline   *pipeline;      /* running pipeline (owned by main thread)   */
  SkimPipeline   *starting;      /* pipeline mid-handshake on a worker thread */
  char           *host;          /* TCI server host (persisted preference)    */
  gboolean        cq_only;       /* spot only CALLING stations (persisted)    */
  int             decode_font;   /* decode pane font size, pt (persisted)     */
  GtkCssProvider *css;           /* carries the decode pane font rule         */
  gboolean        probing;       /* port probe / handshake in flight          */
  double          vfo_hz;        /* the radio's tuned frequency (vfo:0,0)     */
  GListStore     *stations;      /* of SkimRow                                */
  GtkSortListModel *sorted;
  GtkTextBuffer  *tuned;         /* decode text at the tuned frequency        */
  GtkTextView    *tuned_view;
  GtkLabel       *tuned_label;
  GtkWidget      *tuned_scroll;  /* the decode pane's scroller                */
  GtkWidget      *list_scroll;   /* the station list's scroller               */
  GtkWidget      *list_sep;      /* separator between list and decode pane    */
  gboolean        list_visible;  /* station list shown (persisted); the CW
                                  * decode pane is ALWAYS visible             */
  AdwWindowTitle *title;
  GtkLabel       *status;
  GtkWindow      *window;
  GPtrArray      *freq_logs;     /* of FreqLog — decode history per frequency */
} App;

/* The bottom pane's header: the station heard on the tuned frequency (the
 * frequency itself lives in the footer). */
static void tuned_label_update(App *app, const SkimStation *st) {
  char s[128];
  if (st) {
    g_snprintf(s, sizeof(s), "%s · %.0f WPM · %.0f dB",
               st->call, st->speed, st->snr_db);
  } else {
    g_strlcpy(s, "—", sizeof(s));
  }
  gtk_label_set_text(app->tuned_label, s);
}

/* --- per-frequency decode history ----------------------------------------------------
 * Decoded text is kept per frequency slot so switching stations shows what
 * happened THERE — the pane never mixes frequencies (contest-proof). The
 * tuned pane renders the slot the radio sits on and follows it live. */

typedef struct {
  double   freq_hz;                          /* follows the signal's drift   */
  GString *text;
  gint64   last_seen;
} FreqLog;

static void freqlog_free(gpointer data) {
  FreqLog *fl = data;
  g_string_free(fl->text, TRUE);
  g_free(fl);
}

static FreqLog *freqlog_find(App *app, double freq_hz) {
  FreqLog *best = NULL;
  for (guint i = 0; i < app->freq_logs->len; i++) {
    FreqLog *fl = g_ptr_array_index(app->freq_logs, i);
    if (ABS(fl->freq_hz - freq_hz) <= TUNED_WINDOW_HZ &&
        (!best || ABS(fl->freq_hz - freq_hz) < ABS(best->freq_hz - freq_hz))) {
      best = fl;
    }
  }
  return best;
}

static FreqLog *freqlog_get(App *app, double freq_hz) {
  FreqLog *fl = freqlog_find(app, freq_hz);
  if (fl) { return fl; }
  if (app->freq_logs->len >= FREQLOG_MAX) {  /* drop the least recent slot   */
    guint victim = 0;
    for (guint i = 1; i < app->freq_logs->len; i++) {
      FreqLog *a = g_ptr_array_index(app->freq_logs, i);
      FreqLog *v = g_ptr_array_index(app->freq_logs, victim);
      if (a->last_seen < v->last_seen) { victim = i; }
    }
    g_ptr_array_remove_index_fast(app->freq_logs, victim);
  }
  fl = g_new0(FreqLog, 1);
  fl->freq_hz = freq_hz;
  fl->text    = g_string_new(NULL);
  g_ptr_array_add(app->freq_logs, fl);
  return fl;
}

/* Re-resolve which known station sits on the tuned frequency. When two
 * records overlap the window (frequency changing hands), the FRESHEST one
 * wins — updating from every incoming event made the label flicker between
 * the old and the new occupant. */
static void tuned_station_refresh(App *app) {
  SkimStation hit;
  gboolean have = FALSE;
  if (app->vfo_hz > 0) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
    for (guint i = 0; i < n; i++) {
      SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->stations), i);
      if (ABS(r->st.freq_hz - app->vfo_hz) <= TUNED_WINDOW_HZ &&
          (!have || r->st.last_heard > hit.last_heard)) {
        hit  = r->st;
        have = TRUE;
      }
      g_object_unref(r);
    }
  }
  tuned_label_update(app, have ? &hit : NULL);
}

/* Append at the tail of a monitor-style buffer (needs a "tail" mark): insert,
 * trim the head past 20 k chars, keep the view scrolled to the end. */
static void tail_append(GtkTextView *view, GtkTextBuffer *buf, const char *text) {
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buf, &end);
  gtk_text_buffer_insert(buf, &end, text, -1);
  if (gtk_text_buffer_get_char_count(buf) > 20000) {
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(buf, &s);
    gtk_text_buffer_get_iter_at_offset(buf, &e, 2000);
    gtk_text_buffer_delete(buf, &s, &e);
  }
  gtk_text_buffer_get_end_iter(buf, &end);
  GtkTextMark *mark = gtk_text_buffer_get_mark(buf, "tail");
  gtk_text_buffer_move_mark(buf, mark, &end);
  gtk_text_view_scroll_mark_onscreen(view, mark);
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
    /* The tracker keeps ONE record per call (QSY moves it) — match by call. */
    gboolean same = g_strcmp0(r->st.call, ev->st.call) == 0;
    g_object_unref(r);
    if (same) {
      /* Rows carry no notify — replace to refresh the bound labels. */
      g_list_store_remove(app->stations, i);
      break;
    }
  }
  g_list_store_append(app->stations, skim_row_new(&ev->st));
  /* Keep the tuned pane's header fresh if this touches the tuned window —
   * via the freshest-record resolver, never straight from the event. */
  if (app->vfo_hz > 0 && ABS(ev->st.freq_hz - app->vfo_hz) <= TUNED_WINDOW_HZ) {
    tuned_station_refresh(app);
  }
  g_free(ev);
  return G_SOURCE_REMOVE;
}

/* Station left the tracker (TTL / frequency takeover) — drop its row. */
typedef struct {
  App        *app;
  SkimStation st;
} GoneEvent;

static gboolean on_gone_idle(gpointer data) {
  GoneEvent *ev = data;
  App *app = ev->app;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
  for (guint i = 0; i < n; i++) {
    SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->stations), i);
    gboolean same = g_strcmp0(r->st.call, ev->st.call) == 0;
    g_object_unref(r);
    if (same) {
      g_list_store_remove(app->stations, i);
      break;
    }
  }
  tuned_station_refresh(app);
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
  App *app = ev->app;
  /* Record into the frequency's own history slot... */
  FreqLog *fl = freqlog_get(app, ev->freq_hz);
  fl->freq_hz   = ev->freq_hz;               /* follow drift                 */
  fl->last_seen = g_get_monotonic_time();
  g_string_append(fl->text, ev->text);
  if (fl->text->len > FREQLOG_CAP_CHARS) {
    g_string_erase(fl->text, 0,
                   (gssize)(fl->text->len - FREQLOG_CAP_CHARS + 2000));
  }
  /* ...and live-follow in the pane when the radio sits on this frequency. */
  if (app->vfo_hz > 0 && ABS(ev->freq_hz - app->vfo_hz) <= TUNED_WINDOW_HZ) {
    tail_append(app->tuned_view, app->tuned, ev->text);
  }
  g_free(ev->text);
  g_free(ev);
  return G_SOURCE_REMOVE;
}

typedef struct {
  App   *app;
  double vfo_hz;
} VfoEvent;

static gboolean on_vfo_idle(gpointer data) {
  VfoEvent *ev = data;
  App *app = ev->app;
  if (ev->vfo_hz != app->vfo_hz) {
    app->vfo_hz = ev->vfo_hz;
    tuned_station_refresh(app);
    /* Swap the pane to THIS frequency's history — one frequency, no mixing. */
    gtk_text_buffer_set_text(app->tuned, "", -1);
    FreqLog *fl = freqlog_find(app, app->vfo_hz);
    if (fl && fl->text->len) {
      tail_append(app->tuned_view, app->tuned, fl->text->str);
    }
  }
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
  App *app = ev->app;
  if (ev->connected) {
    adw_window_title_set_subtitle(app->title, ev->detail);
  } else if (app->pipeline) {
    /* The server dropped us — tear down; the scanner reconnects when it is
     * back. (App-initiated stops land here with pipeline already NULL.) */
    skim_pipeline_stop(app->pipeline);
    g_clear_pointer(&app->pipeline, skim_pipeline_free);
    app->vfo_hz = 0;
    tuned_label_update(app, NULL);
    adw_window_title_set_subtitle(app->title, "connection lost — searching…");
  }
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
static void pipe_vfo_cb(double vfo_hz, gpointer user) {
  VfoEvent *ev = g_new0(VfoEvent, 1);
  ev->app    = user;
  ev->vfo_hz = vfo_hz;
  g_idle_add(on_vfo_idle, ev);
}
static void pipe_gone_cb(const SkimStation *st, gpointer user) {
  GoneEvent *ev = g_new0(GoneEvent, 1);
  ev->app = user;
  ev->st  = *st;
  g_idle_add(on_gone_idle, ev);
}

/* --- status line -------------------------------------------------------------------- */

/* Rebind visible rows so the Age column ticks even for silent stations. */
static gboolean age_tick(gpointer data) {
  App *app = data;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
  if (n) { g_list_model_items_changed(G_LIST_MODEL(app->stations), 0, n, n); }
  return G_SOURCE_CONTINUE;
}

static gboolean status_tick(gpointer data) {
  App *app = data;
  if (app->pipeline) {
    char s[160];
    char vfo[32] = "";
    if (app->vfo_hz > 0) {
      g_snprintf(vfo, sizeof(vfo), "%.2f kHz · ", app->vfo_hz / 1000.0);
    }
    g_snprintf(s, sizeof(s),
               "%s%u stations · %" G_GUINT64_FORMAT " spots · %"
               G_GUINT64_FORMAT " Mframes",
               vfo, skim_pipeline_stations(app->pipeline),
               skim_pipeline_spots(app->pipeline),
               skim_pipeline_frames(app->pipeline) / 1000000);
    gtk_label_set_text(app->status, s);
  } else {
    gtk_label_set_text(app->status, "not connected");
  }
  return G_SOURCE_CONTINUE;
}

/* --- settings (persisted host) --------------------------------------------------------- */

static char *settings_file(void) {
  return g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                          "settings.ini", NULL);
}

static char *settings_load_host(void) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  char *host = NULL;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    host = g_key_file_get_string(kf, "tci", "host", NULL);
  }
  g_key_file_free(kf);
  g_free(path);
  if (host && host[0]) { return host; }
  g_free(host);
  return g_strdup("127.0.0.1");
}

static gboolean settings_load_cq_only(void) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  gboolean v = TRUE;                           /* RBN etiquette by default   */
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) &&
      g_key_file_has_key(kf, "spots", "cq_only", NULL)) {
    v = g_key_file_get_boolean(kf, "spots", "cq_only", NULL);
  }
  g_key_file_free(kf);
  g_free(path);
  return v;
}

static int settings_load_decode_font(void) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  int v = 11;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) &&
      g_key_file_has_key(kf, "ui", "decode_font_pt", NULL)) {
    v = g_key_file_get_integer(kf, "ui", "decode_font_pt", NULL);
  }
  g_key_file_free(kf);
  g_free(path);
  return CLAMP(v, 8, 32);
}

static gboolean settings_load_list_visible(void) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  gboolean v = TRUE;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) &&
      g_key_file_has_key(kf, "ui", "station_list", NULL)) {
    v = g_key_file_get_boolean(kf, "ui", "station_list", NULL);
  }
  g_key_file_free(kf);
  g_free(path);
  return v;
}

static void settings_save(const App *app) {
  char *path = settings_file();
  char *dir  = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  GKeyFile *kf = g_key_file_new();
  g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  g_key_file_set_string(kf, "tci", "host", app->host);
  g_key_file_set_boolean(kf, "spots", "cq_only", app->cq_only);
  g_key_file_set_integer(kf, "ui", "decode_font_pt", app->decode_font);
  g_key_file_set_boolean(kf, "ui", "station_list", app->list_visible);
  GError *err = NULL;
  if (!g_key_file_save_to_file(kf, path, &err)) {
    g_warning("settings: %s not saved: %s", path, err ? err->message : "?");
    g_clear_error(&err);
  }
  g_key_file_free(kf);
  g_free(dir);
  g_free(path);
}

/* Station list show/hide (the header-bar toggle next to Preferences). The
 * CW decode pane stays put and takes over the space when the list hides. */
static void on_list_toggled(GtkToggleButton *btn, gpointer user) {
  App *app = user;
  gboolean vis = gtk_toggle_button_get_active(btn);
  gtk_widget_set_visible(app->list_scroll, vis);
  gtk_widget_set_visible(app->list_sep, vis);
  gtk_widget_set_vexpand(app->tuned_scroll, !vis);
  if (vis != app->list_visible) {
    app->list_visible = vis;
    settings_save(app);
  }
}

/* The decode pane's font size rides a CSS provider — reloading the rule
 * restyles the view live. */
static void decode_font_apply(App *app) {
  if (!app->css) {
    app->css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(app->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  char rule[96];
  g_snprintf(rule, sizeof(rule), "textview.decode-pane { font-size: %dpt; }",
             app->decode_font);
  gtk_css_provider_load_from_string(app->css, rule);
}

/* --- auto-connect ------------------------------------------------------------------------
 * No connect button: while disconnected a 3 s scanner probes <host>:40001;
 * when the TCI port answers the pipeline comes up (handshake on a worker
 * thread, the UI never blocks). A lost connection tears it down
 * (on_state_idle) and the scanner takes over again. */

static void start_pipeline_thread(GTask *task, gpointer src, gpointer data,
                                  GCancellable *cancel) {
  (void)src; (void)cancel;
  GError *err = NULL;
  if (skim_pipeline_start(data, &err)) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_error(task, err);
  }
}

static void start_pipeline_done(GObject *src, GAsyncResult *res, gpointer user) {
  (void)src;
  App *app = user;
  GError *err = NULL;
  gboolean ok = g_task_propagate_boolean(G_TASK(res), &err);
  app->probing = FALSE;
  if (!ok) {
    g_clear_error(&err);                       /* scanner keeps trying       */
    g_clear_pointer(&app->starting, skim_pipeline_free);
    return;
  }
  app->pipeline = app->starting;
  app->starting = NULL;
  skim_pipeline_set_spot_cq_only(app->pipeline, app->cq_only);
  app->vfo_hz   = skim_pipeline_vfo_hz(app->pipeline);
  tuned_label_update(app, NULL);
}

static void probe_done(GObject *src, GAsyncResult *res, gpointer user) {
  App *app = user;
  GError *err = NULL;
  GSocketConnection *conn =
      g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, &err);
  if (!conn) {
    g_clear_error(&err);
    app->probing = FALSE;
    return;
  }
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);

  /* The TCI port answers — bring the pipeline up off the main thread. */
  char *dict = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                "master.scp", NULL);
  /* Raw decodes go to a per-day log for decoder QA (M3 off-air A/B). */
  char *logdir = g_build_filename(g_get_user_data_dir(), "skimmer-for-linux",
                                  NULL);
  g_mkdir_with_parents(logdir, 0755);
  GDateTime *now = g_date_time_new_now_local();
  char *day  = g_date_time_format(now, "%Y-%m-%d");
  char *dlog = g_strdup_printf("%s/decodes-%s.log", logdir, day);
  g_date_time_unref(now);
  g_free(day);
  g_free(logdir);
  SkimPipelineConfig cfg = {
    .host = app->host,
    .port = 40001,
    .iq_rate = 192000,
    .chan_bw_hz = 125.0,
    .dict_path = g_file_test(dict, G_FILE_TEST_EXISTS) ? dict : NULL,
    .decode_log_path = dlog,
  };
  app->starting = skim_pipeline_new(&cfg);
  g_free(dict);
  g_free(dlog);
  skim_pipeline_set_station_cb(app->starting, pipe_station_cb, app);
  skim_pipeline_set_station_gone_cb(app->starting, pipe_gone_cb, app);
  skim_pipeline_set_text_cb(app->starting, pipe_text_cb, app);
  skim_pipeline_set_state_cb(app->starting, pipe_state_cb, app);
  skim_pipeline_set_vfo_cb(app->starting, pipe_vfo_cb, app);
  adw_window_title_set_subtitle(app->title, "connecting…");
  GTask *t = g_task_new(NULL, NULL, start_pipeline_done, app);
  g_task_set_task_data(t, app->starting, NULL);
  g_task_run_in_thread(t, start_pipeline_thread);
  g_object_unref(t);
}

static gboolean scan_tick(gpointer data) {
  App *app = data;
  if (app->pipeline || app->probing) { return G_SOURCE_CONTINUE; }
  app->probing = TRUE;
  char s[160];
  g_snprintf(s, sizeof(s), "searching for %s:40001…", app->host);
  adw_window_title_set_subtitle(app->title, s);
  GSocketClient *sc = g_socket_client_new();
  g_socket_client_set_timeout(sc, 2);
  g_socket_client_connect_to_host_async(sc, app->host, 40001, NULL,
                                        probe_done, app);
  g_object_unref(sc);
  return G_SOURCE_CONTINUE;
}

/* --- preferences -------------------------------------------------------------------------- */

static void prefs_closed(AdwDialog *dlg, gpointer user) {
  App *app = user;
  GtkWidget *row  = g_object_get_data(G_OBJECT(dlg), "host-row");
  GtkWidget *sw   = g_object_get_data(G_OBJECT(dlg), "cq-row");
  GtkWidget *frow = g_object_get_data(G_OBJECT(dlg), "font-row");
  const char *h = gtk_editable_get_text(GTK_EDITABLE(row));
  char *host = g_strstrip(g_strdup((h && h[0]) ? h : "127.0.0.1"));
  gboolean cq_only = adw_switch_row_get_active(ADW_SWITCH_ROW(sw));
  int font_pt = (int)adw_spin_row_get_value(ADW_SPIN_ROW(frow));
  gboolean host_changed = host[0] && g_strcmp0(host, app->host) != 0;
  gboolean cq_changed   = cq_only != app->cq_only;
  gboolean font_changed = font_pt != app->decode_font;

  if (host_changed) {
    g_free(app->host);
    app->host = host;
  } else {
    g_free(host);
  }
  if (cq_changed) {
    app->cq_only = cq_only;
    if (app->pipeline) {                       /* applies live               */
      skim_pipeline_set_spot_cq_only(app->pipeline, cq_only);
    }
  }
  if (font_changed) {
    app->decode_font = font_pt;
    decode_font_apply(app);
  }
  if (host_changed || cq_changed || font_changed) {
    settings_save(app);
  }
  if (host_changed) {
    if (app->pipeline) {                       /* move to the new server      */
      skim_pipeline_stop(app->pipeline);
      g_clear_pointer(&app->pipeline, skim_pipeline_free);
      app->vfo_hz = 0;
      tuned_label_update(app, NULL);
    }
    scan_tick(app);
  }
}

static void prefs_open(GtkButton *btn, gpointer user) {
  (void)btn;
  App *app = user;
  AdwDialog *dlg = adw_preferences_dialog_new();
  adw_dialog_set_title(dlg, "Preferences");
  GtkWidget *page = adw_preferences_page_new();
  GtkWidget *grp  = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(grp), "TCI server");
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(grp),
      "sdr-for-linux WebSocket host, port 40001 — the connection is automatic");
  GtkWidget *row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Host");
  gtk_editable_set_text(GTK_EDITABLE(row), app->host);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(grp), row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                           ADW_PREFERENCES_GROUP(grp));

  GtkWidget *sgrp = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(sgrp), "Spots");
  GtkWidget *sw = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sw), "CQ only");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(sw),
      "Spot only stations heard calling (CQ, TEST, QRZ) — "
      "S&P answers stay off the panadapter");
  adw_switch_row_set_active(ADW_SWITCH_ROW(sw), app->cq_only);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(sgrp), sw);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                           ADW_PREFERENCES_GROUP(sgrp));

  GtkWidget *ugrp = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(ugrp), "Display");
  GtkWidget *frow = adw_spin_row_new_with_range(8, 32, 1);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(frow),
                                "Decode pane font size (pt)");
  adw_spin_row_set_value(ADW_SPIN_ROW(frow), app->decode_font);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(ugrp), frow);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                           ADW_PREFERENCES_GROUP(ugrp));

  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg),
                             ADW_PREFERENCES_PAGE(page));
  g_object_set_data(G_OBJECT(dlg), "host-row", row);
  g_object_set_data(G_OBJECT(dlg), "cq-row", sw);
  g_object_set_data(G_OBJECT(dlg), "font-row", frow);
  g_signal_connect(dlg, "closed", G_CALLBACK(prefs_closed), app);
  adw_dialog_present(dlg, GTK_WIDGET(app->window));
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
  g_snprintf(o, n, "%.2f", st->freq_hz / 1000.0);   /* 10 Hz — the estimate's
                                                     * real accuracy         */
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
static void fmt_cq(const SkimStation *st, char *o, gsize n) {
  g_strlcpy(o, st->cq ? "CQ" : "", n);
}
static void fmt_age(const SkimStation *st, char *o, gsize n) {
  int s = (int)((g_get_monotonic_time() - st->last_heard) / G_USEC_PER_SEC);
  if (s < 0) { s = 0; }
  if (s < 60) {
    g_snprintf(o, n, "%ds", s);
  } else {
    g_snprintf(o, n, "%dm%02ds", s / 60, s % 60);
  }
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

/* Row activated (SINGLE click / Enter) → tune the radio to the station; the
 * vfo broadcast comes back and swaps the tuned pane to that frequency. */
static void on_row_activated(GtkColumnView *view, guint position, gpointer user) {
  (void)view;
  App *app = user;
  SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->sorted), position);
  if (r) {
    if (app->pipeline) { skim_pipeline_tune(app->pipeline, r->st.freq_hz); }
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
  app->window       = GTK_WINDOW(window);
  app->host         = settings_load_host();
  app->cq_only      = settings_load_cq_only();
  app->decode_font  = settings_load_decode_font();
  app->list_visible = settings_load_list_visible();

  app->title = ADW_WINDOW_TITLE(adw_window_title_new("Skimmer for Linux", ""));
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                  GTK_WIDGET(app->title));

  GtkWidget *prefs_btn = gtk_button_new_from_icon_name("emblem-system-symbolic");
  gtk_widget_set_tooltip_text(prefs_btn, "Preferences");
  g_signal_connect(prefs_btn, "clicked", G_CALLBACK(prefs_open), app);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), prefs_btn);

  GtkWidget *list_btn = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(list_btn), "view-list-symbolic");
  gtk_widget_set_tooltip_text(list_btn, "Show station list");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(list_btn), app->list_visible);
  g_signal_connect(list_btn, "toggled", G_CALLBACK(on_list_toggled), app);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), list_btn);

  /* Station list, sorted by frequency — a text band map. Activate a row
   * (double-click / Enter) to tune the radio to that station. */
  app->freq_logs = g_ptr_array_new_with_free_func(freqlog_free);
  app->stations = g_list_store_new(SKIM_TYPE_ROW);
  GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(freq_cmp, NULL, NULL));
  app->sorted = gtk_sort_list_model_new(G_LIST_MODEL(app->stations), sorter);
  g_object_ref(app->sorted);
  GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(app->sorted));
  GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(sel));
  gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(view), TRUE);
  g_signal_connect(view, "activate", G_CALLBACK(on_row_activated), app);
  add_column(GTK_COLUMN_VIEW(view), "Call", fmt_call, TRUE);
  add_column(GTK_COLUMN_VIEW(view), "kHz", fmt_freq, FALSE);
  add_column(GTK_COLUMN_VIEW(view), "WPM", fmt_wpm, FALSE);
  add_column(GTK_COLUMN_VIEW(view), "SNR", fmt_snr, FALSE);
  add_column(GTK_COLUMN_VIEW(view), "Heard", fmt_heard, FALSE);
  add_column(GTK_COLUMN_VIEW(view), "CQ", fmt_cq, FALSE);
  add_column(GTK_COLUMN_VIEW(view), "Age", fmt_age, FALSE);
  GtkWidget *list_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll), view);
  gtk_widget_set_vexpand(list_scroll, TRUE);

  /* The tuned pane: decode text at the frequency the radio is tuned to. */
  app->tuned_label = GTK_LABEL(gtk_label_new("Tuned: —"));
  gtk_label_set_xalign(app->tuned_label, 0.0);
  gtk_widget_add_css_class(GTK_WIDGET(app->tuned_label), "heading");
  gtk_widget_set_margin_start(GTK_WIDGET(app->tuned_label), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(app->tuned_label), 8);
  gtk_widget_set_margin_bottom(GTK_WIDGET(app->tuned_label), 4);

  app->tuned = gtk_text_buffer_new(NULL);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(app->tuned, &end);
  gtk_text_buffer_create_mark(app->tuned, "tail", &end, FALSE);
  GtkWidget *tuned_view = gtk_text_view_new_with_buffer(app->tuned);
  app->tuned_view = GTK_TEXT_VIEW(tuned_view);
  gtk_text_view_set_editable(app->tuned_view, FALSE);
  gtk_text_view_set_monospace(app->tuned_view, TRUE);
  gtk_text_view_set_cursor_visible(app->tuned_view, FALSE);
  gtk_text_view_set_wrap_mode(app->tuned_view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(app->tuned_view, 12);
  gtk_widget_add_css_class(tuned_view, "decode-pane");
  decode_font_apply(app);
  GtkWidget *tuned_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tuned_scroll), tuned_view);
  gtk_widget_set_size_request(tuned_scroll, -1, 160);
  app->tuned_scroll = tuned_scroll;

  app->status = GTK_LABEL(gtk_label_new("not connected"));
  gtk_label_set_xalign(app->status, 0.0);
  gtk_widget_add_css_class(GTK_WIDGET(app->status), "dim-label");
  gtk_widget_set_margin_start(GTK_WIDGET(app->status), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(app->status), 4);
  gtk_widget_set_margin_bottom(GTK_WIDGET(app->status), 4);
  g_timeout_add_seconds(1, status_tick, app);
  g_timeout_add_seconds(2, age_tick, app);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), list_scroll);
  app->list_scroll = list_scroll;
  app->list_sep    = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(box), app->list_sep);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->tuned_label));
  gtk_box_append(GTK_BOX(box), tuned_scroll);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->status));
  gtk_widget_set_visible(list_scroll, app->list_visible);
  gtk_widget_set_visible(app->list_sep, app->list_visible);
  gtk_widget_set_vexpand(tuned_scroll, !app->list_visible);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));

  /* Find the server: probe now, then keep scanning while disconnected. */
  scan_tick(app);
  g_timeout_add_seconds(3, scan_tick, app);
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
