/*
 * skimmer-scp-test — offline gate for the MASTER.SCP updater (gh#15).
 *
 * A mock of supercheckpartial.com on 127.0.0.1 (GThreadedSocketService, plain
 * HTTP/1.1) and a fake clock. Proven here:
 *   - the good path: fresh install, "nothing new" without touching the file,
 *     a new release swapped in LIVE, conditional requests, the User-Agent;
 *   - every way a server or a download can be wrong — down, 500, an error
 *     page, broken JSON, a short / corrupt / tiny / shrunken / junk / endless
 *     body, a stalled connection — ends in FAILED with the file on disk and
 *     the loaded dictionary byte-for-byte as they were;
 *   - the rate limits, which live in the state file: once a day, an hour
 *     after a failure, four attempts a day, three downloads a week, a clock
 *     set back, a mangled state file, a state file that cannot be written;
 *   - the app handle: a slow server never stalls the main loop, and freeing
 *     the updater mid-transfer returns at once.
 */
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app/scp_update.h"
#include "engine/callsign.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  fflush(stdout);
}

/* --- the mock server -------------------------------------------------------------- */

typedef enum { API_GOOD, API_HTML, API_TRUNCATED, API_NO_ENTRY, API_BUSY } ApiMode;
typedef enum { FILE_GOOD, FILE_SHORT, FILE_CORRUPT, FILE_HUGE } FileMode;

typedef struct {
  GMutex      lock;
  GByteArray *file;             /* what /MASTER.SCP serves                    */
  char       *etag;             /* NULL = "<sha256 of file>" (the site's way) */
  char       *api_etag;         /* NULL = follow `etag`                       */
  int         api_status, file_status;
  ApiMode     api_mode;
  FileMode    file_mode;
  guint       api_delay_ms;     /* answer late                                */
  guint       api_stall_ms;     /* accept, never answer                       */
  guint       file_stall_ms;    /* half the body, then silence                */
  gint        quit;             /* atomic: stalls end early                   */
  guint       n_api, n_file, n_other;
  char        last_ua[256], last_inm[256];
} Mock;

static Mock mock;

static char *mock_etag_locked(void) {
  if (mock.etag) { return g_strdup(mock.etag); }
  char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, mock.file->data,
                                          mock.file->len);
  char *q = g_strdup_printf("\"%s\"", sha);
  g_free(sha);
  return q;
}

static void nap(guint ms) {
  for (guint t = 0; t < ms && !g_atomic_int_get(&mock.quit); t += 20) {
    g_usleep(20 * 1000);
  }
}

static gboolean send_all(GOutputStream *os, const void *data, gsize len) {
  return g_output_stream_write_all(os, data, len, NULL, NULL, NULL);
}

static void reply(GOutputStream *os, int status, const char *ctype,
                  const char *extra, const guint8 *body, gsize len,
                  gssize claim_len) {
  char *cl = claim_len >= 0
      ? g_strdup_printf("Content-Length: %" G_GSSIZE_FORMAT "\r\n", claim_len)
      : g_strdup("");
  char *head = g_strdup_printf(
      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%s%s"
      "Connection: close\r\n\r\n",
      status, status == 200 ? "OK" : status == 304 ? "Not Modified" : "Error",
      ctype, extra ? extra : "", cl);
  g_free(cl);
  send_all(os, head, strlen(head));
  if (body && len) { send_all(os, body, len); }
  g_free(head);
}

static char *header_value(const char *req, const char *name) {
  char *low = g_ascii_strdown(req, -1);
  char *key = g_strdup_printf("\r\n%s:", name);
  char *at = strstr(low, key);
  char *val = NULL;
  if (at) {
    const char *v = req + (at - low) + strlen(key);
    const char *e = strstr(v, "\r\n");
    val = g_strndup(v, e ? (gsize)(e - v) : strlen(v));
    g_strstrip(val);
  }
  g_free(low);
  g_free(key);
  return val;
}

