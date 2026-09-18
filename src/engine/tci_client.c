/* tci_client.c — TCI WebSocket client (M1: transport, handshake, IQ ingest).
 *
 * libwebsockets client on its own service thread (the sdr-for-linux
 * tci_test.c/piHPSDR pattern): lws_service loop + lws_cancel_service wakeups,
 * outgoing text queued and flushed on WRITEABLE, ONE command per text frame
 * (the spec says nothing about batching; ftl/tci, written for ExpertSDR,
 * sends one per frame — SKM-10). Incoming text is accumulated and split on
 * ';' (sdr-for-linux batches its init block into one frame). Binary: ONE
 * complete WebSocket message = ONE Stream block (spec 3.4 draws it as a
 * struct) — fragments are collected until the message ends, the header is
 * parsed once, trailing bytes are ignored and a short message is dropped
 * with a warning (SKM-9; the old byte-stream cut read any padding as the
 * next header). Blocks of other receivers are dropped (SKM-7: Thetis'
 * AlwaysStreamIQ pushes every receiver to every client) and the centre
 * stamps in the reserved words count only after the server echoed
 * iq_stamp:1 (SKM-8: the spec promises nothing about those words).
 *
 * Wire orientation: the wire already carries the TRUE spectrum — the server
 * conjugates its RF-inverted raw DDC feed on send (the ExpertSDR convention
 * SDC/CW Skimmer consume as-is; sdr-for-linux docs/TCI-SCOPE.md F6d-2d).
 * Ingest is pass-through: a signal above the DDC centre lands at a positive
 * offset. Do NOT conjugate here — live-caught 2026-07-15 mirroring the band.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "tci_client.h"

#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>

#define SKIM_TCI_ERROR      (g_quark_from_static_string("skim-tci-error"))
#define HANDSHAKE_TIMEOUT_S 5
#define STOP_FLUSH_MS       300

/* Binary Stream header: 16 × u32 (TCI spec / sdr-for-linux tci_server.c).
 * [0]=receiver [1]=sample_rate [2]=format(3=float32) [3]=codec [4]=crc
 * [5]=length in samples (= frames×2 for IQ) [6]=type(0=IQ) [7]=channels. */
#define STREAM_HDR_BYTES  64
#define STREAM_TYPE_IQ    0
#define STREAM_FMT_FLOAT  3

struct _SkimTciClient {
  char    *host;
  guint16  port;

  SkimTciIqCb iq_cb;
  gpointer    iq_cb_data;
  SkimTciVfoCb    vfo_cb;       /* fires on the LWS thread                     */
  gpointer        vfo_cb_data;
  SkimTciTxCb     tx_cb;        /* fires on the LWS thread (SCOPE: TX hold)    */
  gpointer        tx_cb_data;
  SkimTciClosedCb closed_cb;    /* fires on the LWS thread                     */
  gpointer        closed_cb_data;

  struct lws_context *ctx;
  struct lws         *wsi;    /* LWS thread only (except on_writable kick)   */
  GThread            *thread;

  GMutex lock;                /* guards everything below                     */
  GCond  cond;
  gboolean up;                /* WS established                              */
  gboolean ready;             /* server finished its init block (ready;)     */
  gboolean failed;            /* connection error                            */
  GQueue   out;               /* outgoing text commands (char*)              */
  double   center_hz;         /* dds:0,<hz>                                  */
  double   vfo_hz;            /* vfo:0,0,<hz> — the tuned frequency          */
  gboolean trx;               /* trx:0,<bool> — the radio's REAL keyed state */
  gboolean tune;              /* tune:0,<bool>                               */
  guint    iq_rate;           /* iq_samplerate announced/echoed              */
  char     device[64];
  char     protocol[64];

  guint    iq_req_rate;       /* iq_samplerate we asked for (0 = none)       */
  gint64   iq_start_us;       /* when iq_start:0 was queued (0 = not yet)    */

