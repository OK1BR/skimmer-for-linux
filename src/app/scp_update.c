/*
 * scp_update — keeps MASTER.SCP current from supercheckpartial.com (gh#15).
 * See scp_update.h for the contract. GLib + libcurl, no GTK.
 *
 * Facts about the site, each checked by hand 2026-09-19:
 *  - GET /api/v1/files → a JSON array of { name, size, etag, modified }; the
 *    API names files, it does not give URLs — the file is at /<name>. (The
 *    Developers page's own example path /downloads/master.scp answered 404
 *    on 2026-09-13.)
 *  - GET /MASTER.SCP → ETag + Last-Modified, 304 to If-None-Match.
 *  - The ETag IS the sha256 of the body (hex, quoted), and the API's "etag"
 *    is the same value unquoted. Not documented, so it is used where it holds
 *    (a 64-hex ETag must match the body) and not relied on where it doesn't.
 */
#include "scp_update.h"

#include <curl/curl.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>

#include "callsign.h"

#define SCP_SITE       "https://www.supercheckpartial.com"
#define SCP_API_PATH   "/api/v1/files"
#define SCP_FILE_NAME  "MASTER.SCP"
#define SCP_API_MAX    (256u << 10)            /* the API answer is ~2 kB     */
#define STATE_GROUP    "scp"
#define DAY_S          (24 * 3600)
#define WEEK_S         (7 * DAY_S)

#ifndef SKIMMER_VERSION
#define SKIMMER_VERSION "0"
#endif

/* --- one-time process setup -------------------------------------------------- */

static gpointer global_init(gpointer unused) {
  (void)unused;
  /* CURLOPT_NOSIGNAL (a must in a threaded program) also stops libcurl from
   * shielding the process against SIGPIPE, and an unhandled SIGPIPE on a
   * connection the far end dropped KILLS the app. GSocket does the same
   * process-wide ignore on first use; here it must not depend on the telnet
   * feed having been switched on. */
  signal(SIGPIPE, SIG_IGN);
  return GINT_TO_POINTER(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
}

static gboolean curl_ready(void) {
  static GOnce once = G_ONCE_INIT;
  return GPOINTER_TO_INT(g_once(&once, global_init, NULL));
}

/* --- persisted state ----------------------------------------------------------- */

typedef struct {
  gint64 last_check;                           /* completed check, unix s    */
  gint64 attempts[SKIM_SCP_ATTEMPTS_PER_DAY];  /* newest first               */
  guint  n_attempts;
  gint64 downloads[SKIM_SCP_DOWNLOADS_PER_WEEK];
  guint  n_downloads;
  char  *etag;                                 /* verbatim, as served        */
  char  *sha256;                               /* of the file etag describes */
} State;

static void state_clear(State *s) {
  g_free(s->etag);
  g_free(s->sha256);
  memset(s, 0, sizeof(*s));
}

static gint cmp_desc(gconstpointer a, gconstpointer b) {
  gint64 x = *(const gint64 *)a, y = *(const gint64 *)b;
  return x < y ? 1 : x > y ? -1 : 0;
}

/* A stamp list from the file → the newest `cap` stamps inside the window. A
 * stamp from the future (the clock was set back) counts as "now": a wrong
 * clock must never open the limits early. */
static guint stamps_load(GKeyFile *kf, const char *key, gint64 now,
                         gint64 window, gint64 *out, guint cap,
                         gboolean *clamped) {
  gsize n = 0;
  char **v = g_key_file_get_string_list(kf, STATE_GROUP, key, &n, NULL);
  GArray *a = g_array_new(FALSE, FALSE, sizeof(gint64));
  for (gsize i = 0; v && i < n; i++) {
    char *end = NULL;
    gint64 t = g_ascii_strtoll(v[i], &end, 10);
    if (end == v[i] || t <= 0) { continue; }
    if (t > now) { t = now; *clamped = TRUE; }
    if (now - t < window) { g_array_append_val(a, t); }
  }
  g_strfreev(v);
  g_array_sort(a, cmp_desc);
  guint k = MIN(a->len, cap);
  for (guint i = 0; i < k; i++) { out[i] = g_array_index(a, gint64, i); }
  g_array_free(a, TRUE);
  return k;
}

/* A missing or mangled state file is an empty state, never an error. */
static void state_load(const char *path, gint64 now, State *s,
                       gboolean *clamped) {
  memset(s, 0, sizeof(*s));
  *clamped = FALSE;
  GKeyFile *kf = g_key_file_new();
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    char *lc = g_key_file_get_string(kf, STATE_GROUP, "last_check", NULL);
    if (lc) {
      s->last_check = g_ascii_strtoll(lc, NULL, 10);
      if (s->last_check < 0) { s->last_check = 0; }
      if (s->last_check > now) { s->last_check = now; *clamped = TRUE; }
      g_free(lc);
    }
    s->n_attempts = stamps_load(kf, "attempts", now, DAY_S, s->attempts,
                                G_N_ELEMENTS(s->attempts), clamped);
    s->n_downloads = stamps_load(kf, "downloads", now, WEEK_S, s->downloads,
                                 G_N_ELEMENTS(s->downloads), clamped);
    s->etag = g_key_file_get_string(kf, STATE_GROUP, "etag", NULL);
    s->sha256 = g_key_file_get_string(kf, STATE_GROUP, "sha256", NULL);
  }
  g_key_file_free(kf);
}

