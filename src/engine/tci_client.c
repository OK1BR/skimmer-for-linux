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
 * AlwaysStreamIQ pushes every receiver to every client) and header words
 * 8–11 are read only in the LAYOUT the server echoed to iq_stamp (SKM-8:
 * the spec promised nothing about those words; TCI 2.1 made word 8 the
 * stream's packet counter — gh#38, see start() and handle_block()).
 *
 * Wire orientation: the wire already carries the TRUE spectrum — the server
 * conjugates its RF-inverted raw DDC feed on send (the ExpertSDR convention
 * SDC/CW Skimmer consume as-is; sdr-for-linux docs/TCI-SCOPE.md F6d-2d).
 * Ingest is pass-through: a signal above the DDC centre lands at a positive
 * offset. Do NOT conjugate here — live-caught 2026-07-15 mirroring the band.
 *
 * Transports (gh#39, the client half of sdr-for-linux#46): the server is
 * named by one string — unix:/path, ws://host[:port] or host[:port] — or by
 * none, which means the default order: sdr-for-linux's Unix socket at
 * $XDG_RUNTIME_DIR/sdr-for-linux/tci.sock first, then ws://127.0.0.1:40001.
 * start() dials the candidates in turn and the first one that answers
 * ready; wins; a socket nobody serves fails inside connect(), so the
 * fallback costs a radio on TCP nothing. lws dials a Unix socket when the
 * connect ADDRESS starts with '+' (lib/core-net/client/connect2.c) — not the
 * URL path, whatever the #39 ticket text says; sdr-for-linux proved it on
 * this lws with sdrfl-tci-send -u.
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
  GPtrArray *order;           /* SkimTciEndpoint* candidates, tried in turn  */
  char      *bad;             /* the first malformed server string, if any   */
  char      *dial;            /* the address handed to lws for the live dial */
  char      *endpoint;        /* the winner's canonical spelling (lock)      */

  SkimTciIqCb iq_cb;
  gpointer    iq_cb_data;
  SkimTciVfoCb    vfo_cb;       /* fires on the LWS thread                     */
  gpointer        vfo_cb_data;
  SkimTciTxCb     tx_cb;        /* fires on the LWS thread (TX hold)           */
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
  int         stamp_layout;   /* iq_stamp echo (gh#38): -1 none, 0 counter at
                               * word 8 only, 1 legacy stamps at 8–10 and no
                               * counter, 2 counter at 8 + stamps at 9–11   */
  gboolean    count_have;     /* count_last holds the previous block's word 8 */
  guint32     count_last;
  guint       gap_pending;    /* frames lost before the next delivered block */
  guint64     lost_blocks;    /* counted by the server, never decoded (lock) */
  gint64      gap_log_us;     /* last counter-gap line (rate limit)          */
  guint       gaps_unlogged;  /* gaps since that line                        */
  guint       blocks;         /* IQ blocks delivered this session            */
  gboolean    warned_fmt, warned_rx, warned_short, warned_pad, warned_bogus,
              warned_noiq, warned_rate, warned_count;
  lws_sorted_usec_list_t noiq_sul; /* wakes the service loop 3 s after iq_start
                                    * left — lws_service() sleeps until an event,
                                    * so a polling check alone came ~5 s late */

  volatile gint run;          /* service loop keeps going while 1            */
  gboolean      started;      /* start() succeeded (iq_start sent)           */
};

/* ---- server address ----------------------------------------------------------- */

gboolean skim_tci_endpoint_parse(const char *s, SkimTciEndpoint *out) {
  g_return_val_if_fail(out != NULL, FALSE);
  if (!s) { return FALSE; }
  while (g_ascii_isspace(*s)) { s++; }
  char *t = g_strchomp(g_strdup(s));
  SkimTciEndpoint ep;
  memset(&ep, 0, sizeof(ep));
  gboolean ok = FALSE;

  if (g_str_has_prefix(t, "unix:")) {
    /* An absolute, dialable path. lws would also take "+@name" for an
     * abstract-namespace socket, but the radio offers no such thing. */
    const char *p = t + 5;
    if (p[0] == '/' && strlen(p) < SKIM_TCI_SOCK_PATH_MAX) {
      ep.unix_sock = TRUE;
      g_strlcpy(ep.path, p, sizeof(ep.path));
      ok = TRUE;
    }
  } else if (strstr(t, "://") == NULL || g_str_has_prefix(t, "ws://")) {
    /* ws://host[:port] or bare host[:port]; wss:// and friends fail here
     * (no TLS on a TCI link). A pasted URL may end in "/". No IPv6
     * literals — the radio's localhost-only listener is 127.0.0.1. */
    char *hp = g_strdup(g_str_has_prefix(t, "ws://") ? t + 5 : t);
    size_t n = strlen(hp);
    if (n && hp[n - 1] == '/') { hp[n - 1] = '\0'; }
    guint64 port = SKIM_TCI_DEFAULT_PORT;
    gboolean port_ok = TRUE;
    char *colon = strrchr(hp, ':');
    if (colon) {
      *colon = '\0';
      port_ok = colon[1] &&
                g_ascii_string_to_unsigned(colon + 1, 10, 1, 65535, &port, NULL);
    }
    if (port_ok && hp[0] && strpbrk(hp, "/:@ \t") == NULL &&
        strlen(hp) < sizeof(ep.host)) {
      g_strlcpy(ep.host, hp, sizeof(ep.host));
      ep.port = (guint16)port;
      ok = TRUE;
    }
    g_free(hp);
  }

  g_free(t);
  if (ok) { *out = ep; }
  return ok;
}

char *skim_tci_endpoint_to_string(const SkimTciEndpoint *ep) {
  g_return_val_if_fail(ep != NULL, NULL);
  return ep->unix_sock ? g_strdup_printf("unix:%s", ep->path)
                       : g_strdup_printf("ws://%s:%u", ep->host, ep->port);
}

char *skim_tci_default_socket_path(void) {
  return g_build_filename(g_get_user_runtime_dir(), "sdr-for-linux", "tci.sock",
                          NULL);
}

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
  gboolean     layout_log = FALSE;
  long         layout_v = 0;

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
    /* sdr-for-linux's echo of our family extension: the LAYOUT of header
     * words 8–11 the server SERVES (gh#38; sdr-for-linux#43) — only now do
     * those words mean anything (SKM-8). We ask for 2; a server before its
     * #43 answers any non-zero request with 1 (the legacy image this client
     * read before #38), one that knows 2 answers 2. Above 2 is a layout this
     * client does not know: read none of the words. Word 8 changes meaning
     * with the layout, so the packet counter re-anchors on the next block. */
    layout_v = strtol(args, NULL, 10);
    const int layout = (layout_v >= 0 && layout_v <= 2) ? (int)layout_v : -1;
    if (layout != c->stamp_layout) {
      c->stamp_layout = layout;
      c->count_have = FALSE;
      layout_log = TRUE;
    }
  } else if ((strcmp(cmd, "trx") == 0 || strcmp(cmd, "tune") == 0) && args) {
    /* trx:<rx>,<bool> / tune:<rx>,<bool> — rx 0. sdr-for-linux ≥ cc470af
     * reports the REAL keyed state (CW/RTTY text keying included); tune is
     * OR-ed in for servers where a tune carrier does not raise trx. The
     * combined value drives the pipeline's TX hold (TX hold). */
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

  if (layout_log) {
    /* The line a live log shows next to "IQ stream up". */
    static const char *const what[] = {
      "packet counter at word 8, no centre stamps",
      "legacy centre stamps at words 8–10, no packet counter "
      "(sdr-for-linux before its #43)",
      "packet counter at word 8, centre stamps at words 9–11 (TCI 2.1)",
    };
    if (layout_v >= 0 && layout_v <= 2) {
      g_message("tci: IQ header layout %ld — %s", layout_v, what[layout_v]);
    } else {
      g_message("tci: server echoed iq_stamp:%ld — a header layout this client "
                "does not know; words 8–15 ignored", layout_v);
    }
  }
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

/* Frames pending as the next block's gap — a long outage must not wrap. */
static guint sat_add(guint a, guint64 b) {
  const guint64 s = (guint64)a + b;
  return s > G_MAXUINT ? G_MAXUINT : (guint)s;
}

static const char *layout_note(int layout) {
  switch (layout) {
  case 0:  return ", packet counter on";
  case 1:  return ", centre stamps on (legacy layout 1)";
  case 2:  return ", centre stamps + packet counter on (layout 2)";
  default: return "";
  }
}

/* The packet counter skipped `lost` blocks before the block carrying `to`
 * (gh#38): their frames ride on the next delivered block as its gap, and
 * every gap is logged — at most one line per second, carrying the tally, so
 * a server that skips on every block cannot flood the log. LWS thread. */
#define GAP_LOG_EVERY_US (1 * G_TIME_SPAN_SECOND)
static void count_gap(SkimTciClient *c, guint32 from, guint32 to, guint32 lost,
                      guint nframes, guint32 rate) {
  c->gap_pending = sat_add(c->gap_pending, (guint64)lost * nframes);
  g_mutex_lock(&c->lock);
  c->lost_blocks += lost;
  const guint64 total = c->lost_blocks;
  g_mutex_unlock(&c->lock);
  c->gaps_unlogged++;
  const gint64 now = g_get_monotonic_time();
  if (c->gap_log_us && now - c->gap_log_us < GAP_LOG_EVERY_US) { return; }
  const double ms = rate ? 1000.0 * (double)lost * nframes / rate : 0.0;
  char more[48] = "";
  if (c->gaps_unlogged > 1) {
    g_snprintf(more, sizeof(more), ", %u gaps since the last line",
               c->gaps_unlogged);
  }
  g_message("tci: IQ packet counter jumped %u → %u — %u block%s ≈ %.0f ms of IQ "
            "lost%s (%" G_GUINT64_FORMAT " block%s lost this session)",
            from, to, lost, lost == 1 ? "" : "s", ms, more,
            total, total == 1 ? "" : "s");
  c->gaps_unlogged = 0;
  c->gap_log_us = now;
}

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
  const int   layout  = c->stamp_layout;    /* written on this thread too  */
  /* TCI 2.1 word 8 = the stream's packet counter (gh#38) — read only under
   * a layout the server echoed as carrying it (0 or 2): under 1 the word is
   * a frequency, and with no echo a pre-2.1 server leaves it zero (or
   * anything: the spec called it reserved). The first counted block anchors
   * the sequence — ExpertSDR's start value is unknown, so consecutiveness
   * is the test, not the value. A jump forward = blocks the server emitted
   * that never got here (the link itself, TCP or Unix, loses nothing — the
   * server skipped them); their frames ride on the next delivered block as
   * its gap. Standing still or running backwards = a server restart (or a
   * server that does not really count): re-anchor, say so once. */
  if (layout == 0 || layout == 2) {
    const guint32 cnt = h[8];
    if (c->count_have) {
      const guint32 d = cnt - c->count_last;              /* wrap-safe      */
      if (d > 1 && d < 0x80000000u) {
        count_gap(c, c->count_last, cnt, d - 1, nframes, h[1]);
      } else if (d != 1 && !c->warned_count) {
        c->warned_count = TRUE;
        g_message("tci: IQ packet counter went %u → %u — sequence re-anchored",
                  c->count_last, cnt);
      }
    }
    c->count_last = cnt;
    c->count_have = TRUE;
  }
  if (len < need) {
    /* The header arrived (and advanced the sequence above), the samples did
     * not — to the decoders this block is as lost as a counted-out one. */
    if (!c->warned_short) {
      c->warned_short = TRUE;
      g_warning("tci: Stream block truncated — header says %u samples (%" G_GSIZE_FORMAT
                " bytes), message carries %" G_GSIZE_FORMAT ", dropped", samples, need, len);
    }
    c->gap_pending = sat_add(c->gap_pending, nframes);
    g_mutex_lock(&c->lock);
    c->lost_blocks++;
    g_mutex_unlock(&c->lock);
    return;
  }
  if (len > need && !c->warned_pad) {
    /* Padding (the spec's fixed data[16384]?) or a server concatenating
     * blocks — either way the one line in the log says which. */
    c->warned_pad = TRUE;
    g_message("tci: binary message carries %" G_GSIZE_FORMAT " bytes past the Stream "
              "block (%u samples) — trailing bytes ignored", len - need, samples);
  }
  if (c->blocks++ == 0) {
    /* The line a remote tester's log needs (gh#2 question 1). */
    g_message("tci: IQ stream up — receiver %u, %u Hz, float32, %u channels, "
              "%u frames/block%s", h[0], h[1], h[7], nframes, layout_note(layout));
  }
  if (!c->iq_cb) {
    c->gap_pending = 0;
    return;
  }
  float *iq = (float *)(void *)(c->bin->data + STREAM_HDR_BYTES);
  /* The wire is already TRUE spectrum orientation — sdr-for-linux
   * conjugates its RF-inverted raw DDC feed on send (the ExpertSDR
   * convention SDC/CW Skimmer consume as-is). Do NOT conjugate here:
   * that mirrors every frequency around the DDC centre (live-caught
   * 2026-07-15, spots landed out of band). */
  g_mutex_lock(&c->lock);
  double center = c->center_hz;
  g_mutex_unlock(&c->lock);
  const guint gap = c->gap_pending;         /* frames missing before THIS block */
  c->gap_pending = 0;
  /* centre stamps (iq_stamp, see start()) — in the layout the server
   * echoed: 1 = words 8–10 (legacy), 2 = words 9–11 (TCI 2.1), none
   * otherwise (the spec calls h[8..15] "reserved", not "zero"). The block's
   * own centre beats the label; a retune inside the block splits it at the
   * stamped frame so both halves carry the centre of their samples. */
  guint32 st_hz0 = 0, st_off = 0, st_hz1 = 0;
  if (layout == 1)      { st_hz0 = h[8]; st_off = h[9];  st_hz1 = h[10]; }
  else if (layout == 2) { st_hz0 = h[9]; st_off = h[10]; st_hz1 = h[11]; }
  if (st_hz0 != 0) { center = (double)st_hz0; }
  if (st_hz0 != 0 && st_hz1 != 0 && st_off != 0 && st_off < nframes) {
    c->iq_cb(iq, st_off, (double)h[1], center, gap, c->iq_cb_data);
    c->iq_cb(iq + 2 * st_off, nframes - st_off, (double)h[1], (double)st_hz1,
             0, c->iq_cb_data);
    return;
  }
  c->iq_cb(iq, nframes, (double)h[1], center, gap, c->iq_cb_data);
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

SkimTciClient *skim_tci_client_new_order(const char *const *servers) {
  SkimTciClient *c = g_new0(SkimTciClient, 1);
  c->order = g_ptr_array_new_with_free_func(g_free);
  for (; servers && *servers; servers++) {
    SkimTciEndpoint ep;
    if (skim_tci_endpoint_parse(*servers, &ep)) {
      g_ptr_array_add(c->order, g_memdup2(&ep, sizeof(ep)));
    } else if (!c->bad) {
      c->bad = g_strdup(*servers);
    }
  }
  g_mutex_init(&c->lock);
  g_cond_init(&c->cond);
  g_queue_init(&c->out);
  c->txt = g_string_new(NULL);
  c->bin = g_byte_array_new();
  c->stamp_layout = -1;            /* no echo yet — words 8–15 not read */
  return c;
}

SkimTciClient *skim_tci_client_new(const char *server) {
  while (server && g_ascii_isspace(*server)) { server++; }
  if (server && *server) {
    const char *one[] = { server, NULL };
    return skim_tci_client_new_order(one);
  }
  /* The default order: the local radio's socket, then its TCP port — a
   * fresh install finds sdr-for-linux whichever listener it has on. The
   * socket path is sdr-for-linux's own default; a socket moved by its
   * preference is typed in here as unix:/path. */
  char *sock = skim_tci_default_socket_path();
  char *unix_addr = g_strdup_printf("unix:%s", sock);
  const char *def[] = { unix_addr, SKIM_TCI_DEFAULT_TCP, NULL };
  SkimTciClient *c = skim_tci_client_new_order(def);
  g_free(unix_addr);
  g_free(sock);
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
  g_ptr_array_free(c->order, TRUE);
  g_free(c->bad);
  g_free(c->dial);
  g_free(c->endpoint);
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

/* Tear one dial down: stop the service thread, drop the context, forget
 * every per-session fact. Shared by a failed candidate (so the next one
 * starts clean) and stop(). */
static void teardown(SkimTciClient *c) {
  g_atomic_int_set(&c->run, 0);
  if (c->ctx) { lws_cancel_service(c->ctx); }
  if (c->thread) {
    g_thread_join(c->thread);
    c->thread = NULL;
  }
  lws_sul_cancel(&c->noiq_sul);      /* service thread gone — safe to unlink */
  if (c->ctx) {
    lws_context_destroy(c->ctx);
    c->ctx = NULL;
  }
  c->wsi = NULL;
  c->up = c->ready = c->failed = FALSE;
  c->iq_req_rate = 0;
  c->iq_start_us = 0;
  g_string_set_size(c->txt, 0);
  g_byte_array_set_size(c->bin, 0);
  c->stamp_layout = -1;
  c->count_have = FALSE;
  c->count_last = 0;
  c->gap_pending = 0;
  c->lost_blocks = 0;
  c->gap_log_us = 0;
  c->gaps_unlogged = 0;
  c->blocks = 0;
  c->warned_fmt = c->warned_rx = c->warned_short = c->warned_pad =
      c->warned_bogus = c->warned_noiq = c->warned_rate = c->warned_count = FALSE;
  g_clear_pointer(&c->dial, g_free);
}

/* One candidate: context, dial, service thread, then wait for ready; or a
 * failure. On failure the attempt is torn down and *why names the reason
 * for start()'s error. */
static gboolean try_endpoint(SkimTciClient *c, const SkimTciEndpoint *ep,
                             const char **why) {
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
    *why = "lws_create_context failed";
    return FALSE;
  }

  c->ready = c->failed = c->up = FALSE;
  c->center_hz = c->vfo_hz = 0;
  c->trx = c->tune = FALSE;
  c->iq_rate = 0;
  c->device[0] = c->protocol[0] = '\0';
  g_string_set_size(c->txt, 0);

  struct lws_client_connect_info ci;
  memset(&ci, 0, sizeof(ci));
  ci.context  = c->ctx;
  ci.path     = "/";
  ci.protocol = "tci";
  g_free(c->dial);
  if (ep->unix_sock) {
    /* lws dials a Unix socket when the ADDRESS starts with '+' (its
     * lib/core-net/client/connect2.c) — not the URL path, whatever the
     * comment on lws_client_connect_info.path and the #39 ticket text
     * say; sdr-for-linux proved it on this lws with sdrfl-tci-send -u.
     * The port is ignored and the path stays "/", but the Host: header
     * still needs a name. The client owns the address string for the
     * life of the dial rather than trusting lws to copy it. */
    c->dial  = g_strdup_printf("+%s", ep->path);
    ci.address = c->dial;
    ci.port    = 0;
    ci.host = ci.origin = "localhost";
  } else {
    c->dial = g_strdup(ep->host);
    ci.address = ci.host = ci.origin = c->dial;
    ci.port    = ep->port;
  }
  /* A NULL back from the dial is a failure lws need not also report
   * through the callback (a missing socket file fails inside connect()). */
  if (!lws_client_connect_via_info(&ci)) {
    g_mutex_lock(&c->lock);
    c->failed = TRUE;
    g_mutex_unlock(&c->lock);
  }

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

  if (ready) { return TRUE; }
  *why = failed ? "refused" : "no ready; within " G_STRINGIFY(HANDSHAKE_TIMEOUT_S) " s";
  teardown(c);
  return FALSE;
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
  if (c->order->len == 0) {
    g_set_error(error, SKIM_TCI_ERROR, 5, "malformed TCI server address \"%s\"",
                c->bad ? c->bad : "");
    return FALSE;
  }

  /* The first candidate to answer ready; wins; the rest are never dialled.
   * A candidate that is not there fails inside connect(), so the default
   * order costs nothing when the radio only has TCP on. */
  GString *tried = g_string_new(NULL);
  char *won = NULL;
  for (guint i = 0; i < c->order->len && !won; i++) {
    const SkimTciEndpoint *ep = g_ptr_array_index(c->order, i);
    const char *why = NULL;
    char *name = skim_tci_endpoint_to_string(ep);
    if (try_endpoint(c, ep, &why)) {
      won = name;
      break;
    }
    g_string_append_printf(tried, "%s%s (%s)", tried->len ? ", " : "", name, why);
    g_free(name);
  }
  if (!won) {
    g_set_error(error, SKIM_TCI_ERROR, 4, "no TCI server answered: %s", tried->str);
    g_string_free(tried, TRUE);
    return FALSE;
  }
  g_string_free(tried, TRUE);
  g_mutex_lock(&c->lock);
  g_free(c->endpoint);
  c->endpoint = won;
  g_mutex_unlock(&c->lock);
  /* The line a remote tester's log needs: which transport carries the link. */
  g_message("tci: connected over %s", won);

  /* iq_samplerate is device-global radio state — request ours (the SDC lesson:
   * say it explicitly or inherit whatever the device last used), then start. */
  /* iq_stamp:2 = sdr-for-linux's family extension, layout 2 (gh#38): every
   * IQ block carries the TCI 2.1 packet counter in word 8, the DDC centre
   * of its first frame in word 9 and, when the radio retuned inside the
   * block, the frame offset (10) + the new centre (11). The server echoes
   * the layout it SERVES — a server before sdr-for-linux#43 answers 1 and
   * keeps the stamps at 8–10 with no counter; handle_block reads whatever
   * the echo promised. Other servers ignore the command and leave the words
   * zero — then the dds label at block arrival is used, as before (±1
   * block of jitter), and no counter is read. */
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
  cli_queue(c, g_strdup("iq_stamp:2;"));
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
  teardown(c);
  g_mutex_lock(&c->lock);
  g_clear_pointer(&c->endpoint, g_free);
  g_mutex_unlock(&c->lock);
}

const char *skim_tci_client_endpoint(SkimTciClient *c) {
  if (!c) { return NULL; }
  g_mutex_lock(&c->lock);
  const char *e = c->endpoint;
  g_mutex_unlock(&c->lock);
  return e;
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

int skim_tci_client_stamp_layout(SkimTciClient *c) {
  g_mutex_lock(&c->lock);
  const int l = c->stamp_layout;
  g_mutex_unlock(&c->lock);
  return l;
}

guint64 skim_tci_client_lost_blocks(SkimTciClient *c) {
  g_mutex_lock(&c->lock);
  const guint64 n = c->lost_blocks;
  g_mutex_unlock(&c->lock);
  return n;
}