  /* LWS thread only — no lock needed (lws_service runs the callbacks on the
   * service thread, and so does the no-IQ watchdog). */
  GString    *txt;            /* text command accumulator                    */
  GByteArray *bin;            /* the binary message being reassembled        */
  gboolean    stamp_ok;       /* server echoed iq_stamp:1 — h[8..10] valid   */
  guint       blocks;         /* IQ blocks delivered this session            */
  gboolean    warned_fmt, warned_rx, warned_short, warned_pad, warned_bogus,
              warned_noiq, warned_rate;
  lws_sorted_usec_list_t noiq_sul; /* wakes the service loop 3 s after iq_start
                                    * left — lws_service() sleeps until an event,
                                    * so a polling check alone came ~5 s late */

  volatile gint run;          /* service loop keeps going while 1            */
  gboolean      started;      /* start() succeeded (iq_start sent)           */
};

/* ---- outgoing text ---------------------------------------------------------- */

/* Queue a command string; the service loop asks for WRITEABLE and flushes. */
static void cli_queue(SkimTciClient *c, char *msg /* takes ownership */) {
  g_mutex_lock(&c->lock);
  g_queue_push_tail(&c->out, msg);
  g_mutex_unlock(&c->lock);
  if (c->ctx) { lws_cancel_service(c->ctx); }
}

/* ---- incoming text ---------------------------------------------------------- */

/* One complete command "name:args" (no ';'). Updates handshake state. */
static void handle_command(SkimTciClient *c, char *cmd) {
  char *args = strchr(cmd, ':');
  if (args) { *args++ = '\0'; }
  for (char *p = cmd; *p; p++) { *p = (char)g_ascii_tolower(*p); }

  SkimTciVfoCb vfo_cb = NULL;
  gpointer     vfo_user = NULL;
  double       vfo_hz = 0;
  SkimTciTxCb  tx_cb = NULL;
  gpointer     tx_user = NULL;
  gboolean     tx_val = FALSE;

  g_mutex_lock(&c->lock);
  if (strcmp(cmd, "ready") == 0) {
    c->ready = TRUE;
    g_cond_broadcast(&c->cond);
  } else if (strcmp(cmd, "protocol") == 0 && args) {
    g_strlcpy(c->protocol, args, sizeof(c->protocol));
  } else if (strcmp(cmd, "device") == 0 && args) {
    g_strlcpy(c->device, args, sizeof(c->device));
  } else if (strcmp(cmd, "dds") == 0 && args) {
    /* dds:<rx>,<hz> — only receiver 0 feeds our IQ stream. */
    char *comma = strchr(args, ',');
    if (comma && strtol(args, NULL, 10) == 0) {
      double hz = g_ascii_strtod(comma + 1, NULL);
      if (hz > 0) { c->center_hz = hz; }
    }
  } else if (strcmp(cmd, "vfo") == 0 && args) {
    /* vfo:<rx>,<ch>,<hz> — the tuned frequency; we track rx 0 channel A. */
    char *c1 = strchr(args, ',');
    char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    if (c2 && strtol(args, NULL, 10) == 0 && strtol(c1 + 1, NULL, 10) == 0) {
      double hz = g_ascii_strtod(c2 + 1, NULL);
      if (hz > 0 && hz != c->vfo_hz) {
        c->vfo_hz = hz;
        vfo_cb   = c->vfo_cb;        /* fire outside the lock */
        vfo_user = c->vfo_cb_data;
        vfo_hz   = hz;
      }
    }
  } else if (strcmp(cmd, "iq_samplerate") == 0 && args) {
    long r = strtol(args, NULL, 10);
    if (r > 0) {
      c->iq_rate = (guint)r;
      /* The echo after OUR request: the rate is device-global, a server may
       * answer with what its device runs. The pipeline builds the bank from
       * the blocks' own rate, so this is information, not an error — but a
       * remote tester's log must show it (gh#2). */
      if (c->iq_start_us && c->iq_req_rate && (guint)r != c->iq_req_rate &&
          !c->warned_rate) {
        c->warned_rate = TRUE;
        g_message("tci: server runs IQ at %ld Hz (asked for %u)", r, c->iq_req_rate);
      }
    }
  } else if (strcmp(cmd, "iq_stamp") == 0 && args) {
    /* sdr-for-linux's echo of our family extension — only now do the
     * reserved header words mean anything (SKM-8). */
    c->stamp_ok = strtol(args, NULL, 10) == 1;
  } else if ((strcmp(cmd, "trx") == 0 || strcmp(cmd, "tune") == 0) && args) {
    /* trx:<rx>,<bool> / tune:<rx>,<bool> — rx 0. sdr-for-linux ≥ cc470af
     * reports the REAL keyed state (CW/RTTY text keying included); tune is
     * OR-ed in for servers where a tune carrier does not raise trx. The
     * combined value drives the pipeline's TX hold (SCOPE: TX hold). */
    char *comma = strchr(args, ',');
    if (comma && strtol(args, NULL, 10) == 0) {
      const gboolean v = g_ascii_strncasecmp(comma + 1, "true", 4) == 0;
      const gboolean was = c->trx || c->tune;
      if (cmd[1] == 'r') { c->trx = v; } else { c->tune = v; }
      const gboolean now = c->trx || c->tune;
      if (now != was) {
        tx_cb   = c->tx_cb;          /* fire outside the lock */
        tx_user = c->tx_cb_data;
        tx_val  = now;
      }
    }
  }
  g_mutex_unlock(&c->lock);

  if (vfo_cb) { vfo_cb(vfo_hz, vfo_user); }
  if (tx_cb) { tx_cb(tx_val, tx_user); }
}

