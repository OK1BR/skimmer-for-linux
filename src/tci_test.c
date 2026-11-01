/*
 * skimmer-tci-test — offline gate for the TCI client (M1). No radio, no GUI.
 *
 * Runs a mock TCI server (libwebsockets, the sdr-for-linux wire dialect) with
 * BOTH of sdr-for-linux's listeners — TCP 127.0.0.1:40124 and a Unix socket
 * at the client's default path under a scratch XDG_RUNTIME_DIR (never the
 * live path: lws unlinks whatever sits there before it binds) — and drives
 * skim_tci_client against it. The whole session below runs once over each
 * transport (gh#39):
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
 *     ignored (dds label, one whole-block callback),
 *   - header layouts + packet counter (gh#38, sdr-for-linux#43): the client
 *     asks iq_stamp:2 and reads the words the ECHO promises — stamps at 9–11
 *     and the TCI 2.1 counter at word 8 from a server that answers 2
 *     (sdr-for-linux ≥ 0.6.0), stamps at 8–10 and no counter from one that
 *     answers 1 (before #43), nothing without an echo. A counter jump and a
 *     truncated block each reach the IQ callback as the gap before the next
 *     block (lost blocks × block size), logged with their numbers; under
 *     layout 1 a jump goes unnoticed (word 8 is a frequency there),
 *   - the remote-tester diagnostics fire: a mock that answers iq_samplerate
 *     with ANOTHER rate and never starts IQ produces the "server runs IQ at
 *     X Hz (asked for Y)" line and the 3 s "no IQ block" warning,
 *   - transports (gh#39): the server-address parser (unix:/path,
 *     ws://host[:port], host[:port]; wss://, relative paths, bad ports
 *     refused), the candidate order — an absent socket or a stale file at
 *     its path fails inside connect() and TCP answers well inside the
 *     handshake timeout, the default order (no server string) takes the
 *     socket at the default path, nothing answering names every candidate
 *     in the error, a malformed string is its own error before any dial —
 *     and the endpoint getter names the address that won.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <libwebsockets.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/tci_client.h"

#define PORT      40124           /* TCP listener — never the live 40001       */
#define RATE      48000

/* The two listeners' addresses (filled in main) and the scratch runtime dir
 * the socket lives in. The socket path is a static: lws keeps the iface
 * pointer and unlink()s it at context destroy. */
static char  s_tcp_addr[64];
static char  s_unix_path[SKIM_TCI_SOCK_PATH_MAX];
static char  s_unix_addr[SKIM_TCI_SOCK_PATH_MAX + 8];
static char *s_scratch;
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
/* The header layout the mock SERVES (gh#38): 2 = sdr-for-linux ≥ 0.6.0
 * (echoes min(request, 2); count at word 8, stamps at 9–11 under 2), 1 = a
 * server before sdr-for-linux#43 (echoes 1 to any non-zero request; stamps
 * at 8–10, no counter), -1 = a server that never heard of iq_stamp (no echo;
 * words 8–10 carry whatever the fixture wrote — junk to the client). */
static volatile gint s_served = 2;
static volatile gint s_layout = 0;       /* layout of the blocks queued NOW: the
                                          * real server serves 0 until the echo */
static guint32       s_count, s_count_rx1;  /* per-stream packet counters (s_lock) */
static volatile gint s_skip;             /* counts the rx0 counter skips before
                                          * the next block (a gap fixture)       */
static volatile gint s_text_frames;      /* complete text frames received from the client */
static volatile gint s_marks_sent;       /* MARKER blocks queued so far (one per fixture) */
static volatile gint s_rate_echo;        /* ≠0: echo THIS iq_samplerate instead of the request */
static volatile gint s_ignore_start;     /* 1: never start IQ (a device that cannot)      */

/* log capture — the diagnostics are g_message/g_warning lines; the gate
 * reads them back through a structured-log writer (default output kept) */
