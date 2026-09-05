/* main.c — skimmer-for-linux GTK4/libadwaita front-end (M5, M8).
 *
 * A light window over the headless engine pipeline: the waterfall with its
 * callsign column (M8 — CW Skimmer's layout; the frequency-sorted station
 * list it replaced went 2026-09-05 at Richard's call, the column's tooltip
 * carries its columns) and a bottom pane showing the decode text at the
 * radio's TUNED frequency (the vfo:0,0 the server broadcasts). Decoded text
 * is kept per frequency, so the pane always shows one frequency's history —
 * never a mixed log. A click on a callsign TUNES the radio there (the only
 * radio state the skimmer ever touches, and only on the user's click). No
 * connect button: a scanner probes
 * <host>:40001 (host set in Preferences, persisted) and the pipeline follows
 * the server up and down automatically. Engine callbacks fire on
 * engine/network threads and are coalesced into ONE queue drained by a
 * single pending idle (see "marshalled engine events") — the engine stays
 * GLib-only (src/engine/ has no GTK).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <adwaita.h>
#include <string.h>

#include "callsign.h"
#include "pane_log.h"
#include "spot_out.h"
#include "pipeline.h"
#include "wf_compose.h"
#include "wf_view.h"

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
  guint pos;                     /* position in the UNSORTED store — kept by
                                  * apply_station/apply_gone so an update can
                                  * announce itself without scanning          */
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

enum { VIEW_NONE = 0, VIEW_WF = 1 };   /* the station list view is gone   */

/* Spectrum rows are display-only: bound what a stalled UI can pile up (a
 * 16 KB row 94× a second is 1.5 MB/s of pending memory otherwise). */
#define SPEC_PENDING_MAX 48

typedef struct {
  SkimPipeline   *pipeline;      /* running pipeline (owned by main thread)   */
  SkimPipeline   *starting;      /* pipeline mid-handshake on a worker thread */
  char           *host;          /* TCI server host (persisted preference)    */
  gboolean        cq_only;       /* spot only CALLING stations (persisted)    */
  guint           spot_round;    /* outgoing spot freq grid Hz, 0=exact
                                  * (persisted; SDC-style spot accuracy)      */
  guint           dec_mode;      /* 0 = CW, 1 = RTTY (persisted [decode]) —
                                  * picks the engine backend + bank geometry;
                                  * a change reconnects the pipeline          */
  SkimRbnFeed    *rbn;           /* RBN telnet server — app-owned so
                                  * aggregator sessions ride out reconnects   */
  gboolean        rbn_enabled;   /* persisted [rbn]                           */
  char           *rbn_call;      /* spotter callsign (persisted)              */
  int             rbn_port;      /* telnet port (persisted, default 7300)     */
  int             decode_font;   /* decode pane font size, pt (persisted)     */
  GtkCssProvider *css;           /* carries the decode pane font rule         */
  gboolean        probing;       /* port probe / handshake in flight          */
  gboolean        closing;       /* close-request seen — the ONE sentinel every
                                  * late main-loop callback checks (SKM-1). The
                                  * widget tree is finalized right after it, so
                                  * a GTK_IS_LABEL() probe on app->status would
                                  * read freed memory, not guard anything.     */
  guint           status_tick_id, age_tick_id, lag_tick_id, scan_tick_id;
  double          vfo_hz;        /* the radio's tuned frequency (vfo:0,0)     */
  char            tuned_call[16]; /* the station the pane is FIXED on — set
                                  * on retune, held while it lives; header
                                  * and text feed both key off it, so they
                                  * can never disagree                        */
  double          tuned_slot_hz; /* its pinned frequency (slot key)           */
  GListStore     *stations;      /* of SkimRow — the station table's mirror;
                                  * the waterfall column and the tuned-pane
                                  * resolver read it                          */
  GHashTable     *row_by_call;   /* call → SkimRow* (owns a ref) — O(1) event
                                  * application; the per-event store scan
                                  * pegged the UI thread (2026-08-01)         */
  GtkTextBuffer  *tuned;         /* decode text at the tuned frequency        */
  GtkTextTag     *draft_tag;     /* dim: reader may still rewrite this text   */
  GtkTextTag     *scp_tag;       /* green+underline: MASTER.SCP knows this
                                  * call (Richard, 2026-07-19)                */
  GtkTextTag     *scp_dup_tag;   /* gray+underline: the logbook already has
                                  * it (DUP/B4 — Richard, 2026-08-01)         */
  GtkTextView    *tuned_view;
  gboolean        pane_hand;     /* hand cursor currently shown over the pane */
  GtkLabel       *tuned_label;
  GtkWidget      *tuned_scroll;  /* the decode pane's scroller                */
  GtkWidget      *pane_sep;      /* separator between waterfall and pane      */
  GtkWidget      *head_sep;      /* hairline under the header — waterfall only */
  guint           view;          /* top area: VIEW_WF / VIEW_NONE (persisted
                                  * [ui] view); the decode pane is ALWAYS
                                  * visible                                   */
  gboolean        view_syncing;  /* toggle set by code, not by the user       */
  GtkWidget      *wf_btn;        /* header toggle: waterfall on / off         */
  SkimWfView     *wf;            /* M8 waterfall view                         */
  int             palette;       /* waterfall colour scheme (persisted [ui]) — the
                                  * same table sdr-for-linux offers            */
  GThread        *replay_thread; /* SKIM_IQ_FILE: offline feeder (dev/demo)   */
  volatile gint   replay_run;
  char           *replay_path;
  double          replay_rate, replay_center;
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
  gboolean        resolve_pending; /* run the pane resolver ONCE at drain end
                                  * — a pileup on the VFO used to walk the
                                  * whole table per report (49 ms drains)     */

  /* SKIM_LAG_DEBUG: main-loop congestion forensics (2026-08-01 — the UI
   * thread pegged a core and nothing in the logs said why). */
  gint64          lag_prev;      /* last lag_tick arrival                     */
  guint           lag_ticks;
  guint           ctr_ev, ctr_append, ctr_retag, ctr_reload;
  gint64          ctr_drain_us;  /* worst event-drain this window             */
} App;

static SkimPipeline *pipeline_create(App *app);
static void replay_stop(App *app);
static void replay_start(App *app);

/* The bottom pane's header: the station heard on the tuned frequency (the
 * frequency itself lives in the footer). */
