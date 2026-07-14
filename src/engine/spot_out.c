/* spot_out.c — spot output. M0: skeleton (stores config, emits nothing).
 * TCI SPOT wiring lands in M5, the RBN telnet feed in M6 (docs/SCOPE.md).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "spot_out.h"

struct _SkimSpotOut {
  SkimTciClient *tci;      /* not owned */
  char          *rbn_host;
  guint16        rbn_port;
  /* M6: telnet socket to the RBN, dedup cache, rate limiter. */
};

SkimSpotOut *skim_spot_out_new(SkimTciClient *tci, const char *rbn_host, guint16 rbn_port) {
  SkimSpotOut *s = g_new0(SkimSpotOut, 1);
  s->tci      = tci;
  s->rbn_host = g_strdup(rbn_host); /* NULL-safe: g_strdup(NULL) == NULL */
  s->rbn_port = rbn_port;
  return s;
}

void skim_spot_out_free(SkimSpotOut *s) {
  if (!s)
    return;
  g_free(s->rbn_host);
  g_free(s);
}

void skim_spot_out_emit(SkimSpotOut *s, const char *call, const char *mode,
                        double freq_hz, double snr_db) {
  (void)s; (void)call; (void)mode; (void)freq_hz; (void)snr_db;
  /* M5: skim_tci_client_spot(...). M6: telnet spot line to the RBN. */
}
