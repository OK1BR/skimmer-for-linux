/*
 * skimmer-tci-test — offline gate for the TCI client (M1). No radio, no GUI.
 *
 * Runs a mock TCI server (libwebsockets, the sdr-for-linux wire dialect) on
 * 127.0.0.1:40124 and drives skim_tci_client against it:
 *   - handshake: batched init block ending ready;/start; → start() returns,
 *     protocol/device/dds/iq_samplerate land in the getters,
 *   - the client requests iq_samplerate + iq_start:0 (the SDC lesson: say the
 *     device-global rate explicitly),
 *   - binary Stream type=0 blocks are reassembled across WS fragmentation
 *     (16448-byte blocks vs. an 8192-byte rx buffer),
 *   - ORIENTATION: the mock puts a +12 kHz tone on the wire in TRUE orientation
 *     (the real server conjugates its RF-inverted DDC feed on send, so the wire
 *     is already true); after pass-through ingest it must still sit at +12 kHz
 *     with the −12 kHz image down > 40 dB — the M1 wire-convention check
 *     codified (live-caught 2026-07-15: a client-side conjugate mirrors),
 *   - dds retune broadcasts update center_hz live,
 *   - spot() emits a well-formed spot: command with reserved chars scrubbed,
 *   - stop() sends iq_stop:0; and no IQ callback fires afterwards,
 *   - TCI hardening for foreign servers (gh#2, SKM-7..10): the IQ request
 *     goes out as THREE text frames; a block for receiver 1 is never
 *     delivered; a message with trailing bytes delivers its one block and
 *     the stream goes on; a truncated message is dropped without desyncing
 *     the next; and with the iq_stamp echo withheld the reserved words are
 *     ignored (dds label, one whole-block callback).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <libwebsockets.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/tci_client.h"

#define PORT      40124
#define RATE      48000
#define TONE_HZ   12000.0
#define BLK       2048            /* frames per Stream block                  */
#define MARK_RATE 47999           /* h[1] of the MARKER block the mock queues right
                                   * after every fixture: in-order delivery means
                                   * that once the marker's callback shows, the
                                   * fixture has been fully handled (gate timing
                                   * trap caught 2026-09-11: three checks passed
                                   * on the pre-fix code because the fixture was
                                   * still queued behind regular blocks) */

/* ---- mock TCI server --------------------------------------------------------- */

typedef struct {
  guint8  *data;
  size_t   len;
  int      binary;
} Msg;

static GMutex      s_lock;
static GQueue      s_out;             /* Msg* pending to the client            */
static GString    *s_rx;              /* text received from the client         */
static char        s_spot[256];       /* last spot: command                    */
static struct lws *s_wsi;
static struct lws_context *s_ctx;
static volatile gint s_run = 1, s_streaming, s_push_dds, s_blocks_sent, s_stamp_req;
static volatile gint s_echo_stamp = 1;   /* mock echoes iq_stamp:1 (sdr-for-linux does; others do not) */
static volatile gint s_text_frames;      /* complete text frames received from the client */
static volatile gint s_marks_sent;       /* MARKER blocks queued so far (one per fixture) */

static void srv_queue_text(const char *txt) {
  Msg *m = g_new0(Msg, 1);
  m->data = (guint8 *)g_strdup(txt);
  m->len  = strlen(txt);
  g_mutex_lock(&s_lock);
  g_queue_push_tail(&s_out, m);
  g_mutex_unlock(&s_lock);
}

/* One Stream block carrying the wire-convention tone: a station +12 kHz above
 * the centre arrives at +12 kHz on the wire (true orientation — the server has
 * already conjugated its RF-inverted DDC feed on send). Blocks are 512 whole
 * cycles, so per-block phase restarts at 0. */
/* rx/rate = header words 0/1; hz0/off/hz1 = sdr-for-linux's centre stamps in
 * the reserved words (0 = none); pad = trailing bytes after the block in the
 * SAME WebSocket message; trunc = payload bytes cut off the end (the header
 * still claims the full block). */
