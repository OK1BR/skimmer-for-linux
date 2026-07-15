/* tci_client.c — TCI WebSocket client (M1: transport, handshake, IQ ingest).
 *
 * libwebsockets client on its own service thread (the sdr-for-linux
 * tci_test.c/piHPSDR pattern): lws_service loop + lws_cancel_service wakeups,
 * outgoing text queued and flushed on WRITEABLE. Text frames are accumulated
 * and split on ';' (the server batches commands per frame); binary frames are
 * accumulated as a byte stream and drained one Stream block at a time — the
 * 64-byte header carries the payload length, so WS fragmentation is invisible.
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
  guint    iq_rate;           /* iq_samplerate announced/echoed              */
  char     device[64];
  char     protocol[64];

  /* LWS thread only — no lock needed. */
  GString    *txt;            /* text command accumulator                    */
  GByteArray *bin;            /* binary Stream byte accumulator              */
  gboolean    warned_fmt;

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
    if (r > 0) { c->iq_rate = (guint)r; }
  }
  g_mutex_unlock(&c->lock);

  if (vfo_cb) { vfo_cb(vfo_hz, vfo_user); }
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

static void drain_binary(SkimTciClient *c) {
  while (c->bin->len >= STREAM_HDR_BYTES) {
    guint32 h[16];
    memcpy(h, c->bin->data, sizeof(h));   /* GByteArray data may be unaligned */
    const guint32 samples = h[5];
    const gsize   need    = STREAM_HDR_BYTES + (gsize)samples * sizeof(float);
    if (samples == 0 || samples > (1u << 20)) {      /* desynced — resync hard */
      g_warning("tci: bogus Stream length %u, dropping buffer", samples);
      g_byte_array_set_size(c->bin, 0);
      return;
    }
    if (c->bin->len < need) { return; }

    if (h[6] == STREAM_TYPE_IQ && h[7] == 2 && samples >= 2) {
      if (h[2] != STREAM_FMT_FLOAT) {
        if (!c->warned_fmt) {
          c->warned_fmt = TRUE;
          g_warning("tci: IQ Stream format %u (want float32=3), skipping", h[2]);
        }
      } else {
        float *iq = (float *)(void *)(c->bin->data + STREAM_HDR_BYTES);
        const guint nframes = samples / 2;
        /* The wire is already TRUE spectrum orientation — sdr-for-linux
         * conjugates its RF-inverted raw DDC feed on send (the ExpertSDR
         * convention SDC/CW Skimmer consume as-is). Do NOT conjugate here:
         * that mirrors every frequency around the DDC centre (live-caught
         * 2026-07-15, spots landed out of band). */
        if (c->iq_cb) {
          g_mutex_lock(&c->lock);
          const double center = c->center_hz;
          g_mutex_unlock(&c->lock);
          c->iq_cb(iq, nframes, (double)h[1], center, c->iq_cb_data);
        }
      }
    }
    g_byte_array_remove_range(c->bin, 0, (guint)need);
  }
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
      g_byte_array_append(c->bin, (const guint8 *)in, (guint)len);
      drain_binary(c);
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
  if (iq_samplerate) {
    cli_queue(c, g_strdup_printf("iq_samplerate:%u;iq_start:0;", iq_samplerate));
  } else {
    cli_queue(c, g_strdup("iq_start:0;"));
  }
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
  lws_context_destroy(c->ctx);
  c->ctx = NULL;
  c->wsi = NULL;
  c->up = c->ready = c->failed = FALSE;
  g_string_set_size(c->txt, 0);
  g_byte_array_set_size(c->bin, 0);
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
