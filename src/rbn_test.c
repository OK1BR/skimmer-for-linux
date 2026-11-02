/*
 * skimmer-rbn-test — offline gate for M6: the RBN telnet feed.
 *
 * Units (a local telnet sink against the feed alone):
 *   - login handshake (banner, prompt, hello),
 *   - spot line format: the classic cluster line the RBN Aggregator parses
 *     (DX de <call>-#: kHz call CW dB WPM CQ HHMMZ, CRLF-terminated),
 *   - broadcast to several logged-in clients, client/line counters,
 *   - a disconnected client leaves the count.
 *
 * Integration: the OFFLINE pipeline (skim_pipeline_feed) chews a synthesized
 * 48 kHz band with three stations — OK1BR calling CQ TEST, DL1ABC calling
 * CQ, and IK2ABC answering S&P (no CQ context) — with the RBN feed wired in.
 * The telnet sink must capture exactly the two CALLING stations at the right
 * kHz, ONCE each (dedup across two passes of the band), and never IK2ABC:
 * S&P answers do not own the frequency, so they stay off the network even
 * though the station tracker lists them. The same band tuned 30 kHz lower
 * puts OK1BR on 7002.0 kHz and DL1ABC on 6982.5, below 40 m: the tracker
 * must still list DL1ABC (the panadapter shows what it hears), the feed
 * must never send it.
 *
 * gh#28 decode tap: the same band through the offline pipeline with the
 * RBN tap and the decode tap collected — two passes, a 130 s hole (every
 * station TTL-pruned, so the comeback re-spots), a third pass, then the
 * band 30 kHz lower (a centre change flushes the extractors). Every tap
 * line must read back as the event it was written from; a tap-mode
 * pipeline fed those lines must produce the RBN tap byte for byte and the
 * same station table; and with the prune ticks dropped from the tap the
 * replay must DIVERGE — the ticks are what makes the replay exact.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <gio/gio.h>
#include <locale.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/pipeline.h"
#include "engine/rbn_feed.h"

#define RATE   48000
#define CENTER 7020000.0
#define BLK    2048

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/* --- telnet sink: connect, capture everything on a reader thread ---------------- */

typedef struct {
  GSocketConnection *conn;
  GThread           *reader;
  GMutex             lock;
  GString           *rx;
} Cap;

static gpointer cap_reader(gpointer data) {
  Cap *c = data;
  GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(c->conn));
  char buf[512];
  for (;;) {
    gssize n = g_input_stream_read(in, buf, sizeof(buf), NULL, NULL);
    if (n <= 0)
      break;
    g_mutex_lock(&c->lock);
    g_string_append_len(c->rx, buf, n);
    g_mutex_unlock(&c->lock);
  }
  return NULL;
}

static Cap *cap_new(guint16 port) {
  GSocketClient *sc = g_socket_client_new();
  GSocketConnection *conn =
      g_socket_client_connect_to_host(sc, "127.0.0.1", port, NULL, NULL);
  g_object_unref(sc);
  if (!conn)
    return NULL;
  Cap *c = g_new0(Cap, 1);
  c->conn = conn;
  c->rx   = g_string_new(NULL);
  g_mutex_init(&c->lock);
  c->reader = g_thread_new("cap-reader", cap_reader, c);
  return c;
}

static gboolean cap_wait(Cap *c, const char *needle, int ms) {
  for (int t = 0; t < ms; t += 20) {
    g_mutex_lock(&c->lock);
    gboolean hit = strstr(c->rx->str, needle) != NULL;
    g_mutex_unlock(&c->lock);
    if (hit)
      return TRUE;
    g_usleep(20 * 1000);
  }
  return FALSE;
}

static gboolean cap_send(Cap *c, const char *s) {
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c->conn));
  return g_output_stream_write_all(out, s, strlen(s), NULL, NULL, NULL);
}

/* The CALL FIELD of a DX line, never the spotter: "DX de OK1BR-#: …" would
 * otherwise make every line match OK1BR. The call field is space-padded, so
 * match " CALL ". */
static gboolean line_spots(const char *line, const char *call) {
  if (!g_str_has_prefix(line, "DX de"))
    return FALSE;
  char *needle = g_strdup_printf(" %s ", call);
  gboolean hit = strstr(line, needle) != NULL;
  g_free(needle);
  return hit;
}

