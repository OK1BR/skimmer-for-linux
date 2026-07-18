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
 * engine/network threads and are coalesced into ONE queue drained by a
 * single pending idle (see "marshalled engine events") — the engine stays
 * GLib-only (src/engine/ has no GTK).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <adwaita.h>

#include "callsign.h"
#include "pane_log.h"
#include "pipeline.h"

#ifndef SKIMMER_VERSION
#define SKIMMER_VERSION "0.0.0"
#endif

/* Matching the radio's TUNED frequency to a station/slot needs slack (the
 * radio tunes on its 100 Hz step, the station sits on its exact carrier). */
#define TUNED_WINDOW_HZ   SKIM_STATION_MERGE_HZ
/* ROUTING decode text into a history slot must be TIGHT: the engine pins
 * each signal's frequency now, so a slot is one signal — a ±300 Hz routing
 * window let neighbouring stations interleave into one another's history
 * (live-caught 2026-07-15: the decode pane "resyncing" between stations).
 * Tightened 75 → 25: two carriers 20 Hz apart each carry their OWN lock,
 * and ±75 interleaved both streams into one pane (live-caught 2026-07-16,
 * EA1EYL vs the beat stream 20 Hz up). Locks are stable to a few Hz, so
 * ±25 keeps a signal's own wobble and excludes the neighbour's. */
#define FREQLOG_ROUTE_HZ  25.0
#define FREQLOG_MAX       1024     /* cap on remembered frequency slots      */
#define FREQLOG_BABBLE    512      /* below this many chars a slot is babble */
/* Routing into the PANE while no station is fixed: an ear-tuned VFO sits
 * tens of Hz off the carrier (live 2026-07-16: station locked at +27 Hz,
 * the ±25 Hz route left the pane dead after a retune). Wider only in the
 * unfixed phase — once a station is fixed the tight window rules, so the
 * neighbour-interleave the 25 Hz limit exists for cannot return. */
#define FREQLOG_FREE_HZ   60.0
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
  guint           spot_round;    /* outgoing spot freq grid Hz, 0=exact
                                  * (persisted; SDC-style spot accuracy)      */
  SkimRbnFeed    *rbn;           /* RBN telnet server — app-owned so
                                  * aggregator sessions ride out reconnects   */
  gboolean        rbn_enabled;   /* persisted [rbn]                           */
  char           *rbn_call;      /* spotter callsign (persisted)              */
  int             rbn_port;      /* telnet port (persisted, default 7300)     */
  int             decode_font;   /* decode pane font size, pt (persisted)     */
  GtkCssProvider *css;           /* carries the decode pane font rule         */
  gboolean        probing;       /* port probe / handshake in flight          */
  double          vfo_hz;        /* the radio's tuned frequency (vfo:0,0)     */
  char            tuned_call[16]; /* the station the pane is FIXED on — set
                                  * on retune, held while it lives; header
                                  * and text feed both key off it, so they
                                  * can never disagree                        */
  double          tuned_slot_hz; /* its pinned frequency (slot key)           */
  GListStore     *stations;      /* of SkimRow                                */
  GtkSortListModel *sorted;
  GtkTextBuffer  *tuned;         /* decode text at the tuned frequency        */
  GtkTextTag     *draft_tag;     /* dim: reader may still rewrite this text   */
  GtkTextTag     *scp_tag;       /* green+underline: MASTER.SCP knows this
                                  * call (Richard, 2026-07-19)                */
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
  gsize           pane_over;     /* chars of the widget's live over region at
                                  * the buffer tail (phase B hybrid); plain
                                  * appends insert BEFORE it                  */
  GMutex          evq_lock;      /* guards evq + evq_scheduled                */
  GPtrArray      *evq;           /* engine events awaiting the main loop      */
  gboolean        evq_scheduled; /* a drain idle is already pending           */
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
  double       freq_hz;                      /* follows the signal's drift   */
  SkimPaneLog *log;                          /* text + live over region      */
  gint64       last_seen;
} FreqLog;

static void freqlog_free(gpointer data) {
  FreqLog *fl = data;
  skim_pane_log_free(fl->log);
  g_free(fl);
}

static FreqLog *freqlog_find(App *app, double freq_hz, double window_hz) {
  FreqLog *best = NULL;
  for (guint i = 0; i < app->freq_logs->len; i++) {
    FreqLog *fl = g_ptr_array_index(app->freq_logs, i);
    if (ABS(fl->freq_hz - freq_hz) <= window_hz &&
        (!best || ABS(fl->freq_hz - freq_hz) < ABS(best->freq_hz - freq_hz))) {
      best = fl;
    }
  }
  return best;
}