static void tuned_label_update(App *app, const SkimStation *st) {
  char s[128];
  if (st) {
    g_snprintf(s, sizeof(s), "%s · %.0f %s · %.0f dB",
               st->call, st->speed,
               g_strcmp0(st->mode, "RTTY") == 0 ? "Bd" : "WPM", st->snr_db);
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
  if (g_getenv("SKIM_PANE_DEBUG")) {
    g_printerr("pane: refresh vfo %.2f fixed '%s'->'%s' slot %.2f\n",
               app->vfo_hz, before, app->tuned_call, app->tuned_slot_hz);
  }
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
    /* Tint by the logbook's dup verdict, same rule as the outgoing spot
     * colour. The verdict can sharpen between overlapping scans (UNKNOWN →
     * answer arrived), so the losing tag is removed, not just outvoted. */
    const double hz = app->tuned_slot_hz > 0 ? app->tuned_slot_hz
                                             : app->vfo_hz;
    const SkimDupVerdict v = app->pipeline
        ? skim_pipeline_dup_verdict(app->pipeline, tok, hz)
        : SKIM_DUP_UNKNOWN;
    const gboolean dup = skim_spot_argb_for_dup(v) == SKIM_SPOT_ARGB_DUP;
    GtkTextTag *want  = dup ? app->scp_dup_tag : app->scp_tag;
    GtkTextTag *other = dup ? app->scp_tag : app->scp_dup_tag;
    /* Touch the tag table only on a real change: overlapping scans revisit
     * every token, and a blind remove+apply invalidates the text layout each
     * time — enough churn to stutter the whole app under contest load. */
    if (!gtk_text_iter_has_tag(&s, want) || gtk_text_iter_has_tag(&s, other)) {
      gtk_text_buffer_remove_tag(app->tuned, other, &s, &e);
      gtk_text_buffer_apply_tag(app->tuned, want, &s, &e);
      app->ctr_retag++;
    }
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
  app->ctr_reload++;
  gtk_text_buffer_set_text(app->tuned, "", -1);
  app->pane_over = 0;
  FreqLog *fl = NULL;
  if (app->tuned_call[0]) {
    fl = freqlog_find(app, app->tuned_slot_hz, FREQLOG_ROUTE_HZ);
  } else if (app->vfo_hz > 0) {
    fl = freqlog_find(app, app->vfo_hz, TUNED_WINDOW_HZ);
  }
  if (g_getenv("SKIM_PANE_DEBUG")) {
    g_printerr("pane: reload fixed '%s' slot %.2f vfo %.2f -> %s (%.2f, "
               "%zu ch)\n", app->tuned_call, app->tuned_slot_hz, app->vfo_hz,
               fl ? "hit" : "MISS", fl ? fl->freq_hz : 0.0,
               fl ? skim_pane_log_len(fl->log) : 0);
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

typedef enum { EV_STATION, EV_GONE, EV_TEXT, EV_OVER, EV_VFO, EV_STATE,
               EV_SPECTRUM } EvKind;

typedef struct {
  EvKind      kind;
  SkimStation st;                /* EV_STATION / EV_GONE                      */
  double      hz;                /* EV_TEXT/EV_OVER: channel; EV_VFO: tuned   */
  char       *str;               /* EV_TEXT/EV_OVER: text; EV_STATE: detail   */
  gboolean    connected;         /* EV_STATE                                  */
  guint       op_kind;           /* EV_OVER: SkimPaneOpKind                   */
  guint       op_erase;          /* EV_OVER: OPEN's draft take-back           */
  guint       op_final;          /* EV_OVER: reader-final prefix bytes        */
  guint8     *blob;              /* EV_SPECTRUM: nbins bytes; hz = centre     */
  guint       nbins;
  double      bin;               /* EV_SPECTRUM: bin width Hz                 */
} Ev;

static void ev_free(gpointer data) {
  Ev *ev = data;
  g_free(ev->str);
  g_free(ev->blob);
  g_free(ev);
}

static void apply_station(App *app, const SkimStation *st) {
  /* The tracker keeps ONE record per call (QSY moves it) — match by call,
   * O(1). Updating in place + a single-row items_changed re-sorts and
   * rebinds JUST this row; the old remove+append walked and churned the
   * whole store per event, and with a contest table that pegged the UI
   * thread — drain cost grew with the row count (live-measured
   * 2026-08-01: 3→16 ms/drain over minutes, main thread to 99 %). */
  SkimRow *r = g_hash_table_lookup(app->row_by_call, st->call);
  if (r) {
    r->st = *st;
    g_list_model_items_changed(G_LIST_MODEL(app->stations), r->pos, 1, 1);
  } else {
    SkimRow *nr = skim_row_new(st);
    nr->pos = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
    g_list_store_append(app->stations, nr);     /* store takes its own ref  */
    g_hash_table_insert(app->row_by_call, g_strdup(st->call), nr);
  }
  /* Keep the tuned pane's header fresh if this touches the tuned window —
   * via the sticky resolver, never straight from the event. When the
   * fixation resolves to a (new) station, pull in its history: the first
   * seconds after a retune decoded before the station validated. */
  if (app->vfo_hz > 0 && ABS(st->freq_hz - app->vfo_hz) <= TUNED_WINDOW_HZ) {
    app->resolve_pending = TRUE;         /* once per drain, at the end       */
  }
}

/* Station left the tracker (TTL / frequency takeover) — drop its row. */
static void apply_gone(App *app, const SkimStation *st) {
  SkimRow *r = g_hash_table_lookup(app->row_by_call, st->call);
  if (r) {
    const guint pos = r->pos;
    g_hash_table_remove(app->row_by_call, st->call);
    g_list_store_remove(app->stations, pos);
    /* Rare path: shift the cached positions of the successors. */
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, app->row_by_call);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
      SkimRow *o = v;
      if (o->pos > pos) { o->pos--; }
    }
  }
  const gboolean was_fixed = g_strcmp0(st->call, app->tuned_call) == 0;
  if (was_fixed) {
    app->tuned_call[0] = '\0';                 /* the fixed station left     */
  }
  /* Re-resolve the pane only when this departure could touch it — the
   * resolver walks every row, and a contest TTL-prune WAVE ran it once per
   * evicted station (lag telemetry: 24-32 ms drains, growing with the
   * table). A station far from the VFO can't change what the pane shows. */
  if (was_fixed || app->vfo_hz <= 0 ||
      ABS(st->freq_hz - app->vfo_hz) <= TUNED_WINDOW_HZ) {
    app->resolve_pending = TRUE;         /* once per drain, at the end       */
  }
}

/* The waterfall's callsign column is a snapshot of the station table: one
 * label per tracked station (CQ and S&P alike — the column is the list in
 * another shape), coloured by the logbook's verdict exactly as the pane and
 * the panadapter spots are, the pane's fixed station bold. Rebuilt at the
 * end of a drain that touched the table or the fixation (and à 2 s for
 * verdict recolours, like the pane tail) — a few hundred cache lookups,
 * and only while the waterfall is the shown view. */
static void wf_stations_sync(App *app) {
  if (!app->wf || app->view != VIEW_WF) { return; }
  const guint n = g_list_model_get_n_items(G_LIST_MODEL(app->stations));
  SkimWfStation *st = g_new0(SkimWfStation, MAX(n, 1u));
  for (guint i = 0; i < n; i++) {
    SkimRow *r = g_list_model_get_item(G_LIST_MODEL(app->stations), i);
    SkimWfStation *o = &st[i];
    g_strlcpy(o->call, r->st.call, sizeof(o->call));
    o->hz     = r->st.freq_hz;
    o->cq     = r->st.cq;
    o->snr_db = r->st.snr_db;
    o->speed  = r->st.speed;
    o->reports    = r->st.reports;
    o->last_heard = r->st.last_heard;
    g_strlcpy(o->mode, r->st.mode, sizeof(o->mode));
    o->gray   = app->pipeline &&
                skim_dup_verdict_gray(skim_pipeline_dup_verdict(app->pipeline,
                                                                r->st.call,
                                                                r->st.freq_hz));
    o->tuned  = g_strcmp0(r->st.call, app->tuned_call) == 0;
    g_object_unref(r);
  }
  skim_wf_view_set_stations(app->wf, st, n);
  g_free(st);
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
  const gboolean ok = key > 0 && ABS(freq_hz - key) <= win;
  if (!ok && g_getenv("SKIM_PANE_DEBUG")) {
    g_printerr("pane: DROP %.2f (key %.2f win %.0f fixed '%s' slot %.2f "
               "vfo %.2f)\n", freq_hz, key, win, app->tuned_call,
               app->tuned_slot_hz, app->vfo_hz);
  }
  return ok;
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
    if (app->wf) { skim_wf_view_set_vfo(app->wf, vfo_hz); }
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
    app->ctr_append++;
  }
}

