/* tci_client.c — TCI WebSocket client. M0: skeleton (no transport yet).
 * libwebsockets client, handshake, IQ ingest + conjugate land in M1
 * (docs/SCOPE.md). Reference: piHPSDR tci.c.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "tci_client.h"

struct _SkimTciClient {
  char        *host;
  guint16      port;
  SkimTciIqCb  iq_cb;
  gpointer     iq_cb_data;
  /* M1: struct lws_context, connection, Stream-frame reassembly buffer. */
};

SkimTciClient *skim_tci_client_new(const char *host, guint16 port) {
  SkimTciClient *c = g_new0(SkimTciClient, 1);
  c->host = g_strdup(host ? host : "127.0.0.1");
  c->port = port ? port : 40001;
  return c;
}

void skim_tci_client_free(SkimTciClient *c) {
  if (!c)
    return;
  g_free(c->host);
  g_free(c);
}

void skim_tci_client_set_iq_cb(SkimTciClient *c, SkimTciIqCb cb, gpointer user_data) {
  c->iq_cb      = cb;
  c->iq_cb_data = user_data;
}

gboolean skim_tci_client_start(SkimTciClient *c, guint iq_samplerate, GError **error) {
  (void)c; (void)iq_samplerate;
  g_set_error(error, g_quark_from_static_string("skim-tci-error"), 1,
              "TCI client not implemented yet (M1)");
  return FALSE;
}

void skim_tci_client_stop(SkimTciClient *c) {
  (void)c;
}

void skim_tci_client_spot(SkimTciClient *c, const char *call, const char *mode,
                          double freq_hz, guint32 argb, const char *text) {
  (void)c; (void)call; (void)mode; (void)freq_hz; (void)argb; (void)text;
  /* M5: format SPOT:... and send over the WebSocket text channel. */
}
