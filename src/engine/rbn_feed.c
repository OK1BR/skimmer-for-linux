/* rbn_feed.c — the CW-Skimmer-dialect telnet server behind the RBN feed
 * (M6, docs/SCOPE.md).
 *
 * All socket work lives on one dedicated thread running a private
 * GMainContext: the GSocketService accept sources and every client's
 * receive source are attached there, so the client list needs no lock.
 * skim_rbn_feed_spot() formats the spot line on the caller's thread and
 * marshals the broadcast over with g_main_context_invoke().
 *
 * Sockets are non-blocking. A client that cannot take a full line (kernel
 * buffer full — a stalled or dead aggregator) is dropped on the spot: the
 * feed is realtime, there is nothing worth queueing for a reader that
 * stopped reading. The Aggregator reconnects by itself.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "rbn_feed.h"

#include <gio/gio.h>
#include <string.h>

#define RBN_GREETING "skimmer-for-linux telnet feed\r\n"
#define RBN_PROMPT   "Please enter your call: "

typedef struct {
  SkimRbnFeed       *f;
  GSocketConnection *conn;                     /* owned                      */
  GSocket           *sock;                     /* borrowed from conn         */
  GSource           *rx;
  GString           *rxbuf;
  char               login[24];                /* empty until logged in      */
} Client;

struct _SkimRbnFeed {
  char            spotter[24];                 /* "OK1BR-#:" — the DX de …   */
  GMainContext   *ctx;
  GMainLoop      *loop;
  GThread        *thread;
  GSocketService *svc;
  guint16         port;
  GPtrArray      *clients;                     /* Client* — feed thread only */
  volatile gint   nclients;                    /* logged-in (any thread)     */
  volatile gint   lines;                       /* spot lines written         */
};

/* --- client lifecycle (feed thread) ---------------------------------------------- */

static void client_free(gpointer data) {
  Client *c = data;
  if (c->login[0]) { g_atomic_int_add(&c->f->nclients, -1); }
  if (c->rx) {
    g_source_destroy(c->rx);
    g_source_unref(c->rx);
  }
  g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
  g_object_unref(c->conn);
  g_string_free(c->rxbuf, TRUE);
  g_free(c);
}

static void client_drop(Client *c) {
  g_ptr_array_remove(c->f->clients, c);        /* frees via client_free      */
}

/* Write the whole string or fail; WOULD_BLOCK counts as failure (see top). */
static gboolean client_send(Client *c, const char *s) {
  gsize len = strlen(s), done = 0;
  while (done < len) {
    gssize n = g_socket_send(c->sock, s + done, len - done, NULL, NULL);
    if (n <= 0)
      return FALSE;
    done += (gsize)n;
  }
  return TRUE;
}

static void client_line(Client *c, char *line) {
  g_strstrip(line);
  if (!c->login[0]) {
    if (!line[0]) {                            /* empty ⇒ ask again          */
      if (!client_send(c, RBN_PROMPT)) { client_drop(c); }
      return;
    }
    for (char *p = line; *p; p++) { *p = g_ascii_toupper(*p); }
    g_strlcpy(c->login, line, sizeof(c->login));
    g_atomic_int_inc(&c->f->nclients);
    char hello[96];
    g_snprintf(hello, sizeof(hello), "%s Hello %s, spots follow.\r\n",
               c->f->spotter, c->login);
    if (!client_send(c, hello)) { client_drop(c); }
    return;
  }
  /* Logged in: this is a one-way feed — cluster commands (SET/…, SH/DX)
   * are accepted and ignored; only an explicit goodbye acts. */
  if (g_ascii_strcasecmp(line, "bye") == 0 ||
      g_ascii_strcasecmp(line, "quit") == 0) {
    client_drop(c);
  }
}