/* The single drain: steal the whole queue, apply it in order. Pane text
 * accumulates across consecutive text events and flushes before any event
 * that could change routing or reload the pane (and once at the end). */
static gboolean evq_drain(gpointer data) {
  App *app = data;
  const gint64 drain_t0 = g_get_monotonic_time();
  g_mutex_lock(&app->evq_lock);
  GPtrArray *batch = app->evq;
  app->evq = g_ptr_array_new_with_free_func(ev_free);
  app->evq_scheduled = FALSE;
  g_mutex_unlock(&app->evq_lock);
  if (app->closing) {                          /* widgets are gone — drop   */
    g_ptr_array_unref(batch);
    return G_SOURCE_REMOVE;
  }

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
  gboolean wf_dirty = FALSE, st_dirty = FALSE;
  for (guint i = 0; i < batch->len; i++) {
    Ev *ev = g_ptr_array_index(batch, i);
    if (ev->kind != EV_TEXT && ev->kind != EV_SPECTRUM) { pane_flush(app, pane); }
    switch (ev->kind) {
    case EV_SPECTRUM:
      if (app->wf) {
        skim_wf_view_push(app->wf, ev->blob, ev->nbins, ev->hz, ev->bin);
        wf_dirty = TRUE;                       /* ONE queue_draw per drain   */
      }
      break;
    case EV_STATION:
      if (GPOINTER_TO_UINT(g_hash_table_lookup(last, ev->st.call)) == i + 1) {
        apply_station(app, &ev->st);
        st_dirty = TRUE;
      }
      break;
    case EV_GONE:
      apply_gone(app, &ev->st);
      st_dirty = TRUE;
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
      st_dirty = TRUE;                         /* the fixation may move      */
      break;
    case EV_STATE:
      apply_state(app, ev->connected, ev->str);
      break;
    }
  }
  pane_flush(app, pane);
  g_string_free(pane, TRUE);
  g_hash_table_unref(last);
  if (wf_dirty) { gtk_widget_queue_draw(GTK_WIDGET(app->wf)); }
  if (app->resolve_pending) {
    /* One resolver pass over the FINAL table state covers every station/gone
     * event of the batch — running it per event walked the table once per
     * pileup report (49 ms drains, live-measured 2026-08-01). */
    app->resolve_pending = FALSE;
    if (tuned_station_refresh(app)) { tuned_pane_reload(app); }
    st_dirty = TRUE;
  }
  if (st_dirty) { wf_stations_sync(app); }     /* after the resolver: the
                                                * bold label is the pane's   */
  app->ctr_ev += batch->len;
  g_ptr_array_unref(batch);
  const gint64 drain_us = g_get_monotonic_time() - drain_t0;
  if (drain_us > app->ctr_drain_us) { app->ctr_drain_us = drain_us; }
  return G_SOURCE_REMOVE;
}

/* Engine-thread side: append + schedule the drain unless one is pending. */
static void ev_post(App *app, Ev *ev) {
  g_mutex_lock(&app->evq_lock);
  if (ev->kind == EV_SPECTRUM) {
    guint n = 0, first = G_MAXUINT;
    for (guint i = 0; i < app->evq->len; i++) {
      const Ev *e = g_ptr_array_index(app->evq, i);
      if (e->kind == EV_SPECTRUM) { n++; if (first == G_MAXUINT) { first = i; } }
    }
    if (n >= SPEC_PENDING_MAX) { g_ptr_array_remove_index(app->evq, first); }
  }
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
static void pipe_spectrum_cb(const guint8 *row, guint nbins, double center_hz,
                             double bin_hz, gpointer user) {
  Ev *ev = g_new0(Ev, 1);
  ev->kind  = EV_SPECTRUM;
  ev->blob  = g_memdup2(row, nbins);
  ev->nbins = nbins;
  ev->hz    = center_hz;
  ev->bin   = bin_hz;
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
/* SKIM_LAG_DEBUG: a 250 ms heartbeat on the main loop. A late beat means
 * something hogged the UI thread; the 5 s summary says what the loop was
 * busy with (engine events, pane appends, tag churn, pane reloads). */
static gboolean lag_tick(gpointer data) {
  App *app = data;
  if (app->closing) { return G_SOURCE_REMOVE; }
  const gint64 now = g_get_monotonic_time();
  if (app->lag_prev && now - app->lag_prev > 750 * 1000) {
    g_message("lag: main loop stalled %.0f ms",
              (now - app->lag_prev - 250 * 1000) / 1000.0);
  }
  app->lag_prev = now;
  if (++app->lag_ticks >= 20) {
    g_message("lag: 5s ev=%u append=%u retag=%u reload=%u worst-drain=%.1f ms",
              app->ctr_ev, app->ctr_append, app->ctr_retag, app->ctr_reload,
              app->ctr_drain_us / 1000.0);
    app->lag_ticks = 0;
    app->ctr_ev = app->ctr_append = app->ctr_retag = app->ctr_reload = 0;
    app->ctr_drain_us = 0;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean age_tick(gpointer data) {
  App *app = data;
  if (app->closing) { return G_SOURCE_REMOVE; }
  /* Re-tint the recent pane tail with fresh logbook verdicts — a call
   * logged a moment ago must gray out in place, not only in new text.
   * The no-churn guard in scp_tag_token makes an unchanged pass free. */
  scp_highlight(app, 2000);
  wf_stations_sync(app);                       /* verdict recolours          */
  return G_SOURCE_CONTINUE;
}

static gboolean status_tick(gpointer data) {
  App *app = data;
  if (app->closing) { return G_SOURCE_REMOVE; }
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

static int settings_load_palette(void) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  int v = 0;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) &&
      g_key_file_has_key(kf, "ui", "palette", NULL)) {
    v = g_key_file_get_integer(kf, "ui", "palette", NULL);
  }
  g_key_file_free(kf);
  g_free(path);
  return (v < 0 || v >= skim_wf_palette_count()) ? 0 : v;
}

/* The top view: the waterfall (default — with the callsign column it is the
 * view one works from, Richard 2026-09-05) or nothing (pane only). A saved
 * choice wins; "list" and the pre-M8 `station_list` key fall to the
 * waterfall, the station list's successor (the list went 2026-09-05). */
static guint settings_load_view(void) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  guint v = VIEW_WF;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    char *s = g_key_file_get_string(kf, "ui", "view", NULL);
    if (s) {
      v = g_strcmp0(s, "none") == 0 ? VIEW_NONE : VIEW_WF;
      g_free(s);
    } else if (g_key_file_has_key(kf, "ui", "station_list", NULL)) {
      /* pre-M8 settings: the list toggle alone */
      v = g_key_file_get_boolean(kf, "ui", "station_list", NULL) ? VIEW_WF
                                                                  : VIEW_NONE;
    }
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

static guint settings_load_mode(void) {
  char *path = settings_file();
  GKeyFile *kf = g_key_file_new();
  guint v = 0;                                 /* CW by default              */
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    char *m = g_key_file_get_string(kf, "decode", "mode", NULL);
    if (m && g_ascii_strcasecmp(m, "rtty") == 0) { v = 1; }
    g_free(m);
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
  g_key_file_set_string(kf, "decode", "mode", app->dec_mode ? "rtty" : "cw");
  g_key_file_set_boolean(kf, "spots", "cq_only", app->cq_only);
  g_key_file_set_integer(kf, "spots", "round_hz", (gint)app->spot_round);
  g_key_file_set_boolean(kf, "rbn", "enabled", app->rbn_enabled);
  g_key_file_set_string(kf, "rbn", "call", app->rbn_call);
  g_key_file_set_integer(kf, "rbn", "port", app->rbn_port);
  g_key_file_set_integer(kf, "ui", "decode_font_pt", app->decode_font);
  g_key_file_set_integer(kf, "ui", "palette", app->palette);
  g_key_file_set_string(kf, "ui", "view",
                        app->view == VIEW_WF ? "waterfall" : "none");
  g_key_file_remove_key(kf, "ui", "station_list", NULL);   /* pre-M8 key   */
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

/* The top area shows the waterfall or nothing (pane only) — the header-bar
 * toggle next to the primary menu; the decode pane stays put and takes over
 * the space when the waterfall hides. The engine computes spectrum rows
 * ONLY while the waterfall shows. (Until 2026-09-05 a second toggle put the
 * station list here; the column's tooltip carries its columns now.) */
static void view_apply(App *app) {
  const gboolean top = app->view == VIEW_WF;
  gtk_widget_set_visible(GTK_WIDGET(app->wf), top);
  gtk_widget_set_visible(app->pane_sep, top);
  gtk_widget_set_visible(app->head_sep, top);
  gtk_widget_set_vexpand(app->tuned_scroll, !top);
  if (app->pipeline) {
    skim_pipeline_set_spectrum_enabled(app->pipeline, top);
  }
  wf_stations_sync(app);                       /* no-op unless waterfall     */
  app->view_syncing = TRUE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->wf_btn), top);
  app->view_syncing = FALSE;
}

static void on_view_toggled(GtkToggleButton *btn, gpointer user) {
  App *app = user;
  if (app->view_syncing) { return; }
  const guint want = gtk_toggle_button_get_active(btn) ? VIEW_WF : VIEW_NONE;
  if (want != app->view) {
    app->view = want;
    settings_save(app);
  }
  view_apply(app);
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
  if (app->closing) {
    /* The window went away during the handshake. The start ran on its
     * worker thread, so teardown could not touch it — reap it here (a
     * failed start has no engine thread; free is stop + release). */
    g_clear_error(&err);
    g_clear_pointer(&app->starting, skim_pipeline_free);
    return;
  }
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
  if (app->wf) { skim_wf_view_set_vfo(app->wf, app->vfo_hz); }
  skim_pipeline_set_spectrum_enabled(app->pipeline, app->view == VIEW_WF);
  tuned_label_update(app, NULL);
}

static void probe_done(GObject *src, GAsyncResult *res, gpointer user) {
  App *app = user;
  GError *err = NULL;
  GSocketConnection *conn =
      g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, &err);
  if (app->closing) {                          /* window went mid-probe      */
    if (conn) {
      g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
      g_object_unref(conn);
    }
    g_clear_error(&err);
    app->probing = FALSE;
    return;
  }
  if (!conn) {
    g_clear_error(&err);
    app->probing = FALSE;
    return;
  }
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);

  /* The TCI port answers — bring the pipeline up off the main thread. */
  app->starting = pipeline_create(app);
  adw_window_title_set_subtitle(app->title, "connecting…");
  GTask *t = g_task_new(NULL, NULL, start_pipeline_done, app);
  g_task_set_task_data(t, app->starting, NULL);
  g_task_run_in_thread(t, start_pipeline_thread);
  g_object_unref(t);
}