static void stamps_save(GKeyFile *kf, const char *key, const gint64 *t,
                        guint n) {
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < n; i++) {
    g_string_append_printf(s, "%" G_GINT64_FORMAT ";", t[i]);
  }
  g_key_file_set_value(kf, STATE_GROUP, key, s->str);
  g_string_free(s, TRUE);
}

static gboolean state_save(const char *path, const State *s) {
  GKeyFile *kf = g_key_file_new();
  char lc[32];
  g_snprintf(lc, sizeof(lc), "%" G_GINT64_FORMAT, s->last_check);
  g_key_file_set_value(kf, STATE_GROUP, "last_check", lc);
  stamps_save(kf, "attempts", s->attempts, s->n_attempts);
  stamps_save(kf, "downloads", s->downloads, s->n_downloads);
  if (s->etag) { g_key_file_set_string(kf, STATE_GROUP, "etag", s->etag); }
  if (s->sha256) { g_key_file_set_string(kf, STATE_GROUP, "sha256", s->sha256); }
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  gboolean ok = g_key_file_save_to_file(kf, path, NULL);
  g_key_file_free(kf);
  return ok;
}

static void stamp_push(gint64 *t, guint *n, guint cap, gint64 now) {
  guint keep = MIN(*n, cap - 1);
  memmove(t + 1, t, keep * sizeof(*t));
  t[0] = now;
  *n = keep + 1;
}

/* Seconds until the limits allow an attempt; 0 = now. */
static gint64 limits_wait(const State *s, gint64 now, const char **why) {
  gint64 wait = 0;
  const char *w = "";
  if (s->last_check && s->last_check + SKIM_SCP_CHECK_SPACING_S - now > wait) {
    wait = s->last_check + SKIM_SCP_CHECK_SPACING_S - now;
    w = "checked within the last 24 h";
  }
  if (s->n_attempts &&
      s->attempts[0] + SKIM_SCP_ATTEMPT_SPACING_S - now > wait) {
    wait = s->attempts[0] + SKIM_SCP_ATTEMPT_SPACING_S - now;
    w = "last attempt less than an hour ago";
  }
  if (s->n_attempts >= SKIM_SCP_ATTEMPTS_PER_DAY &&
      s->attempts[s->n_attempts - 1] + DAY_S - now > wait) {
    wait = s->attempts[s->n_attempts - 1] + DAY_S - now;
    w = "attempt limit for 24 h reached";
  }
  if (why) { *why = w; }
  return wait;
}

/* --- small helpers ------------------------------------------------------------- */

static gint64 wall_now_s(gpointer unused) {
  (void)unused;
  return g_get_real_time() / G_USEC_PER_SEC;
}

static gint64 cfg_now(const SkimScpConfig *cfg) {
  return cfg->now_s ? cfg->now_s(cfg->now_user) : wall_now_s(NULL);
}

static char *cfg_state_path(const char *dest, const char *state) {
  return state ? g_strdup(state) : g_strconcat(dest, ".state", NULL);
}

static gboolean is_hex64(const char *s) {
  if (!s || strlen(s) != 64) { return FALSE; }
  for (const char *p = s; *p; p++) {
    if (!g_ascii_isxdigit(*p)) { return FALSE; }
  }
  return TRUE;
}

/* `W/"abc"` / `"abc"` / `abc` → `abc` (new string). */
static char *etag_bare(const char *etag) {
  if (!etag) { return NULL; }
  const char *p = etag;
  while (*p == ' ') { p++; }
  if (p[0] == 'W' && p[1] == '/') { p += 2; }
  char *s = g_strdup(p);
  g_strstrip(s);
  gsize n = strlen(s);
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
    memmove(s, s + 1, n - 2);
    s[n - 2] = '\0';
  }
  return s;
}

static char *sha256_hex(const guint8 *data, gsize len) {
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, len);
}

/* The TLS trust store. A distro's libcurl knows its own; the one bundled into
 * an AppImage knows the BUILD distro's path, which a Fedora or openSUSE host
 * does not have — there the bundle is looked up. */
char *skim_scp_env_ca_path(void) {
  static const char *paths[] = {
    "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Arch     */
    "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora, RHEL     */
    "/etc/ssl/ca-bundle.pem",                            /* openSUSE         */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* RHEL variants    */
    "/etc/ssl/cert.pem",                                 /* Alpine           */
  };
  const char *env = g_getenv("SSL_CERT_FILE");
  if (env && g_file_test(env, G_FILE_TEST_IS_REGULAR)) { return g_strdup(env); }
  if (!g_getenv("APPIMAGE") && !g_getenv("APPDIR")) { return NULL; }
  for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
    if (g_file_test(paths[i], G_FILE_TEST_IS_REGULAR)) {
      return g_strdup(paths[i]);
    }
  }
  return NULL;
}

