/* tci_client.h — TCI (ExpertSDR) WebSocket client.
 *
 * Connects to the sdr-for-linux TCI server (ws://host:40001), completes the
 * handshake, subscribes to the wideband IQ stream, and reassembles binary
 * Stream (type=0) blocks into interleaved float I/Q. Also sends spots back
 * (SPOT:call,mode,freq,...). Reference: piHPSDR tci.c (the client side).
 *
 * NOTE: the TCI IQ orientation is the complex CONJUGATE of the DDC feed — the
 * ingest path conjugates to recover the true spectrum (docs/SCOPE.md, M1).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_TCI_CLIENT_H
#define SKIMMER_TCI_CLIENT_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimTciClient SkimTciClient;

/* Called with each reassembled IQ block (nframes interleaved I/Q pairs), already
 * conjugated to true orientation. center_hz is the stream centre frequency. */
typedef void (*SkimTciIqCb)(const float *iq, guint nframes,
                            double sample_rate, double center_hz,
                            gpointer user_data);

SkimTciClient *skim_tci_client_new(const char *host, guint16 port);
void           skim_tci_client_free(SkimTciClient *c);

void     skim_tci_client_set_iq_cb(SkimTciClient *c, SkimTciIqCb cb, gpointer user_data);

/* Connect, handshake, request iq_samplerate + iq_start. M1. */
gboolean skim_tci_client_start(SkimTciClient *c, guint iq_samplerate, GError **error);
void     skim_tci_client_stop(SkimTciClient *c);

/* Send a spot back to the radio panadapter (SPOT:call,mode,freq_hz,argb,text). M5. */
void     skim_tci_client_spot(SkimTciClient *c, const char *call, const char *mode,
                             double freq_hz, guint32 argb, const char *text);

G_END_DECLS

#endif /* SKIMMER_TCI_CLIENT_H */