static FreqLog *freqlog_get(App *app, double freq_hz) {
  FreqLog *fl = freqlog_find(app, freq_hz, FREQLOG_ROUTE_HZ);
  if (fl) { return fl; }
  if (app->freq_logs->len >= FREQLOG_MAX) {
    /* Evict BABBLE first: noise channels mint a slot for every E/T burst
     * and pure LRU let them crowd out real histories — a station quiet for
     * a few minutes came back to a wiped pane (Richard, 2026-07-16). A few
     * chars of babble are worth less than any real history regardless of
     * age; among peers the least recent goes. */
    guint victim = 0;
    for (guint i = 1; i < app->freq_logs->len; i++) {
      FreqLog *a = g_ptr_array_index(app->freq_logs, i);
      FreqLog *v = g_ptr_array_index(app->freq_logs, victim);
      const gboolean ab = skim_pane_log_len(a->log) < FREQLOG_BABBLE;
      const gboolean vb = skim_pane_log_len(v->log) < FREQLOG_BABBLE;
      if ((ab && !vb) || (ab == vb && a->last_seen < v->last_seen)) {
        victim = i;
      }
    }
    g_ptr_array_remove_index_fast(app->freq_logs, victim);
  }
  fl = g_new0(FreqLog, 1);
  fl->freq_hz = freq_hz;
  fl->log     = skim_pane_log_new();
  g_ptr_array_add(app->freq_logs, fl);
  return fl;
}

/* Re-resolve which station the pane is FIXED on. Sticky: while the fixed
 * station lives and stays within the tuned window, it KEEPS the pane —
 * freshest-wins re-resolving flipped the header (and the text feed) between
 * co-channel stations on every report. Only when there is no fixation (a
 * retune cleared it) or the fixed station left does the freshest station
 * within the window take over. Returns TRUE when the fixation CHANGED —
 * the caller then reloads the pane from the new station's history. */
static gboolean tuned_station_refresh(App *app) {
  char before[16];
  g_strlcpy(before, app->tuned_call, sizeof(before));
  SkimStation hit;
  gboolean have = FALSE;
  if (app->vfo_hz > 0) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
    if (app->tuned_call[0]) {                  /* sticky: keep the fixed one */
      for (guint i = 0; i < n && !have; i++) {
        SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->stations), i);
        if (g_strcmp0(r->st.call, app->tuned_call) == 0 &&
            ABS(r->st.freq_hz - app->vfo_hz) <= TUNED_WINDOW_HZ) {
          hit  = r->st;
          have = TRUE;
        }
        g_object_unref(r);
      }
    }
    if (!have) {                               /* else: freshest in window   */
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
  }
  if (have) {
    g_strlcpy(app->tuned_call, hit.call, sizeof(app->tuned_call));
    app->tuned_slot_hz = hit.freq_hz;          /* the pinned slot key        */
  } else {
    app->tuned_call[0] = '\0';
    app->tuned_slot_hz = 0;
  }
  tuned_label_update(app, have ? &hit : NULL);
  return g_strcmp0(before, app->tuned_call) != 0;
}

/* Trim the head past 20 k chars and keep the view scrolled to the end (the
 * buffer carries a "tail" mark for that). */
static void buffer_trim_scroll(GtkTextView *view, GtkTextBuffer *buf) {
  if (gtk_text_buffer_get_char_count(buf) > 20000) {
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(buf, &s);
    gtk_text_buffer_get_iter_at_offset(buf, &e, 2000);
    gtk_text_buffer_delete(buf, &s, &e);
  }
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buf, &end);
  GtkTextMark *mark = gtk_text_buffer_get_mark(buf, "tail");
  gtk_text_buffer_move_mark(buf, mark, &end);
  gtk_text_view_scroll_mark_onscreen(view, mark);
}

/* Append at the tail of a monitor-style buffer. */
static void tail_append(GtkTextView *view, GtkTextBuffer *buf, const char *text) {
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buf, &end);
  gtk_text_buffer_insert(buf, &end, text, -1);
  buffer_trim_scroll(view, buf);
}

/* --- dictionary call marking -------------------------------------------------------
 * Underline-green every COMPLETED token the MASTER.SCP dictionary knows,
 * scanning the last `back` chars of the tuned pane (Richard, 2026-07-19 —
 * calls the dictionary vouches for stand out from the decode flow). The
 * token still growing at the very end of the buffer stays untouched: live
 * text lands per character, and "YT1" must not light up before its A
 * arrives — the tag lands when the boundary (space, over mark) does.
 * Re-tagging an already tagged range is a no-op, so overlapping scans on
 * consecutive batches are harmless. */
static gboolean scp_token_char(gunichar c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/';
}

static void scp_tag_token(App *app, gint s_off, gint e_off) {
  if (e_off - s_off < 3 || e_off - s_off > 12)
    return;                              /* no dictionary call is that shape */
  GtkTextIter s, e;
  gtk_text_buffer_get_iter_at_offset(app->tuned, &s, s_off);
  gtk_text_buffer_get_iter_at_offset(app->tuned, &e, e_off);
  char *tok = gtk_text_buffer_get_text(app->tuned, &s, &e, FALSE);
  if (skim_callsign_dict_has(tok)) {
    gtk_text_buffer_apply_tag(app->tuned, app->scp_tag, &s, &e);
  }
  g_free(tok);
}