static gboolean on_client(GThreadedSocketService *svc, GSocketConnection *conn,
                          GObject *src, gpointer user) {
  (void)svc; (void)src; (void)user;
  g_socket_set_timeout(g_socket_connection_get_socket(conn), 5);
  GInputStream  *is = g_io_stream_get_input_stream(G_IO_STREAM(conn));
  GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  char req[8192];
  gsize n = 0;
  while (n + 1 < sizeof(req)) {
    gssize r = g_input_stream_read(is, req + n, sizeof(req) - 1 - n, NULL, NULL);
    if (r <= 0) { break; }
    n += (gsize)r;
    req[n] = '\0';
    if (strstr(req, "\r\n\r\n")) { break; }
  }
  req[n] = '\0';
  char *ua = header_value(req, "user-agent");
  char *inm = header_value(req, "if-none-match");

  g_mutex_lock(&mock.lock);
  gboolean is_api  = g_str_has_prefix(req, "GET /api/v1/files ");
  /* the updater asks for the file under the name the list gave it */
  gboolean is_file = g_ascii_strncasecmp(req, "GET /MASTER.SCP ", 16) == 0;
  g_strlcpy(mock.last_ua, ua ? ua : "", sizeof(mock.last_ua));
  if (is_file) { g_strlcpy(mock.last_inm, inm ? inm : "", sizeof(mock.last_inm)); }
  if (is_api) { mock.n_api++; } else if (is_file) { mock.n_file++; } else { mock.n_other++; }
  GByteArray *file = g_byte_array_ref(mock.file);
  char *etag = mock_etag_locked();
  char *api_etag = mock.api_etag ? g_strdup(mock.api_etag)
                                 : g_strndup(etag + 1, strlen(etag) - 2);
  int api_status = mock.api_status, file_status = mock.file_status;
  ApiMode am = mock.api_mode;
  FileMode fm = mock.file_mode;
  guint api_delay = mock.api_delay_ms, api_stall = mock.api_stall_ms,
        file_stall = mock.file_stall_ms;
  g_mutex_unlock(&mock.lock);

  if (is_api) {
    nap(api_delay);
    if (api_stall) {
      nap(api_stall);
    } else if (api_status != 200) {
      reply(os, api_status, "text/html", NULL, (const guint8 *)"<h1>no</h1>", 11, 11);
    } else if (am == API_HTML) {
      const char *h = "<!doctype html><html><body>502 Bad Gateway</body></html>";
      reply(os, 200, "text/html", NULL, (const guint8 *)h, strlen(h), (gssize)strlen(h));
    } else {
      GString *j = g_string_new(NULL);
      if (am == API_NO_ENTRY) {
        g_string_append(j, "[{\"name\":\"MASTERDX.SCP\",\"size\":1,\"etag\":\"x\"}]");
      } else if (am == API_BUSY) {             /* order, space, nesting, escapes */
        g_string_append_printf(j,
            " [ {\"name\":\"SCP.DB\",\"meta\":{\"a\":[1,2,{\"b\":null}],\"c\":\"}]\\\"\"},"
            "\"size\":2834432,\"etag\":\"zz\"},\n  { \"modified\" : \"2026-09-18T00:05:23Z\","
            " \"etag\" : \"%s\", \"extra\":[true,false,-1.5e3], \"size\" : %u ,"
            " \"name\" : \"master.scp\" } ] ", api_etag, file->len);
      } else {
        g_string_append_printf(j,
            "[{\"name\":\"MASTER.DTA\",\"size\":1329849,\"etag\":\"aa\","
            "\"modified\":\"2026-09-18T00:05:24Z\"},"
            "{\"name\":\"MASTER.SCP\",\"size\":%u,\"etag\":\"%s\","
            "\"modified\":\"2026-09-18T00:05:23Z\"}]", file->len, api_etag);
      }
      gsize len = am == API_TRUNCATED ? j->len / 2 : j->len;
      reply(os, 200, "application/json", NULL, (const guint8 *)j->str, len, (gssize)len);
      g_string_free(j, TRUE);
    }
  } else if (is_file) {
    char *eh = g_strdup_printf("ETag: %s\r\n", etag);
    if (file_status != 200) {
      reply(os, file_status, "text/html", NULL, (const guint8 *)"<h1>no</h1>", 11, 11);
    } else if (inm && strcmp(inm, etag) == 0) {
      reply(os, 304, "application/octet-stream", eh, NULL, 0, -1);
    } else if (fm == FILE_HUGE) {
      reply(os, 200, "application/octet-stream", eh, NULL, 0, -1);
      char chunk[65536];
      memset(chunk, 'A', sizeof(chunk));
      for (guint i = 0; i < 160 && send_all(os, chunk, sizeof(chunk)); i++) { }
    } else if (file_stall) {
      reply(os, 200, "application/octet-stream", eh, file->data, file->len / 2,
            (gssize)file->len);
      nap(file_stall);
    } else if (fm == FILE_SHORT) {
      reply(os, 200, "application/octet-stream", eh, file->data, file->len / 2,
            (gssize)file->len);
    } else if (fm == FILE_CORRUPT) {
      guint8 *bad = g_memdup2(file->data, file->len);
      bad[file->len / 2] ^= 0x01;              /* one bit, same length       */
      reply(os, 200, "application/octet-stream", eh, bad, file->len, (gssize)file->len);
      g_free(bad);
    } else {
      reply(os, 200, "application/octet-stream", eh, file->data, file->len,
            (gssize)file->len);
    }
    g_free(eh);
  } else {
    reply(os, 404, "text/plain", NULL, (const guint8 *)"nope", 4, 4);
  }
  g_byte_array_unref(file);
  g_free(etag);
  g_free(api_etag);
  g_free(ua);
  g_free(inm);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  return TRUE;
}

