/* tci_client.h — TCI (ExpertSDR) WebSocket client.
 *
 * Connects to the sdr-for-linux TCI server (ws://host:40001), completes the
 * handshake, subscribes to the wideband IQ stream, and reassembles binary
 * Stream (type=0) blocks into interleaved float I/Q. Also sends spots back
 * (SPOT:call,mode,freq,...). Reference: piHPSDR tci.c (the client side).
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

/* Called with each reassembled IQ block (nframes interleaved I/Q pairs) in
 * true orientation as received. center_hz is the stream centre frequency.
 * ⚠ Fires on the client's LWS service thread, not the GLib main loop — consumers
 * queue/marshal themselves (the engine pipeline runs headless anyway). */
typedef void (*SkimTciIqCb)(const float *iq, guint nframes,
                            double sample_rate, double center_hz,
                            gpointer user_data);

/* Tuned frequency changed (vfo:0,0,<hz> — rx 0 channel A). LWS thread. */
typedef void (*SkimTciVfoCb)(double vfo_hz, gpointer user_data);

/* The server dropped the connection (WS closed while running). LWS thread.
 * Does NOT fire on skim_tci_client_stop(). */
typedef void (*SkimTciClosedCb)(gpointer user_data);

SkimTciClient *skim_tci_client_new(const char *host, guint16 port);
void           skim_tci_client_free(SkimTciClient *c);

void     skim_tci_client_set_iq_cb(SkimTciClient *c, SkimTciIqCb cb, gpointer user_data);
void     skim_tci_client_set_vfo_cb(SkimTciClient *c, SkimTciVfoCb cb, gpointer user_data);
void     skim_tci_client_set_closed_cb(SkimTciClient *c, SkimTciClosedCb cb, gpointer user_data);

/* Connect, handshake (wait for ready;), request iq_samplerate + iq_start:0.
 * iq_samplerate ∈ {48000, 96000, 192000, 384000}, or 0 = keep the rate the
 * server announced in its init block (it is device-global radio state).
 * Blocks up to a few seconds for the handshake. M1. */
gboolean skim_tci_client_start(SkimTciClient *c, guint iq_samplerate, GError **error);
void     skim_tci_client_stop(SkimTciClient *c);

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

G_END_DECLS

#endif /* SKIMMER_TCI_CLIENT_H */