static void scp_highlight(App *app, gsize back) {
  if (!skim_callsign_dict_size())
    return;
  const gsize total = (gsize)gtk_text_buffer_get_char_count(app->tuned);
  GtkTextIter it, end;
  gtk_text_buffer_get_end_iter(app->tuned, &end);
  it = end;
  gtk_text_iter_backward_chars(&it, (gint)MIN(back + 16, total));
  char *slice = gtk_text_buffer_get_text(app->tuned, &it, &end, FALSE);
  gint off = gtk_text_iter_get_offset(&it);
  gint tok_start = -1;
  for (const char *p = slice; *p; p = g_utf8_next_char(p), off++) {
    const gboolean t = scp_token_char(g_utf8_get_char(p));
    if (t && tok_start < 0) { tok_start = off; }
    if (!t && tok_start >= 0) {
      scp_tag_token(app, tok_start, off);
      tok_start = -1;
    }
  }
  g_free(slice);                         /* a trailing token is still open   */
}

/* Swap the pane to the fixed station's history (or, with nothing fixed,
 * to whatever history sits near the VFO). A live over region re-dims its
 * draft tail and re-arms the widget-side region size. */
static void tuned_pane_reload(App *app) {
  gtk_text_buffer_set_text(app->tuned, "", -1);
  app->pane_over = 0;
  FreqLog *fl = NULL;
  if (app->tuned_call[0]) {
    fl = freqlog_find(app, app->tuned_slot_hz, FREQLOG_ROUTE_HZ);
  } else if (app->vfo_hz > 0) {
    fl = freqlog_find(app, app->vfo_hz, TUNED_WINDOW_HZ);
  }
  if (fl && skim_pane_log_len(fl->log)) {
    tail_append(app->tuned_view, app->tuned, skim_pane_log_text(fl->log));
    scp_highlight(app, (gsize)gtk_text_buffer_get_char_count(app->tuned));
    const gsize over = skim_pane_log_over_len(fl->log);
    const gsize fin  = skim_pane_log_final_len(fl->log);
    app->pane_over = over;             /* over text is ASCII: bytes == chars */
    if (over > fin) {
      GtkTextIter s, e;
      gtk_text_buffer_get_end_iter(app->tuned, &e);
      s = e;
      gtk_text_iter_backward_chars(&s, (gint)(over - fin));
      gtk_text_buffer_apply_tag(app->tuned, app->draft_tag, &s, &e);
    }
  }
}

/* --- marshalled engine events ------------------------------------------------------
 * Engine threads never touch GTK: every callback lands in ONE mutex-guarded
 * queue and a SINGLE pending idle drains the whole backlog per dispatch.
 * The first shape — one g_idle_add per event — melted under contest load:
 * hundreds of events/s outran the UI work each handler did, the pending
 * source list grew without bound and every main-loop iteration walked all
 * of it, until GNOME offered the force-quit dialog (live-caught
 * 2026-07-18, 20 m contest). Coalescing keeps at most one source alive,
 * collapses a batch to the LAST report per station, and lands a batch's
 * pane text in one append. */

typedef enum { EV_STATION, EV_GONE, EV_TEXT, EV_OVER, EV_VFO, EV_STATE } EvKind;

typedef struct {
  EvKind      kind;
  SkimStation st;                /* EV_STATION / EV_GONE                      */
  double      hz;                /* EV_TEXT/EV_OVER: channel; EV_VFO: tuned   */
  char       *str;               /* EV_TEXT/EV_OVER: text; EV_STATE: detail   */
  gboolean    connected;         /* EV_STATE                                  */
  guint       op_kind;           /* EV_OVER: SkimPaneOpKind                   */
  guint       op_erase;          /* EV_OVER: OPEN's draft take-back           */
  guint       op_final;          /* EV_OVER: reader-final prefix bytes        */
} Ev;

static void ev_free(gpointer data) {
  Ev *ev = data;
  g_free(ev->str);
  g_free(ev);
}

static void apply_station(App *app, const SkimStation *st) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
  for (guint i = 0; i < n; i++) {
    SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->stations), i);
    /* The tracker keeps ONE record per call (QSY moves it) — match by call. */
    gboolean same = g_strcmp0(r->st.call, st->call) == 0;
    g_object_unref(r);
    if (same) {
      /* Rows carry no notify — replace to refresh the bound labels. */
      g_list_store_remove(app->stations, i);
      break;
    }
  }
  g_list_store_append(app->stations, skim_row_new(st));
  /* Keep the tuned pane's header fresh if this touches the tuned window —
   * via the sticky resolver, never straight from the event. When the
   * fixation resolves to a (new) station, pull in its history: the first
   * seconds after a retune decoded before the station validated. */
  if (app->vfo_hz > 0 && ABS(st->freq_hz - app->vfo_hz) <= TUNED_WINDOW_HZ) {
    if (tuned_station_refresh(app)) { tuned_pane_reload(app); }
  }
}