/* A pipeline wired to this App: MASTER.SCP if present, the per-day decode
 * log, the telnet feed, and every UI callback (all of them just ev_post —
 * whichever thread the engine runs on). Shared by the live path (probe_done)
 * and the offline replay (SKIM_IQ_FILE). */
static SkimPipeline *pipeline_create(App *app) {
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
    .mode = app->dec_mode ? SKIM_PIPELINE_MODE_RTTY : SKIM_PIPELINE_MODE_CW,
    .chan_bw_hz = 0,                           /* mode default: 125/250 Hz   */
    .dict_path = g_file_test(dict, G_FILE_TEST_EXISTS) ? dict : NULL,
    .decode_log_path = dlog,
    .rbn = app->rbn,                           /* NULL when the feed is off  */
  };
  SkimPipeline *p = skim_pipeline_new(&cfg);
  g_free(dict);
  g_free(dlog);
  skim_pipeline_set_station_cb(p, pipe_station_cb, app);
  skim_pipeline_set_station_gone_cb(p, pipe_gone_cb, app);
  skim_pipeline_set_text_cb(p, pipe_text_cb, app);
  skim_pipeline_set_over_cb(p, pipe_over_cb, app);
  skim_pipeline_set_state_cb(p, pipe_state_cb, app);
  skim_pipeline_set_vfo_cb(p, pipe_vfo_cb, app);
  skim_pipeline_set_spectrum_cb(p, pipe_spectrum_cb, app);
  return p;
}

/* --- offline replay into the UI (SKIM_IQ_FILE) ------------------------------------------
 * Development/demo path: SKIM_IQ_FILE=<capture.cf32> feeds a recording through
 * the OFFLINE pipeline at real-time pace on its own thread, looping at EOF —
 * no TCI, no radio, the port scanner never starts. Rate and centre come from
 * the capture's .meta sidecar (rate_hz: / center_hz:, as skimmer-tci-probe
 * writes it) or SKIM_IQ_RATE / SKIM_IQ_CENTER. Callbacks arrive on the feeder
 * thread exactly as the engine thread's do — the same event queue. */
static double meta_num(const char *meta_path, const char *key) {
  char *txt = NULL;
  double v = 0;
  if (g_file_get_contents(meta_path, &txt, NULL, NULL)) {
    for (char *line = txt; line && *line; ) {
      char *nl = strchr(line, '\n');
      if (g_str_has_prefix(line, key)) { v = g_ascii_strtod(line + strlen(key), NULL); break; }
      line = nl ? nl + 1 : NULL;
    }
    g_free(txt);
  }
  return v;
}

static gpointer replay_thread(gpointer data) {
  App *app = data;
  const guint blk = 8192;
  float *buf = g_new(float, 2 * blk);
  while (g_atomic_int_get(&app->replay_run)) {
    FILE *f = fopen(app->replay_path, "rb");
    if (!f) { g_warning("replay: cannot open %s", app->replay_path); break; }
    const gint64 t0 = g_get_monotonic_time();
    guint64 frames = 0;
    size_t n;
    while (g_atomic_int_get(&app->replay_run) &&
           (n = fread(buf, 2 * sizeof(float), blk, f)) > 0) {
      skim_pipeline_feed(app->pipeline, buf, (guint)n, app->replay_rate,
                         app->replay_center);
      frames += n;
      const gint64 due = t0 + (gint64)((double)frames / app->replay_rate * G_USEC_PER_SEC);
      const gint64 now = g_get_monotonic_time();
      if (due > now) { g_usleep((gulong)(due - now)); }
    }
    fclose(f);
    if (g_atomic_int_get(&app->replay_run)) {
      g_message("replay: %s — end of file, looping", app->replay_path);
    }
  }
  g_free(buf);
  return NULL;
}