static void drain_text(SkimTciClient *c) {
  char *s = c->txt->str;
  char *semi;
  gsize used = 0;
  while ((semi = strchr(s, ';')) != NULL) {
    *semi = '\0';
    handle_command(c, s);
    used = (gsize)(semi + 1 - c->txt->str);
    s = semi + 1;
  }
  if (used) { g_string_erase(c->txt, 0, (gssize)used); }
}

/* ---- incoming binary (Stream blocks) ---------------------------------------- */

/* One complete binary WebSocket message = one Stream block (TCI spec 3.4).
 * Called once the last fragment is in; c->bin holds the whole message. */
static void handle_block(SkimTciClient *c) {
  const gsize len = c->bin->len;
  if (len < STREAM_HDR_BYTES) {
    if (!c->warned_short) {
      c->warned_short = TRUE;
      g_warning("tci: binary message of %" G_GSIZE_FORMAT " bytes is shorter than a "
                "Stream header, ignored", len);
    }
    return;
  }
  guint32 h[16];
  memcpy(h, c->bin->data, sizeof(h));     /* GByteArray data may be unaligned */
  const guint32 samples = h[5];
  const gsize   need    = STREAM_HDR_BYTES + (gsize)samples * sizeof(float);
  if (samples == 0 || samples > (1u << 20)) {
    if (!c->warned_bogus) {
      c->warned_bogus = TRUE;
      g_warning("tci: bogus Stream length %u samples, message dropped", samples);
    }
    return;
  }
  if (len < need) {
    if (!c->warned_short) {
      c->warned_short = TRUE;
      g_warning("tci: Stream block truncated — header says %u samples (%" G_GSIZE_FORMAT
                " bytes), message carries %" G_GSIZE_FORMAT ", dropped", samples, need, len);
    }
    return;
  }
  if (len > need && !c->warned_pad) {
    /* Padding (the spec's fixed data[16384]?) or a server concatenating
     * blocks — either way the one line in the log says which. */
    c->warned_pad = TRUE;
    g_message("tci: binary message carries %" G_GSIZE_FORMAT " bytes past the Stream "
              "block (%u samples) — trailing bytes ignored", len - need, samples);
  }
  if (h[0] != 0) {
    /* We asked for iq_start:0 only; a server may push other receivers too
     * (Thetis AlwaysStreamIQ, a two-receiver SunSDR). RX1 samples through
     * the RX0 channelizer = two bands in one waterfall (SKM-7). */
    if (!c->warned_rx) {
      c->warned_rx = TRUE;
      g_message("tci: Stream blocks for receiver %u ignored (we stream receiver 0)", h[0]);
    }
    return;
  }
  if (h[6] != STREAM_TYPE_IQ || h[7] != 2 || samples < 2) { return; }
  if (h[2] != STREAM_FMT_FLOAT) {
    if (!c->warned_fmt) {
      c->warned_fmt = TRUE;
      g_warning("tci: IQ Stream format %u (want float32=3), skipping", h[2]);
    }
    return;
  }
  const guint nframes = samples / 2;
  if (c->blocks++ == 0) {
    /* The line a remote tester's log needs (gh#2 question 1). */
    g_message("tci: IQ stream up — receiver %u, %u Hz, float32, %u channels, "
              "%u frames/block%s", h[0], h[1], h[7], nframes,
              c->stamp_ok ? ", centre stamps on" : "");
  }
  if (!c->iq_cb) { return; }
  float *iq = (float *)(void *)(c->bin->data + STREAM_HDR_BYTES);
  /* The wire is already TRUE spectrum orientation — sdr-for-linux
   * conjugates its RF-inverted raw DDC feed on send (the ExpertSDR
   * convention SDC/CW Skimmer consume as-is). Do NOT conjugate here:
   * that mirrors every frequency around the DDC centre (live-caught
   * 2026-07-15, spots landed out of band). */
  g_mutex_lock(&c->lock);
  double center = c->center_hz;
  g_mutex_unlock(&c->lock);
  /* centre stamps (iq_stamp:1, see start()) — ONLY after the server echoed
   * the command; the spec calls h[8..15] "reserved", not "zero". The
   * block's own centre beats the label; a retune inside the block splits
   * it at the stamped frame so both halves carry the centre of their
   * samples. */
  if (c->stamp_ok) {
    const guint32 st_hz0 = h[8], st_off = h[9], st_hz1 = h[10];
    if (st_hz0 != 0) { center = (double)st_hz0; }
    if (st_hz0 != 0 && st_hz1 != 0 && st_off != 0 && st_off < nframes) {
      c->iq_cb(iq, st_off, (double)h[1], center, c->iq_cb_data);
      c->iq_cb(iq + 2 * st_off, nframes - st_off, (double)h[1], (double)st_hz1, c->iq_cb_data);
      return;
    }
  }
  c->iq_cb(iq, nframes, (double)h[1], center, c->iq_cb_data);
}