/* Station left the tracker (TTL / frequency takeover) — drop its row. */
static void apply_gone(App *app, const SkimStation *st) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
  for (guint i = 0; i < n; i++) {
    SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->stations), i);
    gboolean same = g_strcmp0(r->st.call, st->call) == 0;
    g_object_unref(r);
    if (same) {
      g_list_store_remove(app->stations, i);
      break;
    }
  }
  if (g_strcmp0(st->call, app->tuned_call) == 0) {
    app->tuned_call[0] = '\0';                 /* the fixed station left     */
  }
  if (tuned_station_refresh(app)) { tuned_pane_reload(app); }
}

static void pane_flush(App *app, GString *pane);

/* Is this event's frequency routed into the tuned pane? The pane belongs to
 * the FIXED station's slot — never to "anything near the VFO", which
 * interleaved a neighbour within the window into the tuned station's text.
 * Before a station is resolved (fresh retune to a quiet spot), follow the
 * VFO tightly. */
static gboolean pane_routed(const App *app, double freq_hz) {
  const double key = app->tuned_call[0] ? app->tuned_slot_hz : app->vfo_hz;
  const double win = app->tuned_call[0] ? FREQLOG_ROUTE_HZ : FREQLOG_FREE_HZ;
  return key > 0 && ABS(freq_hz - key) <= win;
}

/* Record text into the frequency's history slot; the pane-routed part goes
 * into `pane` — the drain flushes it to the widget once per batch segment.
 * While the widget's tail holds a live over region (phase B), routed text
 * must land BEFORE it, mirroring skim_pane_log_append. */
static void apply_text(App *app, double freq_hz, const char *text,
                       GString *pane) {
  FreqLog *fl = freqlog_get(app, freq_hz);
  fl->freq_hz   = freq_hz;                   /* follow drift                 */
  fl->last_seen = g_get_monotonic_time();
  skim_pane_log_append(fl->log, text);
  if (skim_pane_log_len(fl->log) > FREQLOG_CAP_CHARS) {
    skim_pane_log_trim_head(
        fl->log, skim_pane_log_len(fl->log) - FREQLOG_CAP_CHARS + 2000);
  }
  if (pane_routed(app, freq_hz)) {
    if (app->pane_over > 0) {
      pane_flush(app, pane);
      GtkTextIter it;
      gtk_text_buffer_get_end_iter(app->tuned, &it);
      gtk_text_iter_backward_chars(&it, (gint)app->pane_over);
      gtk_text_buffer_insert(app->tuned, &it, text, -1);
      buffer_trim_scroll(app->tuned_view, app->tuned);
    } else {
      g_string_append(pane, text);
    }
  }
}

/* Phase B hybrid over op (OPEN/SET/CLOSE): the frequency's history slot
 * applies it via SkimPaneLog; when routed, the widget mirrors it — the over
 * region lives at the buffer tail, reader-final text plain, the live draft
 * tail dim. Full-state ops make every step self-healing. */
static void apply_over(App *app, double freq_hz, SkimPaneOpKind kind,
                       guint erase, const char *text, guint final_len) {
  FreqLog *fl = freqlog_get(app, freq_hz);
  fl->freq_hz   = freq_hz;
  fl->last_seen = g_get_monotonic_time();
  const gsize before = skim_pane_log_over_len(fl->log);
  const SkimPaneOp op = { kind, erase, final_len, (char *)text, NULL };
  skim_pane_log_apply(fl->log, &op);
  if (!pane_routed(app, freq_hz))
    return;
  /* Widget mirror: take back the previous region (or, on OPEN, the shown
   * draft), then insert the new view. Over text is ASCII — bytes == chars. */
  gsize del = kind == SKIM_PANE_OP_OPEN ? MIN((gsize)erase,
                                              (gsize)gtk_text_buffer_get_char_count(app->tuned))
                                        : MIN(app->pane_over, before);
  if (kind == SKIM_PANE_OP_OPEN && app->pane_over > 0) {
    del = MIN(del + app->pane_over, /* stale region never sealed — eat both */
              (gsize)gtk_text_buffer_get_char_count(app->tuned));
  }
  GtkTextIter s, e;
  gtk_text_buffer_get_end_iter(app->tuned, &e);
  if (del > 0) {
    s = e;
    gtk_text_iter_backward_chars(&s, (gint)del);
    gtk_text_buffer_delete(app->tuned, &s, &e);
    gtk_text_buffer_get_end_iter(app->tuned, &e);
  }
  const gsize tlen = strlen(text);
  const gsize fin  = MIN((gsize)final_len, tlen);
  gtk_text_buffer_insert(app->tuned, &e, text, (gint)fin);
  if (tlen > fin) {
    gtk_text_buffer_get_end_iter(app->tuned, &e);
    gtk_text_buffer_insert_with_tags(app->tuned, &e, text + fin,
                                     (gint)(tlen - fin), app->draft_tag,
                                     NULL);
  }
  buffer_trim_scroll(app->tuned_view, app->tuned);
  app->pane_over = kind == SKIM_PANE_OP_CLOSE ? 0 : tlen;
}