/* First captured DX line spotting call (dup'd), or NULL. */
static char *cap_line(Cap *c, const char *call) {
  char *found = NULL;
  g_mutex_lock(&c->lock);
  char **lines = g_strsplit(c->rx->str, "\r\n", -1);
  for (char **l = lines; *l && !found; l++) {
    if (line_spots(*l, call)) { found = g_strdup(*l); }
  }
  g_strfreev(lines);
  g_mutex_unlock(&c->lock);
  return found;
}

static int cap_count(Cap *c, const char *call) {
  int n = 0;
  g_mutex_lock(&c->lock);
  char **lines = g_strsplit(c->rx->str, "\r\n", -1);
  for (char **l = lines; *l; l++) {
    if (line_spots(*l, call)) { n++; }
  }
  g_strfreev(lines);
  g_mutex_unlock(&c->lock);
  return n;
}

static void cap_free(Cap *c) {
  if (!c)
    return;
  /* shutdown(2) wakes the blocking read with EOF — close(2) would not. */
  g_socket_shutdown(g_socket_connection_get_socket(c->conn), TRUE, TRUE, NULL);
  g_thread_join(c->reader);
  g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
  g_object_unref(c->conn);
  g_string_free(c->rx, TRUE);
  g_mutex_clear(&c->lock);
  g_free(c);
}

/* --- CW band synthesis (as the M5 gate, plus an S&P station) --------------------- */

static const struct { char ch; const char *m; } MORSE[] = {
  {'A',".-"},{'B',"-..."},{'C',"-.-."},{'D',"-.."},{'E',"."},{'F',"..-."},
  {'G',"--."},{'H',"...."},{'I',".."},{'J',".---"},{'K',"-.-"},{'L',".-.."},
  {'M',"--"},{'N',"-."},{'O',"---"},{'P',".--."},{'Q',"--.-"},{'R',".-."},
  {'S',"..."},{'T',"-"},{'U',"..-"},{'V',"...-"},{'W',".--"},{'X',"-..-"},
  {'Y',"-.--"},{'Z',"--.."},{'0',"-----"},{'1',".----"},{'2',"..---"},
  {'3',"...--"},{'4',"....-"},{'5',"....."},{'6',"-...."},{'7',"--..."},
  {'8',"---.."},{'9',"----."},
};
static const char *morse_of(char ch) {
  for (guint i = 0; i < G_N_ELEMENTS(MORSE); i++) {
    if (MORSE[i].ch == ch) { return MORSE[i].m; }
  }
  return NULL;
}
static void key_run(GArray *env, double samps, float on) {
  guint n = (guint)(samps + 0.5);
  for (guint i = 0; i < n; i++) { g_array_append_val(env, on); }
}
static GArray *gen_env(const char *text, double wpm) {
  GArray *env = g_array_new(FALSE, FALSE, sizeof(float));
  double dit = 1.2 / wpm * RATE;
  key_run(env, 3 * dit, 0);
  for (const char *p = text; *p; p++) {
    if (*p == ' ') { key_run(env, 7 * dit, 0); continue; }
    const char *m = morse_of(*p);
    if (!m) { continue; }
    for (const char *e = m; *e; e++) {
      key_run(env, *e == '-' ? 3 * dit : dit, 1);
      if (e[1]) { key_run(env, dit, 0); }
    }
    key_run(env, 3 * dit, 0);
  }
  key_run(env, 0.8 * RATE, 0);
  return env;
}
static void shape_env(GArray *env, double rate) {
  guint L = (guint)(rate * 0.005);
  float *w = g_new(float, L);
  double sum = 0.0;
  for (guint i = 0; i < L; i++) {
    w[i] = 0.5f - 0.5f * (float)cos(2.0 * G_PI * i / (L - 1));
    sum += w[i];
  }
  for (guint i = 0; i < L; i++) { w[i] = (float)(w[i] / sum); }
  float *src = (float *)env->data;
  float *dst = g_new0(float, env->len);
  for (guint n = 0; n < env->len; n++) {
    float acc = 0.0f;
    guint kmax = MIN(L, n + 1);
    for (guint k = 0; k < kmax; k++) { acc += w[k] * src[n - k]; }
    dst[n] = acc;
  }
  memcpy(src, dst, env->len * sizeof(float));
  g_free(dst);
  g_free(w);
}
static double gauss(GRand *r) {
  double u1 = 1.0 - g_rand_double(r), u2 = g_rand_double(r);
  return sqrt(-2.0 * log(u1)) * cos(2.0 * G_PI * u2);
}