/* No IQ within a few seconds of iq_start:0 — the "connected, no output"
 * picture a remote tester sees (gh#2). One warning that names what the
 * server said, so the log carries the diagnosis. LWS/service thread. */
#define NO_IQ_WARN_US (3 * G_TIME_SPAN_SECOND)
static void check_no_iq(SkimTciClient *c) {
  if (c->warned_noiq || c->blocks) { return; }
  g_mutex_lock(&c->lock);
  const gint64 t0 = c->iq_start_us;
  const guint  rate = c->iq_rate, req = c->iq_req_rate;
  g_mutex_unlock(&c->lock);
  /* -1 ms slack: the sul fires at exactly +3 s of the write, a hair after t0 */
  if (!t0 || g_get_monotonic_time() - t0 < NO_IQ_WARN_US - 1000) { return; }
  c->warned_noiq = TRUE;
  g_warning("tci: no IQ block %d s after iq_start:0 — server announced "
            "iq_samplerate:%u (asked for %u); check the server's IQ / receiver "
            "settings (device bandwidth ≥ the IQ rate?)",
            (int)(NO_IQ_WARN_US / G_TIME_SPAN_SECOND), rate, req);
}

/* lws scheduler callback (service thread): the 3 s mark after iq_start:0. */
static void noiq_sul_cb(lws_sorted_usec_list_t *sul) {
  SkimTciClient *c = lws_container_of(sul, SkimTciClient, noiq_sul);
  check_no_iq(c);
}

/* ---- LWS plumbing ------------------------------------------------------------ */