static void apply_vfo(App *app, double vfo_hz) {
  if (vfo_hz != app->vfo_hz) {
    app->vfo_hz = vfo_hz;
    /* The sticky resolver keeps the fixed station across small retunes
     * (our own click-tune rounds on the radio's step). But when the new
     * frequency clearly points at a DIFFERENT known station — a spot
     * click on a neighbour within the window — break the fixation, or
     * the pane would keep showing the previous station's header + text
     * (live-caught 2026-07-15). */
    if (app->tuned_call[0]) {
      double fixed_d = 1e12, best_d = 1e12;
      char best_call[16] = "";
      guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
      for (guint i = 0; i < n; i++) {
        SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->stations), i);
        const double dist = ABS(r->st.freq_hz - app->vfo_hz);
        if (g_strcmp0(r->st.call, app->tuned_call) == 0) { fixed_d = dist; }
        if (dist < best_d) {
          best_d = dist;
          g_strlcpy(best_call, r->st.call, sizeof(best_call));
        }
        g_object_unref(r);
      }
      if (best_call[0] && g_strcmp0(best_call, app->tuned_call) != 0 &&
          best_d + 20.0 < fixed_d) {
        app->tuned_call[0] = '\0';
      }
    }
    tuned_station_refresh(app);
    tuned_pane_reload(app);
  }
}

static void apply_state(App *app, gboolean connected, const char *detail) {
  if (connected) {
    adw_window_title_set_subtitle(app->title, detail);
  } else if (app->pipeline) {
    /* The server dropped us — tear down; the scanner reconnects when it is
     * back. (App-initiated stops land here with pipeline already NULL.) */
    skim_pipeline_stop(app->pipeline);
    g_clear_pointer(&app->pipeline, skim_pipeline_free);
    app->vfo_hz = 0;
    app->tuned_call[0] = '\0';
    app->tuned_slot_hz = 0;
    tuned_label_update(app, NULL);
    adw_window_title_set_subtitle(app->title, "connection lost — searching…");
  }
}

static void pane_flush(App *app, GString *pane) {
  if (pane->len) {
    const gsize n = pane->len;           /* bytes ≥ chars — scan margin      */
    tail_append(app->tuned_view, app->tuned, pane->str);
    g_string_truncate(pane, 0);
    scp_highlight(app, n);
  }
}

/* The single drain: steal the whole queue, apply it in order. Pane text
 * accumulates across consecutive text events and flushes before any event
 * that could change routing or reload the pane (and once at the end). */
static gboolean evq_drain(gpointer data) {
  App *app = data;
  g_mutex_lock(&app->evq_lock);
  GPtrArray *batch = app->evq;
  app->evq = g_ptr_array_new_with_free_func(ev_free);
  app->evq_scheduled = FALSE;
  g_mutex_unlock(&app->evq_lock);

  /* Only the LAST report per station builds a row — earlier ones in the
   * same batch would be replaced within this very dispatch. (Keyed i+1:
   * a missing hash entry must never alias index 0.) */
  GHashTable *last = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < batch->len; i++) {
    Ev *ev = g_ptr_array_index(batch, i);
    if (ev->kind == EV_STATION) {
      g_hash_table_replace(last, ev->st.call, GUINT_TO_POINTER(i + 1));
    }
  }
  GString *pane = g_string_new(NULL);
  for (guint i = 0; i < batch->len; i++) {
    Ev *ev = g_ptr_array_index(batch, i);
    if (ev->kind != EV_TEXT) { pane_flush(app, pane); }
    switch (ev->kind) {
    case EV_STATION:
      if (GPOINTER_TO_UINT(g_hash_table_lookup(last, ev->st.call)) == i + 1) {
        apply_station(app, &ev->st);
      }
      break;
    case EV_GONE:
      apply_gone(app, &ev->st);
      break;
    case EV_TEXT:
      apply_text(app, ev->hz, ev->str, pane);
      break;
    case EV_OVER:
      apply_over(app, ev->hz, (SkimPaneOpKind)ev->op_kind, ev->op_erase,
                 ev->str ? ev->str : "", ev->op_final);
      break;
    case EV_VFO:
      apply_vfo(app, ev->hz);
      break;
    case EV_STATE:
      apply_state(app, ev->connected, ev->str);
      break;
    }
  }
  pane_flush(app, pane);
  g_string_free(pane, TRUE);
  g_hash_table_unref(last);
  g_ptr_array_unref(batch);
  return G_SOURCE_REMOVE;
}

/* Engine-thread side: append + schedule the drain unless one is pending. */
static void ev_post(App *app, Ev *ev) {
  g_mutex_lock(&app->evq_lock);
  g_ptr_array_add(app->evq, ev);
  const gboolean need = !app->evq_scheduled;
  app->evq_scheduled = TRUE;
  g_mutex_unlock(&app->evq_lock);
  if (need) { g_idle_add(evq_drain, app); }
}