static void replay_stop(App *app) {
  if (!app->replay_thread) { return; }
  g_atomic_int_set(&app->replay_run, 0);
  g_thread_join(app->replay_thread);
  app->replay_thread = NULL;
}

static void replay_start(App *app) {
  const char *path = g_getenv("SKIM_IQ_FILE");
  if (!path || !*path) { return; }
  g_free(app->replay_path);
  app->replay_path = g_strdup(path);
  char *meta = g_strdup_printf("%s.meta", path);
  const char *er = g_getenv("SKIM_IQ_RATE"), *ec = g_getenv("SKIM_IQ_CENTER");
  app->replay_rate   = er ? g_ascii_strtod(er, NULL) : meta_num(meta, "rate_hz:");
  app->replay_center = ec ? g_ascii_strtod(ec, NULL) : meta_num(meta, "center_hz:");
  g_free(meta);
  if (app->replay_rate <= 0 || app->replay_center <= 0) {
    g_warning("replay: %s — no rate/centre (.meta sidecar or SKIM_IQ_RATE/SKIM_IQ_CENTER)", path);
    return;
  }
  SkimPipeline *p = pipeline_create(app);
  GError *err = NULL;
  if (!skim_pipeline_start_offline(p, &err)) {
    g_warning("replay: %s", err ? err->message : "offline start failed");
    g_clear_error(&err);
    skim_pipeline_free(p);
    return;
  }
  app->pipeline = p;
  skim_pipeline_set_spectrum_enabled(p, app->view == VIEW_WF);
  char *base = g_path_get_basename(path);
  char *sub  = g_strdup_printf("replay: %s · %.0f k · %.3f kHz", base,
                               app->replay_rate / 1000.0, app->replay_center / 1000.0);
  adw_window_title_set_subtitle(app->title, sub);
  g_free(sub);
  g_free(base);
  g_message("replay: %s at %.0f Hz, centre %.0f Hz (offline pipeline, looping)",
            path, app->replay_rate, app->replay_center);
  g_atomic_int_set(&app->replay_run, 1);
  app->replay_thread = g_thread_new("skim-replay", replay_thread, app);
}

static gboolean scan_tick(gpointer data) {
  App *app = data;
  if (app->closing) { return G_SOURCE_REMOVE; }
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

static void on_pref_palette(AdwComboRow *r, GParamSpec *ps, gpointer user) {
  (void)ps;
  App *app = user;
  const int sel = (int)adw_combo_row_get_selected(r);
  if (sel < 0 || sel >= skim_wf_palette_count() || sel == app->palette) { return; }
  app->palette = sel;
  skim_wf_view_set_palette(app->wf, sel);      /* live, whole history        */
  settings_save(app);
}

static void prefs_closed(AdwDialog *dlg, gpointer user) {
  App *app = user;
  GtkWidget *row  = g_object_get_data(G_OBJECT(dlg), "host-row");
  GtkWidget *mrow = g_object_get_data(G_OBJECT(dlg), "mode-row");
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
  guint dec_mode = MIN(adw_combo_row_get_selected(ADW_COMBO_ROW(mrow)), 1u);
  gboolean host_changed = host[0] && g_strcmp0(host, app->host) != 0;
  gboolean mode_changed = dec_mode != app->dec_mode;
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
  if (mode_changed) {
    app->dec_mode = dec_mode;
  }
  if (host_changed || mode_changed || cq_changed || round_changed ||
      font_changed || rbn_changed) {
    settings_save(app);
  }
  /* The pipeline's config carries the feed pointer — an RBN change needs a
   * fresh pipeline just like a host change does; a mode change swaps the
   * backend and the bank geometry, which only a rebuild can do. */
  if (host_changed || mode_changed || rbn_changed) {
    const gboolean replaying = app->replay_thread != NULL;
    if (replaying) { replay_stop(app); }        /* feeder off BEFORE the free */
    if (app->pipeline) {
      skim_pipeline_stop(app->pipeline);
      g_clear_pointer(&app->pipeline, skim_pipeline_free);
      app->vfo_hz = 0;
      app->tuned_call[0] = '\0';
      app->tuned_slot_hz = 0;
      tuned_label_update(app, NULL);
    }
    if (replaying) { replay_start(app); } else { scan_tick(app); }
  }
}

/* One tab of the Preferences dialog — the family look (sdr-for-linux's
 * Radio / CW / TCI / Audio pages): a title and a symbolic icon; libadwaita
 * shows the switcher as soon as the dialog has more than one page. */
static AdwPreferencesPage *prefs_page(AdwDialog *dlg, const char *title,
                                      const char *icon) {
  AdwPreferencesPage *p = ADW_PREFERENCES_PAGE(g_object_new(
      ADW_TYPE_PREFERENCES_PAGE, "title", title, "icon-name", icon, NULL));
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg), p);
  return p;
}