static int client_cb(struct lws *wsi, enum lws_callback_reasons reason,
                     void *user, void *in, size_t len) {
  SkimTciClient *c = lws_context_user(lws_get_context(wsi));
  (void)user;

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    g_mutex_lock(&c->lock);
    c->wsi = wsi;
    c->up  = TRUE;
    g_cond_broadcast(&c->cond);
    g_mutex_unlock(&c->lock);
    return 0;

  case LWS_CALLBACK_CLIENT_RECEIVE:
    if (lws_frame_is_binary(wsi)) {
      /* lws hands a frame over in rx-buffer-sized pieces; FIN is reported
       * on every piece of the final frame, so the message is complete only
       * when the frame's payload is also fully in. */
      g_byte_array_append(c->bin, (const guint8 *)in, (guint)len);
      if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
        handle_block(c);
        g_byte_array_set_size(c->bin, 0);
      }
    } else {
      g_string_append_len(c->txt, (const char *)in, (gssize)len);
      drain_text(c);
    }
    return 0;

  case LWS_CALLBACK_CLIENT_WRITEABLE: {
    g_mutex_lock(&c->lock);
    char *msg = g_queue_pop_head(&c->out);
    gboolean more = !g_queue_is_empty(&c->out);
    g_mutex_unlock(&c->lock);
    if (msg) {
      size_t n = strlen(msg);
      unsigned char *buf = g_malloc(LWS_PRE + n);
      memcpy(buf + LWS_PRE, msg, n);
      lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
      if (g_str_has_prefix(msg, "iq_start:")) {
        /* the no-IQ watchdog's alarm — on the service thread, as lws wants */
        lws_sul_schedule(lws_get_context(wsi), 0, &c->noiq_sul, noiq_sul_cb,
                         (lws_usec_t)NO_IQ_WARN_US);
      }
      g_free(buf);
      g_free(msg);
    }
    if (more) { lws_callback_on_writable(wsi); }
    return 0;
  }

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    g_mutex_lock(&c->lock);
    c->failed = TRUE;
    g_cond_broadcast(&c->cond);
    g_mutex_unlock(&c->lock);
    return -1;

  case LWS_CALLBACK_CLIENT_CLOSED:
    g_mutex_lock(&c->lock);
    c->up  = FALSE;
    c->wsi = NULL;
    g_cond_broadcast(&c->cond);
    g_mutex_unlock(&c->lock);
    /* Unexpected loss (run still set = nobody called stop): tell the owner. */
    if (g_atomic_int_get(&c->run) && c->closed_cb) {
      c->closed_cb(c->closed_cb_data);
    }
    return 0;

  default:
    return 0;
  }
}