char *skim_scp_env_base_url(void) {
  const char *env = g_getenv("SKIM_SCP_URL");
  return env && env[0] ? g_strdup(env) : NULL;
}

/* --- the API answer: a flat JSON array, read without a JSON library ------------
 * Tolerant of order, whitespace and unknown keys; anything it cannot follow
 * (an error page, a truncated body, nesting past the limit) is "no entry",
 * never a crash: every read is bounded by `end`. */

typedef struct { const char *p, *end; } Json;

static void j_ws(Json *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' ||
                           *j->p == '\r')) {
    j->p++;
  }
}

/* A string value → out (truncated to fit; escapes flattened — nothing we
 * compare holds one). */
static gboolean j_string(Json *j, char *out, gsize outsz) {
  if (j->p >= j->end || *j->p != '"') { return FALSE; }
  j->p++;
  gsize n = 0;
  while (j->p < j->end && *j->p != '"') {
    char c = *j->p++;
    if (c == '\\') {
      if (j->p >= j->end) { return FALSE; }
      char e = *j->p++;
      if (e == 'u') {
        if (j->end - j->p < 4) { return FALSE; }
        j->p += 4;
        c = '?';
      } else {
        c = (e == 'n' || e == 'r' || e == 't' || e == 'b' || e == 'f') ? ' ' : e;
      }
    }
    if (out && n + 1 < outsz) { out[n++] = c; }
  }
  if (j->p >= j->end) { return FALSE; }        /* no closing quote           */
  j->p++;
  if (out && outsz) { out[n] = '\0'; }
  return TRUE;
}

static gboolean j_skip(Json *j, int depth) {
  j_ws(j);
  if (j->p >= j->end || depth > 16) { return FALSE; }
  char c = *j->p;
  if (c == '"') { return j_string(j, NULL, 0); }
  if (c == '{' || c == '[') {
    char close = c == '{' ? '}' : ']';
    j->p++;
    j_ws(j);
    if (j->p < j->end && *j->p == close) { j->p++; return TRUE; }
    for (;;) {
      if (c == '{') {
        j_ws(j);
        if (!j_string(j, NULL, 0)) { return FALSE; }
        j_ws(j);
        if (j->p >= j->end || *j->p++ != ':') { return FALSE; }
      }
      if (!j_skip(j, depth + 1)) { return FALSE; }
      j_ws(j);
      if (j->p >= j->end) { return FALSE; }
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == close) { j->p++; return TRUE; }
      return FALSE;
    }
  }
  const char *start = j->p;                    /* number, true, false, null  */
  while (j->p < j->end && (g_ascii_isalnum(*j->p) || *j->p == '-' ||
                           *j->p == '+' || *j->p == '.')) {
    j->p++;
  }
  return j->p > start;
}

typedef struct {
  char   name[64];
  char   etag[160];
  gint64 size;
} ApiEntry;

/* Find the entry whose name is `want` (ASCII case-insensitive). */
static gboolean api_find(const guint8 *body, gsize len, const char *want,
                         ApiEntry *out) {
  Json j = { (const char *)body, (const char *)body + len };
  j_ws(&j);
  if (j.p >= j.end || *j.p++ != '[') { return FALSE; }
  for (;;) {
    j_ws(&j);
    if (j.p >= j.end || *j.p == ']') { return FALSE; }
    if (*j.p++ != '{') { return FALSE; }
    ApiEntry e = { .size = -1 };
    j_ws(&j);
    if (j.p < j.end && *j.p == '}') {
      j.p++;
    } else {
      for (;;) {
        char key[32];
        j_ws(&j);
        if (!j_string(&j, key, sizeof(key))) { return FALSE; }
        j_ws(&j);
        if (j.p >= j.end || *j.p++ != ':') { return FALSE; }
        j_ws(&j);
        gboolean is_str = j.p < j.end && *j.p == '"';
        if (is_str && strcmp(key, "name") == 0) {
          if (!j_string(&j, e.name, sizeof(e.name))) { return FALSE; }
        } else if (is_str && strcmp(key, "etag") == 0) {
          if (!j_string(&j, e.etag, sizeof(e.etag))) { return FALSE; }
        } else if (!is_str && strcmp(key, "size") == 0) {
          char num[24];
          gsize k = 0;
          while (j.p < j.end && g_ascii_isdigit(*j.p) && k + 1 < sizeof(num)) {
            num[k++] = *j.p++;
          }
          num[k] = '\0';
          if (!k) { return FALSE; }
          e.size = g_ascii_strtoll(num, NULL, 10);
          if (j.p < j.end && (g_ascii_isalnum(*j.p) || *j.p == '.')) {
            e.size = -1;                       /* 1e6, 12.5 — not a size     */
            if (!j_skip(&j, 1)) { return FALSE; }
          }
        } else if (!j_skip(&j, 1)) {
          return FALSE;
        }
        j_ws(&j);
        if (j.p >= j.end) { return FALSE; }
        if (*j.p == ',') { j.p++; continue; }
        if (*j.p == '}') { j.p++; break; }
        return FALSE;
      }
    }
    if (e.name[0] && g_ascii_strcasecmp(e.name, want) == 0) {
      *out = e;
      return TRUE;
    }
    j_ws(&j);
    if (j.p < j.end && *j.p == ',') { j.p++; }
  }
}