static GMutex   l_lock;
static GString *l_log;
static GLogWriterOutput log_tap(GLogLevelFlags lvl, const GLogField *f, gsize n, gpointer u) {
  (void)u;
  for (gsize i = 0; i < n; i++) {
    if (strcmp(f[i].key, "MESSAGE") == 0) {
      g_mutex_lock(&l_lock);
      if (f[i].length < 0) { g_string_append(l_log, (const char *)f[i].value); }
      else { g_string_append_len(l_log, (const char *)f[i].value, f[i].length); }
      g_string_append_c(l_log, '\n');
      g_mutex_unlock(&l_lock);
    }
  }
  return g_log_writer_default(lvl, f, n, u);
}
static int log_contains(const char *needle) {
  g_mutex_lock(&l_lock);
  const int hit = strstr(l_log->str, needle) != NULL;
  g_mutex_unlock(&l_lock);
  return hit;
}

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
  /* word 8 / stamps per the layout in effect (gh#38) — one producer thread,
   * so the counters advance in queue order like the real server's */
  const gint layout = g_atomic_int_get(&s_layout);
  g_mutex_lock(&s_lock);
  guint32 cnt;
  if (rx == 0) {
    s_count += (guint32)g_atomic_int_get(&s_skip);
    g_atomic_int_set(&s_skip, 0);
    cnt = s_count++;
  } else {
    cnt = s_count_rx1++;                       /* receiver 1 has its own stream */
  }
  g_mutex_unlock(&s_lock);
  switch (layout) {
  case 1: case -1: h[8] = hz0; h[9] = off; h[10] = hz1; break; /* legacy / junk */
  case 2:  h[8] = cnt; h[9] = hz0; h[10] = off; h[11] = hz1; break;
  default: h[8] = cnt; break;                                  /* 0: count only */
  }
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
    const gint ovr = g_atomic_int_get(&s_rate_echo);
    if (ovr) { g_snprintf(echo, sizeof(echo), "iq_samplerate:%d;", ovr); }
    else     { g_snprintf(echo, sizeof(echo), "%s;", cmd); }
    srv_queue_text(echo);
  } else if (g_str_has_prefix(cmd, "iq_start:")) {
    if (!g_atomic_int_get(&s_ignore_start)) {
      srv_queue_text("iq_start:0;");
      g_atomic_int_set(&s_streaming, 1);
    }                                          /* else: silence, like a device that cannot */
  } else if (g_str_has_prefix(cmd, "iq_stop:")) {
    g_atomic_int_set(&s_streaming, 0);
    srv_queue_text("iq_stop:0;");
  } else if (g_str_has_prefix(cmd, "iq_stamp:")) {
    const gint served = g_atomic_int_get(&s_served);
    if (served >= 0) {
      /* the family extension echo: the layout SERVED, min(request, served)
       * — a pre-#43 server answers any non-zero request with 1; the flag
       * flips BEFORE the echo is queued and the FIFO carries text and
       * binary in order, exactly like the real server */
      gint req = atoi(cmd + 9);
      const gint l = MIN(MAX(req, 0), served);
      g_atomic_int_set(&s_layout, l);
      char echo[32];
      g_snprintf(echo, sizeof(echo), "iq_stamp:%d;", l);
      srv_queue_text(echo);
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
    /* A fresh session starts with an EMPTY outbox — the queue is global and
     * blocks left over from the previous client would be sent to this one
     * (caught when the no-IQ session received the old session's tail). */
    g_mutex_lock(&s_lock);
    Msg *left;
    while ((left = g_queue_pop_head(&s_out)) != NULL) { g_free(left->data); g_free(left); }
    g_mutex_unlock(&s_lock);
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
    g_atomic_int_set(&s_streaming, 0);         /* the subscription died with the socket */
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
    /* gap fixtures (gh#38): the packet counter skips 1 / 8 blocks before
     * the next regular block — the server emitted them, we never got them */
    if (sr == 6) { g_atomic_int_set(&s_skip, 1); srv_queue_iq_block(); }
    if (sr == 7) { g_atomic_int_set(&s_skip, 8); srv_queue_iq_block(); }
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
static volatile gint c_gaps;            /* callbacks that carried a gap (gh#38)      */
static guint         c_gap_last, c_gap_total;   /* frames (c_lock)                   */

static void capture_reset(void) {
  g_mutex_lock(&c_lock);
  memset(c_ring_n, 0, sizeof(c_ring_n)); memset(c_ring_hz, 0, sizeof(c_ring_hz)); c_ring_i = 0;
  c_have_block = 0; c_nframes = 0; c_rate = 0; c_center = 0;
  c_gap_last = 0; c_gap_total = 0;
  g_mutex_unlock(&c_lock);
  g_atomic_int_set(&c_gaps, 0);
  g_atomic_int_set(&c_blocks, 0);
  g_atomic_int_set(&c_rx1_blocks, 0);
  g_atomic_int_set(&c_marks, 0);
  g_atomic_int_set(&s_marks_sent, 0);          /* a new session, no marker in flight */
  g_atomic_int_set(&c_hz_7022000, 0); g_atomic_int_set(&c_hz_7023000, 0);
  g_atomic_int_set(&c_hz_7021000, 0); g_atomic_int_set(&c_odd_frames, 0);
}

/* A fresh session on the mock: what the client said so far, the last spot
 * and the frame counter go — the session checks read them back, and the
 * same session runs over two transports. */
static void mock_reset(void) {
  g_mutex_lock(&s_lock);
  g_string_truncate(s_rx, 0);
  s_spot[0] = '\0';
  g_mutex_unlock(&s_lock);
  g_atomic_int_set(&s_text_frames, 0);
  g_atomic_int_set(&s_push_dds, 0);
  g_atomic_int_set(&s_stamp_req, 0);
  g_mutex_lock(&s_lock);
  s_count = s_count_rx1 = 0;                   /* a fresh iq_start counts from 0 */
  g_mutex_unlock(&s_lock);
  g_atomic_int_set(&s_skip, 0);
  g_atomic_int_set(&s_layout, g_atomic_int_get(&s_served) >= 0 ? 0 : -1);
  capture_reset();
}

static void iq_cb(const float *iq, guint nframes, double rate, double center,
                  guint gap_frames, gpointer user) {
  (void)user;
  g_mutex_lock(&c_lock);
  c_ring_n[c_ring_i % CAP_RING] = nframes; c_ring_hz[c_ring_i % CAP_RING] = center; c_ring_i++;
  if (gap_frames) { c_gap_last = gap_frames; c_gap_total += gap_frames; }
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
  if (gap_frames) { g_atomic_int_inc(&c_gaps); }
}

static guint gap_last(void) {
  g_mutex_lock(&c_lock);
  const guint g = c_gap_last;
  g_mutex_unlock(&c_lock);
  return g;
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
static int cond_diag(void) {
  return log_contains("tci: server runs IQ at 96000 Hz (asked for 48000)") &&
         log_contains("tci: no IQ block 3 s after iq_start:0");
}
static int cond_stamp_asked(void) { return srv_rx_contains("iq_stamp:2;"); }
static int cond_layout_echoed(void) { return skim_tci_client_stamp_layout(g_client) >= 0; }

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

/* The whole M1 session — handshake, IQ reassembly, centre stamps, the
 * hardening fixtures, orientation, retune, spots, stop — over ONE transport
 * against a mock serving header layout `served` (2 = sdr-for-linux ≥ 0.6.0,
 * 1 = before its #43). Run over TCP and over the Unix socket (gh#39): the
 * WebSocket layer is the same, but the dial, the Host: header and the way a
 * failure surfaces are not; and once more against the old layout (gh#38):
 * the cross-version pair that exists the day either side updates. */
static void session_full(const char *server, int served) {
  g_atomic_int_set(&s_served, served);
  mock_reset();
  SkimTciClient *c = g_client = skim_tci_client_new(server);
  skim_tci_client_set_iq_cb(c, iq_cb, NULL);

  GError *err = NULL;
  check("start() completes the handshake (ready;)",
        skim_tci_client_start(c, RATE, &err));
  if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
  check("endpoint getter names the address that won",
        g_strcmp0(skim_tci_client_endpoint(c), server) == 0);
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
  check("client asked for header layout 2 (iq_stamp:2 after iq_start)", srv_rx_contains("iq_stamp:2;"));
  char what[160];
  g_snprintf(what, sizeof(what), "gh#38: layout getter = %d — what the server echoed, not what we asked", served);
  check(what, wait_ms(cond_layout_echoed, 2000) && skim_tci_client_stamp_layout(c) == served);
  g_snprintf(what, sizeof(what), "gh#38: log names the layout ('tci: IQ header layout %d — …')", served);
  check(what, log_contains(served == 2
            ? "tci: IQ header layout 2 — packet counter at word 8, centre stamps at words 9–11"
            : "tci: IQ header layout 1 — legacy centre stamps at words 8–10, no packet counter"));
  g_atomic_int_set(&s_stamp_req, 1);
  g_snprintf(what, sizeof(what), "stamped block: the callback carries the stamped centre (7021000, word %d), not the dds label", served == 2 ? 9 : 8);
  check(what, wait_ms(cond_stamp_single, 3000));
  g_atomic_int_set(&s_stamp_req, 2);
  check("stamped boundary at 700: two callbacks — 700 frames @ 7020000, 1348 @ 7025000", wait_ms(cond_stamp_pair, 3000));
  check("gh#38: the contiguous stream reports no gap (counter consecutive / no counter)",
        g_atomic_int_get(&c_gaps) == 0);

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
  check("gh#38: the truncated block is reported as a 2048-frame gap before the next block (both layouts)",
        g_atomic_int_get(&c_gaps) == 1 && gap_last() == BLK &&
        skim_tci_client_lost_blocks(c) == 1);
  /* gh#38: packet-counter gaps — only a layout WITH the counter sees them */
  marked = fixture(6);
  if (served == 2) {
    check("gh#38: counter skips 1 block → the next block carries a 2048-frame gap",
          marked && g_atomic_int_get(&c_gaps) == 2 && gap_last() == BLK);
    check("gh#38: the gap is logged with its numbers (1 block ≈ 43 ms at 48 kHz)",
          log_contains("tci: IQ packet counter jumped") &&
          log_contains("1 block ≈ 43 ms of IQ lost"));
  } else {
    check("gh#38: layout 1 has no counter — a skipped block goes unnoticed",
          marked && g_atomic_int_get(&c_gaps) == 1);
  }
  marked = fixture(7);
  if (served == 2) {
    check("gh#38: counter skips 8 blocks → 16384-frame gap; 10 blocks lost this session (1 truncated + 1 + 8)",
          marked && g_atomic_int_get(&c_gaps) == 3 && gap_last() == 8 * BLK &&
          skim_tci_client_lost_blocks(c) == 10);
  } else {
    check("gh#38: layout 1 — still only the truncated block counted as lost",
          marked && skim_tci_client_lost_blocks(c) == 1);
  }

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
  check("after stop() the endpoint getter is NULL", skim_tci_client_endpoint(c) == NULL);

  skim_tci_client_free(c);
}

int main(void) {
  /* ⛔ A scratch runtime dir, set before GLib reads XDG_RUNTIME_DIR, so the
   * client's default socket path lands in it — never the live one: lws
   * unlinks whatever sits at the path before it binds, so the DEFAULT path
   * would steal a running sdr-for-linux's socket (the socket-path
   * counterpart of using port 40124 instead of 40001). */
  s_scratch = g_dir_make_tmp("skimmer-tci-XXXXXX", NULL);
  if (!s_scratch) { printf("FAIL — no scratch directory\n"); return 1; }
  g_setenv("XDG_RUNTIME_DIR", s_scratch, TRUE);

  printf("=== TCI client gate (offline, mock server) ===\n");
  s_rx = g_string_new(NULL);
  l_log = g_string_new(NULL);
  g_log_set_writer_func(log_tap, NULL, NULL);
  g_queue_init(&s_out);

  /* The mock: one context, two explicit vhosts sharing the protocol table —
   * TCP on 127.0.0.1:PORT and the Unix socket at the client's DEFAULT path
   * under the scratch dir, so the default-order check finds it there. */
  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  lws_set_log_level(LLL_ERR, NULL);
  info.port      = CONTEXT_PORT_NO_LISTEN;
  info.options   = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
  info.gid       = (gid_t)-1;
  info.uid       = (uid_t)-1;
  s_ctx = lws_create_context(&info);
  if (!s_ctx) {
    printf("FAIL — mock context would not start\n");
    return 1;
  }
  struct lws_context_creation_info vi;
  memset(&vi, 0, sizeof(vi));
  vi.vhost_name = "tcp";
  vi.port       = PORT;
  vi.iface      = "127.0.0.1";
  vi.options    = LWS_SERVER_OPTION_DISABLE_IPV6;
  vi.protocols  = s_protocols;
  if (!lws_create_vhost(s_ctx, &vi)) {
    printf("FAIL — mock server would not start on :%d\n", PORT);
    return 1;
  }
  g_snprintf(s_tcp_addr, sizeof(s_tcp_addr), "ws://127.0.0.1:%d", PORT);

  char *sock = skim_tci_default_socket_path();
  char *sdir = g_path_get_dirname(sock);
  check("default socket path = $XDG_RUNTIME_DIR/sdr-for-linux/tci.sock (under the scratch dir)",
        g_str_has_prefix(sock, s_scratch) &&
        g_str_has_suffix(sock, "/sdr-for-linux/tci.sock"));
  if (g_mkdir_with_parents(sdir, 0700) != 0 || strlen(sock) >= sizeof(s_unix_path)) {
    printf("FAIL — socket directory %s\n", sdir);
    return 1;
  }
  g_strlcpy(s_unix_path, sock, sizeof(s_unix_path));
  g_snprintf(s_unix_addr, sizeof(s_unix_addr), "unix:%s", s_unix_path);
  g_free(sock);
  memset(&vi, 0, sizeof(vi));
  vi.vhost_name = "unix";
  vi.port       = 0;
  vi.iface      = s_unix_path;
  vi.options    = LWS_SERVER_OPTION_UNIX_SOCK;
  vi.protocols  = s_protocols;
  if (!lws_create_vhost(s_ctx, &vi)) {
    printf("FAIL — mock socket vhost would not bind %s\n", s_unix_path);
    return 1;
  }
  check("mock socket file exists at the default path",
        g_file_test(s_unix_path, G_FILE_TEST_EXISTS));
  GThread *st = g_thread_new("mock-tci-server", server_thread, NULL);

  printf("\n--- full session over TCP %s (server layout 2) ---\n", s_tcp_addr);
  session_full(s_tcp_addr, 2);
  printf("\n--- full session over the Unix socket %s (server layout 2) ---\n", s_unix_addr);
  session_full(s_unix_addr, 2);
  printf("\n--- full session over TCP %s against a pre-#43 server (layout 1) ---\n", s_tcp_addr);
  session_full(s_tcp_addr, 1);

  SkimTciClient *c;
  GError *err = NULL;
  int marked;

  /* --- second session: a server that never echoes iq_stamp (SKM-8) --- */
  /* Thetis zeroes the reserved words, ExpertSDR3 is closed source — a
   * server putting anything there must NOT move our centre. */
  g_atomic_int_set(&s_served, -1);
  mock_reset();
  c = g_client = skim_tci_client_new(s_tcp_addr);
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
  check("gh#38: without an echo the layout getter stays -1 and word 8 is never read — no gap",
        skim_tci_client_stamp_layout(c) == -1 && g_atomic_int_get(&c_gaps) == 0);
  skim_tci_client_stop(c);
  skim_tci_client_free(c);
  g_atomic_int_set(&s_served, 2);              /* back to today's server      */

  /* --- third session: the "connected, no output" picture (gh#2) --- */
  /* The mock answers iq_samplerate with 96000 and never starts IQ; the
   * client must say both in its log within ~3 s — that is what a remote
   * tester's log has to carry. */
  g_atomic_int_set(&s_rate_echo, 96000);
  g_atomic_int_set(&s_ignore_start, 1);
  capture_reset();
  c = g_client = skim_tci_client_new(s_tcp_addr);
  skim_tci_client_set_iq_cb(c, iq_cb, NULL);
  check("diag session: start() completes (the init block still ends in ready;)",
        skim_tci_client_start(c, RATE, &err));
  if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
  const int diag_ok = wait_ms(cond_diag, 5000);
  check("diag: 'server runs IQ at 96000 Hz (asked for 48000)' + 'no IQ block 3 s after iq_start:0' logged",
        diag_ok);
  if (!diag_ok) {
    g_mutex_lock(&l_lock);
    printf("       captured log:\n%s", l_log->str);
    g_mutex_unlock(&l_lock);
  }
  check("diag session: no IQ callback at all", g_atomic_int_get(&c_blocks) == 0);
  skim_tci_client_stop(c);
  skim_tci_client_free(c);
  g_atomic_int_set(&s_rate_echo, 0);
  g_atomic_int_set(&s_ignore_start, 0);

  /* --- transports (gh#39) --- */
  printf("\n--- server address parser ---\n");
  {
    static const struct { const char *in, *canon; } good[] = {
      { "unix:/run/user/1000/sdr-for-linux/tci.sock", "unix:/run/user/1000/sdr-for-linux/tci.sock" },
      { "ws://127.0.0.1:40001",     "ws://127.0.0.1:40001" },
      { "ws://127.0.0.1",           "ws://127.0.0.1:40001" },
      { "127.0.0.1",                "ws://127.0.0.1:40001" },
      { "radio.local:40002",        "ws://radio.local:40002" },
      { "ws://sunsdr.lan:50001/",   "ws://sunsdr.lan:50001" },
      { "  ws://10.0.0.5:40001  ",  "ws://10.0.0.5:40001" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(good); i++) {
      SkimTciEndpoint ep;
      gboolean ok = skim_tci_endpoint_parse(good[i].in, &ep);
      char *canon = ok ? skim_tci_endpoint_to_string(&ep) : NULL;
      char what[200];
      g_snprintf(what, sizeof(what), "parse \"%s\" → %s", good[i].in, good[i].canon);
      check(what, ok && g_strcmp0(canon, good[i].canon) == 0);
      g_free(canon);
    }
    char longpath[SKIM_TCI_SOCK_PATH_MAX + 2];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[0] = '/'; longpath[sizeof(longpath) - 1] = '\0';
    char *longaddr = g_strdup_printf("unix:%s", longpath);
    const char *bad[] = {
      "", "   ", "unix:", "unix:tci.sock", "unix:@tci", "/run/user/1000/tci.sock",
      "wss://127.0.0.1:40001", "http://127.0.0.1:40001", "ws://127.0.0.1:0",
      "ws://127.0.0.1:70000", "ws://127.0.0.1:abc", "ws://:40001", "ws://a b:40001",
      "ws://127.0.0.1:40001:1", longaddr,
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
      SkimTciEndpoint ep;
      memset(&ep, 0, sizeof(ep));
      ep.port = 1234;                          /* must survive a refusal    */
      gboolean ok = skim_tci_endpoint_parse(bad[i], &ep);
      char what[200];
      g_snprintf(what, sizeof(what), "refuse \"%s\"",
                 strlen(bad[i]) > 40 ? "unix:/<108+ chars>" : bad[i]);
      check(what, !ok && ep.port == 1234);
    }
    g_free(longaddr);
  }

  printf("\n--- candidate order ---\n");
  char *absent = g_build_filename(s_scratch, "nobody", "tci.sock", NULL);
  char *absent_addr = g_strdup_printf("unix:%s", absent);
  {
    /* An absent socket fails inside connect(), so the client is on the TCP
     * candidate well inside the per-candidate handshake timeout — the
     * default order must not cost a radio on TCP seconds per reconnect. */
    const char *order[] = { absent_addr, s_tcp_addr, NULL };
    mock_reset();
    c = skim_tci_client_new_order(order);
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = skim_tci_client_start(c, RATE, &err);
    gint64 took_ms = (g_get_monotonic_time() - t0) / 1000;
    if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
    printf("       absent socket → TCP took %lld ms\n", (long long)took_ms);
    check("fallback: an absent socket is skipped, the TCP candidate answers", ok);
    check("fallback: well inside the handshake timeout (< 1500 ms)", took_ms < 1500);
    check("fallback: the endpoint getter names the TCP address",
          g_strcmp0(skim_tci_client_endpoint(c), s_tcp_addr) == 0);
    skim_tci_client_stop(c);
    skim_tci_client_free(c);

    /* A stale file at the path (a crashed server's leftover): ECONNREFUSED
     * at once, the same cheap fallback. */
    char *stale = g_build_filename(s_scratch, "stale.sock", NULL);
    g_file_set_contents(stale, "", 0, NULL);
    char *stale_addr = g_strdup_printf("unix:%s", stale);
    const char *order2[] = { stale_addr, s_tcp_addr, NULL };
    mock_reset();
    c = skim_tci_client_new_order(order2);
    t0 = g_get_monotonic_time();
    ok = skim_tci_client_start(c, RATE, &err);
    took_ms = (g_get_monotonic_time() - t0) / 1000;
    if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
    printf("       stale file → TCP took %lld ms\n", (long long)took_ms);
    check("stale file at the socket path: refused at once, TCP answers (< 1500 ms)",
          ok && took_ms < 1500 &&
          g_strcmp0(skim_tci_client_endpoint(c), s_tcp_addr) == 0);
    skim_tci_client_stop(c);
    skim_tci_client_free(c);
    g_remove(stale);
    g_free(stale_addr);
    g_free(stale);
  }
  {
    /* The default order with no server string: the socket at the radio's
     * default path wins (the TCP candidate would be the live 40001 — with
     * nothing there, or a real radio that is not MockSDR). */
    mock_reset();
    c = skim_tci_client_new(NULL);
    skim_tci_client_set_iq_cb(c, iq_cb, NULL);
    gboolean ok = skim_tci_client_start(c, RATE, &err);
    if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
    check("default order (NULL server): the socket at the default path wins",
          ok && g_strcmp0(skim_tci_client_endpoint(c), s_unix_addr) == 0);
    check("default order: the init block came over the socket (device MockSDR)",
          ok && strcmp(skim_tci_client_device(c), "MockSDR") == 0);
    check("default order: IQ flows over the socket", ok && wait_ms(cond_blocks, 5000));
    skim_tci_client_stop(c);
    skim_tci_client_free(c);

    mock_reset();
    c = skim_tci_client_new("   ");
    ok = skim_tci_client_start(c, RATE, &err);
    if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
    check("blank server string = the default order",
          ok && g_strcmp0(skim_tci_client_endpoint(c), s_unix_addr) == 0);
    skim_tci_client_stop(c);
    skim_tci_client_free(c);
  }
  {
    /* Nothing answers: the error names every candidate and why. A malformed
     * server string is its own error, before any dial. */
    const char *order[] = { absent_addr, "ws://127.0.0.1:9", NULL };   /* discard */
    c = skim_tci_client_new_order(order);
    gboolean ok = skim_tci_client_start(c, RATE, &err);
    check("nothing answers: start() fails", !ok && err != NULL);
    if (err) {
      printf("       (%s)\n", err->message);
      check("nothing answers: the error names the socket candidate",
            strstr(err->message, absent_addr) != NULL);
      check("nothing answers: the error names the TCP candidate as refused",
            strstr(err->message, "ws://127.0.0.1:9 (refused)") != NULL);
      g_clear_error(&err);
    } else { checks += 2; fails += 2; }
    check("nothing answers: no endpoint", skim_tci_client_endpoint(c) == NULL);
    skim_tci_client_free(c);

    c = skim_tci_client_new("wss://127.0.0.1:40001");
    ok = skim_tci_client_start(c, RATE, &err);
    check("malformed server string: start() fails before any dial",
          !ok && err && strstr(err->message, "malformed") &&
          strstr(err->message, "wss://127.0.0.1:40001"));
    if (err) { printf("       (%s)\n", err->message); g_clear_error(&err); }
    skim_tci_client_free(c);
  }
  g_free(absent_addr);
  g_free(absent);

  g_atomic_int_set(&s_run, 0);
  lws_cancel_service(s_ctx);
  g_thread_join(st);
  lws_context_destroy(s_ctx);
  /* lws unlinks the socket file at destroy — the real server relies on the
   * same, so a stale file never survives a clean stop. */
  check("socket file gone after the mock stopped", !g_file_test(s_unix_path, G_FILE_TEST_EXISTS));
  g_rmdir(sdir);
  g_rmdir(s_scratch);
  g_free(sdir);

  printf("\n=== %d checks, %d failures ===\n", checks, fails);
  if (fails) {
    g_mutex_lock(&s_lock);
    printf("server received:\n%s\n", s_rx->str);
    g_mutex_unlock(&s_lock);
    printf("FAIL\n");
    return 1;
  }
  printf("PASS — handshake, IQ reassembly, true-orientation ingest, spot "
         "plumbing, the foreign-server hardening and both transports all behave.\n");
  return 0;
}