static void srv_queue_block(guint32 rx, guint32 rate, guint32 hz0, guint32 off,
                            guint32 hz1, gsize pad, gsize trunc) {
  Msg *m = g_new0(Msg, 1);
  const gsize full = 64 + BLK * 2 * sizeof(float);
  m->len    = full + pad - trunc;
  m->data   = g_malloc0(full + pad);
  m->binary = 1;
  guint32 h[16] = { 0 };
  h[0] = rx; h[1] = rate; h[2] = 3; h[5] = BLK * 2; h[6] = 0; h[7] = 2;
  h[8] = hz0; h[9] = off; h[10] = hz1;
  memcpy(m->data, h, sizeof(h));
  float *iq = (float *)(void *)(m->data + 64);
  for (int i = 0; i < BLK; i++) {
    double a = 2.0 * G_PI * TONE_HZ * (double)i / (double)RATE;
    iq[2 * i]     = 0.5f * (float)cos(a);
    iq[2 * i + 1] = 0.5f * (float)sin(a);
  }
  memset(m->data + full, 0xA5, pad);           /* padding that is NOT a header */
  g_mutex_lock(&s_lock);
  g_queue_push_tail(&s_out, m);
  g_mutex_unlock(&s_lock);
  g_atomic_int_inc(&s_blocks_sent);
}
static void srv_queue_iq_block_ex(guint32 hz0, guint32 off, guint32 hz1) {
  srv_queue_block(0, RATE, hz0, off, hz1, 0, 0);
}
static void srv_queue_iq_block(void) { srv_queue_iq_block_ex(0, 0, 0); }

/* Complete command from the client (no ';'). Echo iq_* like the real server. */
static void srv_exec(char *cmd) {
  g_mutex_lock(&s_lock);
  g_string_append(s_rx, cmd);
  g_string_append_c(s_rx, ';');
  g_mutex_unlock(&s_lock);

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
  } else if (g_str_has_prefix(cmd, "iq_stamp:")) {
    if (g_atomic_int_get(&s_echo_stamp)) {
      srv_queue_text("iq_stamp:1;");           /* the family extension echo   */
    }                                          /* else: a server that never heard of it */
  } else if (g_str_has_prefix(cmd, "spot:")) {
    g_mutex_lock(&s_lock);
    g_strlcpy(s_spot, cmd, sizeof(s_spot));
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
    /* The whole init block batched into ONE text frame — the real server sends
     * many small frames, SDC-era clients must cope with either. Ends with
     * ready; then start; (the piHPSDR tail). */
    srv_queue_text("protocol:ExpertSDR3,1.9;device:MockSDR;receive_only:true;"
                   "trx_count:1;channels_count:2;vfo_limits:0,61440000;"
                   "if_limits:-24000,24000;modulations_list:am,lsb,usb,cw;"
                   "dds:0,7020000;if:0,0,0;vfo:0,0,7020000;modulation:0,cw;"
                   "rx_enable:0,true;iq_samplerate:48000;ready;start;");
    lws_callback_on_writable(wsi);
    return 0;
  case LWS_CALLBACK_RECEIVE: {
    if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
      g_atomic_int_inc(&s_text_frames);        /* one complete WS message     */
    }
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
    if (g_atomic_int_get(&s_push_dds)) {
      g_atomic_int_set(&s_push_dds, 0);
      srv_queue_text("dds:0,7030000;");
    }
    const gint sr = g_atomic_int_get(&s_stamp_req);
    if (sr == 1) { srv_queue_iq_block_ex(7021000, 0, 0); }
    if (sr == 2) { srv_queue_iq_block_ex(7020000, 700, 7025000); }
    /* hardening fixtures (gh#2): a receiver-1 block at a telltale rate, a
     * stamped block with 100 trailing bytes, a block cut 1000 bytes short */
    if (sr == 3) { srv_queue_block(1, 96000, 0, 0, 0, 0, 0); }
    if (sr == 4) { srv_queue_block(0, RATE, 7022000, 0, 0, 100, 0); }
    if (sr == 5) { srv_queue_block(0, RATE, 7023000, 0, 0, 0, 1000); }
    if (sr) {                                  /* the MARKER right behind it  */
      g_atomic_int_set(&s_stamp_req, 0);
      srv_queue_block(0, MARK_RATE, 0, 0, 0, 0, 0);
      g_atomic_int_inc(&s_marks_sent);
    }
    /* Keep a small stock of IQ blocks queued while streaming. */
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
    g_usleep(1000);
  }
  return NULL;
}