static float *g_band;
static guint  g_band_frames;

static void build_band(void) {
  GRand *rng = g_rand_new_with_seed(20260715);
  GArray *ea = gen_env("VVV CQ TEST DE OK1BR OK1BR K", 20);
  GArray *eb = gen_env("VVV CQ DE DL1ABC DL1ABC K", 24);
  GArray *ec = gen_env("OK1BR DE IK2ABC IK2ABC K", 28);   /* S&P — no CQ    */
  shape_env(ea, RATE);
  shape_env(eb, RATE);
  shape_env(ec, RATE);
  g_band_frames = MAX(MAX(ea->len, eb->len), ec->len) + RATE / 2;
  g_band = g_new0(float, (gsize)g_band_frames * 2);
  double pa = 0, pb = 0, pc = 0;
  const double da = 2.0 * G_PI * 12018.0 / RATE;
  const double db = 2.0 * G_PI * -7480.0 / RATE;
  const double dc = 2.0 * G_PI * -15000.0 / RATE;
  for (guint i = 0; i < g_band_frames; i++) {
    double a = i < ea->len ? 0.50 * g_array_index(ea, float, i) : 0.0;
    double b = i < eb->len ? 0.30 * g_array_index(eb, float, i) : 0.0;
    double c = i < ec->len ? 0.35 * g_array_index(ec, float, i) : 0.0;
    g_band[2 * i]     = (float)(a * cos(pa) + b * cos(pb) + c * cos(pc) +
                                0.005 * gauss(rng));
    g_band[2 * i + 1] = (float)(a * sin(pa) + b * sin(pb) + c * sin(pc) +
                                0.005 * gauss(rng));
    pa += da;
    pb += db;
    pc += dc;
  }
  g_array_free(ea, TRUE);
  g_array_free(eb, TRUE);
  g_array_free(ec, TRUE);
  g_rand_free(rng);
}

/* --- pipeline station capture ------------------------------------------------------ */

static GMutex   c_lock;
static GString *c_stations;

static void on_station(const SkimStation *st, gpointer user) {
  (void)user;
  g_mutex_lock(&c_lock);
  if (!strstr(c_stations->str, st->call)) {
    g_string_append_printf(c_stations, "%s ", st->call);
  }
  g_mutex_unlock(&c_lock);
}

/* --- gh#28 decode tap: collectors and the tap-mode replay -------------------------- */

static SkimPipeline *t_pipe;           /* the pipeline whose taps we collect   */
static GPtrArray    *t_events;         /* SkimTapEvent* copies, in order       */
static GPtrArray    *t_lines;          /* their lines, same order              */
static GHashTable   *t_stations;       /* call → SkimStation* (last state)     */

/* The RBN tap as one text line per record, stamped with the pipeline's
 * stream frames — what skimmer-replay writes, minus the UTC. */
