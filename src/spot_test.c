/*
 * skimmer-spot-test — offline gate for M5: tracker, spot policy, and the
 * WHOLE PIPELINE end to end.
 *
 * Units:
 *   - station table: ghost merge within 300 Hz (stronger SNR positions the
 *     station), separate stations beyond it, prune,
 *   - spot policy: dedup, QSY re-spot, interval re-spot, rate limiter.
 *
 * Integration: a mock TCI server (the sdr-for-linux wire dialect) streams a
 * synthesized 48 kHz band with TWO CW stations (OK1BR at +12.018 kHz,
 * DL1ABC at −7.48 kHz); the real pipeline connects over a real WebSocket,
 * channelizes, decodes, validates and spots BACK to the mock — which then
 * asserts: both calls spotted at the right absolute frequency (±30 Hz, the
 * per-channel tone-offset estimate at work), and not one other call ever
 * emitted (the RBN precision rule, end to end).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <libwebsockets.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/pipeline.h"
#include "engine/spot_out.h"
#include "engine/station.h"

#define PORT     40125
#define RATE     48000
#define CENTER   7020000.0
#define BLK      2048

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/* --- CW band synthesis (48 kHz complex) ---------------------------------------- */

static const struct { char c; const char *m; } MORSE[] = {
  {'A',".-"},{'B',"-..."},{'C',"-.-."},{'D',"-.."},{'E',"."},{'F',"..-."},
  {'G',"--."},{'H',"...."},{'I',".."},{'J',".---"},{'K',"-.-"},{'L',".-.."},
  {'M',"--"},{'N',"-."},{'O',"---"},{'P',".--."},{'Q',"--.-"},{'R',".-."},
  {'S',"..."},{'T',"-"},{'U',"..-"},{'V',"...-"},{'W',".--"},{'X',"-..-"},
  {'Y',"-.--"},{'Z',"--.."},{'0',"-----"},{'1',".----"},{'2',"..---"},
  {'3',"...--"},{'4',"....-"},{'5',"....."},{'6',"-...."},{'7',"--..."},
  {'8',"---.."},{'9',"----."},
};
static const char *morse_of(char c) {
  for (guint i = 0; i < G_N_ELEMENTS(MORSE); i++) {
    if (MORSE[i].c == c) { return MORSE[i].m; }
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

/* The summed band: two stations + noise, interleaved I/Q at 48 k. */
static float  *g_band;
static guint   g_band_frames;

static void build_band(void) {
  GRand *rng = g_rand_new_with_seed(20260715);
  GArray *ea = gen_env("VVV CQ TEST DE OK1BR OK1BR K", 20);
  GArray *eb = gen_env("VVV CQ DE DL1ABC DL1ABC K", 24);
  shape_env(ea, RATE);
  shape_env(eb, RATE);
  g_band_frames = MAX(ea->len, eb->len) + RATE / 2;
  g_band = g_new0(float, (gsize)g_band_frames * 2);
  double pa = 0, pb = 0;
  const double da = 2.0 * G_PI * 12018.0 / RATE;
  const double db = 2.0 * G_PI * -7480.0 / RATE;
  for (guint i = 0; i < g_band_frames; i++) {
    double a = i < ea->len ? 0.5 * g_array_index(ea, float, i) : 0.0;
    double b = i < eb->len ? 0.3 * g_array_index(eb, float, i) : 0.0;
    g_band[2 * i]     = (float)(a * cos(pa) + b * cos(pb) + 0.005 * gauss(rng));
    g_band[2 * i + 1] = (float)(a * sin(pa) + b * sin(pb) + 0.005 * gauss(rng));
    pa += da;
    pb += db;
  }
  g_array_free(ea, TRUE);
  g_array_free(eb, TRUE);
  g_rand_free(rng);
}

/* --- mock TCI server (streams the band, captures spots) ------------------------- */

typedef struct {
  guint8 *data;
  size_t  len;
  int     binary;
} Msg;

static GMutex      s_lock;
static GQueue      s_out;
static GString    *s_spots;                    /* raw spot: lines            */
static struct lws *s_wsi;
static struct lws_context *s_ctx;
static volatile gint s_run = 1, s_streaming;
static guint       s_pos;                      /* frame cursor in the band   */

static void srv_queue_text(const char *txt) {
  Msg *m = g_new0(Msg, 1);
  m->data = (guint8 *)g_strdup(txt);
  m->len  = strlen(txt);
  g_mutex_lock(&s_lock);
  g_queue_push_tail(&s_out, m);
  g_mutex_unlock(&s_lock);
}

static void srv_queue_iq_block(void) {
  Msg *m = g_new0(Msg, 1);
  m->len    = 64 + BLK * 2 * sizeof(float);
  m->data   = g_malloc0(m->len);
  m->binary = 1;
  guint32 h[16] = { 0 };
  h[1] = RATE; h[2] = 3; h[5] = BLK * 2; h[6] = 0; h[7] = 2;
  memcpy(m->data, h, sizeof(h));
  float *iq = (float *)(void *)(m->data + 64);
  for (guint i = 0; i < BLK; i++) {
    guint src = (s_pos + i) % g_band_frames;
    /* The wire carries TRUE orientation (server conjugates its DDC feed). */
    iq[2 * i]     = g_band[2 * src];
    iq[2 * i + 1] = g_band[2 * src + 1];
  }
  s_pos = (s_pos + BLK) % g_band_frames;
  g_mutex_lock(&s_lock);
  g_queue_push_tail(&s_out, m);
  g_mutex_unlock(&s_lock);
}

static void srv_exec(char *cmd) {
  if (g_str_has_prefix(cmd, "iq_samplerate:")) {
    char echo[64];
    g_snprintf(echo, sizeof(echo), "%s;", cmd);
    srv_queue_text(echo);
  } else if (g_str_has_prefix(cmd, "iq_start:")) {
    srv_queue_text("iq_start:0;");
    g_atomic_int_set(&s_streaming, 1);
  } else if (g_str_has_prefix(cmd, "iq_stop:")) {
    g_atomic_int_set(&s_streaming, 0);
    srv_queue_text("iq_stop:0;");
  } else if (g_str_has_prefix(cmd, "spot:")) {
    g_mutex_lock(&s_lock);
    g_string_append(s_spots, cmd);
    g_string_append_c(s_spots, '\n');
    g_mutex_unlock(&s_lock);
  }
}

static int srv_cb(struct lws *wsi, enum lws_callback_reasons reason,
                  void *user, void *in, size_t len) {
  static GString *rxbuf;
  (void)user;
  switch (reason) {
  case LWS_CALLBACK_ESTABLISHED:
    s_wsi = wsi;
    if (!rxbuf) { rxbuf = g_string_new(NULL); }
    srv_queue_text("protocol:ExpertSDR3,1.9;device:MockSDR;receive_only:true;"
                   "trx_count:1;channels_count:2;vfo_limits:0,61440000;"
                   "if_limits:-24000,24000;modulations_list:am,lsb,usb,cw;"
                   "dds:0,7020000;if:0,0,0;vfo:0,0,7020000;modulation:0,cw;"
                   "rx_enable:0,true;iq_samplerate:48000;ready;start;");
    lws_callback_on_writable(wsi);
    return 0;
  case LWS_CALLBACK_RECEIVE: {
    g_string_append_len(rxbuf, (const char *)in, (gssize)len);
    char *s = rxbuf->str, *semi;
    gsize used = 0;
    while ((semi = strchr(s, ';')) != NULL) {
      *semi = '\0';
      srv_exec(s);
      used = (gsize)(semi + 1 - rxbuf->str);
      s = semi + 1;
    }
    if (used) { g_string_erase(rxbuf, 0, (gssize)used); }
    return 0;
  }
  case LWS_CALLBACK_SERVER_WRITEABLE: {
    g_mutex_lock(&s_lock);
    Msg *m = g_queue_pop_head(&s_out);
    gboolean more = !g_queue_is_empty(&s_out);
    g_mutex_unlock(&s_lock);
    if (m) {
      unsigned char *buf = g_malloc(LWS_PRE + m->len);
      memcpy(buf + LWS_PRE, m->data, m->len);
      lws_write(wsi, buf + LWS_PRE, m->len,
                m->binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
      g_free(buf);
      g_free(m->data);
      g_free(m);
    }
    if (more) { lws_callback_on_writable(wsi); }
    return 0;
  }
  case LWS_CALLBACK_CLOSED:
    s_wsi = NULL;
    return 0;
  default:
    return 0;
  }
}

static const struct lws_protocols s_protocols[] = {
  { "tci", srv_cb, 0, 8192, 0, NULL, 0 },
  { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static gpointer server_thread(gpointer data) {
  (void)data;
  while (g_atomic_int_get(&s_run)) {
    if (g_atomic_int_get(&s_streaming)) {
      g_mutex_lock(&s_lock);
      guint depth = g_queue_get_length(&s_out);
      g_mutex_unlock(&s_lock);
      if (depth < 4) { srv_queue_iq_block(); }
    }
    g_mutex_lock(&s_lock);
    gboolean pending = !g_queue_is_empty(&s_out) && s_wsi;
    g_mutex_unlock(&s_lock);
    if (pending) { lws_callback_on_writable(s_wsi); }
    lws_service(s_ctx, 0);
    g_usleep(500);
  }
  return NULL;
}

/* --- pipeline callbacks capture -------------------------------------------------- */

static GMutex   c_lock;
static GString *c_text;
static GString *c_stations;

static void on_text(double freq_hz, const char *text, gpointer user) {
  (void)user;
  /* Channels decode concurrently — collect only OK1BR's channel, otherwise
   * the two stations' characters interleave in the capture. */
  if (fabs(freq_hz - 7032018.0) > 200.0)
    return;
  g_mutex_lock(&c_lock);
  g_string_append(c_text, text);
  g_mutex_unlock(&c_lock);
}
static void on_station(const SkimStation *st, gpointer user) {
  (void)user;
  g_mutex_lock(&c_lock);
  if (!strstr(c_stations->str, st->call)) {
    g_string_append_printf(c_stations, "%s ", st->call);
  }
  g_mutex_unlock(&c_lock);
}

static gboolean spots_have(const char *call) {
  g_mutex_lock(&s_lock);
  char *needle = g_strdup_printf("spot:%s,", call);
  gboolean hit = strstr(s_spots->str, needle) != NULL;
  g_free(needle);
  g_mutex_unlock(&s_lock);
  return hit;
}

/* Parse the frequency of the first captured spot of a call; NAN if none. */
static double spot_freq(const char *call) {
  g_mutex_lock(&s_lock);
  char *needle = g_strdup_printf("spot:%s,CW,", call);
  char *at = strstr(s_spots->str, needle);
  double f = NAN;
  if (at) { f = g_ascii_strtod(at + strlen(needle), NULL); }
  g_free(needle);
  g_mutex_unlock(&s_lock);
  return f;
}

/* --- unit sink for spot_out ------------------------------------------------------- */

static int  u_spots;
static void u_sink(const char *call, const char *mode, double hz, double snr,
                   gpointer user) {
  (void)call; (void)mode; (void)hz; (void)snr; (void)user;
  u_spots++;
}

int main(void) {
  printf("=== spot/station/pipeline gate (offline) ===\n");

  /* -- station table units --------------------------------------------------- */
  {
    SkimStationTable *t = skim_station_table_new();
    SkimStation a = { .call = "OK1BR", .mode = "CW", .freq_hz = 7032018,
                      .snr_db = 25, .score = 1.0, .speed = 20,
                      .first_heard = 1, .last_heard = 1 };
    const SkimStation *m1 = skim_station_table_report(t, &a);
    SkimStation ghost = a;                     /* splatter: next channel,    */
    ghost.freq_hz = 7032140;                   /* 122 Hz off, much weaker    */
    ghost.snr_db  = 6;
    const SkimStation *m2 = skim_station_table_report(t, &ghost);
    check("ghost within 300 Hz merges (1 station)", skim_station_table_size(t) == 1);
    check("stronger report keeps the frequency",
          m1 == m2 && fabs(m2->freq_hz - 7032018) < 1);
    SkimStation qsy = a;
    qsy.freq_hz = 7040000;
    skim_station_table_report(t, &qsy);
    check("same call far away is a second station",
          skim_station_table_size(t) == 2);
    check("lookup finds the call", skim_station_table_lookup(t, "OK1BR") != NULL);
    check("prune drops idle stations",
          skim_station_table_prune(t, 0) == 2 && skim_station_table_size(t) == 0);
    skim_station_table_free(t);
  }

  /* -- spot policy units -------------------------------------------------------- */
  {
    SkimSpotOut *so = skim_spot_out_new(NULL, NULL, 0);
    skim_spot_out_set_sink(so, u_sink, NULL);
    skim_spot_out_set_policy(so, 3600, 150.0, 100);
    check("first spot goes out",
          skim_spot_out_emit(so, "OK1BR", "CW", 7032018, 25) && u_spots == 1);
    check("immediate repeat is deduped",
          !skim_spot_out_emit(so, "OK1BR", "CW", 7032020, 25) && u_spots == 1);
    check("a real QSY re-spots",
          skim_spot_out_emit(so, "OK1BR", "CW", 7032300, 25) && u_spots == 2);
    skim_spot_out_set_policy(so, 3600, 150.0, 2);   /* tiny rate budget      */
    int sent = 0;
    for (int i = 0; i < 10; i++) {
      char call[16];
      g_snprintf(call, sizeof(call), "DL%dAA", i);
      if (skim_spot_out_emit(so, call, "CW", 7010000 + i * 1000, 10)) { sent++; }
    }
    check("rate limiter caps a flood (≤3 of 10)", sent <= 3);
    check("emitted counter matches", skim_spot_out_count(so) == (guint64)(2 + sent));
    skim_spot_out_free(so);
  }

  /* -- full pipeline over a real WebSocket --------------------------------------- */
  build_band();
  s_spots = g_string_new(NULL);
  c_text = g_string_new(NULL);
  c_stations = g_string_new(NULL);
  g_queue_init(&s_out);

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  lws_set_log_level(LLL_ERR, NULL);
  info.port      = PORT;
  info.iface     = "127.0.0.1";
  info.protocols = s_protocols;
  info.gid       = (gid_t)-1;
  info.uid       = (uid_t)-1;
  s_ctx = lws_create_context(&info);
  if (!s_ctx) {
    printf("FAIL — mock server would not start\n");
    return 1;
  }
  GThread *st = g_thread_new("mock-tci", server_thread, NULL);

  SkimPipelineConfig cfg = {
    .host = "127.0.0.1", .port = PORT, .iq_rate = RATE, .chan_bw_hz = 125.0,
  };
  SkimPipeline *p = skim_pipeline_new(&cfg);
  skim_pipeline_set_text_cb(p, on_text, NULL);
  skim_pipeline_set_station_cb(p, on_station, NULL);
  GError *err = NULL;
  check("pipeline connects and starts", skim_pipeline_start(p, &err));
  if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }

  /* The mock streams flat out (paced only by the socket) — wait until both
   * stations were spotted or 30 s wall time passed. */
  for (int ms = 0; ms < 30000; ms += 50) {
    if (spots_have("OK1BR") && spots_have("DL1ABC")) { break; }
    g_usleep(50 * 1000);
  }

  check("OK1BR spotted back over TCI", spots_have("OK1BR"));
  check("DL1ABC spotted back over TCI", spots_have("DL1ABC"));
  double fa = spot_freq("OK1BR"), fb = spot_freq("DL1ABC");
  printf("       spot freqs: OK1BR %.0f Hz (want 7032018), DL1ABC %.0f Hz "
         "(want 7012520)\n", fa, fb);
  check("OK1BR frequency within ±30 Hz (channel + tone offset)",
        fabs(fa - 7032018.0) <= 30.0);
  check("DL1ABC frequency within ±30 Hz", fabs(fb - 7012520.0) <= 30.0);

  g_mutex_lock(&c_lock);
  gboolean text_ok = strstr(c_text->str, "OK1BR") != NULL;
  gboolean st_ok   = strstr(c_stations->str, "OK1BR") &&
                     strstr(c_stations->str, "DL1ABC");
  g_mutex_unlock(&c_lock);
  check("decode-log callback carried the text", text_ok);
  check("station callback reported both stations", st_ok);

  /* RBN precision end to end: nothing but the two real calls ever went out. */
  g_mutex_lock(&s_lock);
  int bogus = 0;
  char **lines = g_strsplit(s_spots->str, "\n", -1);
  for (char **l = lines; *l; l++) {
    if (!**l) { continue; }
    if (!g_str_has_prefix(*l, "spot:OK1BR,") &&
        !g_str_has_prefix(*l, "spot:DL1ABC,")) {
      bogus++;
      printf("       bogus spot: %s\n", *l);
    }
  }
  g_strfreev(lines);
  g_mutex_unlock(&s_lock);
  check("not one bogus call spotted (RBN rule, end to end)", bogus == 0);
  check("no IQ blocks dropped", skim_pipeline_dropped_blocks(p) == 0);
  printf("       %u stations, %" G_GUINT64_FORMAT " frames, %" G_GUINT64_FORMAT
         " spots\n", skim_pipeline_stations(p), skim_pipeline_frames(p),
         skim_pipeline_spots(p));
  check("station table holds exactly the two stations",
        skim_pipeline_stations(p) == 2);

  skim_pipeline_stop(p);
  skim_pipeline_free(p);
  g_atomic_int_set(&s_run, 0);
  lws_cancel_service(s_ctx);
  g_thread_join(st);
  lws_context_destroy(s_ctx);

  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — the whole chain skims: IQ in, validated "
                          "spots out.");
  return fails ? 1 : 0;
}