static gpointer mock_thread(gpointer data) {
  GAsyncQueue *ready = data;
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);
  GSocketService *svc = g_threaded_socket_service_new(16);
  guint16 port = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(svc), NULL, NULL);
  g_signal_connect(svc, "run", G_CALLBACK(on_client), NULL);
  g_socket_service_start(svc);
  g_async_queue_push(ready, GUINT_TO_POINTER((guint)port));
  g_main_loop_run(g_main_loop_new(ctx, FALSE));   /* for the life of the gate */
  return NULL;
}

/* --- fixtures ------------------------------------------------------------------------ */

/* A call list in the real file's shape. `prefix` "OK" validates, "QQ" is no
 * allocated prefix; `junk` lines are interleaved. */
static GByteArray *make_list(const char *release, guint n, const char *prefix,
                             guint junk) {
  GString *s = g_string_new("!!Order,1,1\r\n#\r\n# Super Check Partial\r\n");
  g_string_append_printf(s, "# Release %s\r\n# Generated by bb-scp\r\n#\r\n", release);
  for (guint i = 0; i < n; i++) {
    g_string_append_printf(s, "%s%u%c%c%c\r\n", prefix, i / 17576 % 10,
                           'A' + i / 676 % 26, 'A' + i / 26 % 26, 'A' + i % 26);
    if (junk && i % (n / junk) == 0) { g_string_append(s, "<td>x</td>\r\n"); }
  }
  gsize len = s->len;
  return g_byte_array_new_take((guint8 *)g_string_free(s, FALSE), len);
}

static void mock_serve(GByteArray *file) {     /* takes the reference        */
  g_mutex_lock(&mock.lock);
  if (mock.file) { g_byte_array_unref(mock.file); }
  mock.file = file;
  g_mutex_unlock(&mock.lock);
}

static void mock_reset_modes(void) {
  g_mutex_lock(&mock.lock);
  mock.api_status = mock.file_status = 200;
  mock.api_mode = API_GOOD;
  mock.file_mode = FILE_GOOD;
  mock.api_delay_ms = mock.api_stall_ms = mock.file_stall_ms = 0;
  g_clear_pointer(&mock.etag, g_free);
  g_clear_pointer(&mock.api_etag, g_free);
  g_mutex_unlock(&mock.lock);
}

static gint64 fake_now;
static gint64 fake_clock(gpointer unused) { (void)unused; return fake_now; }
#define HOURS(h) ((gint64)(h) * 3600)
#define DAYS(d)  ((gint64)(d) * 86400)

static char *dest, *base;

