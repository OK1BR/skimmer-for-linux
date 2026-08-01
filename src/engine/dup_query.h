/* dup_query — ask the logbook whether a call is already worked.
 *
 * log-for-linux runs a read-only UDP lookup service on 127.0.0.1:2238 while
 * it is open (docs/SCOPE.md, 2026-08-01): request `DUP? <call> <freq_hz>
 * <mode>` in one datagram, answer `NEW <call>` / `B4 <call>` / `DUP <call>`
 * (malformed requests get silence). The verdict colours our spots and the
 * decode-pane highlight so the operator never clicks a duplicate.
 *
 * The logbook NOT running must never break spotting: every failure mode —
 * no listener, lost datagram, malformed answer — collapses to UNKNOWN, which
 * keeps the default colour. Answers are cached (60 s TTL), so the pane can
 * ask per highlight without spamming the socket.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SKIMMER_DUP_QUERY_H
#define SKIMMER_DUP_QUERY_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  SKIM_DUP_UNKNOWN = 0,   /* no/late/unparsable answer → default colour     */
  SKIM_DUP_NEW,           /* not in the log                                 */
  SKIM_DUP_B4,            /* worked before, some earlier time               */
  SKIM_DUP_DUP,           /* call+band+mode already in the ACTIVE contest   */
} SkimDupVerdict;

typedef struct _SkimDupQuery SkimDupQuery;

SkimDupQuery *skim_dup_query_new(void);              /* 127.0.0.1:2238      */
SkimDupQuery *skim_dup_query_new_full(const char *host, guint16 port);
void          skim_dup_query_free(SkimDupQuery *q);

/* The cached verdict for call. On a miss the request goes out and up to
 * wait_ms is spent listening for the answer — 0 fires and returns UNKNOWN
 * immediately; the answer lands in the cache for the next ask. Thread-safe;
 * call it with wait_ms 0 from anything latency-sensitive. */
SkimDupVerdict skim_dup_query_lookup(SkimDupQuery *q, const char *call,
                                     double freq_hz, const char *mode,
                                     guint wait_ms);

G_END_DECLS

#endif /* SKIMMER_DUP_QUERY_H */