static void rbn_collect(const char *call, const char *mode, double freq_hz,
                        double snr_db, double speed, gpointer user) {
  char khz[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(khz, sizeof(khz), "%.2f", freq_hz / 1000.0);
  g_string_append_printf(user, "%" G_GUINT64_FORMAT "\t%s\t%s\t%s\t%.0f\t%.0f\n",
                         skim_pipeline_frames(t_pipe), call, khz, mode, snr_db,
                         speed);
}

static void tap_collect(const SkimTapEvent *ev, gpointer user) {
  (void)user;
  g_ptr_array_add(t_events, g_memdup2(ev, sizeof(*ev)));
  g_ptr_array_add(t_lines, skim_tap_event_to_line(ev));
}

static void t_station(const SkimStation *st, gpointer user) {
  (void)user;
  SkimStation *copy = g_hash_table_lookup(t_stations, st->call);
  if (!copy) {
    copy = g_new0(SkimStation, 1);
    g_hash_table_insert(t_stations, g_strdup(st->call), copy);
  }
  *copy = *st;
}

static void t_gone(const SkimStation *st, gpointer user) {
  (void)user;
  g_hash_table_remove(t_stations, st->call);
}

static int by_call(gconstpointer a, gconstpointer b) {
  return strcmp((*(const SkimStation *const *)a)->call,
                (*(const SkimStation *const *)b)->call);
}

/* The final station table as text (sorted by call); frees t_stations. */
static char *t_table(void) {
  GPtrArray *rows = g_ptr_array_new();
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t_stations);
  while (g_hash_table_iter_next(&it, &key, &val)) { g_ptr_array_add(rows, val); }
  g_ptr_array_sort(rows, by_call);
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < rows->len; i++) {
    const SkimStation *st = g_ptr_array_index(rows, i);
    char khz[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(khz, sizeof(khz), "%.2f", st->freq_hz / 1000.0);
    g_string_append_printf(s, "%s %s %u %.2f %s\n", st->call, khz, st->reports,
                           st->score, st->cq ? "CQ" : "-");
  }
  g_ptr_array_free(rows, TRUE);
  g_hash_table_destroy(t_stations);
  t_stations = NULL;
  return g_string_free(s, FALSE);
}

static void feed_band(SkimPipeline *p, double center) {
  for (guint off = 0; off + BLK <= g_band_frames; off += BLK) {
    skim_pipeline_feed(p, g_band + 2 * (gsize)off, BLK, RATE, center);
  }
}

/* A tap-mode pipeline over the collected lines; its RBN tap into rbn, its
 * station table returned. drop_prune leaves the P lines out. */
static char *tap_replay(GPtrArray *lines, GString *rbn, gboolean drop_prune) {
  SkimPipelineConfig cfg = {
    .chan_bw_hz   = 125.0,
    .rbn_tap      = rbn_collect,
    .rbn_tap_user = rbn,
  };
  SkimPipeline *p = skim_pipeline_new(&cfg);
  t_pipe = p;
  skim_pipeline_set_station_cb(p, t_station, NULL);
  skim_pipeline_set_station_gone_cb(p, t_gone, NULL);
  GError *err = NULL;
  if (!skim_pipeline_start_tap(p, &err)) {
    printf("       tap-mode pipeline did not start: %s\n", err ? err->message : "?");
    g_clear_error(&err);
  }
  for (guint i = 0; i < lines->len; i++) {
    SkimTapEvent e;
    if (!skim_tap_line_parse(g_ptr_array_index(lines, i), &e))
      continue;
    if (drop_prune && e.kind == SKIM_TAP_PRUNE)
      continue;
    skim_pipeline_feed_tap(p, &e);
  }
  skim_pipeline_stop(p);
  char *table = t_table();
  skim_pipeline_free(p);
  t_pipe = NULL;
  return table;
}