static void prefs_open(GtkButton *btn, gpointer user) {
  (void)btn;
  App *app = user;
  AdwDialog *dlg = adw_preferences_dialog_new();
  adw_dialog_set_title(dlg, "Preferences");
  /* Four tabs (Richard, 2026-09-05 — "there is getting to be a lot in
   * there"): Radio = the TCI link, Decoding = the engine, Spots = every
   * spot output (panadapter policy + telnet feed), Display = the look. */
  AdwPreferencesPage *p_radio = prefs_page(dlg, "Radio", "network-transmit-receive-symbolic");
  AdwPreferencesPage *p_dec   = prefs_page(dlg, "Decoding", "input-keyboard-symbolic");
  AdwPreferencesPage *p_spots = prefs_page(dlg, "Spots", "mark-location-symbolic");
  AdwPreferencesPage *p_disp  = prefs_page(dlg, "Display", "video-display-symbolic");
  GtkWidget *grp  = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(grp), "TCI server");
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(grp),
      "sdr-for-linux WebSocket host, port 40001 — the connection is automatic");
  GtkWidget *row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Host");
  gtk_editable_set_text(GTK_EDITABLE(row), app->host);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(grp), row);
  adw_preferences_page_add(p_radio, ADW_PREFERENCES_GROUP(grp));

  GtkWidget *dgrp = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dgrp), "Decoding");
  GtkWidget *mrow = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(mrow), "Mode");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(mrow),
      "The whole IQ segment decodes as one mode — a change reconnects "
      "the engine");
  static const char *MODES[] = { "CW", "RTTY", NULL };
  adw_combo_row_set_model(ADW_COMBO_ROW(mrow),
                          G_LIST_MODEL(gtk_string_list_new(MODES)));
  adw_combo_row_set_selected(ADW_COMBO_ROW(mrow), app->dec_mode);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(dgrp), mrow);
  adw_preferences_page_add(p_dec, ADW_PREFERENCES_GROUP(dgrp));

  GtkWidget *sgrp = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(sgrp), "Spots");
  GtkWidget *sw = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sw), "CQ only");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(sw),
      "Spot only stations heard calling (CQ, TEST, QRZ) — "
      "S&amp;P answers stay off the panadapter");
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
  adw_preferences_page_add(p_spots, ADW_PREFERENCES_GROUP(sgrp));

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
  adw_preferences_page_add(p_spots, ADW_PREFERENCES_GROUP(rgrp));

  GtkWidget *ugrp = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(ugrp), "Display");
  GtkWidget *frow = adw_spin_row_new_with_range(8, 32, 1);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(frow),
                                "Decode pane font size (pt)");
  adw_spin_row_set_value(ADW_SPIN_ROW(frow), app->decode_font);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(ugrp), frow);
  /* Colour scheme — the same palette table as sdr-for-linux's waterfall, so
   * the two apps can be set alike; applies live, the whole history
   * recolours at once (Richard, 2026-09-05). */
  const int npal = skim_wf_palette_count();
  const char **pnames = g_new0(const char *, npal + 1);
  for (int i = 0; i < npal; i++) { pnames[i] = skim_wf_palette_name(i); }
  GtkStringList *pl = gtk_string_list_new(pnames);
  g_free(pnames);
  GtkWidget *prow = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(prow), "Colour scheme");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(prow), "Waterfall palette");
  adw_combo_row_set_model(ADW_COMBO_ROW(prow), G_LIST_MODEL(pl));
  g_object_unref(pl);
  adw_combo_row_set_selected(ADW_COMBO_ROW(prow), (guint)app->palette);
  g_signal_connect(prow, "notify::selected", G_CALLBACK(on_pref_palette), app);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(ugrp), prow);
  adw_preferences_page_add(p_disp, ADW_PREFERENCES_GROUP(ugrp));

  g_object_set_data(G_OBJECT(dlg), "host-row", row);
  g_object_set_data(G_OBJECT(dlg), "mode-row", mrow);
  g_object_set_data(G_OBJECT(dlg), "cq-row", sw);
  g_object_set_data(G_OBJECT(dlg), "round-row", qrow);
  g_object_set_data(G_OBJECT(dlg), "font-row", frow);
  g_object_set_data(G_OBJECT(dlg), "rbn-row", rsw);
  g_object_set_data(G_OBJECT(dlg), "rbn-call-row", rcall);
  g_object_set_data(G_OBJECT(dlg), "rbn-port-row", rport);
  g_signal_connect(dlg, "closed", G_CALLBACK(prefs_closed), app);
  adw_dialog_present(dlg, GTK_WIDGET(app->window));
}

/* --- about --------------------------------------------------------------------------------
 * The GNOME-correct About every app of the family owes its user (SCOPE,
 * Richard 2026-08-04): the version must be findable FROM THE UI, the strings
 * must agree with the .desktop entry and the AppStream metainfo, and
 * vendored code is acknowledged. Field set per sdr-for-linux's About,
 * debug_info per log-for-linux's. */

static void act_about(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action; (void)param;
  App *app = user;
  AdwDialog *dlg = adw_about_dialog_new();
  AdwAboutDialog *ad = ADW_ABOUT_DIALOG(dlg);
  adw_about_dialog_set_application_name(ad, "Skimmer for Linux");
  /* = the GApplication id = the installed icon's file name; anything else
   * shows a generic gear here. */
  adw_about_dialog_set_application_icon(ad, "cz.ok1br.skimmer_for_linux");
  adw_about_dialog_set_version(ad, SKIMMER_VERSION);
  adw_about_dialog_set_developer_name(ad, "Richard Fakenberg, OK1BR");
  /* Same one-liner the metainfo <summary> and the .desktop Comment carry. */
  adw_about_dialog_set_comments(ad,
      "Multi-channel CW/RTTY skimmer and spot feeder");
  adw_about_dialog_set_copyright(ad, "© 2026 Richard Fakenberg, OK1BR");
  adw_about_dialog_set_license_type(ad, GTK_LICENSE_GPL_3_0);
  adw_about_dialog_set_website(ad,
      "https://github.com/OK1BR/skimmer-for-linux");
  adw_about_dialog_set_issue_url(ad,
      "https://github.com/OK1BR/skimmer-for-linux/issues");
  char feed[48];
  if (app->rbn) {
    g_snprintf(feed, sizeof(feed), "port %u (%u clients)",
               skim_rbn_feed_port(app->rbn), skim_rbn_feed_clients(app->rbn));
  } else {
    g_strlcpy(feed, "off", sizeof(feed));
  }
  char scp_note[32];
  if (skim_callsign_dict_size() > 0) {
    g_snprintf(scp_note, sizeof(scp_note), "%u calls",
               (guint)skim_callsign_dict_size());
  } else {
    g_strlcpy(scp_note, "not loaded", sizeof(scp_note));
  }
  /* Versions and paths, pasteable into a bug report via the Copy button. */
  char *dbg = g_strdup_printf(
      "GTK %u.%u.%u, libadwaita %u.%u.%u\n"
      "TCI: %s:40001\n"
      "Mode: %s\n"
      "Telnet feed: %s\n"
      "Settings: %s/skimmer-for-linux/settings.ini\n"
      "MASTER.SCP: %s/skimmer-for-linux/master.scp (%s)\n"
      "Decode logs: %s/skimmer-for-linux/",
      gtk_get_major_version(), gtk_get_minor_version(),
      gtk_get_micro_version(),
      adw_get_major_version(), adw_get_minor_version(),
      adw_get_micro_version(),
      app->host, app->dec_mode ? "RTTY" : "CW", feed,
      g_get_user_config_dir(), g_get_user_config_dir(), scp_note,
      g_get_user_data_dir());
  adw_about_dialog_set_debug_info(ad, dbg);
  g_free(dbg);
  const char *vendored[] = { "WDSP — Warren Pratt NR0V", NULL };
  adw_about_dialog_add_acknowledgement_section(ad, "Vendored libraries",
                                               vendored);
  adw_dialog_present(dlg, GTK_WIDGET(app->window));
}

static void act_prefs(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action; (void)param;
  prefs_open(NULL, user);
}

/* A click on a callsign in the waterfall column is the panadapter-spot
 * gesture: tune the radio to the station's exact frequency, fix the pane on
 * it, and announce the click over TCI so the logbook prefills its Call entry
 * — the same path a click on a call in the decode pane takes (Richard,
 * 2026-08-01). */
static void on_wf_call_clicked(const char *call, double hz, gpointer user) {
  App *app = user;
  if (app->pipeline) {
    skim_pipeline_tune(app->pipeline, hz);
    skim_pipeline_spot_clicked(app->pipeline, call, hz);
  }
  g_strlcpy(app->tuned_call, call, sizeof(app->tuned_call));
  app->tuned_slot_hz = hz;
  SkimRow *r = g_hash_table_lookup(app->row_by_call, call);
  tuned_label_update(app, r ? &r->st : NULL);
  tuned_pane_reload(app);
  wf_stations_sync(app);
  if (g_getenv("SKIM_PANE_DEBUG")) {
    g_message("wf click: %s @ %.0f Hz — tune + clicked_on_spot%s", call, hz,
              app->pipeline ? "" : " (no pipeline — not sent)");
  }
}

/* --- clickable callsigns in the decode pane ----------------------------------------------- */

/* The whitespace-delimited token under a widget coordinate, trimmed to its
 * [A-Z0-9/] core — NULL when the pointer sits on space, past the text, or
 * the token is no callsign. Caller frees. */