static void pipe_station_cb(const SkimStation *st, gpointer user) {
  Ev *ev = g_new0(Ev, 1);
  ev->kind = EV_STATION;
  ev->st   = *st;
  ev_post(user, ev);
}
static void pipe_text_cb(double freq_hz, const char *text, gpointer user) {
  Ev *ev = g_new0(Ev, 1);
  ev->kind = EV_TEXT;
  ev->hz   = freq_hz;
  ev->str  = g_strdup(text);
  ev_post(user, ev);
}
static void pipe_over_cb(double freq_hz, SkimPaneOpKind kind, guint erase,
                         const char *text, guint final_len, gpointer user) {
  Ev *ev = g_new0(Ev, 1);
  ev->kind     = EV_OVER;
  ev->hz       = freq_hz;
  ev->str      = g_strdup(text);
  ev->op_kind  = (guint)kind;
  ev->op_erase = erase;
  ev->op_final = final_len;
  ev_post(user, ev);
}
static void pipe_state_cb(gboolean connected, const char *detail, gpointer user) {
  Ev *ev = g_new0(Ev, 1);
  ev->kind      = EV_STATE;
  ev->connected = connected;
  ev->str       = g_strdup(detail);
  ev_post(user, ev);
}
static void pipe_vfo_cb(double vfo_hz, gpointer user) {
  Ev *ev = g_new0(Ev, 1);
  ev->kind = EV_VFO;
  ev->hz   = vfo_hz;
  ev_post(user, ev);
}
static void pipe_gone_cb(const SkimStation *st, gpointer user) {
  Ev *ev = g_new0(Ev, 1);
  ev->kind = EV_GONE;
  ev->st   = *st;
  ev_post(user, ev);
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
  char rbn[64] = "";
  if (app->rbn) {
    g_snprintf(rbn, sizeof(rbn), " · feed :%u (%u)",
               skim_rbn_feed_port(app->rbn), skim_rbn_feed_clients(app->rbn));
  }
  if (app->pipeline) {
    char s[224];
    char vfo[32] = "";
    if (app->vfo_hz > 0) {
      g_snprintf(vfo, sizeof(vfo), "%.2f kHz · ", app->vfo_hz / 1000.0);
    }
    g_snprintf(s, sizeof(s),
               "%s%u stations · %" G_GUINT64_FORMAT " spots · %"
               G_GUINT64_FORMAT " Mframes%s",
               vfo, skim_pipeline_stations(app->pipeline),
               skim_pipeline_spots(app->pipeline),
               skim_pipeline_frames(app->pipeline) / 1000000, rbn);
    gtk_label_set_text(app->status, s);
  } else {
    char s[96];
    g_snprintf(s, sizeof(s), "not connected%s", rbn);
    gtk_label_set_text(app->status, s);
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

static guint settings_load_spot_round(void) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  guint v = 0;                                 /* exact by default           */
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) &&
      g_key_file_has_key(kf, "spots", "round_hz", NULL)) {
    int r = g_key_file_get_integer(kf, "spots", "round_hz", NULL);
    if (r >= 0 && r <= 1000) { v = (guint)r; }
  }
  g_key_file_free(kf);
  g_free(path);
  return v;
}

static void settings_load_rbn(App *app) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  app->rbn_enabled = FALSE;
  app->rbn_call    = NULL;
  app->rbn_port    = 7300;                     /* the CW Skimmer convention  */
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    if (g_key_file_has_key(kf, "rbn", "enabled", NULL)) {
      app->rbn_enabled = g_key_file_get_boolean(kf, "rbn", "enabled", NULL);
    }
    app->rbn_call = g_key_file_get_string(kf, "rbn", "call", NULL);
    if (g_key_file_has_key(kf, "rbn", "port", NULL)) {
      int port = g_key_file_get_integer(kf, "rbn", "port", NULL);
      if (port > 0 && port <= 65535) { app->rbn_port = port; }
    }
  }
  g_key_file_free(kf);
  g_free(path);
  if (!app->rbn_call) { app->rbn_call = g_strdup(""); }
}

static void settings_save(const App *app) {
  char *path = settings_file();
  char *dir  = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  GKeyFile *kf = g_key_file_new();
  g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  g_key_file_set_string(kf, "tci", "host", app->host);
  g_key_file_set_boolean(kf, "spots", "cq_only", app->cq_only);
  g_key_file_set_integer(kf, "spots", "round_hz", (gint)app->spot_round);
  g_key_file_set_boolean(kf, "rbn", "enabled", app->rbn_enabled);
  g_key_file_set_string(kf, "rbn", "call", app->rbn_call);
  g_key_file_set_integer(kf, "rbn", "port", app->rbn_port);
  g_key_file_set_integer(kf, "ui", "decode_font_pt", app->decode_font);
  g_key_file_set_boolean(kf, "ui", "station_list", app->list_visible);
  g_key_file_remove_group(kf, "reader", NULL);   /* the neural reader is gone
                                                  * (Richard, 2026-07-19)    */
  GError *err = NULL;
  if (!g_key_file_save_to_file(kf, path, &err)) {
    g_warning("settings: %s not saved: %s", path, err ? err->message : "?");
    g_clear_error(&err);
  }
  g_key_file_free(kf);
  g_free(dir);
  g_free(path);
}