static const struct lws_protocols c_protocols[] = {
  { "tci", client_cb, 0, 8192, 0, NULL, 0 },
  { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static gpointer service_thread(gpointer data) {
  SkimTciClient *c = data;
  while (g_atomic_int_get(&c->run)) {
    g_mutex_lock(&c->lock);
    gboolean pending = c->up && !g_queue_is_empty(&c->out);
    struct lws *wsi = c->wsi;
    g_mutex_unlock(&c->lock);
    if (pending && wsi) { lws_callback_on_writable(wsi); }
    lws_service(c->ctx, 0);
    check_no_iq(c);
    g_usleep(1000);
  }
  return NULL;
}

/* ---- public API -------------------------------------------------------------- */

SkimTciClient *skim_tci_client_new(const char *host, guint16 port) {
  SkimTciClient *c = g_new0(SkimTciClient, 1);
  c->host = g_strdup(host ? host : "127.0.0.1");
  c->port = port ? port : 40001;
  g_mutex_init(&c->lock);
  g_cond_init(&c->cond);
  g_queue_init(&c->out);
  c->txt = g_string_new(NULL);
  c->bin = g_byte_array_new();
  return c;
}

void skim_tci_client_free(SkimTciClient *c) {
  if (!c)
    return;
  skim_tci_client_stop(c);
  char *msg;
  while ((msg = g_queue_pop_head(&c->out)) != NULL) { g_free(msg); }
  g_string_free(c->txt, TRUE);
  g_byte_array_free(c->bin, TRUE);
  g_mutex_clear(&c->lock);
  g_cond_clear(&c->cond);
  g_free(c->host);
  g_free(c);
}

void skim_tci_client_set_iq_cb(SkimTciClient *c, SkimTciIqCb cb, gpointer user_data) {
  c->iq_cb      = cb;
  c->iq_cb_data = user_data;
}

void skim_tci_client_set_tx_cb(SkimTciClient *c, SkimTciTxCb cb, gpointer user_data) {
  c->tx_cb      = cb;
  c->tx_cb_data = user_data;
}

void skim_tci_client_set_vfo_cb(SkimTciClient *c, SkimTciVfoCb cb, gpointer user_data) {
  c->vfo_cb      = cb;
  c->vfo_cb_data = user_data;
}

void skim_tci_client_set_closed_cb(SkimTciClient *c, SkimTciClosedCb cb,
                                   gpointer user_data) {
  c->closed_cb      = cb;
  c->closed_cb_data = user_data;
}

gboolean skim_tci_client_start(SkimTciClient *c, guint iq_samplerate, GError **error) {
  if (c->thread) {
    g_set_error(error, SKIM_TCI_ERROR, 1, "TCI client already started");
    return FALSE;
  }
  if (iq_samplerate != 0 && iq_samplerate != 48000 && iq_samplerate != 96000 &&
      iq_samplerate != 192000 && iq_samplerate != 384000) {
    g_set_error(error, SKIM_TCI_ERROR, 2,
                "invalid iq_samplerate %u (48/96/192/384 kHz)", iq_samplerate);
    return FALSE;
  }

  lws_set_log_level(LLL_ERR, NULL);
  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port      = CONTEXT_PORT_NO_LISTEN;
  info.protocols = c_protocols;
  info.gid       = (gid_t)-1;
  info.uid       = (uid_t)-1;
  info.user      = c;
  c->ctx = lws_create_context(&info);
  if (!c->ctx) {
    g_set_error(error, SKIM_TCI_ERROR, 3, "lws_create_context failed");
    return FALSE;
  }

  struct lws_client_connect_info ci;
  memset(&ci, 0, sizeof(ci));
  ci.context  = c->ctx;
  ci.address  = c->host;
  ci.port     = c->port;
  ci.path     = "/";
  ci.host     = c->host;
  ci.origin   = c->host;
  ci.protocol = "tci";
  lws_client_connect_via_info(&ci);

  g_atomic_int_set(&c->run, 1);
  c->thread = g_thread_new("skim-tci", service_thread, c);

  /* Wait for the whole init block — the server ends it with ready; (+start;). */
  gint64 deadline = g_get_monotonic_time() + HANDSHAKE_TIMEOUT_S * G_TIME_SPAN_SECOND;
  g_mutex_lock(&c->lock);
  while (!c->ready && !c->failed) {
    if (!g_cond_wait_until(&c->cond, &c->lock, deadline)) { break; }
  }
  gboolean ready = c->ready, failed = c->failed;
  g_mutex_unlock(&c->lock);

  if (!ready) {
    g_set_error(error, SKIM_TCI_ERROR, 4,
                failed ? "connection to ws://%s:%u refused"
                       : "handshake timeout against ws://%s:%u (no ready;)",
                c->host, c->port);
    skim_tci_client_stop(c);
    return FALSE;
  }

  /* iq_samplerate is device-global radio state — request ours (the SDC lesson:
   * say it explicitly or inherit whatever the device last used), then start. */
  /* iq_stamp:1 = sdr-for-linux's family extension: every IQ block carries
   * the DDC centre of its first frame (h[8]) and, when the radio retuned
   * inside the block, the frame offset (h[9]) + the new centre (h[10]).
   * Other servers ignore the command and leave the words zero — then the
   * dds label at block arrival is used, as before (±1 block of jitter). */
  /* One command per text frame (SKM-10): the spec does not say a frame may
   * carry several, and a server reading only the first would lose iq_start
   * — "connected, no IQ" with nothing in the log. iq_stamp goes last so its
   * echo precedes the first stamped block. */
  g_mutex_lock(&c->lock);
  c->iq_req_rate = iq_samplerate;
  c->iq_start_us = g_get_monotonic_time();
  g_mutex_unlock(&c->lock);
  if (iq_samplerate) {
    cli_queue(c, g_strdup_printf("iq_samplerate:%u;", iq_samplerate));
  }
  cli_queue(c, g_strdup("iq_start:0;"));
  cli_queue(c, g_strdup("iq_stamp:1;"));
  c->started = TRUE;
  return TRUE;
}

void skim_tci_client_stop(SkimTciClient *c) {
  if (!c->thread)
    return;
  if (c->started) {
    cli_queue(c, g_strdup("iq_stop:0;"));
    /* Give the service loop a moment to flush the polite goodbye. */
    for (int ms = 0; ms < STOP_FLUSH_MS; ms++) {
      g_mutex_lock(&c->lock);
      gboolean flushed = g_queue_is_empty(&c->out) || !c->up;
      g_mutex_unlock(&c->lock);
      if (flushed) { break; }
      g_usleep(1000);
    }
    c->started = FALSE;
  }
  g_atomic_int_set(&c->run, 0);
  lws_cancel_service(c->ctx);
  g_thread_join(c->thread);
  c->thread = NULL;
  lws_sul_cancel(&c->noiq_sul);      /* service thread gone — safe to unlink */
  lws_context_destroy(c->ctx);
  c->ctx = NULL;
  c->wsi = NULL;
  c->up = c->ready = c->failed = FALSE;
  c->iq_req_rate = 0;
  c->iq_start_us = 0;
  g_string_set_size(c->txt, 0);
  g_byte_array_set_size(c->bin, 0);
  c->stamp_ok = FALSE;
  c->blocks = 0;
  c->warned_fmt = c->warned_rx = c->warned_short = c->warned_pad =
      c->warned_bogus = c->warned_noiq = c->warned_rate = FALSE;
}

void skim_tci_client_spot(SkimTciClient *c, const char *call, const char *mode,
                          double freq_hz, guint32 argb, const char *text) {
  if (!c->thread || !call || !call[0])
    return;
  /* ':' ',' ';' are TCI reserved — scrub them out of free-text fields. */
  char *t = g_strdup(text ? text : "");
  for (char *p = t; *p; p++) {
    if (*p == ':' || *p == ',' || *p == ';') { *p = ' '; }
  }
  cli_queue(c, g_strdup_printf("spot:%s,%s,%lld,%u,%s;",
                               call, mode ? mode : "CW",
                               (long long)(freq_hz + 0.5), argb, t));
  g_free(t);
}

void skim_tci_client_spot_delete(SkimTciClient *c, const char *call) {
  if (!c->thread || !call || !call[0])
    return;
  cli_queue(c, g_strdup_printf("spot_delete:%s;", call));
}

void skim_tci_client_tune(SkimTciClient *c, double freq_hz) {
  if (!c->thread || freq_hz <= 0)
    return;
  cli_queue(c, g_strdup_printf("vfo:0,0,%lld;", (long long)(freq_hz + 0.5)));
}

void skim_tci_client_spot_clicked(SkimTciClient *c, const char *call,
                                  double freq_hz) {
  if (!c->thread || !call || !call[0] || freq_hz <= 0)
    return;
  cli_queue(c, g_strdup_printf("clicked_on_spot:%s,%lld;",
                               call, (long long)(freq_hz + 0.5)));
}

double skim_tci_client_center_hz(SkimTciClient *c) {
  g_mutex_lock(&c->lock);
  double hz = c->center_hz;
  g_mutex_unlock(&c->lock);
  return hz;
}

double skim_tci_client_vfo_hz(SkimTciClient *c) {
  g_mutex_lock(&c->lock);
  double hz = c->vfo_hz;
  g_mutex_unlock(&c->lock);
  return hz;
}

guint skim_tci_client_iq_rate(SkimTciClient *c) {
  g_mutex_lock(&c->lock);
  guint r = c->iq_rate;
  g_mutex_unlock(&c->lock);
  return r;
}

const char *skim_tci_client_device(SkimTciClient *c) {
  return c->device;    /* written once during the handshake, then stable */
}

const char *skim_tci_client_protocol(SkimTciClient *c) {
  return c->protocol;
}
