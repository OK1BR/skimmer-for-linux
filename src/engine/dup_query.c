/* dup_query — UDP client for the logbook's dup lookup (see dup_query.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dup_query.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define DUP_TTL_US      (60 * G_USEC_PER_SEC)   /* answered entries          */
#define DUP_RETRY_US    (2 * G_USEC_PER_SEC)    /* re-ask an unanswered call */

typedef struct {
  SkimDupVerdict verdict;
  gint64         fresh_until;   /* verdict valid until (0 = never answered) */
  gint64         asked_at;      /* last request sent                        */
} DupEntry;

struct _SkimDupQuery {
  int         fd;               /* connected non-blocking UDP, −1 on error  */
  GHashTable *cache;            /* call → DupEntry                          */
  GMutex      lock;
};

SkimDupQuery *skim_dup_query_new(void) {
  return skim_dup_query_new_full("127.0.0.1", 2238);
}

SkimDupQuery *skim_dup_query_new_full(const char *host, guint16 port) {
  SkimDupQuery *q = g_new0(SkimDupQuery, 1);
  g_mutex_init(&q->lock);
  q->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  q->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (q->fd >= 0) {
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
        connect(q->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
      close(q->fd);
      q->fd = -1;
    }
  }
  return q;
}

void skim_dup_query_free(SkimDupQuery *q) {
  if (!q)
    return;
  if (q->fd >= 0) { close(q->fd); }
  g_hash_table_destroy(q->cache);
  g_mutex_clear(&q->lock);
  g_free(q);
}

/* Fold every queued answer into the cache. An answer is `VERDICT CALL`
 * (trailing newline included — live-verified 2026-08-01); anything else is
 * dropped. ECONNREFUSED just means the logbook is closed — swallow it. */
static void dup_drain(SkimDupQuery *q) {
  char buf[128];
  for (;;) {
    const gssize n = recv(q->fd, buf, sizeof(buf) - 1, 0);
    if (n < 0) {
      if (errno == EINTR) { continue; }
      break;                    /* EAGAIN, ECONNREFUSED, … — nothing to read */
    }
    buf[n] = '\0';
    g_strstrip(buf);
    char **f = g_strsplit(buf, " ", 3);
    SkimDupVerdict v = SKIM_DUP_UNKNOWN;
    if (f[0]) {
      if (strcmp(f[0], "NEW") == 0)      { v = SKIM_DUP_NEW; }
      else if (strcmp(f[0], "B4") == 0)  { v = SKIM_DUP_B4; }
      else if (strcmp(f[0], "DUP") == 0) { v = SKIM_DUP_DUP; }
    }
    if (v != SKIM_DUP_UNKNOWN && f[1] && f[1][0] && !f[2]) {
      DupEntry *e = g_hash_table_lookup(q->cache, f[1]);
      if (!e) {
        e = g_new0(DupEntry, 1);
        g_hash_table_insert(q->cache, g_strdup(f[1]), e);
      }
      e->verdict     = v;
      e->fresh_until = g_get_monotonic_time() + DUP_TTL_US;
    }
    g_strfreev(f);
  }
}

SkimDupVerdict skim_dup_query_lookup(SkimDupQuery *q, const char *call,
                                     double freq_hz, const char *mode,
                                     guint wait_ms) {
  if (!q || q->fd < 0 || !call || !call[0])
    return SKIM_DUP_UNKNOWN;

  g_mutex_lock(&q->lock);
  dup_drain(q);

  const gint64 now = g_get_monotonic_time();
  DupEntry *e = g_hash_table_lookup(q->cache, call);
  if (e && e->fresh_until > now) {
    const SkimDupVerdict v = e->verdict;
    g_mutex_unlock(&q->lock);
    return v;
  }

  if (!e) {
    e = g_new0(DupEntry, 1);
    g_hash_table_insert(q->cache, g_strdup(call), e);
  }
  if (now - e->asked_at >= DUP_RETRY_US) {
    char req[96];
    g_snprintf(req, sizeof(req), "DUP? %s %lld %s",
               call, (long long)(freq_hz + 0.5), mode ? mode : "CW");
    if (send(q->fd, req, strlen(req), 0) < 0) { /* logbook closed — fine */ }
    e->asked_at = now;
  }

  SkimDupVerdict v = SKIM_DUP_UNKNOWN;
  if (wait_ms > 0) {
    /* Poll OUTSIDE the lock — the GTK thread's wait-0 lookups must never
     * queue behind the pipeline's answer wait. */
    g_mutex_unlock(&q->lock);
    struct pollfd pfd = { .fd = q->fd, .events = POLLIN };
    const int ready = poll(&pfd, 1, (int)wait_ms);
    g_mutex_lock(&q->lock);
    if (ready > 0) {
      dup_drain(q);
      e = g_hash_table_lookup(q->cache, call);
      if (e && e->fresh_until > now) { v = e->verdict; }
    }
  }
  g_mutex_unlock(&q->lock);
  return v;
}