static char *pane_call_at(App *app, double wx, double wy) {
  gint bx, by;
  gtk_text_view_window_to_buffer_coords(app->tuned_view, GTK_TEXT_WINDOW_WIDGET,
                                        (gint)wx, (gint)wy, &bx, &by);
  GtkTextIter it;
  if (!gtk_text_view_get_iter_at_location(app->tuned_view, &it, bx, by)) {
    return NULL;
  }
  if (gtk_text_iter_ends_line(&it) ||
      g_unichar_isspace(gtk_text_iter_get_char(&it))) {
    return NULL;
  }
  GtkTextIter s = it, e = it;
  while (!gtk_text_iter_starts_line(&s)) {
    GtkTextIter p = s;
    gtk_text_iter_backward_char(&p);
    if (g_unichar_isspace(gtk_text_iter_get_char(&p))) { break; }
    s = p;
  }
  while (!gtk_text_iter_ends_line(&e)) {
    if (g_unichar_isspace(gtk_text_iter_get_char(&e))) { break; }
    gtk_text_iter_forward_char(&e);
  }
  char *tok = gtk_text_buffer_get_text(app->tuned, &s, &e, FALSE);
  /* Strip punctuation and over separators off the ends ("OK1BR?", "·");
   * anything non-call left inside fails validation below. */
  char *a = tok;
  while (*a && !g_ascii_isalnum(*a) && *a != '/') { a++; }
  char *z = a + strlen(a);
  while (z > a && !g_ascii_isalnum(z[-1]) && z[-1] != '/') { z--; }
  char *call = g_strndup(a, (gsize)(z - a));
  g_free(tok);
  if (!call[0] || !skim_callsign_is_valid(call)) {
    g_free(call);
    return NULL;
  }
  return call;
}

/* A click on a decoded callsign behaves like a panadapter spot click in
 * sdr-for-linux: tune the radio to the station and announce the click over
 * TCI — the server relays it and log-for-linux prefills its Call entry
 * (SCOPE, Richard 2026-08-01). The exact station frequency (pinned slot)
 * beats the 100 Hz-stepped VFO: the logger's QSY staleness check gets the
 * carrier the station actually sits on. */
static void on_pane_click(GtkGestureClick *g, gint n_press, double x, double y,
                          gpointer user) {
  (void)g; (void)n_press;
  App *app = user;
  const gboolean dbg = g_getenv("SKIM_PANE_DEBUG") != NULL;
  if (gtk_text_buffer_get_has_selection(app->tuned)) {
    if (dbg) { g_message("pane click: release at %.0f,%.0f — drag-select, skip", x, y); }
    return;
  }
  char *call = pane_call_at(app, x, y);
  if (!call) {
    if (dbg) { g_message("pane click: release at %.0f,%.0f — no call there", x, y); }
    return;
  }
  const double hz = app->tuned_slot_hz > 0 ? app->tuned_slot_hz : app->vfo_hz;
  if (app->pipeline && hz > 0) {
    if (dbg) { g_message("pane click: %s @ %.0f Hz — tune + clicked_on_spot", call, hz); }
    skim_pipeline_tune(app->pipeline, hz);
    skim_pipeline_spot_clicked(app->pipeline, call, hz);
  } else if (dbg) {
    g_message("pane click: %s but hz=%.0f pipeline=%p — NOT sent",
              call, hz, (void *)app->pipeline);
  }
  g_free(call);
}

/* SKIM_PANE_DEBUG forensics for the click path: a press that never makes it
 * to a release means the text view's own gestures claimed the sequence and
 * ours got cancelled. */
static void on_pane_press_dbg(GtkGestureClick *g, gint n_press, double x,
                              double y, gpointer user) {
  (void)g; (void)user;
  g_message("pane click: press %d at %.0f,%.0f", n_press, x, y);
}
static void on_pane_cancel_dbg(GtkGesture *g, GdkEventSequence *seq,
                               gpointer user) {
  (void)g; (void)seq; (void)user;
  g_message("pane click: sequence CANCELLED (claimed elsewhere)");
}

/* Hand cursor over anything clickable — the pane's only affordance. The
 * cursor is set only on a state CHANGE: set_cursor_from_name allocates a
 * fresh GdkCursor, and motion events come by the hundred per second. */
static void on_pane_motion(GtkEventControllerMotion *m, double x, double y,
                           gpointer user) {
  (void)m;
  App *app = user;
  char *call = pane_call_at(app, x, y);
  const gboolean hand = call != NULL;
  g_free(call);
  if (hand != app->pane_hand) {
    app->pane_hand = hand;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(app->tuned_view),
                                    hand ? "pointer" : "text");
  }
}

/* --- teardown (SKM-1) ------------------------------------------------------------------------
 * Closing the window used to leave the four periodic sources attached. GTK
 * destroys the widget tree synchronously inside the close-request dispatch,
 * and GLib finishes the SAME iteration's dispatch list before the run loop
 * notices the application released its last window — so any tick that was
 * due together with the close event ran against finalized widgets (YO DX HF
 * 2026-08-22: gtk_label_set_text + adw_window_title_set_subtitle criticals in
 * one millisecond, 17 min after the last event; reproduced deterministically
 * under gdb 2026-09-05 — close from age_tick with scan_tick due). Everything
 * that can reach the main loop late checks ONE sentinel, app->closing, and
 * the pipeline is stopped here — its engine thread is joined, so no event is
 * minted after this point. Idempotent: close-request runs it with the widgets
 * intact; the application's shutdown signal is the backstop for any other
 * exit path (it touches no widget). */
static void app_teardown(App *app) {
  if (app->closing) { return; }
  app->closing = TRUE;                         /* before stop: its state_cb
                                                * posts an event the drain
                                                * must now drop             */
  g_clear_handle_id(&app->status_tick_id, g_source_remove);
  g_clear_handle_id(&app->age_tick_id, g_source_remove);
  g_clear_handle_id(&app->lag_tick_id, g_source_remove);
  g_clear_handle_id(&app->scan_tick_id, g_source_remove);
  replay_stop(app);                            /* SKIM_IQ_FILE feeder, if any */
  if (app->pipeline) {
    skim_pipeline_stop(app->pipeline);
    g_clear_pointer(&app->pipeline, skim_pipeline_free);
  }
  /* app->starting is mid-handshake on a worker thread and cannot be freed
   * from here — start_pipeline_done reaps it (or process exit does, once
   * the loop is gone). The telnet feed goes AFTER the pipeline: the
   * pipeline's feed spot_out was built on it. */
  g_clear_pointer(&app->rbn, skim_rbn_feed_free);
  g_message("app: window closed — engine stopped, timers cleared");
}

static gboolean on_close_request(GtkWindow *win, gpointer user) {
  (void)win;
  app_teardown(user);
  return FALSE;                                /* proceed with the close     */
}

static void on_shutdown(GApplication *a, gpointer user) {
  (void)a;
  app_teardown(user);
}