int main(void) {
  printf("=== RBN telnet feed gate (offline) ===\n");

  /* Run under a comma-decimal locale when one is available: the GTK app
   * inherits the user's LC_NUMERIC and a cs_CZ %8.1f once emitted
   * "14009,0" on the cluster wire — every parser dropped the lines
   * silently (live-caught 2026-07-19). The dialect requires the dot
   * regardless of locale; the checks below parse with g_ascii_strtod so
   * they stay honest either way. */
  const char *commas[] = { "cs_CZ.UTF-8", "cs_CZ", "de_DE.UTF-8", "fr_FR.UTF-8" };
  for (guint i = 0; i < G_N_ELEMENTS(commas); i++) {
    if (setlocale(LC_NUMERIC, commas[i])) {
      printf("    (LC_NUMERIC forced to %s)\n", commas[i]);
      break;
    }
  }

  /* -- the amateur bands: the feed refuses a frequency outside them ------------- */
  {
    static const struct { double hz; gboolean in; const char *what; } EDGES[] = {
      {   135699.0, FALSE, "below 2200 m"          },
      {   135700.0, TRUE,  "2200 m lower edge"     },
      {   137800.0, TRUE,  "2200 m upper edge"     },
      {   137801.0, FALSE, "above 2200 m"          },
      {  5249999.0, FALSE, "below 60 m"            },
      {  5332000.0, TRUE,  "60 m US channel"       },
      {  6999999.0, FALSE, "below 40 m"            },
      {  7000000.0, TRUE,  "40 m lower edge"       },
      {  7300000.0, TRUE,  "40 m upper edge"       },
      {  7300001.0, FALSE, "above 40 m"            },
      { 10099000.0, FALSE, "between 40 m and 30 m" },
      { 54000000.0, TRUE,  "6 m upper edge"        },
      { 54000001.0, FALSE, "above 6 m"             },
      {        0.0, FALSE, "zero"                  },
    };
    int ok = 0;
    for (guint i = 0; i < G_N_ELEMENTS(EDGES); i++) {
      if (skim_pipeline_in_ham_band(EDGES[i].hz) == EDGES[i].in) {
        ok++;
      } else {
        printf("       band table wrong at %.0f Hz (%s)\n", EDGES[i].hz,
               EDGES[i].what);
      }
    }
    check("amateur bands: both edges of 2200 m, 40 m, 6 m and the 60 m span (14/14)",
          ok == (int)G_N_ELEMENTS(EDGES));
  }

  /* -- feed units: handshake, line format, broadcast ----------------------------- */
  {
    GError *err = NULL;
    SkimRbnFeed *f = skim_rbn_feed_new("ok1br", 0, &err);
    check("feed starts on an ephemeral port", f && skim_rbn_feed_port(f) != 0);
    if (!f) {
      printf("       (%s)\n", err ? err->message : "?");
      return 1;
    }

    Cap *a = cap_new(skim_rbn_feed_port(f));
    check("telnet client connects", a != NULL);
    check("login prompt arrives", a && cap_wait(a, "Please enter your call:", 3000));
    cap_send(a, "aggr1\r\n");
    check("hello names the login (uppercased)",
          cap_wait(a, "Hello AGGR1", 3000));
    check("one client logged in", skim_rbn_feed_clients(f) == 1);

    skim_rbn_feed_spot(f, "DL1ABC", "CW", 14025100.0, 25.0, 22.0);
    check("spot line arrives", cap_wait(a, "DL1ABC", 3000));
    char *line = cap_line(a, "DL1ABC");
    char spotter[24] = "", khzs[24] = "", call[16] = "", mode[8] = "",
         type[8] = "", zt[16] = "";
    double snr = 0, wpm = 0;
    /* The kHz field reads as a STRING + g_ascii_strtod: sscanf's %lf obeys
     * the forced comma locale and would mask a wrong separator. */
    int n = line ? sscanf(line, "DX de %23s %23s %15s %7s %lf dB %lf WPM %7s %15s",
                          spotter, khzs, call, mode, &snr, &wpm, type, zt)
                 : 0;
    printf("       line: %s\n", line ? line : "(none)");
    check("line parses as a cluster spot", n == 8);
    check("spotter is OK1BR-#:", g_strcmp0(spotter, "OK1BR-#:") == 0);
    check("frequency uses the DOT the dialect demands",
          line && strchr(line, ',') == NULL && strchr(khzs, '.') != NULL);
    check("frequency in kHz",
          fabs(g_ascii_strtod(khzs, NULL) - 14025.1) < 0.05);
    check("call, mode, snr, wpm carried",
          g_strcmp0(call, "DL1ABC") == 0 && g_strcmp0(mode, "CW") == 0 &&
          fabs(snr - 25) < 0.5 && fabs(wpm - 22) < 0.5);
    check("type is CQ, time is HHMMZ",
          g_strcmp0(type, "CQ") == 0 && strlen(zt) == 5 && zt[4] == 'Z' &&
          g_ascii_isdigit(zt[0]) && g_ascii_isdigit(zt[3]));
    g_free(line);

    Cap *b = cap_new(skim_rbn_feed_port(f));
    check("second client connects",
          b && cap_wait(b, "Please enter your call:", 3000));
    cap_send(b, "AGGR2\r\n");
    cap_wait(b, "Hello AGGR2", 3000);
    check("two clients logged in", skim_rbn_feed_clients(f) == 2);
    skim_rbn_feed_spot(f, "OK2XYZ", "CW", 7005500.0, 12.0, 30.0);
    check("both clients get the broadcast",
          cap_wait(a, "OK2XYZ", 3000) && cap_wait(b, "OK2XYZ", 3000));
    check("line counter counts written lines (1+2)",
          skim_rbn_feed_lines(f) == 3);

    cap_free(a);
    cap_free(b);
    gboolean gone = FALSE;
    for (int t = 0; t < 3000 && !gone; t += 20) {
      gone = skim_rbn_feed_clients(f) == 0;
      g_usleep(20 * 1000);
    }
    check("disconnected clients leave the count", gone);
    skim_rbn_feed_free(f);
  }

  /* -- the offline pipeline feeds the RBN: CQ-only, deduped, exact kHz ------------ */
  {
    build_band();
    GError *err = NULL;
    SkimRbnFeed *f = skim_rbn_feed_new("OK1BR", 0, &err);
    Cap *a = cap_new(skim_rbn_feed_port(f));
    cap_wait(a, "Please enter your call:", 3000);
    cap_send(a, "AGGR\r\n");
    cap_wait(a, "Hello AGGR", 3000);

    c_stations = g_string_new(NULL);
    SkimPipelineConfig cfg = {
      .chan_bw_hz = 125.0,
      .rbn        = f,                         /* rbn_min_score = default    */
    };
    SkimPipeline *p = skim_pipeline_new(&cfg);
    skim_pipeline_set_station_cb(p, on_station, NULL);
    check("offline pipeline starts", skim_pipeline_start_offline(p, &err));

    /* Two passes over the band: the second must be swallowed by the dedup. */
    for (int pass = 0; pass < 2; pass++) {
      for (guint off = 0; off + BLK <= g_band_frames; off += BLK) {
        skim_pipeline_feed(p, g_band + 2 * (gsize)off, BLK, RATE, CENTER);
      }
    }
    gboolean both = FALSE;
    for (int t = 0; t < 10000 && !both; t += 50) {   /* broadcast is async   */
      both = cap_count(a, "OK1BR") > 0 && cap_count(a, "DL1ABC") > 0;
      g_usleep(50 * 1000);
    }

    check("OK1BR reached the RBN sink", cap_count(a, "OK1BR") >= 1);
    check("DL1ABC reached the RBN sink", cap_count(a, "DL1ABC") >= 1);
    char *la = cap_line(a, "OK1BR"), *lb = cap_line(a, "DL1ABC");
    printf("       %s\n       %s\n", la ? la : "(none)", lb ? lb : "(none)");
    check("OK1BR at 7032.0 kHz", la && strstr(la, "7032.0") != NULL);
    check("DL1ABC at 7012.5 kHz", lb && strstr(lb, "7012.5") != NULL);
    g_free(la);
    g_free(lb);
    check("dedup: one line per call across two band passes",
          cap_count(a, "OK1BR") == 1 && cap_count(a, "DL1ABC") == 1);

    g_mutex_lock(&c_lock);
    gboolean snp_tracked = strstr(c_stations->str, "IK2ABC") != NULL;
    g_mutex_unlock(&c_lock);
    check("S&P station IK2ABC is tracked locally", snp_tracked);
    check("…but NEVER fed to the RBN (CQ-only etiquette)",
          cap_count(a, "IK2ABC") == 0);

    int bogus = 0;
    g_mutex_lock(&a->lock);
    char **lines = g_strsplit(a->rx->str, "\r\n", -1);
    for (char **l = lines; *l; l++) {
      if (!g_str_has_prefix(*l, "DX de"))
        continue;
      char spotter[24] = "", khzs[24] = "", spotted[16] = "";
      if (sscanf(*l, "DX de %23s %23s %15s", spotter, khzs, spotted) != 3 ||
          g_ascii_strtod(khzs, NULL) <= 0 ||
          (g_strcmp0(spotted, "OK1BR") != 0 &&
           g_strcmp0(spotted, "DL1ABC") != 0)) {
        bogus++;
        printf("       bogus RBN line: %s\n", *l);
      }
    }
    g_strfreev(lines);
    g_mutex_unlock(&a->lock);
    check("no unvalidated call ever hit the wire", bogus == 0);
    check("pipeline RBN counter matches (2)", skim_pipeline_rbn_spots(p) == 2);

    skim_pipeline_stop(p);
    skim_pipeline_free(p);
    cap_free(a);
    skim_rbn_feed_free(f);
    g_string_free(c_stations, TRUE);
  }

  /* -- the same band 30 kHz lower: a station below the band edge stays off ------- */
  {
    GError *err = NULL;
    SkimRbnFeed *f = skim_rbn_feed_new("OK1BR", 0, &err);
    Cap *a = cap_new(skim_rbn_feed_port(f));
    cap_wait(a, "Please enter your call:", 3000);
    cap_send(a, "AGGR\r\n");
    cap_wait(a, "Hello AGGR", 3000);

    c_stations = g_string_new(NULL);
    SkimPipelineConfig cfg = {
      .chan_bw_hz = 125.0,
      .rbn        = f,
    };
    SkimPipeline *p = skim_pipeline_new(&cfg);
    skim_pipeline_set_station_cb(p, on_station, NULL);
    check("offline pipeline starts (band edge)",
          skim_pipeline_start_offline(p, &err));
    /* OK1BR lands on 7002.0 kHz, DL1ABC on 6982.5: the receiver hears it,
     * the tracker lists it, the feed must not send it. */
    for (int pass = 0; pass < 2; pass++) {
      for (guint off = 0; off + BLK <= g_band_frames; off += BLK) {
        skim_pipeline_feed(p, g_band + 2 * (gsize)off, BLK, RATE,
                           CENTER - 30000.0);
      }
    }
    for (int t = 0; t < 10000 && cap_count(a, "OK1BR") == 0; t += 50) {
      g_usleep(50 * 1000);                       /* broadcast is async       */
    }
    g_usleep(500 * 1000);                        /* a DL1ABC line would lag  */
    char *la = cap_line(a, "OK1BR");
    printf("       %s\n", la ? la : "(none)");
    check("band edge: OK1BR at 7002.0 kHz goes out",
          la && strstr(la, "7002.0") != NULL);
    g_free(la);
    g_mutex_lock(&c_lock);
    gboolean tracked = strstr(c_stations->str, "DL1ABC") != NULL;
    g_mutex_unlock(&c_lock);
    check("band edge: DL1ABC at 6982.5 kHz is tracked (the panadapter shows it)",
          tracked);
    check("…but never reaches the feed (below 7000 kHz)",
          cap_count(a, "DL1ABC") == 0);
    check("pipeline RBN counter matches (1)", skim_pipeline_rbn_spots(p) == 1);

    skim_pipeline_stop(p);
    skim_pipeline_free(p);
    cap_free(a);
    skim_rbn_feed_free(f);
    g_string_free(c_stations, TRUE);
  }

  /* -- gh#28: the decode tap replays to the identical RBN tap ------------------- */
  {
    /* Run A: IQ through the whole pipeline, both taps collected. */
    GString *rbn_a = g_string_new(NULL);
    GPtrArray *events = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *lines  = g_ptr_array_new_with_free_func(g_free);
    t_lines = lines;
    t_events = events;
    t_stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    SkimPipelineConfig cfg = {
      .chan_bw_hz      = 125.0,
      .rbn_tap         = rbn_collect,
      .rbn_tap_user    = rbn_a,
      .decode_tap      = tap_collect,
      .decode_tap_user = NULL,
    };
    SkimPipeline *p = skim_pipeline_new(&cfg);
    t_pipe = p;
    skim_pipeline_set_station_cb(p, t_station, NULL);
    skim_pipeline_set_station_gone_cb(p, t_gone, NULL);
    GError *err = NULL;
    check("tap: offline pipeline starts", skim_pipeline_start_offline(p, &err));
    for (int pass = 0; pass < 2; pass++) { feed_band(p, CENTER); }
    /* 130 s of nothing: past the 120 s station TTL — the prune tick that
     * follows takes every station out, their RBN memo with them, and the
     * third pass re-spots both callers (the tick's time decides that). */
    skim_pipeline_feed_gap(p, 130 * RATE);
    feed_band(p, CENTER);
    feed_band(p, CENTER - 30000.0);              /* retune: extractors flushed */
    skim_pipeline_stop(p);
    char *table_a = t_table();
    const guint64 rbn_n = skim_pipeline_rbn_spots(p);
    skim_pipeline_free(p);
    t_pipe = NULL;

    guint n_kind[5] = { 0 };
    for (guint i = 0; i < events->len; i++) {
      const SkimTapEvent *e = g_ptr_array_index(events, i);
      if ((guint)e->kind < 5) { n_kind[e->kind]++; }
    }
    printf("       tap: %u decodes, %u resets, %u flushes, %u prunes, %u bank; "
           "RBN tap %" G_GUINT64_FORMAT " records\n",
           n_kind[SKIM_TAP_DECODE], n_kind[SKIM_TAP_RESET],
           n_kind[SKIM_TAP_FLUSH], n_kind[SKIM_TAP_PRUNE], n_kind[SKIM_TAP_BANK],
           rbn_n);
    check("tap: the RBN tap re-spotted both callers after the hole (≥ 5 records)",
          rbn_n >= 5 && strstr(rbn_a->str, "OK1BR") && strstr(rbn_a->str, "DL1ABC"));
    check("tap: carries decodes, the bank, the centre flush and prune ticks",
          n_kind[SKIM_TAP_DECODE] > 0 && n_kind[SKIM_TAP_BANK] == 1 &&
          n_kind[SKIM_TAP_FLUSH] == 1 && n_kind[SKIM_TAP_PRUNE] >= 20);

    /* Every line reads back as the event it came from — exactly. */
    guint bad = 0;
    for (guint i = 0; i < lines->len; i++) {
      SkimTapEvent e;
      const SkimTapEvent *o = g_ptr_array_index(events, i);
      if (!skim_tap_line_parse(g_ptr_array_index(lines, i), &e) ||
          e.kind != o->kind || e.frames != o->frames || e.now_us != o->now_us ||
          e.chan != o->chan || e.slot != o->slot || e.contested != o->contested ||
          e.sig_hz != o->sig_hz || e.d.confidence != o->d.confidence ||
          e.d.speed != o->d.speed || e.d.snr_db != o->d.snr_db ||
          e.rate != o->rate || e.nchan != o->nchan ||
          e.chan_bw_hz != o->chan_bw_hz || strcmp(e.d.text, o->d.text) != 0) {
        if (bad < 3) {
          printf("       line %u does not round-trip: %s\n", i,
                 (char *)g_ptr_array_index(lines, i));
        }
        bad++;
      }
    }
    check("tap: every line parses back to its event, doubles and text exact",
          bad == 0 && lines->len == events->len && lines->len > 0);
    gboolean comment_ok = TRUE;
    {
      SkimTapEvent e;
      comment_ok = !skim_tap_line_parse("# a comment", &e) &&
                   !skim_tap_line_parse("", &e) &&
                   !skim_tap_line_parse("X\t1\t2", &e) &&
                   !skim_tap_line_parse("D\t1\t2\t3", &e);
    }
    check("tap: comments, blanks and short lines are not events", comment_ok);

    /* Run B: the tap alone through a tap-mode pipeline. */
    GString *rbn_b = g_string_new(NULL);
    t_stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *table_b = tap_replay(lines, rbn_b, FALSE);
    check("tap: the tap replay reproduces the RBN tap byte for byte",
          g_string_equal(rbn_a, rbn_b) && rbn_b->len > 0);
    check("tap: …and the station table (call, kHz, reports, score, cq)",
          g_strcmp0(table_a, table_b) == 0);
    if (!g_string_equal(rbn_a, rbn_b) || g_strcmp0(table_a, table_b) != 0) {
      printf("       RBN A:\n%s       RBN B:\n%s       table A:\n%s       table B:\n%s",
             rbn_a->str, rbn_b->str, table_a, table_b);
    }

    /* Run C: the same tap WITHOUT its prune ticks must come out different —
     * the pruned stations' memos never clear, the comeback after the hole
     * is swallowed by the 600 s re-spot window. */
    GString *rbn_c = g_string_new(NULL);
    t_stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *table_c = tap_replay(lines, rbn_c, TRUE);
    check("tap: without the prune ticks the replay diverges (the ticks matter)",
          !g_string_equal(rbn_a, rbn_c));

    g_free(table_a);
    g_free(table_b);
    g_free(table_c);
    g_string_free(rbn_a, TRUE);
    g_string_free(rbn_b, TRUE);
    g_string_free(rbn_c, TRUE);
    g_ptr_array_free(events, TRUE);
    g_ptr_array_free(lines, TRUE);
  }
  g_free(g_band);

  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — validated CQ spots flow to the RBN sink, "
                          "deduped and well-formed.");
  return fails ? 1 : 0;
}