static void run(SkimScpOutcome *out, guint total_timeout_s) {
  SkimScpConfig cfg = {
    .dest_path = dest,
    .base_url = base,
    .load_dict = TRUE,
    .connect_timeout_s = 3,
    .total_timeout_s = total_timeout_s ? total_timeout_s : 10,
    .now_s = fake_clock,
  };
  skim_scp_update_run(&cfg, out);
}

static gboolean dest_equals(const GByteArray *want) {
  char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents(dest, &data, &len, NULL)) { return want == NULL; }
  gboolean eq = want && len == want->len && memcmp(data, want->data, len) == 0;
  g_free(data);
  return eq;
}

/* One failure case: the server misbehaves as the caller set it up; the file
 * on disk and the loaded dictionary must come out exactly as they went in. */
static void expect_failure(const char *what, const GByteArray *on_disk,
                           const char *detail_has, guint timeout_s) {
  guint dict_before = skim_callsign_dict_size();
  char *rel_before = skim_callsign_dict_release();
  SkimScpOutcome o;
  gint64 t0 = g_get_monotonic_time();
  run(&o, timeout_s);
  double took = (g_get_monotonic_time() - t0) / 1e6;
  char *rel_after = skim_callsign_dict_release();
  gboolean ok = o.result == SKIM_SCP_FAILED && dest_equals(on_disk) &&
                skim_callsign_dict_size() == dict_before &&
                g_strcmp0(rel_before, rel_after) == 0 &&
                (!detail_has || strstr(o.detail, detail_has)) &&
                o.wait_s > 0 && took < (timeout_s ? timeout_s * 2 + 6 : 8);
  char *line = g_strdup_printf("%s → FAILED, file + dictionary untouched "
                               "(%.2f s: \"%s\")", what, took, o.detail);
  check(line, ok);
  g_free(line);
  g_free(rel_before);
  g_free(rel_after);
}

/* --- the app handle on a main loop ------------------------------------------------- */

typedef struct {
  GMainLoop     *loop;
  SkimScpOutcome out;
  guint          calls;
  gint64         last_beat, worst_gap;
} Async;

static void async_cb(const SkimScpOutcome *out, gpointer user) {
  Async *a = user;
  a->out = *out;
  a->calls++;
  g_main_loop_quit(a->loop);
}

static gboolean async_beat(gpointer user) {
  Async *a = user;
  gint64 now = g_get_monotonic_time();
  if (a->last_beat && now - a->last_beat > a->worst_gap) {
    a->worst_gap = now - a->last_beat;
  }
  a->last_beat = now;
  return G_SOURCE_CONTINUE;
}

static gboolean async_quit(gpointer user) {
  g_main_loop_quit(((Async *)user)->loop);
  return G_SOURCE_REMOVE;
}