/* ---- client-side capture ------------------------------------------------------ */

static GMutex       c_lock;
static float        c_block[BLK * 2];   /* first full block, post-ingest        */
static volatile gint c_blocks, c_have_block;
static guint        c_nframes;
static double       c_rate, c_center;

#define CAP_RING 8
static guint  c_ring_n[CAP_RING]; static double c_ring_hz[CAP_RING]; static guint c_ring_i;
static volatile gint c_rx1_blocks;      /* callbacks at the receiver-1 telltale rate */
static volatile gint c_marks;           /* MARKER blocks delivered (rate MARK_RATE)  */
static volatile gint c_hz_7022000, c_hz_7023000, c_hz_7021000, c_odd_frames;

static void capture_reset(void) {
  g_mutex_lock(&c_lock);
  memset(c_ring_n, 0, sizeof(c_ring_n)); memset(c_ring_hz, 0, sizeof(c_ring_hz)); c_ring_i = 0;
  c_have_block = 0; c_nframes = 0; c_rate = 0; c_center = 0;
  g_mutex_unlock(&c_lock);
  g_atomic_int_set(&c_blocks, 0);
  g_atomic_int_set(&c_rx1_blocks, 0);
  g_atomic_int_set(&c_marks, 0);
  g_atomic_int_set(&s_marks_sent, 0);          /* a new session, no marker in flight */
  g_atomic_int_set(&c_hz_7022000, 0); g_atomic_int_set(&c_hz_7023000, 0);
  g_atomic_int_set(&c_hz_7021000, 0); g_atomic_int_set(&c_odd_frames, 0);
}

static void iq_cb(const float *iq, guint nframes, double rate, double center,
                  gpointer user) {
  (void)user;
  g_mutex_lock(&c_lock);
  c_ring_n[c_ring_i % CAP_RING] = nframes; c_ring_hz[c_ring_i % CAP_RING] = center; c_ring_i++;
  if (!c_have_block && nframes == BLK) {
    memcpy(c_block, iq, sizeof(c_block));
    c_have_block = 1;
  }
  c_nframes = nframes;
  c_rate    = rate;
  c_center  = center;
  g_mutex_unlock(&c_lock);
  g_atomic_int_inc(&c_blocks);
  if (rate == 96000.0) { g_atomic_int_inc(&c_rx1_blocks); }
  if (rate == (double)MARK_RATE) { g_atomic_int_inc(&c_marks); }
  if (center == 7022000.0) { g_atomic_int_inc(&c_hz_7022000); }
  if (center == 7023000.0) { g_atomic_int_inc(&c_hz_7023000); }
  if (center == 7021000.0) { g_atomic_int_inc(&c_hz_7021000); }
  if (nframes != BLK) { g_atomic_int_inc(&c_odd_frames); }
}

/* centre-stamp captures: a stamped block's own centre, and a stamped
 * boundary → two callbacks (700 old-centre frames, 1348 new-centre ones) */
static gboolean cond_stamp_single(void) {
  gboolean ok = FALSE;
  g_mutex_lock(&c_lock);
  for (guint k = 0; k < CAP_RING; k++) { if (c_ring_n[k] == BLK && c_ring_hz[k] == 7021000.0) { ok = TRUE; } }
  g_mutex_unlock(&c_lock);
  return ok;
}
static gboolean cond_stamp_pair(void) {
  gboolean ok = FALSE;
  g_mutex_lock(&c_lock);
  for (guint k = 0; k < CAP_RING; k++) {
    const guint a = k, b = (k + 1) % CAP_RING;
    if (c_ring_n[a] == 700 && c_ring_hz[a] == 7020000.0 && c_ring_n[b] == BLK - 700 && c_ring_hz[b] == 7025000.0) { ok = TRUE; }
  }
  g_mutex_unlock(&c_lock);
  return ok;
}