static gboolean client_rx_cb(GSocket *sock, GIOCondition cond, gpointer data) {
  Client *c = data;
  (void)sock;
  if (cond & (G_IO_HUP | G_IO_ERR)) {
    client_drop(c);
    return G_SOURCE_REMOVE;
  }
  char buf[256];
  for (;;) {
    GError *err = NULL;
    gssize n = g_socket_receive(c->sock, buf, sizeof(buf), NULL, &err);
    if (n > 0) {
      g_string_append_len(c->rxbuf, buf, n);
      continue;
    }
    if (n < 0 && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
      g_clear_error(&err);
      break;                                   /* drained                    */
    }
    g_clear_error(&err);                       /* EOF or hard error          */
    client_drop(c);
    return G_SOURCE_REMOVE;
  }
  SkimRbnFeed *f = c->f;                       /* c dies if client_line drops */
  char *nl;
  while ((nl = strchr(c->rxbuf->str, '\n')) != NULL) {
    *nl = '\0';
    char *line = g_strdup(c->rxbuf->str);
    g_string_erase(c->rxbuf, 0, (gssize)(nl - c->rxbuf->str) + 1);
    guint before = f->clients->len;
    client_line(c, line);                      /* may drop (only) c          */
    g_free(line);
    if (f->clients->len != before)
      return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean incoming_cb(GSocketService *svc, GSocketConnection *conn,
                            GObject *source, gpointer user) {
  (void)svc; (void)source;
  SkimRbnFeed *f = user;
  Client *c = g_new0(Client, 1);
  c->f     = f;
  c->conn  = g_object_ref(conn);
  c->sock  = g_socket_connection_get_socket(conn);
  c->rxbuf = g_string_new(NULL);
  g_socket_set_blocking(c->sock, FALSE);
  g_ptr_array_add(f->clients, c);
  if (!client_send(c, RBN_GREETING RBN_PROMPT)) {
    client_drop(c);
    return TRUE;
  }
  c->rx = g_socket_create_source(c->sock, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
  g_source_set_callback(c->rx, G_SOURCE_FUNC(client_rx_cb), c, NULL);
  g_source_attach(c->rx, f->ctx);
  return TRUE;
}

/* --- broadcast (marshalled to the feed thread) ------------------------------------ */

typedef struct {
  SkimRbnFeed *f;
  char        *line;
} Broadcast;

static gboolean broadcast_cb(gpointer data) {
  Broadcast *b = data;
  GPtrArray *cl = b->f->clients;
  for (guint i = cl->len; i > 0; i--) {        /* drops mutate the array     */
    Client *c = g_ptr_array_index(cl, i - 1);
    if (!c->login[0])
      continue;                                /* still at the login prompt  */
    if (client_send(c, b->line)) {
      g_atomic_int_inc(&b->f->lines);
    } else {
      g_ptr_array_remove_index(cl, i - 1);
    }
  }
  return G_SOURCE_REMOVE;
}

static void broadcast_free(gpointer data) {
  Broadcast *b = data;
  g_free(b->line);
  g_free(b);
}

void skim_rbn_feed_spot(SkimRbnFeed *f, const char *call, const char *mode,
                        double freq_hz, double snr_db, double speed) {
  g_return_if_fail(f && call && call[0]);
  GDateTime *now = g_date_time_new_now_utc();
  Broadcast *b = g_new0(Broadcast, 1);
  b->f = f;
  /* The classic cluster line the Aggregator parses; frequency in kHz. CW
   * speed is WPM; the digital backends (RTTY/PSK, later) report baud. */
  b->line = g_strdup_printf(
      "DX de %-9s %8.1f  %-12s %-4s %3.0f dB  %2.0f %s  CQ      %02d%02dZ\r\n",
      f->spotter, freq_hz / 1000.0, call, mode, snr_db, speed,
      g_strcmp0(mode, "CW") == 0 ? "WPM" : "BPS",
      g_date_time_get_hour(now), g_date_time_get_minute(now));
  g_date_time_unref(now);
  g_main_context_invoke_full(f->ctx, G_PRIORITY_DEFAULT, broadcast_cb, b,
                             broadcast_free);
}

/* --- server lifecycle -------------------------------------------------------------- */

static gpointer feed_thread(gpointer data) {
  SkimRbnFeed *f = data;
  g_main_context_push_thread_default(f->ctx);
  g_main_loop_run(f->loop);
  g_main_context_pop_thread_default(f->ctx);
  return NULL;
}

SkimRbnFeed *skim_rbn_feed_new(const char *mycall, guint16 port, GError **error) {
  g_return_val_if_fail(mycall && mycall[0], NULL);
  SkimRbnFeed *f = g_new0(SkimRbnFeed, 1);
  char up[16];
  g_strlcpy(up, mycall, sizeof(up));
  for (char *p = up; *p; p++) { *p = g_ascii_toupper(*p); }
  g_snprintf(f->spotter, sizeof(f->spotter), "%s-#:", g_strstrip(up));

  f->ctx     = g_main_context_new();
  f->loop    = g_main_loop_new(f->ctx, FALSE);
  f->clients = g_ptr_array_new_with_free_func(client_free);

  /* Create the service (and bind) with our context as thread-default, so
   * its accept sources — and thus ::incoming — dispatch on the feed thread. */
  g_main_context_push_thread_default(f->ctx);
  f->svc = g_socket_service_new();
  gboolean ok;
  if (port == 0) {
    guint16 got = g_socket_listener_add_any_inet_port(
        G_SOCKET_LISTENER(f->svc), NULL, error);
    f->port = got;
    ok = got != 0;
  } else {
    ok = g_socket_listener_add_inet_port(G_SOCKET_LISTENER(f->svc), port,
                                         NULL, error);
    f->port = port;
  }
  g_signal_connect(f->svc, "incoming", G_CALLBACK(incoming_cb), f);
  g_main_context_pop_thread_default(f->ctx);

  if (!ok) {
    g_object_unref(f->svc);
    g_ptr_array_free(f->clients, TRUE);
    g_main_loop_unref(f->loop);
    g_main_context_unref(f->ctx);
    g_free(f);
    return NULL;
  }
  f->thread = g_thread_new("skim-rbn", feed_thread, f);
  return f;
}

static gboolean shutdown_cb(gpointer data) {
  SkimRbnFeed *f = data;
  g_socket_service_stop(f->svc);
  g_socket_listener_close(G_SOCKET_LISTENER(f->svc));
  g_main_loop_quit(f->loop);
  return G_SOURCE_REMOVE;
}

void skim_rbn_feed_free(SkimRbnFeed *f) {
  if (!f)
    return;
  g_main_context_invoke(f->ctx, shutdown_cb, f);
  g_thread_join(f->thread);
  g_ptr_array_free(f->clients, TRUE);          /* thread is gone — safe      */
  g_object_unref(f->svc);
  g_main_loop_unref(f->loop);
  g_main_context_unref(f->ctx);
  g_free(f);
}

guint16 skim_rbn_feed_port(const SkimRbnFeed *f) { return f->port; }

guint skim_rbn_feed_clients(const SkimRbnFeed *f) {
  return (guint)g_atomic_int_get(&((SkimRbnFeed *)f)->nclients);
}

guint64 skim_rbn_feed_lines(const SkimRbnFeed *f) {
  return (guint64)g_atomic_int_get(&((SkimRbnFeed *)f)->lines);
}