int main(void) {
  printf("=== MASTER.SCP updater gate (offline, mock server) ===\n");
  char *tmp = g_dir_make_tmp("skimmer-scp-XXXXXX", NULL);
  dest = g_build_filename(tmp, "cfg", "master.scp", NULL);  /* dir not there yet */
  char *state = g_strconcat(dest, ".state", NULL);

  g_mutex_init(&mock.lock);
  mock_reset_modes();
  /* The service accepts on a GMainContext, and run() blocks this thread — so
   * the mock gets a thread and a context of its own. */
  GAsyncQueue *ready = g_async_queue_new();
  g_thread_unref(g_thread_new("mock-http", mock_thread, ready));
  guint16 port = (guint16)GPOINTER_TO_UINT(g_async_queue_pop(ready));
  g_async_queue_unref(ready);
  base = g_strdup_printf("http://127.0.0.1:%u/", port);   /* trailing slash on purpose */
  fake_now = 1790000000;                                   /* 2026-09-21      */

  SkimScpOutcome o;
  GByteArray *rel1 = make_list("2026.09.18", 24000, "OK", 0);
  GByteArray *rel2 = make_list("2026.09.22", 24500, "OK", 0);

  /* -- the good path ---------------------------------------------------------- */
  mock_serve(g_byte_array_ref(rel1));
  run(&o, 0);
  check("fresh install: no file → downloaded, in place, byte-identical",
        o.result == SKIM_SCP_UPDATED && dest_equals(rel1) && o.requests == 2 &&
        mock.n_api == 1 && mock.n_file == 1 && o.calls == 24000 &&
        strcmp(o.release, "2026.09.18") == 0);
  check("fresh install: the dictionary is live (no reconnect)",
        skim_callsign_dict_size() == 24000 && skim_callsign_dict_has("OK0AAA"));
  check("User-Agent names the software and its version; first GET unconditional",
        g_str_has_prefix(mock.last_ua, "skimmer-for-linux/" SKIMMER_VERSION) &&
        mock.last_inm[0] == '\0');
  check("state file written beside the dictionary", g_file_test(state, G_FILE_TEST_EXISTS));

  run(&o, 0);
  check("asked again at once: NOT_DUE, zero requests, ~24 h to wait",
        o.result == SKIM_SCP_NOT_DUE && o.requests == 0 && mock.n_api == 1 &&
        mock.n_file == 1 && o.wait_s > HOURS(23) && o.wait_s <= HOURS(24));
  fake_now += HOURS(23);
  run(&o, 0);
  check("23 h later: still NOT_DUE, zero requests",
        o.result == SKIM_SCP_NOT_DUE && mock.n_api == 1);

  fake_now += HOURS(2);
  run(&o, 0);
  check("25 h later, same file out: UP_TO_DATE from the file list alone",
        o.result == SKIM_SCP_UP_TO_DATE && o.requests == 1 && mock.n_api == 2 &&
        mock.n_file == 1 && dest_equals(rel1));

  fake_now += HOURS(25);
  mock_serve(g_byte_array_ref(rel2));
  run(&o, 0);
  check("new release out: UPDATED, swapped in live",
        o.result == SKIM_SCP_UPDATED && dest_equals(rel2) && o.calls == 24500 &&
        skim_callsign_dict_size() == 24500 && mock.n_file == 2);
  {
    char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, rel1->data, rel1->len);
    char *want = g_strdup_printf("\"%s\"", sha);
    check("…and that GET was conditional on the old file's ETag",
          strcmp(mock.last_inm, want) == 0);
    g_free(sha);
    g_free(want);
  }
  char *rel = skim_callsign_dict_release();
  check("About's release string follows the swap", g_strcmp0(rel, "2026.09.22") == 0);
  g_free(rel);

  /* -- a server whose ETag is NOT a sha256 (the scheme may change) -------------- */
  fake_now += DAYS(8);
  GByteArray *rel3 = make_list("2026.10.01", 24600, "OK", 0);
  mock_serve(g_byte_array_ref(rel3));
  g_mutex_lock(&mock.lock);
  mock.etag = g_strdup("W/\"v42\"");
  mock.api_etag = g_strdup("v42");
  mock.api_mode = API_BUSY;       /* reordered keys, nesting, "master.scp"      */
  g_mutex_unlock(&mock.lock);
  run(&o, 0);
  check("opaque ETag + a busier file list (order, nesting, case): UPDATED by size",
        o.result == SKIM_SCP_UPDATED && dest_equals(rel3));
  fake_now += HOURS(25);
  run(&o, 0);
  check("opaque ETag, nothing new: UP_TO_DATE from the stored ETag, file not asked",
        o.result == SKIM_SCP_UP_TO_DATE && o.requests == 1 && mock.n_file == 3);
  fake_now += HOURS(25);
  g_mutex_lock(&mock.lock);
  g_free(mock.api_etag);
  mock.api_etag = g_strdup("v43-says-the-list");   /* list and file disagree   */
  g_mutex_unlock(&mock.lock);
  run(&o, 0);
  check("file list claims news, the file answers 304: UP_TO_DATE, nothing moved",
        o.result == SKIM_SCP_UP_TO_DATE && o.requests == 2 && mock.n_file == 4 &&
        strcmp(mock.last_inm, "W/\"v42\"") == 0 && dest_equals(rel3));
  mock_reset_modes();

  /* -- a file copied in by hand: the stored ETag no longer describes it ---------- */
  fake_now += DAYS(8);
  g_file_set_contents(dest, (const char *)rel1->data, (gssize)rel1->len, NULL);
  run(&o, 0);
  check("hand-copied old file: unconditional GET, UPDATED back to the current one",
        o.result == SKIM_SCP_UPDATED && mock.last_inm[0] == '\0' && dest_equals(rel3));

  /* -- everything that can go wrong ------------------------------------------------ */
  printf("--- failures: file + dictionary must survive each\n");
  GByteArray *newer = make_list("2026.10.08", 24700, "OK", 0);
  mock_serve(g_byte_array_ref(newer));          /* there IS news, every time  */

  fake_now += DAYS(8);
  {
    /* a port nobody listens on: bind one, note it, close it */
    GSocketListener *l = g_socket_listener_new();
    guint16 dead = g_socket_listener_add_any_inet_port(l, NULL, NULL);
    g_socket_listener_close(l);
    g_object_unref(l);
    char *save = base;
    base = g_strdup_printf("http://127.0.0.1:%u", dead);
    expect_failure("server unreachable (connection refused)", rel3, "file list", 0);
    g_free(base);
    base = save;
  }

  fake_now += DAYS(8);
  {
    char *save = base;
    base = g_strdup("http://skimmer-gate.invalid/");
    expect_failure("host name that does not resolve", rel3, "file list", 0);
    g_free(base);
    base = save;
  }

  fake_now += DAYS(8); mock_reset_modes(); mock.api_status = 500;
  expect_failure("file list: HTTP 500", rel3, "HTTP 500", 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.api_mode = API_HTML;
  expect_failure("file list: an HTML error page with 200", rel3, "no MASTER.SCP", 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.api_mode = API_TRUNCATED;
  expect_failure("file list: JSON cut in half", rel3, "no MASTER.SCP", 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.api_mode = API_NO_ENTRY;
  expect_failure("file list: no MASTER.SCP entry", rel3, "no MASTER.SCP", 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.file_status = 404;
  expect_failure("file: HTTP 404", rel3, "HTTP 404", 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.file_status = 503;
  expect_failure("file: HTTP 503", rel3, "HTTP 503", 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.file_mode = FILE_SHORT;
  expect_failure("file: connection dropped mid-body", rel3, NULL, 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.file_mode = FILE_CORRUPT;
  expect_failure("file: one flipped bit, right length", rel3, "checksum", 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.file_mode = FILE_HUGE;
  expect_failure("file: a body that never ends (cap)", rel3,
                 "maximum allowed file size", 0);

  fake_now += DAYS(8); mock_reset_modes();
  {
    const char *h = "<!doctype html><html><head><title>Maintenance</title></head>"
                    "<body><p>We'll be right back.</p></body></html>\n";
    mock_serve(g_byte_array_new_take((guint8 *)g_strdup(h), strlen(h)));
    expect_failure("file: an HTML page with 200 and a VALID checksum", rel3,
                   "not a call list", 0);
  }
  fake_now += DAYS(8);
  mock_serve(make_list("2026.10.08", 500, "OK", 0));
  expect_failure("file: a list of 500 calls", rel3, "only 500 calls", 0);
  fake_now += DAYS(8);
  mock_serve(make_list("2026.10.08", 11000, "OK", 0));
  expect_failure("file: a list shrunk under half of the one on disk", rel3,
                 "would replace", 0);
  fake_now += DAYS(8);
  mock_serve(make_list("2026.10.08", 24000, "OK", 2000));
  expect_failure("file: every 12th line is not a call", rel3, "are not calls", 0);
  fake_now += DAYS(8);
  mock_serve(make_list("2026.10.08", 24000, "QQ", 0));
  expect_failure("file: call-shaped lines that validate nowhere", rel3, "validate", 0);
  fake_now += DAYS(8);
  {
    GByteArray *bin = g_byte_array_sized_new(400000);
    GRand *rng = g_rand_new_with_seed(15);
    for (guint i = 0; i < 400000; i++) {
      guint8 b = (guint8)g_rand_int_range(rng, 0, 256);
      g_byte_array_append(bin, &b, 1);
    }
    g_rand_free(rng);
    mock_serve(bin);
    expect_failure("file: 400 kB of random bytes (NULs, no line ends)", rel3, NULL, 0);
  }

  mock_serve(g_byte_array_ref(newer));
  fake_now += DAYS(8); mock_reset_modes(); mock.api_stall_ms = 20000;
  expect_failure("file list: server accepts and never answers (2 s timeout)",
                 rel3, "file list", 2);
  g_atomic_int_set(&mock.quit, 1); g_usleep(60 * 1000); g_atomic_int_set(&mock.quit, 0);
  fake_now += DAYS(8); mock_reset_modes(); mock.file_stall_ms = 20000;
  expect_failure("file: half the body, then silence (2 s timeout)", rel3, NULL, 2);
  g_atomic_int_set(&mock.quit, 1); g_usleep(60 * 1000); g_atomic_int_set(&mock.quit, 0);
  mock_reset_modes();

  /* -- the limits ----------------------------------------------------------------- */
  printf("--- rate limits (persisted)\n");
  fake_now += DAYS(8);
  mock.api_status = 500;
  guint api0 = mock.n_api;
  run(&o, 0);                                   /* attempt 1 fails            */
  gboolean a1 = o.result == SKIM_SCP_FAILED && o.wait_s > 3500 && o.wait_s <= 3600;
  fake_now += 1800;
  run(&o, 0);
  check("30 min after a failure: NOT_DUE, zero requests",
        a1 && o.result == SKIM_SCP_NOT_DUE && o.requests == 0 && mock.n_api == api0 + 1);
  guint failed = 1;
  for (int i = 0; i < 3; i++) {
    fake_now += 3660;
    run(&o, 0);
    if (o.result == SKIM_SCP_FAILED && o.requests == 1) { failed++; }
  }
  check("an hour apart the retries go out: 4 attempts in the day",
        failed == 4 && mock.n_api == api0 + 4);
  fake_now += 3660;
  run(&o, 0);
  check("the 5th within 24 h: NOT_DUE \"attempt limit\", zero requests",
        o.result == SKIM_SCP_NOT_DUE && o.requests == 0 && mock.n_api == api0 + 4 &&
        strstr(o.detail, "attempt limit") && o.wait_s > HOURS(18));
  mock_reset_modes();
  fake_now += HOURS(21);
  run(&o, 0);
  check("a day after the first attempt it goes out again → UPDATED",
        o.result == SKIM_SCP_UPDATED && dest_equals(newer));

  /* three downloads in seven days, then the budget */
  GByteArray *cur = NULL;
  guint file0 = mock.n_file;
  gboolean two_more = TRUE;
  for (int i = 0; i < 2; i++) {
    fake_now += HOURS(25);
    char relname[16];
    g_snprintf(relname, sizeof(relname), "2026.11.%02d", i + 1);
    if (cur) { g_byte_array_unref(cur); }
    cur = make_list(relname, 24800 + (guint)i, "OK", 0);
    mock_serve(g_byte_array_ref(cur));
    run(&o, 0);
    two_more = two_more && o.result == SKIM_SCP_UPDATED && dest_equals(cur);
  }
  check("a release a day: the 2nd and 3rd download of the week go through",
        two_more && mock.n_file == file0 + 2);
  fake_now += HOURS(25);
  GByteArray *fourth = make_list("2026.11.03", 24900, "OK", 0);
  mock_serve(g_byte_array_ref(fourth));
  run(&o, 0);
  check("the 4th in 7 days: refused after the file list, the file is not asked",
        o.result == SKIM_SCP_NOT_DUE && o.requests == 1 && mock.n_file == file0 + 2 &&
        dest_equals(cur) && strstr(o.detail, "downloads"));
  fake_now += DAYS(6);
  run(&o, 0);
  check("once the week has passed: UPDATED",
        o.result == SKIM_SCP_UPDATED && dest_equals(fourth));

  /* the state file itself */
  fake_now += HOURS(25);
  g_file_set_contents(state, "\x01\x02 not a keyfile [[[\n=\n", -1, NULL);
  run(&o, 0);
  check("a mangled state file is an empty state, not a crash: UP_TO_DATE",
        o.result == SKIM_SCP_UP_TO_DATE && o.requests == 1);
  {
    char *future = g_strdup_printf("[scp]\nlast_check=%" G_GINT64_FORMAT "\n",
                                   fake_now + DAYS(3650));
    g_file_set_contents(state, future, -1, NULL);
    g_free(future);
    guint api1 = mock.n_api;
    run(&o, 0);
    gboolean held = o.result == SKIM_SCP_NOT_DUE && o.requests == 0 &&
                    o.wait_s <= HOURS(24);
    fake_now += HOURS(25);
    run(&o, 0);
    check("a last_check ten years ahead (clock was wrong): held for 24 h, not 10 years",
          held && o.result == SKIM_SCP_UP_TO_DATE && mock.n_api == api1 + 1);
  }
  {
    fake_now += HOURS(25);
    char *dir = g_path_get_dirname(dest);
    guint api1 = mock.n_api;
    g_chmod(dir, 0555);
    run(&o, 0);
    g_chmod(dir, 0755);
    check("state file cannot be written: no request goes out at all",
          o.result == SKIM_SCP_FAILED && o.requests == 0 && mock.n_api == api1 &&
          strstr(o.detail, "cannot write"));
    g_free(dir);
  }

  /* -- the app handle ---------------------------------------------------------------- */
  printf("--- app handle (main loop)\n");
  {
    char *adest = g_build_filename(tmp, "app", "master.scp", NULL);
    g_setenv("SKIM_SCP_URL", base, TRUE);
    mock_reset_modes();
    mock.api_delay_ms = 1500;                   /* a slow server              */
    Async a = { .loop = g_main_loop_new(NULL, FALSE) };
    guint beat = g_timeout_add(10, async_beat, &a);
    guint guard = g_timeout_add_seconds(20, async_quit, &a);
    SkimScpUpdater *u = skim_scp_updater_new(adest, async_cb, &a);
    skim_scp_updater_start(u, 1);
    g_main_loop_run(a.loop);
    char *line = g_strdup_printf("slow server (1.5 s): UPDATED arrives by callback, "
                                 "main loop never stalled (worst beat gap %.1f ms)",
                                 a.worst_gap / 1000.0);
    check(line, a.calls == 1 && a.out.result == SKIM_SCP_UPDATED &&
                a.worst_gap < 100 * 1000 && g_file_test(adest, G_FILE_TEST_EXISTS));
    g_free(line);
    skim_scp_updater_free(u);
    g_source_remove(guard);

    /* free() mid-transfer: the server sits on the request for 20 s */
    g_remove(adest);
    char *astate = g_strconcat(adest, ".state", NULL);
    g_remove(astate);
    mock_reset_modes();
    mock.api_stall_ms = 20000;
    a.calls = 0;
    u = skim_scp_updater_new(adest, async_cb, &a);
    skim_scp_updater_start(u, 1);
    guint q = g_timeout_add(2200, async_quit, &a);   /* 1 s delay + 1.2 s in flight */
    g_main_loop_run(a.loop);
    (void)q;
    gboolean was_busy = skim_scp_updater_busy(u);
    gint64 t0 = g_get_monotonic_time();
    skim_scp_updater_free(u);
    double took_ms = (g_get_monotonic_time() - t0) / 1000.0;
    guint q2 = g_timeout_add(600, async_quit, &a);   /* let the landing run   */
    g_main_loop_run(a.loop);
    (void)q2;
    line = g_strdup_printf("free() with a transfer in flight returns at once "
                           "(%.1f ms), no callback after it", took_ms);
    check(line, was_busy && took_ms < 450.0 && a.calls == 0 &&
                !g_file_test(adest, G_FILE_TEST_EXISTS));
    g_free(line);
    g_atomic_int_set(&mock.quit, 1);
    g_source_remove(beat);
    g_main_loop_unref(a.loop);
    g_free(adest);
    g_free(astate);
  }

  g_usleep(100 * 1000);
  printf("\n=== %d checks, %d failures ===\n", checks, fails);
  printf(fails ? "FAIL\n" : "PASS — the dictionary stays current, and stays put "
                            "when the network does not.\n");
  return fails ? 1 : 0;
}