/* --- activate ----------------------------------------------------------------------------- */

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
  (void)user_data;
  /* The application is unique per session: a second launch forwards its
   * activate HERE, and this function used to build a second window, App,
   * timer set and pipeline, and a feed that could not bind its port
   * (SKM-6, reproduced 2026-09-05). Present the window we have instead. */
  GtkWindow *existing = gtk_application_get_active_window(gtk_app);
  if (existing) {
    g_message("app: second launch — presenting the existing window");
    gtk_window_present(existing);
    return;
  }
  App *app = g_new0(App, 1);
  g_mutex_init(&app->evq_lock);
  app->evq = g_ptr_array_new_with_free_func(ev_free);

  GtkWidget *window = adw_application_window_new(gtk_app);
  gtk_window_set_title(GTK_WINDOW(window), "Skimmer for Linux");
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 640);
  app->window       = GTK_WINDOW(window);
  g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), app);
  g_signal_connect(gtk_app, "shutdown", G_CALLBACK(on_shutdown), app);
  app->host         = settings_load_host();
  app->dec_mode     = settings_load_mode();
  app->cq_only      = settings_load_cq_only();
  app->spot_round   = settings_load_spot_round();
  app->decode_font  = settings_load_decode_font();
  app->view         = settings_load_view();
  app->palette      = settings_load_palette();
  settings_load_rbn(app);
  rbn_apply(app);                /* the telnet server is up before the radio */

  app->title = ADW_WINDOW_TITLE(adw_window_title_new("Skimmer for Linux", ""));
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                  GTK_WIDGET(app->title));

  /* Primary menu (family form, same as sdr-for-linux): Preferences moved
   * in from the old standalone gear button; About LAST, per the GNOME HIG. */
  {
    GSimpleAction *pa = g_simple_action_new("preferences", NULL);
    g_signal_connect(pa, "activate", G_CALLBACK(act_prefs), app);
    g_action_map_add_action(G_ACTION_MAP(gtk_app), G_ACTION(pa));
    g_object_unref(pa);
    GSimpleAction *aa = g_simple_action_new("about", NULL);
    g_signal_connect(aa, "activate", G_CALLBACK(act_about), app);
    g_action_map_add_action(G_ACTION_MAP(gtk_app), G_ACTION(aa));
    g_object_unref(aa);
    GMenu *m = g_menu_new();
    g_menu_append(m, "Preferences", "app.preferences");
    g_menu_append(m, "About Skimmer for Linux", "app.about");
    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn),
                                  "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menu_btn, "Main menu");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn),
                                   G_MENU_MODEL(m));
    g_object_unref(m);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_btn);
  }

  /* View toggle (M8): waterfall on / off — off = the decode pane alone. (A
   * linked pair with the station list until 2026-09-05.) */
  app->wf_btn = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(app->wf_btn), "view-continuous-symbolic");
  gtk_widget_set_tooltip_text(app->wf_btn, "Waterfall");
  g_signal_connect(app->wf_btn, "toggled", G_CALLBACK(on_view_toggled), app);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), app->wf_btn);

  /* The station table's mirror: one SkimRow per tracked station, kept O(1)
   * by call (apply_station / apply_gone) — the waterfall's callsign column
   * and the tuned-pane resolver read it. (Until 2026-09-05 a frequency-sorted
   * GtkColumnView showed it as a text band map; the column took over.) */
  app->freq_logs = g_ptr_array_new_with_free_func(freqlog_free);
  app->stations = g_list_store_new(SKIM_TYPE_ROW);
  app->row_by_call = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_object_unref);

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
  /* Highlight colours = the ARGB our TCI spots carry, so a call in the pane
   * matches its label on the radio panadapter — bright for new ones, gray
   * for what the logbook already has. */
  char scp_col[8], dup_col[8];
  g_snprintf(scp_col, sizeof(scp_col), "#%06X", SKIM_SPOT_ARGB & 0xFFFFFFu);
  g_snprintf(dup_col, sizeof(dup_col), "#%06X", SKIM_SPOT_ARGB_DUP & 0xFFFFFFu);
  app->scp_tag = gtk_text_buffer_create_tag(app->tuned, "scp",
                                            "foreground", scp_col,
                                            "underline", PANGO_UNDERLINE_SINGLE,
                                            NULL);
  app->scp_dup_tag = gtk_text_buffer_create_tag(app->tuned, "scp-dup",
                                                "foreground", dup_col,
                                                "underline",
                                                PANGO_UNDERLINE_SINGLE, NULL);
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
  GtkGesture *pane_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pane_click),
                                GDK_BUTTON_PRIMARY);
  g_signal_connect(pane_click, "released", G_CALLBACK(on_pane_click), app);
  if (g_getenv("SKIM_PANE_DEBUG")) {
    g_signal_connect(pane_click, "pressed", G_CALLBACK(on_pane_press_dbg), app);
    g_signal_connect(pane_click, "cancel", G_CALLBACK(on_pane_cancel_dbg), app);
  }
  gtk_widget_add_controller(tuned_view, GTK_EVENT_CONTROLLER(pane_click));
  GtkEventController *pane_motion = gtk_event_controller_motion_new();
  g_signal_connect(pane_motion, "motion", G_CALLBACK(on_pane_motion), app);
  gtk_widget_add_controller(tuned_view, pane_motion);
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
  app->status_tick_id = g_timeout_add_seconds(1, status_tick, app);
  app->age_tick_id    = g_timeout_add_seconds(2, age_tick, app);
  if (g_getenv("SKIM_LAG_DEBUG")) {
    app->lag_tick_id = g_timeout_add(250, lag_tick, app);
  }

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), header);
  /* A hairline under the header, the same faint line that separates the
   * decode pane below (Richard, 2026-09-05 — the waterfall met the header
   * with no edge). Shown ONLY while the waterfall shows, hidden for the
   * pane-only layout (his second remark). */
  app->head_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(box), app->head_sep);
  app->wf = SKIM_WF_VIEW(skim_wf_view_new());
  skim_wf_view_set_palette(app->wf, app->palette);
  skim_wf_view_set_click_cb(app->wf, on_wf_call_clicked, app);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->wf));
  app->pane_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(box), app->pane_sep);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->tuned_label));
  gtk_box_append(GTK_BOX(box), tuned_scroll);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->status));
  view_apply(app);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));

  /* Find the server: probe now, then keep scanning while disconnected —
   * unless a recording is being replayed into the UI (SKIM_IQ_FILE). */
  if (g_getenv("SKIM_IQ_FILE")) {
    replay_start(app);
  } else {
    scan_tick(app);
    app->scan_tick_id = g_timeout_add_seconds(3, scan_tick, app);
  }
}

/* --version prints and exits in the LOCAL instance, before GApplication
 * uniqueness would forward to (or disturb) a running one. On top of the
 * About dialog, never instead (the UI rule, SCOPE 2026-08-04). */
static gint on_local_options(GApplication *a, GVariantDict *opts,
                             gpointer user) {
  (void)a; (void)user;
  if (g_variant_dict_contains(opts, "version")) {
    g_print("skimmer-for-linux %s\n", SKIMMER_VERSION);
    return 0;
  }
  return -1;                                   /* continue normal startup    */
}

int main(int argc, char **argv) {
  g_set_application_name("Skimmer for Linux");

  /* The id doubles as the Wayland app-id and the icon/desktop/metainfo file
   * name — data/ installs cz.ok1br.skimmer_for_linux.svg, so this must match
   * it exactly or the window shows a generic icon. Same snake_case form as
   * sdr-for-linux and log-for-linux. */
  AdwApplication *app =
      adw_application_new("cz.ok1br.skimmer_for_linux", G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option(G_APPLICATION(app), "version", 'v',
                                G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                "Print the version and exit", NULL);
  g_signal_connect(app, "handle-local-options",
                   G_CALLBACK(on_local_options), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