/* --- one HTTP GET ------------------------------------------------------------------
 * The multi interface, not curl_easy_perform(): a cancel has to end the
 * transfer NOW (curl_multi_wakeup from the cancelling thread), not at the
 * easy interface's next one-second progress tick — closing the app must not
 * wait on a server. */

typedef struct {
  GByteArray *body;
  gsize       cap;
  gboolean    overflow;
  char        etag[160];                       /* verbatim, last response    */
} Xfer;

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *user) {
  Xfer *x = user;
  gsize n = size * nmemb;
  if (x->body->len + n > x->cap) {
    x->overflow = TRUE;
    return 0;                                  /* aborts the transfer        */
  }
  g_byte_array_append(x->body, (const guint8 *)ptr, (guint)n);
  return n;
}

static size_t on_header(char *ptr, size_t size, size_t nmemb, void *user) {
  Xfer *x = user;
  gsize n = size * nmemb;
  if (n >= 5 && g_ascii_strncasecmp(ptr, "HTTP/", 5) == 0) {
    x->etag[0] = '\0';                         /* a redirect's headers go    */
  } else if (n > 5 && g_ascii_strncasecmp(ptr, "etag:", 5) == 0) {
    gsize k = MIN(n - 5, sizeof(x->etag) - 1);
    memcpy(x->etag, ptr + 5, k);
    x->etag[k] = '\0';
    g_strstrip(x->etag);
  }
  return n;
}

static void on_cancelled(GCancellable *c, gpointer multi) {
  (void)c;
  curl_multi_wakeup(multi);                    /* thread-safe by contract    */
}

typedef enum { GET_OK, GET_FAILED, GET_CANCELLED } GetResult;

static GetResult http_get(const SkimScpConfig *cfg, const char *url,
                          const char *if_none_match, gboolean want_json,
                          Xfer *x, long *status, char *err, gsize errsz) {
  *status = 0;
  err[0] = '\0';
  CURL *h = curl_easy_init();
  CURLM *m = curl_multi_init();
  if (!h || !m) {
    if (h) { curl_easy_cleanup(h); }
    if (m) { curl_multi_cleanup(m); }
    g_strlcpy(err, "libcurl would not start", errsz);
    return GET_FAILED;
  }
  guint t_conn = cfg->connect_timeout_s ? cfg->connect_timeout_s : 10;
  guint t_all  = cfg->total_timeout_s ? cfg->total_timeout_s : 60;
  char  ua_def[128];
  g_snprintf(ua_def, sizeof(ua_def),
             "skimmer-for-linux/%s (+https://github.com/OK1BR/skimmer-for-linux)",
             SKIMMER_VERSION);
  char curl_err[CURL_ERROR_SIZE] = "";
  /* plain http only where the base URL itself is http — the gate's mock */
  const char *protos = g_str_has_prefix(url, "http://") ? "http,https" : "https";
  struct curl_slist *hdr = NULL;
  if (want_json) { hdr = curl_slist_append(hdr, "Accept: application/json"); }
  if (if_none_match) {
    char *l = g_strconcat("If-None-Match: ", if_none_match, NULL);
    hdr = curl_slist_append(hdr, l);
    g_free(l);
  }
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_USERAGENT,
                   cfg->user_agent ? cfg->user_agent : ua_def);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, (long)t_conn);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)t_all);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 100L);   /* B/s …             */
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, (long)MIN(t_all, 20u));
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, protos);
  curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, protos);
  curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)x->cap);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, x);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, x);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, curl_err);
  if (hdr) { curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr); }
  if (cfg->ca_path) { curl_easy_setopt(h, CURLOPT_CAINFO, cfg->ca_path); }

  gulong cancel_id = 0;
  if (cfg->cancel) {
    cancel_id = g_cancellable_connect(cfg->cancel, G_CALLBACK(on_cancelled), m,
                                      NULL);
  }
  GetResult res = GET_FAILED;
  CURLcode  code = CURLE_OK;
  gboolean  done = FALSE;
  /* libcurl enforces the timeouts; the wall clock here is the backstop that
   * holds even if it should not. */
  gint64 deadline = g_get_monotonic_time() +
                    ((gint64)t_all + 5) * G_USEC_PER_SEC;
  if (curl_multi_add_handle(m, h) != CURLM_OK) {
    g_strlcpy(err, "libcurl would not take the request", errsz);
    done = TRUE;
  }
  while (!done) {
    if (cfg->cancel && g_cancellable_is_cancelled(cfg->cancel)) {
      res = GET_CANCELLED;
      break;
    }
    if (g_get_monotonic_time() > deadline) {
      g_strlcpy(err, "timed out", errsz);
      break;
    }
    int running = 0;
    if (curl_multi_perform(m, &running) != CURLM_OK) {
      g_strlcpy(err, "libcurl transfer error", errsz);
      break;
    }
    int left = 0;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(m, &left)) != NULL) {
      if (msg->msg == CURLMSG_DONE) {
        code = msg->data.result;
        done = TRUE;
        res = GET_OK;
      }
    }
    if (done || !running) { break; }
    curl_multi_poll(m, NULL, 0, 250, NULL);
  }
  if (cancel_id) { g_cancellable_disconnect(cfg->cancel, cancel_id); }
  if (res == GET_OK) {
    if (code != CURLE_OK) {
      res = GET_FAILED;
      if (x->overflow) {
        g_strlcpy(err, "exceeded the maximum allowed file size", errsz);
      } else {
        g_strlcpy(err, curl_err[0] ? curl_err : curl_easy_strerror(code), errsz);
      }
    } else {
      curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status);
    }
  } else if (res == GET_FAILED && !err[0]) {
    g_strlcpy(err, "transfer ended without a result", errsz);
  }
  curl_multi_remove_handle(m, h);
  curl_easy_cleanup(h);
  curl_multi_cleanup(m);
  curl_slist_free_all(hdr);
  return res;
}