/* (Re)start the RBN telnet server to match the current settings. The feed
 * is app-owned (not per-connection): aggregator sessions survive TCI
 * reconnects. The caller restarts the pipeline if one is running — its
 * config holds the old feed pointer. */
static void rbn_apply(App *app) {
  g_clear_pointer(&app->rbn, skim_rbn_feed_free);
  if (!app->rbn_enabled || !app->rbn_call[0])
    return;
  GError *err = NULL;
  app->rbn = skim_rbn_feed_new(app->rbn_call, (guint16)app->rbn_port, &err);
  if (!app->rbn) {
    g_warning("RBN feed on port %d: %s", app->rbn_port,
              err ? err->message : "?");
    g_clear_error(&err);
  }
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

/* The decode pane's font rides a CSS provider — reloading the rule restyles
 * the view live. Decoded CW is prose, not columns: no alignment to keep, so
 * the pane uses the same face as the window title (the UI sans) — every
 * mono tried read worse in a long history (serif Nimbus via the bare alias,
 * condensed Iosevka, Adwaita Mono; Richard picked the title face live,
 * 2026-07-16). */
static void decode_font_apply(App *app) {
  if (!app->css) {
    app->css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(app->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  char rule[256];
  g_snprintf(rule, sizeof(rule),
             "textview.decode-pane { "
             "font-family: \"Adwaita Sans\", \"Cantarell\", sans-serif; "
             "font-size: %dpt; }",
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
  skim_pipeline_set_spot_round_hz(app->pipeline, app->spot_round);
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
    .rbn = app->rbn,                           /* NULL when the feed is off  */
  };
  app->starting = skim_pipeline_new(&cfg);
  g_free(dict);
  g_free(dlog);
  skim_pipeline_set_station_cb(app->starting, pipe_station_cb, app);
  skim_pipeline_set_station_gone_cb(app->starting, pipe_gone_cb, app);
  skim_pipeline_set_text_cb(app->starting, pipe_text_cb, app);
  skim_pipeline_set_over_cb(app->starting, pipe_over_cb, app);
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
  GtkWidget *qrow = g_object_get_data(G_OBJECT(dlg), "round-row");
  GtkWidget *frow = g_object_get_data(G_OBJECT(dlg), "font-row");
  GtkWidget *rsw  = g_object_get_data(G_OBJECT(dlg), "rbn-row");
  GtkWidget *rcall = g_object_get_data(G_OBJECT(dlg), "rbn-call-row");
  GtkWidget *rport = g_object_get_data(G_OBJECT(dlg), "rbn-port-row");
  const char *h = gtk_editable_get_text(GTK_EDITABLE(row));
  char *host = g_strstrip(g_strdup((h && h[0]) ? h : "127.0.0.1"));
  gboolean cq_only = adw_switch_row_get_active(ADW_SWITCH_ROW(sw));
  static const guint ROUND_VALS[] = { 0, 10, 20, 50, 100 };
  guint rsel = adw_combo_row_get_selected(ADW_COMBO_ROW(qrow));
  guint spot_round =
      ROUND_VALS[MIN(rsel, G_N_ELEMENTS(ROUND_VALS) - 1)];
  int font_pt = (int)adw_spin_row_get_value(ADW_SPIN_ROW(frow));
  gboolean rbn_enabled = adw_switch_row_get_active(ADW_SWITCH_ROW(rsw));
  char *rbn_call = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(rcall))));
  int rbn_port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(rport));
  gboolean host_changed = host[0] && g_strcmp0(host, app->host) != 0;
  gboolean cq_changed   = cq_only != app->cq_only;
  gboolean round_changed = spot_round != app->spot_round;
  gboolean font_changed = font_pt != app->decode_font;
  gboolean rbn_changed  = rbn_enabled != app->rbn_enabled ||
                          g_strcmp0(rbn_call, app->rbn_call) != 0 ||
                          rbn_port != app->rbn_port;
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
  if (round_changed) {
    app->spot_round = spot_round;
    if (app->pipeline) {                       /* applies live               */
      skim_pipeline_set_spot_round_hz(app->pipeline, spot_round);
    }
  }
  if (font_changed) {
    app->decode_font = font_pt;
    decode_font_apply(app);
  }
  if (rbn_changed) {
    app->rbn_enabled = rbn_enabled;
    g_free(app->rbn_call);
    app->rbn_call = rbn_call;
    app->rbn_port = rbn_port;
    rbn_apply(app);
  } else {
    g_free(rbn_call);
  }
  if (host_changed || cq_changed || round_changed || font_changed ||
      rbn_changed) {
    settings_save(app);
  }
  /* The pipeline's config carries the feed pointer — an RBN change needs a
   * fresh pipeline just like a host change does. */
  if (host_changed || rbn_changed) {
    if (app->pipeline) {
      skim_pipeline_stop(app->pipeline);
      g_clear_pointer(&app->pipeline, skim_pipeline_free);
      app->vfo_hz = 0;
      app->tuned_call[0] = '\0';
      app->tuned_slot_hz = 0;
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
  GtkWidget *qrow = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(qrow), "Frequency step");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(qrow),
      "Snap outgoing spot frequencies (panadapter and telnet feed) to a "
      "grid — the measured value stays exact inside the app");
  static const char *STEPS[] = { "Exact", "10 Hz", "20 Hz", "50 Hz",
                                 "100 Hz", NULL };
  adw_combo_row_set_model(ADW_COMBO_ROW(qrow),
                          G_LIST_MODEL(gtk_string_list_new(STEPS)));
  const guint vals[] = { 0, 10, 20, 50, 100 };
  guint sel = 0;
  for (guint i = 0; i < G_N_ELEMENTS(vals); i++) {
    if (vals[i] == app->spot_round) { sel = i; }
  }
  adw_combo_row_set_selected(ADW_COMBO_ROW(qrow), sel);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(sgrp), qrow);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                           ADW_PREFERENCES_GROUP(sgrp));

  GtkWidget *rgrp = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(rgrp),
                                  "Telnet spot feed");
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(rgrp),
      "Cluster-dialect telnet server — point a logger (BRlog) or any "
      "cluster client at this port");
  GtkWidget *rsw = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rsw), "Enable");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(rsw),
      "Feeds only validated stations heard calling CQ");
  adw_switch_row_set_active(ADW_SWITCH_ROW(rsw), app->rbn_enabled);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(rgrp), rsw);
  GtkWidget *rcall = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rcall),
                                "Operator callsign");
  gtk_editable_set_text(GTK_EDITABLE(rcall), app->rbn_call);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(rgrp), rcall);
  GtkWidget *rport = adw_spin_row_new_with_range(1024, 65535, 1);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rport), "Telnet port");
  adw_spin_row_set_value(ADW_SPIN_ROW(rport), app->rbn_port);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(rgrp), rport);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                           ADW_PREFERENCES_GROUP(rgrp));

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
  g_object_set_data(G_OBJECT(dlg), "round-row", qrow);
  g_object_set_data(G_OBJECT(dlg), "font-row", frow);
  g_object_set_data(G_OBJECT(dlg), "rbn-row", rsw);
  g_object_set_data(G_OBJECT(dlg), "rbn-call-row", rcall);
  g_object_set_data(G_OBJECT(dlg), "rbn-port-row", rport);
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
    /* Fix the pane on the CLICKED station right away — the user said which
     * one they want; the vfo broadcast confirms the retune later and the
     * sticky resolver keeps this choice. */
    g_strlcpy(app->tuned_call, r->st.call, sizeof(app->tuned_call));
    app->tuned_slot_hz = r->st.freq_hz;
    tuned_label_update(app, &r->st);
    tuned_pane_reload(app);
    g_object_unref(r);
  }
}

