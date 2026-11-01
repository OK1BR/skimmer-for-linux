/* tci_client.h — TCI (ExpertSDR) WebSocket client.
 *
 * Connects to the sdr-for-linux TCI server — over its Unix socket when the
 * radio runs on this machine, over TCP (ws://host:40001) otherwise —
 * completes the handshake, subscribes to the wideband IQ stream, and
 * reassembles binary Stream (type=0) blocks into interleaved float I/Q. Also
 * sends spots back (SPOT:call,mode,freq,...). Reference: piHPSDR tci.c (the
 * client side).
 *
 * NOTE: the wire already carries TRUE spectrum orientation — the server
 * conjugates its RF-inverted raw DDC feed on send (ExpertSDR convention).
 * Ingest is pass-through; conjugating here mirrors the band (M1, live-caught
 * 2026-07-15).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_TCI_CLIENT_H
#define SKIMMER_TCI_CLIENT_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimTciClient SkimTciClient;

/* ---- server address -----------------------------------------------------------
 *
 * One string names the server, in one of three spellings:
 *   unix:/path        — the sdr-for-linux Unix socket (absolute path)
 *   ws://host[:port]  — TCP, port 40001 when omitted
 *   host[:port]       — the same, the way the old Host field was typed
 * An empty string means the default order: the local socket at
 * $XDG_RUNTIME_DIR/sdr-for-linux/tci.sock first, then ws://127.0.0.1:40001 —
 * a fresh install finds sdr-for-linux whichever listener it has on, and a
 * radio on another machine just gets its TCP address (gh#39, the client half
 * of sdr-for-linux#46). */

#define SKIM_TCI_DEFAULT_PORT 40001
#define SKIM_TCI_DEFAULT_TCP  "ws://127.0.0.1:40001"
/* sockaddr_un.sun_path on Linux — a longer path cannot be dialled. */
#define SKIM_TCI_SOCK_PATH_MAX 108

typedef struct {
  gboolean unix_sock;                       /* TRUE: path; FALSE: host/port */
  char     path[SKIM_TCI_SOCK_PATH_MAX];
  char     host[256];
  guint16  port;
} SkimTciEndpoint;

/* Parse one spelling above into *out. FALSE (out untouched) for an empty
 * or malformed string — a relative socket path, wss://, a bad port. */
gboolean skim_tci_endpoint_parse(const char *s, SkimTciEndpoint *out);

/* Canonical spelling, "unix:/path" or "ws://host:port"; caller frees. */
char *skim_tci_endpoint_to_string(const SkimTciEndpoint *ep);

/* $XDG_RUNTIME_DIR/sdr-for-linux/tci.sock — where sdr-for-linux listens
 * unless its own preference moved the socket; caller frees. */
char *skim_tci_default_socket_path(void);

/* Called with each reassembled IQ block (nframes interleaved I/Q pairs) in
 * true orientation as received. center_hz is the stream centre frequency.
 * ⚠ Fires on the client's LWS service thread, not the GLib main loop — consumers
 * queue/marshal themselves (the engine pipeline runs headless anyway). */
typedef void (*SkimTciIqCb)(const float *iq, guint nframes,
                            double sample_rate, double center_hz,
                            gpointer user_data);

/* Tuned frequency changed (vfo:0,0,<hz> — rx 0 channel A). LWS thread. */
typedef void (*SkimTciVfoCb)(double vfo_hz, gpointer user_data);

/* The radio keyed/unkeyed (trx:0 OR tune:0 — sdr-for-linux ≥ cc470af reports
 * the REAL keyed state, CW/RTTY text keying included). tx = combined
 * trx||tune. Fires on change only. LWS thread. TX hold. */
typedef void (*SkimTciTxCb)(gboolean tx, gpointer user_data);

/* The server dropped the connection (WS closed while running). LWS thread.
 * Does NOT fire on skim_tci_client_stop(). */
typedef void (*SkimTciClosedCb)(gpointer user_data);

/* server: one address in a spelling above, or NULL/"" for the default
 * order. A malformed string is kept and reported by start(). */
SkimTciClient *skim_tci_client_new(const char *server);

/* An explicit try-order (NULL-terminated): start() takes the first server
 * that answers ready;. The default order comes through here; the gate uses
 * it to run the fallback against its own mock instead of the live port. */
SkimTciClient *skim_tci_client_new_order(const char *const *servers);

void           skim_tci_client_free(SkimTciClient *c);

void     skim_tci_client_set_iq_cb(SkimTciClient *c, SkimTciIqCb cb, gpointer user_data);
void     skim_tci_client_set_vfo_cb(SkimTciClient *c, SkimTciVfoCb cb, gpointer user_data);
void     skim_tci_client_set_tx_cb(SkimTciClient *c, SkimTciTxCb cb, gpointer user_data);
void     skim_tci_client_set_closed_cb(SkimTciClient *c, SkimTciClosedCb cb, gpointer user_data);

/* Connect, handshake (wait for ready;), request iq_samplerate + iq_start:0.
 * The candidates are dialled in order and the first one that answers ready;
 * wins — a socket nobody serves fails inside connect(), so the default
 * order costs a radio on TCP nothing. iq_samplerate ∈ {48000, 96000,
 * 192000, 384000}, or 0 = keep the rate the server announced in its init
 * block (it is device-global radio state). Blocks up to a few seconds per
 * candidate for the handshake. M1. */
gboolean skim_tci_client_start(SkimTciClient *c, guint iq_samplerate, GError **error);
void     skim_tci_client_stop(SkimTciClient *c);

/* The address the client is connected over, in the canonical spelling —
 * which of the candidates won. NULL when not started; owned by the client,
 * valid until the next start()/stop(). */
const char *skim_tci_client_endpoint(SkimTciClient *c);

/* Handshake/broadcast state (valid after start; dds retunes update live). */
double      skim_tci_client_center_hz(SkimTciClient *c);  /* dds:0,<hz>        */
double      skim_tci_client_vfo_hz(SkimTciClient *c);     /* vfo:0,0,<hz>      */
guint       skim_tci_client_iq_rate(SkimTciClient *c);    /* echoed/announced  */
const char *skim_tci_client_device(SkimTciClient *c);    /* device:<name>     */
const char *skim_tci_client_protocol(SkimTciClient *c);  /* protocol:<n>,<v>  */

/* Send a spot back to the radio panadapter (SPOT:call,mode,freq_hz,argb,text). M5. */
void     skim_tci_client_spot(SkimTciClient *c, const char *call, const char *mode,
                             double freq_hz, guint32 argb, const char *text);

/* Remove a spot label from the panadapter (SPOT_DELETE:call). */
void     skim_tci_client_spot_delete(SkimTciClient *c, const char *call);

/* Tune the radio (vfo:0,0,<hz>) — only ever on an explicit user action; the
 * skimmer never changes radio state on its own. */
void     skim_tci_client_tune(SkimTciClient *c, double freq_hz);

/* Announce a spot click (clicked_on_spot:call,<hz>) — only ever on an explicit
 * user action. The server relays it to its other clients (rx_clicked_on_spot +
 * legacy), so a logger prefills from OUR decoded-text click exactly like from
 * a panadapter spot click. hz must be the spot's exact frequency (> 0): the
 * logger's QSY-away staleness check keys on it. */
void     skim_tci_client_spot_clicked(SkimTciClient *c, const char *call,
                                      double freq_hz);

G_END_DECLS

#endif /* SKIMMER_TCI_CLIENT_H */