/* --- one check ----------------------------------------------------------------------- */

static void out_set(SkimScpOutcome *out, SkimScpResult r, const char *fmt, ...)
    G_GNUC_PRINTF(3, 4);
static void out_set(SkimScpOutcome *out, SkimScpResult r, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  g_vsnprintf(out->detail, sizeof(out->detail), fmt, ap);
  va_end(ap);
  out->result = r;
}

gint64 skim_scp_update_wait_s(const SkimScpConfig *cfg) {
  char *sp = cfg_state_path(cfg->dest_path, cfg->state_path);
  State st;
  gboolean clamped;
  gint64 now = cfg_now(cfg);
  state_load(sp, now, &st, &clamped);
  gint64 w = limits_wait(&st, now, NULL);
  state_clear(&st);
  g_free(sp);
  return w;
}

gint64 skim_scp_update_last_check(const char *dest_path, const char *state_path) {
  char *sp = cfg_state_path(dest_path, state_path);
  State st;
  gboolean clamped;
  state_load(sp, wall_now_s(NULL), &st, &clamped);
  gint64 t = st.last_check;
  state_clear(&st);
  g_free(sp);
  return t;
}

/* The bytes of a download → NULL when they are a call list fit to replace
 * the old one, else the reason (static text or `buf`). */
static const char *body_problem(const GByteArray *body, guint prev_calls,
                                SkimCallsignDictInfo *info, char *buf,
                                gsize bufsz) {
  skim_callsign_dict_inspect((const char *)body->data, body->len, info);
  if (info->calls < SKIM_SCP_MIN_CALLS) {
    g_snprintf(buf, bufsz, "only %u calls in the download — not a call list",
               info->calls);
    return buf;
  }
  if (info->junk > (info->calls + info->junk) * SKIM_SCP_MAX_JUNK) {
    g_snprintf(buf, bufsz, "%u lines of the download are not calls", info->junk);
    return buf;
  }
  if (info->valid < info->calls * SKIM_SCP_MIN_VALID) {
    g_snprintf(buf, bufsz, "only %u of %u calls in the download validate",
               info->valid, info->calls);
    return buf;
  }
  if (prev_calls >= SKIM_SCP_MIN_CALLS &&
      info->calls < prev_calls * SKIM_SCP_MIN_VS_PREVIOUS) {
    g_snprintf(buf, bufsz, "the download has %u calls, the file it would "
               "replace %u", info->calls, prev_calls);
    return buf;
  }
  return NULL;
}