/* --- activate ----------------------------------------------------------------------------- */

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
  (void)user_data;
  App *app = g_new0(App, 1);
  g_mutex_init(&app->evq_lock);
  app->evq = g_ptr_array_new_with_free_func(ev_free);

  GtkWidget *window = adw_application_window_new(gtk_app);
  gtk_window_set_title(GTK_WINDOW(window), "Skimmer for Linux");
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 640);
  app->window       = GTK_WINDOW(window);
  app->host         = settings_load_host();
  app->cq_only      = settings_load_cq_only();
  app->spot_round   = settings_load_spot_round();
  app->decode_font  = settings_load_decode_font();
  app->list_visible = settings_load_list_visible();
  settings_load_rbn(app);
  rbn_apply(app);                /* the telnet server is up before the radio */

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
  /* Breathing room under the header — station line and decode stream must
   * not read as one block (Richard, 2026-07-16). */
  gtk_widget_set_margin_bottom(GTK_WIDGET(app->tuned_label), 10);

  app->tuned = gtk_text_buffer_new(NULL);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(app->tuned, &end);
  gtk_text_buffer_create_mark(app->tuned, "tail", &end, FALSE);
  /* Live draft the reader may still rewrite renders dim; committed text is
   * plain — the over "firms up" in place (phase B, Richard 2026-07-18). */
  app->draft_tag = gtk_text_buffer_create_tag(app->tuned, "draft",
                                              "foreground", "#808080", NULL);
  app->scp_tag = gtk_text_buffer_create_tag(app->tuned, "scp",
                                            "foreground", "#44cc44",
                                            "underline", PANGO_UNDERLINE_SINGLE,
                                            NULL);
  GtkWidget *tuned_view = gtk_text_view_new_with_buffer(app->tuned);
  app->tuned_view = GTK_TEXT_VIEW(tuned_view);
  gtk_text_view_set_editable(app->tuned_view, FALSE);
  /* No set_monospace: its .monospace theme class fights the .decode-pane
   * font-family rule — the pane's face comes from the CSS provider alone. */
  gtk_text_view_set_cursor_visible(app->tuned_view, FALSE);
  gtk_text_view_set_wrap_mode(app->tuned_view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(app->tuned_view, 12);
  gtk_text_view_set_top_margin(app->tuned_view, 4);
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