/* ---- helpers ------------------------------------------------------------------ */

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static int srv_rx_contains(const char *needle) {
  g_mutex_lock(&s_lock);
  int hit = strstr(s_rx->str, needle) != NULL;
  g_mutex_unlock(&s_lock);
  return hit;
}

static int wait_ms(int (*cond)(void), int ms) {
  for (int i = 0; i < ms; i++) {
    if (cond()) { return 1; }
    g_usleep(1000);
  }
  return 0;
}

static int cond_iq_sub(void)  { return srv_rx_contains("iq_samplerate:48000;") &&
                                       srv_rx_contains("iq_start:0;"); }
static int cond_blocks(void)  { return g_atomic_int_get(&c_blocks) >= 4 &&
                                       g_atomic_int_get(&c_have_block); }
static int cond_stop(void)    { return srv_rx_contains("iq_stop:0;"); }
static int cond_spot(void) {
  g_mutex_lock(&s_lock);
  int hit = s_spot[0] != 0;
  g_mutex_unlock(&s_lock);
  return hit;
}
static int cond_click(void) {
  return srv_rx_contains("clicked_on_spot:9A1AA,7020153;");
}

static SkimTciClient *g_client;
static int cond_dds(void) { return skim_tci_client_center_hz(g_client) == 7030000.0; }
static int cond_stamp_asked(void) { return srv_rx_contains("iq_stamp:1;"); }

/* Queue fixture `req` on the mock and wait until EVERY marker the mock has
 * queued so far (this fixture's included — earlier stamp fixtures leave
 * theirs in flight too) has come back: returns 1 once the fixture is fully
 * handled AND the stream survived it, 0 on timeout (the client lost sync). */
static int s_marks_snap;
static int cond_marked(void) {
  const int sent = g_atomic_int_get(&s_marks_sent);
  return sent > s_marks_snap && g_atomic_int_get(&c_marks) >= sent;
}
static int fixture(int req) {
  s_marks_snap = g_atomic_int_get(&s_marks_sent);
  g_atomic_int_set(&s_stamp_req, req);
  const int ok = wait_ms(cond_marked, 3000);
  printf("       fixture %d: marker %s — blocks %d, rx1 %d, odd %d, hz 7021000×%d 7022000×%d 7023000×%d\n",
         req, ok ? "arrived" : "MISSING", g_atomic_int_get(&c_blocks),
         g_atomic_int_get(&c_rx1_blocks), g_atomic_int_get(&c_odd_frames),
         g_atomic_int_get(&c_hz_7021000), g_atomic_int_get(&c_hz_7022000),
         g_atomic_int_get(&c_hz_7023000));
  return ok;
}