void skim_scp_update_run(const SkimScpConfig *cfg, SkimScpOutcome *out) {
  memset(out, 0, sizeof(*out));
  out->result = SKIM_SCP_FAILED;
  out->wait_s = SKIM_SCP_ATTEMPT_SPACING_S;
  if (!cfg || !cfg->dest_path) {
    out_set(out, SKIM_SCP_FAILED, "no destination path");
    return;
  }
  const char *base = cfg->base_url && cfg->base_url[0] ? cfg->base_url : SCP_SITE;
  char *base_trim = g_strdup(base);
  while (base_trim[0] && base_trim[strlen(base_trim) - 1] == '/') {
    base_trim[strlen(base_trim) - 1] = '\0';
  }
  char *state_path = cfg_state_path(cfg->dest_path, cfg->state_path);
  gint64 now = cfg_now(cfg);
  State st;
  gboolean clamped;
  state_load(state_path, now, &st, &clamped);

  char       *local = NULL, *local_sha = NULL, *file_url = NULL, *api_url = NULL;
  char       *api_etag = NULL, *resp_etag = NULL, *new_sha = NULL;
  gsize       local_len = 0;
  guint       prev_calls = 0;
  Xfer        api = { 0 }, file = { 0 };
  char        err[CURL_ERROR_SIZE + 64];
  long        status = 0;
  const char *why = "";

  /* -- the limits: checked before anything touches the network -------------- */
  gint64 wait = limits_wait(&st, now, &why);
  if (wait > 0) {
    if (clamped) { state_save(state_path, &st); }   /* a clock set back    */
    out_set(out, SKIM_SCP_NOT_DUE, "%s", why);
    goto finish;
  }
  if (!curl_ready()) {
    out_set(out, SKIM_SCP_FAILED, "libcurl would not initialise");
    goto finish;
  }
  /* The attempt is on record BEFORE the request goes out — a crash or a kill
   * mid-transfer still counts — and a state file that cannot be written
   * means no request at all: unlimited polling from a read-only config
   * directory is not an option. */
  stamp_push(st.attempts, &st.n_attempts, G_N_ELEMENTS(st.attempts), now);
  if (!state_save(state_path, &st)) {
    out_set(out, SKIM_SCP_FAILED, "cannot write %s — not asking the server",
            state_path);
    goto finish;
  }

  /* -- what is on disk ------------------------------------------------------- */
  if (g_file_get_contents(cfg->dest_path, &local, &local_len, NULL)) {
    SkimCallsignDictInfo li;
    local_sha = sha256_hex((const guint8 *)local, local_len);
    skim_callsign_dict_inspect(local, local_len, &li);
    prev_calls = li.calls;
  }
  /* a stored ETag describes the file it came with, not one copied in since */
  gboolean etag_ours = st.etag && st.sha256 && local_sha &&
                       strcmp(st.sha256, local_sha) == 0;

  /* -- ask the API what exists ------------------------------------------------ */
  api.body = g_byte_array_new();
  api.cap = SCP_API_MAX;
  api_url = g_strconcat(base_trim, SCP_API_PATH, NULL);
  out->requests++;
  GetResult gr = http_get(cfg, api_url, NULL, TRUE, &api, &status, err,
                          sizeof(err));
  if (gr == GET_CANCELLED) { out_set(out, SKIM_SCP_CANCELLED, "cancelled"); goto finish; }
  if (gr != GET_OK) {
    out_set(out, SKIM_SCP_FAILED, "file list: %s", err);
    goto finish;
  }
  if (status != 200) {
    out_set(out, SKIM_SCP_FAILED, "file list: HTTP %ld", status);
    goto finish;
  }
  ApiEntry ent;
  if (!api_find(api.body->data, api.body->len, SCP_FILE_NAME, &ent)) {
    out_set(out, SKIM_SCP_FAILED, "file list: no %s in the answer", SCP_FILE_NAME);
    goto finish;
  }
  api_etag = etag_bare(ent.etag);
  char *ours_bare = etag_ours ? etag_bare(st.etag) : NULL;
  gboolean same = (local_sha && is_hex64(api_etag) &&
                   g_ascii_strcasecmp(api_etag, local_sha) == 0) ||
                  (ours_bare && api_etag[0] && strcmp(ours_bare, api_etag) == 0);
  g_free(ours_bare);
  if (same) {
    st.last_check = now;
    state_save(state_path, &st);
    out_set(out, SKIM_SCP_UP_TO_DATE, "the server has the file on disk");
    goto finish;
  }

  /* -- a different file is out: the download budget ---------------------------- */
  if (st.n_downloads >= SKIM_SCP_DOWNLOADS_PER_WEEK) {
    st.last_check = now;                       /* the check itself completed */
    state_save(state_path, &st);
    out_set(out, SKIM_SCP_NOT_DUE, "a new file is out, but %d downloads in "
            "7 days is the limit", SKIM_SCP_DOWNLOADS_PER_WEEK);
    goto finish;
  }

  /* -- fetch it, conditionally where an ETag is ours --------------------------- */
  file.body = g_byte_array_new();
  file.cap = SKIM_SCP_MAX_BYTES;
  char *esc = g_uri_escape_string(ent.name, NULL, FALSE);
  file_url = g_strconcat(base_trim, "/", esc, NULL);
  g_free(esc);
  out->requests++;
  gr = http_get(cfg, file_url, etag_ours ? st.etag : NULL, FALSE, &file,
                &status, err, sizeof(err));
  if (file.body->len > 0) {                    /* bytes moved: it counts     */
    stamp_push(st.downloads, &st.n_downloads, G_N_ELEMENTS(st.downloads), now);
    state_save(state_path, &st);
  }
  if (gr == GET_CANCELLED) { out_set(out, SKIM_SCP_CANCELLED, "cancelled"); goto finish; }
  if (gr != GET_OK) {
    out_set(out, SKIM_SCP_FAILED, "%s: %s", ent.name, err);
    goto finish;
  }
  if (status == 304) {
    st.last_check = now;
    state_save(state_path, &st);
    out_set(out, SKIM_SCP_UP_TO_DATE, "not modified");
    goto finish;
  }
  if (status != 200) {
    out_set(out, SKIM_SCP_FAILED, "%s: HTTP %ld", ent.name, status);
    goto finish;
  }

  /* -- is it what it says it is ------------------------------------------------ */
  new_sha = sha256_hex(file.body->data, file.body->len);
  resp_etag = etag_bare(file.etag[0] ? file.etag : NULL);
  gboolean r_hex = is_hex64(resp_etag), a_hex = is_hex64(api_etag);
  gboolean intact;
  if (r_hex || a_hex) {
    intact = (r_hex && g_ascii_strcasecmp(resp_etag, new_sha) == 0) ||
             (a_hex && g_ascii_strcasecmp(api_etag, new_sha) == 0);
  } else {
    intact = ent.size < 0 || (gint64)file.body->len == ent.size;
  }
  if (!intact) {
    out_set(out, SKIM_SCP_FAILED, "%s: %u bytes do not match the announced "
            "checksum/size — discarded", ent.name, file.body->len);
    goto finish;
  }
  SkimCallsignDictInfo info;
  char pbuf[160];
  const char *problem = body_problem(file.body, prev_calls, &info, pbuf,
                                     sizeof(pbuf));
  if (problem) {
    out_set(out, SKIM_SCP_FAILED, "%s — discarded", problem);
    goto finish;
  }
  g_free(st.etag);
  st.etag = file.etag[0] ? g_strdup(file.etag) : NULL;
  g_free(st.sha256);
  st.sha256 = g_strdup(new_sha);
  if (local_sha && strcmp(local_sha, new_sha) == 0) {
    st.last_check = now;                       /* same bytes, state was lost */
    state_save(state_path, &st);
    out_set(out, SKIM_SCP_UP_TO_DATE, "the download equals the file on disk");
    goto finish;
  }

  /* -- in place: temp file + fsync + rename, the old file or the new one ------- */
  {
    char *dir = g_path_get_dirname(cfg->dest_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    GError *werr = NULL;
    if (!g_file_set_contents_full(cfg->dest_path, (const char *)file.body->data,
                                  (gssize)file.body->len,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0644, &werr)) {
      out_set(out, SKIM_SCP_FAILED, "cannot write %s: %s", cfg->dest_path,
              werr ? werr->message : "?");
      g_clear_error(&werr);
      goto finish;
    }
  }
  st.last_check = now;
  state_save(state_path, &st);
  if (cfg->load_dict) { skim_callsign_dict_load(cfg->dest_path, NULL); }
  g_strlcpy(out->release, info.release, sizeof(out->release));
  out->calls = info.calls;
  if (prev_calls) {
    out_set(out, SKIM_SCP_UPDATED, "release %s, %u calls (was %u)",
            info.release[0] ? info.release : "?", info.calls, prev_calls);
  } else {
    out_set(out, SKIM_SCP_UPDATED, "release %s, %u calls",
            info.release[0] ? info.release : "?", info.calls);
  }

finish:
  if (out->result != SKIM_SCP_NOT_DUE || !wait) {
    wait = limits_wait(&st, now, NULL);
  }
  out->wait_s = wait;
  if (api.body) { g_byte_array_unref(api.body); }
  if (file.body) { g_byte_array_unref(file.body); }
  g_free(local);
  g_free(local_sha);
  g_free(new_sha);
  g_free(api_etag);
  g_free(resp_etag);
  g_free(api_url);
  g_free(file_url);
  g_free(base_trim);
  g_free(state_path);
  state_clear(&st);
}

/* --- the app's handle ----------------------------------------------------------------- */

struct _SkimScpUpdater {
  char             *dest;
  SkimScpUpdaterCb  cb;
  gpointer          user;
  char             *base_url;                  /* env, read on the main      */
  char             *ca_path;                   /* thread at creation; or NULL */
  guint             timer;                     /* GSource id, 0 = none       */
  gboolean          armed;
  struct Flight    *flight;                    /* in the air, or NULL        */
};

typedef struct Flight {
  gatomicrefcount ref;                         /* main side + worker side    */
  SkimScpUpdater *u;                           /* main thread only; NULL once
                                                * stop()/free() let it go    */
  char           *dest, *base_url, *ca_path;
  GCancellable   *cancel;
  SkimScpOutcome  out;
  GMutex          lock;
  GCond           landed_cond;
  gboolean        landed;                      /* worker is out of libcurl   */
} Flight;

static void flight_unref(Flight *f) {
  if (!g_atomic_ref_count_dec(&f->ref)) { return; }
  g_free(f->dest);
  g_free(f->base_url);
  g_free(f->ca_path);
  g_object_unref(f->cancel);
  g_mutex_clear(&f->lock);
  g_cond_clear(&f->landed_cond);
  g_free(f);
}

static void updater_arm(SkimScpUpdater *u, gint64 wait_s);

static void flight_thread(GTask *task, gpointer src, gpointer data,
                          GCancellable *unused) {
  (void)src;
  (void)unused;
  Flight *f = data;
  SkimScpConfig cfg = {
    .dest_path = f->dest,
    .base_url = f->base_url,
    .ca_path = f->ca_path,
    .load_dict = TRUE,
    .cancel = f->cancel,
  };
  skim_scp_update_run(&cfg, &f->out);
  g_mutex_lock(&f->lock);
  f->landed = TRUE;
  g_cond_broadcast(&f->landed_cond);
  g_mutex_unlock(&f->lock);
  g_task_return_boolean(task, TRUE);
}

/* Main thread. Two references: the task's (dropped at the end here) and the
 * updater's u->flight (dropped by whoever clears that pointer). f->u is NULL
 * once stop()/free() let the flight go — also when the callback itself did. */
static void flight_done(GObject *src, GAsyncResult *res, gpointer data) {
  (void)src;
  (void)res;
  Flight *f = data;
  if (f->u && f->out.requests > 0 && f->u->cb) {
    f->u->cb(&f->out, f->u->user);
  }
  SkimScpUpdater *u = f->u;                    /* the callback may have stopped */
  if (u) {
    u->flight = NULL;
    f->u = NULL;
    flight_unref(f);
    if (u->armed) { updater_arm(u, f->out.wait_s); }
  }
  flight_unref(f);
}

static gboolean updater_tick(gpointer data) {
  SkimScpUpdater *u = data;
  u->timer = 0;
  if (!u->armed || u->flight) { return G_SOURCE_REMOVE; }
  Flight *f = g_new0(Flight, 1);
  g_atomic_ref_count_init(&f->ref);
  g_atomic_ref_count_inc(&f->ref);             /* one for u->flight          */
  f->u = u;
  f->dest = g_strdup(u->dest);
  f->base_url = g_strdup(u->base_url);
  f->ca_path = g_strdup(u->ca_path);
  f->cancel = g_cancellable_new();
  g_mutex_init(&f->lock);
  g_cond_init(&f->landed_cond);
  u->flight = f;
  GTask *t = g_task_new(NULL, NULL, flight_done, f);
  g_task_set_task_data(t, f, NULL);
  g_task_run_in_thread(t, flight_thread);
  g_object_unref(t);
  return G_SOURCE_REMOVE;
}

/* The limits live on the WALL clock and a GLib timer on the monotonic one,
 * which stands still through a suspend: never sleep longer than an hour,
 * look at the limits again, and let the worker answer "not due" silently. */
static void updater_arm(SkimScpUpdater *u, gint64 wait_s) {
  if (u->timer) { g_source_remove(u->timer); }
  guint s = (guint)CLAMP(wait_s, 60, 3600) + 5;
  u->timer = g_timeout_add_seconds(s, updater_tick, u);
}

SkimScpUpdater *skim_scp_updater_new(const char *dest_path, SkimScpUpdaterCb cb,
                                     gpointer user) {
  curl_ready();                                /* on the main thread, once   */
  SkimScpUpdater *u = g_new0(SkimScpUpdater, 1);
  u->dest = g_strdup(dest_path);
  u->base_url = skim_scp_env_base_url();
  u->ca_path = skim_scp_env_ca_path();
  u->cb = cb;
  u->user = user;
  return u;
}

void skim_scp_updater_start(SkimScpUpdater *u, guint delay_s) {
  if (!u || u->armed) { return; }
  u->armed = TRUE;
  if (u->timer) { g_source_remove(u->timer); }
  u->timer = g_timeout_add_seconds(MAX(delay_s, 1u), updater_tick, u);
}

/* Returns the flight that was cancelled (still referenced), or NULL. */
static Flight *updater_disarm(SkimScpUpdater *u) {
  u->armed = FALSE;
  if (u->timer) { g_source_remove(u->timer); u->timer = 0; }
  Flight *f = u->flight;
  u->flight = NULL;
  if (f) {
    f->u = NULL;                               /* its landing reports to nobody */
    g_cancellable_cancel(f->cancel);           /* wakes curl_multi_poll      */
  }
  return f;
}

void skim_scp_updater_stop(SkimScpUpdater *u) {
  if (!u) { return; }
  Flight *f = updater_disarm(u);
  if (f) { flight_unref(f); }
}

gboolean skim_scp_updater_busy(const SkimScpUpdater *u) {
  return u && u->flight != NULL;
}

/* At teardown a worker still inside libcurl/TLS while the process runs its
 * exit handlers is a crash waiting for its day, so a cancelled flight gets a
 * moment to land — it does within milliseconds (curl_multi_wakeup); the wait
 * is BOUNDED for the one case that cannot be woken, a resolver thread stuck
 * in getaddrinfo(). No flight, no wait. */
#define TEARDOWN_GRACE_US (400 * 1000)

void skim_scp_updater_free(SkimScpUpdater *u) {
  if (!u) { return; }
  Flight *f = updater_disarm(u);
  if (f) {
    gint64 until = g_get_monotonic_time() + TEARDOWN_GRACE_US;
    g_mutex_lock(&f->lock);
    while (!f->landed && g_cond_wait_until(&f->landed_cond, &f->lock, until)) { }
    g_mutex_unlock(&f->lock);
    flight_unref(f);
  }
  g_free(u->dest);
  g_free(u->base_url);
  g_free(u->ca_path);
  g_free(u);
}
