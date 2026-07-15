/* rbn_feed.h — RBN telnet feed: a CW-Skimmer-compatible telnet server.
 *
 * The Reverse Beacon Network does not take spots from skimmers directly:
 * the operator runs the RBN Aggregator, which connects TO the skimmer's
 * telnet server (the CW Skimmer convention, default port 7300), logs in
 * and relays the stream to the network. This module is that server. Any
 * number of clients may connect; each validated spot is broadcast as a
 * classic DX-cluster line:
 *
 *   DX de OK1BR-#:   7032.0  DL1ABC       CW    25 dB  22 WPM  CQ      1234Z
 *
 * Transport only — WHAT may be spotted is the pipeline's RBN policy
 * (always CQ-only, a stricter callsign-score gate than the panadapter,
 * its own dedup and rate budget). The server runs its own GMainContext
 * thread; skim_rbn_feed_spot() may be called from any thread.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_RBN_FEED_H
#define SKIMMER_RBN_FEED_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimRbnFeed SkimRbnFeed;

/* Start the telnet server. mycall names the spotter ("MYCALL-#"). port 0
 * binds an ephemeral port (gates) — skim_rbn_feed_port() tells which. */
SkimRbnFeed *skim_rbn_feed_new(const char *mycall, guint16 port, GError **error);
void         skim_rbn_feed_free(SkimRbnFeed *f);

guint16 skim_rbn_feed_port(const SkimRbnFeed *f);

/* Broadcast one spot to every logged-in client. Thread-safe. */
void skim_rbn_feed_spot(SkimRbnFeed *f, const char *call, const char *mode,
                        double freq_hz, double snr_db, double speed);

/* Logged-in clients right now / spot lines actually written in total. */
guint   skim_rbn_feed_clients(const SkimRbnFeed *f);
guint64 skim_rbn_feed_lines(const SkimRbnFeed *f);

G_END_DECLS

#endif /* SKIMMER_RBN_FEED_H */