int main(void) {
  printf("=== TCI client gate (offline, mock server) ===\n");
  s_rx = g_string_new(NULL);
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
    printf("FAIL — mock server would not start on :%d\n", PORT);
    return 1;
  }
  GThread *st = g_thread_new("mock-tci-server", server_thread, NULL);

  SkimTciClient *c = g_client = skim_tci_client_new("127.0.0.1", PORT);
  skim_tci_client_set_iq_cb(c, iq_cb, NULL);

  GError *err = NULL;
  check("start() completes the handshake (ready;)",
        skim_tci_client_start(c, RATE, &err));
  if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
  check("protocol getter carries ExpertSDR3,1.9",
        strcmp(skim_tci_client_protocol(c), "ExpertSDR3,1.9") == 0);
  check("device getter carries the init-block name",
        strcmp(skim_tci_client_device(c), "MockSDR") == 0);
  check("center_hz picked up from dds:0 (7.020 MHz)",
        skim_tci_client_center_hz(c) == 7020000.0);
  check("iq rate getter reflects the announced rate",
        skim_tci_client_iq_rate(c) == RATE);
  check("client requested iq_samplerate:48000 + iq_start:0",
        wait_ms(cond_iq_sub, 2000));

  check("IQ blocks flow (≥4 reassembled across WS fragments)",
        wait_ms(cond_blocks, 5000));
  g_mutex_lock(&c_lock);
  guint    nframes = c_nframes;
  double   rate = c_rate, center = c_center;
  int      have = c_have_block;
  g_mutex_unlock(&c_lock);
  check("block geometry: 2048 frames @ 48 kHz (length = frames×2)",
        nframes == BLK && rate == (double)RATE);
  check("IQ callback carries the dds centre", center == 7020000.0);
  /* centre stamps (sdr-for-linux family extension): the client asked for
   * them, a stamped block's own centre beats the dds label, and a stamped
   * boundary splits the block into two callbacks at the stamped frame */
  check("client asked for centre stamps (iq_stamp:1 after iq_start)", srv_rx_contains("iq_stamp:1;"));
  g_atomic_int_set(&s_stamp_req, 1);
  check("stamped block: the callback carries h[8] (7021000), not the dds label", wait_ms(cond_stamp_single, 3000));
  g_atomic_int_set(&s_stamp_req, 2);
  check("stamped boundary at 700: two callbacks — 700 frames @ 7020000, 1348 @ 7025000", wait_ms(cond_stamp_pair, 3000));

  /* --- TCI hardening (gh#2) --- */
  /* SKM-10: the request is THREE WebSocket text frames, not one batched
   * frame (srv_rx_contains cannot see framing — the frame counter can). */
  wait_ms(cond_stamp_asked, 2000);
  check("SKM-10: iq_samplerate, iq_start, iq_stamp arrive as three text frames",
        g_atomic_int_get(&s_text_frames) == 3);
  /* SKM-7: a block for receiver 1 (at a telltale 96 kHz) is never delivered;
   * the marker behind it proves receiver-0 blocks keep flowing. */
  int marked = fixture(3);
  check("SKM-7: a receiver-1 block is dropped (no callback at its 96 kHz)",
        marked && g_atomic_int_get(&c_rx1_blocks) == 0);
  /* SKM-9: a message with 100 trailing bytes delivers its ONE block (stamp
   * 7022000, whole 2048 frames) and the marker behind it still parses. */
  marked = fixture(4);
  check("SKM-9: padded message → exactly one block, stream continues",
        marked && g_atomic_int_get(&c_hz_7022000) == 1 &&
        g_atomic_int_get(&c_odd_frames) == 2 /* only the stamped-boundary pair */);
  /* SKM-9: a block cut 1000 bytes short is dropped whole — no partial
   * callback (7023000 never shows), the marker behind it is not misread. */
  marked = fixture(5);
  check("SKM-9: truncated message dropped, no desync of the next block",
        marked && g_atomic_int_get(&c_hz_7023000) == 0 &&
        g_atomic_int_get(&c_odd_frames) == 2);

  if (have) {
    /* Correlate the ingested block against e^{±j2π·12k·t}: ingest is
     * pass-through, so the wire +12 kHz tone must still be at +12 kHz (true
     * orientation), the −12 kHz image at the noise floor. */
    double pr = 0, pi = 0, nr = 0, ni = 0;
    g_mutex_lock(&c_lock);
    for (int i = 0; i < BLK; i++) {
      double a  = 2.0 * G_PI * TONE_HZ * (double)i / (double)RATE;
      double ci = c_block[2 * i], cq = c_block[2 * i + 1];
      pr += ci * cos(a) + cq * sin(a);
      pi += cq * cos(a) - ci * sin(a);
      nr += ci * cos(a) - cq * sin(a);
      ni += cq * cos(a) + ci * sin(a);
    }
    g_mutex_unlock(&c_lock);
    double pos = sqrt(pr * pr + pi * pi) / BLK;
    double neg = sqrt(nr * nr + ni * ni) / BLK;
    check("pass-through ingest: wire +12 kHz tone stays at +12 kHz, amp ~0.5",
          pos > 0.4 && pos < 0.6);
    check("−12 kHz image suppressed > 40 dB", neg < pos * 0.01);
  } else {
    checks += 2; fails += 2;
    printf("  FAIL orientation checks skipped (no block arrived)\n");
  }

  /* Retune: a dds broadcast mid-stream must move center_hz live. */
  g_atomic_int_set(&s_push_dds, 1);
  check("dds:0,7030000; broadcast updates center_hz live", wait_ms(cond_dds, 2000));

  /* Outgoing spot plumbing (format lands in M5's real pipeline). */
  skim_tci_client_spot(c, "OK1BR", "CW", 7020150.4, 0xFFFF0000u, "cq;test");
  check("spot() reaches the server", wait_ms(cond_spot, 2000));
  g_mutex_lock(&s_lock);
  check("spot format: call,mode,rounded hz,argb,scrubbed text",
        strcmp(s_spot, "spot:OK1BR,CW,7020150,4294901760,cq test") == 0);
  g_mutex_unlock(&s_lock);

  /* A decoded-callsign click announces itself like a panadapter spot click
   * (the server relays it; log-for-linux prefills from the broadcast). */
  skim_tci_client_spot_clicked(c, "9A1AA", 7020152.6);
  check("spot_clicked() sends clicked_on_spot:call,rounded hz",
        wait_ms(cond_click, 2000));

  skim_tci_client_stop(c);
  check("stop() sends iq_stop:0;", wait_ms(cond_stop, 1000));
  int frozen = g_atomic_int_get(&c_blocks);
  g_usleep(300 * 1000);
  check("no IQ callback after stop()", g_atomic_int_get(&c_blocks) == frozen);

  skim_tci_client_free(c);

  /* --- second session: a server that never echoes iq_stamp (SKM-8) --- */
  /* Thetis zeroes the reserved words, ExpertSDR3 is closed source — a
   * server putting anything there must NOT move our centre. */
  g_atomic_int_set(&s_echo_stamp, 0);
  capture_reset();
  c = g_client = skim_tci_client_new("127.0.0.1", PORT);
  skim_tci_client_set_iq_cb(c, iq_cb, NULL);
  check("SKM-8 session: start() completes against a server without iq_stamp",
        skim_tci_client_start(c, RATE, &err));
  if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
  check("SKM-8 session: IQ flows", wait_ms(cond_blocks, 5000));
  marked = fixture(1);                         /* junk 7021000 in h[8]        */
  check("SKM-8: without the echo, h[8] is ignored — callbacks carry the dds label",
        marked && g_atomic_int_get(&c_hz_7021000) == 0);
  marked = fixture(2);                         /* junk boundary at 700        */
  check("SKM-8: without the echo, a junk boundary does not split the block",
        marked && g_atomic_int_get(&c_odd_frames) == 0);
  g_mutex_lock(&c_lock);
  check("SKM-8: every callback of the session sits on the dds centre (7020000)",
        c_center == 7020000.0);
  g_mutex_unlock(&c_lock);
  skim_tci_client_stop(c);
  skim_tci_client_free(c);

  g_atomic_int_set(&s_run, 0);
  lws_cancel_service(s_ctx);
  g_thread_join(st);
  lws_context_destroy(s_ctx);

  printf("\n=== %d checks, %d failures ===\n", checks, fails);
  if (fails) {
    g_mutex_lock(&s_lock);
    printf("server received:\n%s\n", s_rx->str);
    g_mutex_unlock(&s_lock);
    printf("FAIL\n");
    return 1;
  }
  printf("PASS — handshake, IQ reassembly, true-orientation ingest, spot "
         "plumbing and the foreign-server hardening all behave.\n");
  return 0;
}
