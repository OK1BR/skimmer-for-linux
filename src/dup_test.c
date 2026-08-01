/* Offline gate for dup_query — the logbook dup lookup client.
 *
 * Runs a mock log-for-linux UDP responder in-process (the live service was
 * probed 2026-08-01: answers carry a trailing newline, malformed requests
 * get silence) and drives skim_dup_query_lookup through every verdict and
 * failure mode. NO logbook, NO radio.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "engine/dup_query.h"
#include "engine/spot_out.h"

static int      s_fd = -1;
static guint16  s_port;
static GMutex   s_lock;
static char     s_last_req[128];     /* last datagram the mock received      */
static int      s_requests;          /* total datagrams received             */
static volatile gint s_run = 1;

/* Mock log-for-linux: DUP? <call> ... → per-call canned verdict, trailing
 * "\n" like the real thing; "SILENT1X" and malformed requests get nothing. */
static gpointer mock_serve(gpointer user) {
  (void)user;
  while (g_atomic_int_get(&s_run)) {
    struct pollfd pfd = { .fd = s_fd, .events = POLLIN };
    if (poll(&pfd, 1, 50) <= 0) { continue; }
    char buf[128];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    const gssize n = recvfrom(s_fd, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&from, &flen);
    if (n <= 0) { continue; }
    buf[n] = '\0';
    g_mutex_lock(&s_lock);
    g_strlcpy(s_last_req, buf, sizeof(s_last_req));
    s_requests++;
    g_mutex_unlock(&s_lock);
    char call[32] = "";
    if (sscanf(buf, "DUP? %31s", call) != 1 || !call[0]) { continue; }
    if (strcmp(call, "SILENT1X") == 0) { continue; }
    char reply[64];
    if (strcmp(call, "9A1AA") == 0) {
      g_snprintf(reply, sizeof(reply), "DUP %s\n", call);
    } else if (strcmp(call, "OK1BR") == 0) {
      g_snprintf(reply, sizeof(reply), "B4 %s\n", call);
    } else {
      g_snprintf(reply, sizeof(reply), "NEW %s\n", call);
    }
    sendto(s_fd, reply, strlen(reply), 0, (struct sockaddr *)&from, flen);
  }
  return NULL;
}

static int s_checks, s_failures;
static void check(const char *what, int ok) {
  s_checks++;
  if (!ok) { s_failures++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

int main(void) {
  printf("=== dup_query gate (offline, mock logbook) ===\n");
  g_mutex_init(&s_lock);

  s_fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in sa = { 0 };
  sa.sin_family      = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t slen = sizeof(sa);
  g_assert(bind(s_fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
  g_assert(getsockname(s_fd, (struct sockaddr *)&sa, &slen) == 0);
  s_port = ntohs(sa.sin_port);
  GThread *server = g_thread_new("mock-log", mock_serve, NULL);

  SkimDupQuery *q = skim_dup_query_new_full("127.0.0.1", s_port);

  /* Round-trips — every verdict, newline-terminated answers parsed. */
  check("NEW round-trip",
        skim_dup_query_lookup(q, "DL1ABC", 14025000, "CW", 500) == SKIM_DUP_NEW);
  check("DUP round-trip",
        skim_dup_query_lookup(q, "9A1AA", 14025000, "CW", 500) == SKIM_DUP_DUP);
  check("B4 round-trip",
        skim_dup_query_lookup(q, "OK1BR", 14025000, "CW", 500) == SKIM_DUP_B4);
  g_mutex_lock(&s_lock);
  check("request format: DUP? <call> <hz> <mode>",
        strcmp(s_last_req, "DUP? OK1BR 14025000 CW") == 0);
  g_mutex_unlock(&s_lock);

  /* Silence (the malformed/unknown-request contract) → UNKNOWN. */
  check("no answer → UNKNOWN",
        skim_dup_query_lookup(q, "SILENT1X", 7002000, "CW", 120) ==
            SKIM_DUP_UNKNOWN);

  /* An unanswered call must not be re-asked on every lookup. */
  g_mutex_lock(&s_lock);
  const int asked_before = s_requests;
  g_mutex_unlock(&s_lock);
  skim_dup_query_lookup(q, "SILENT1X", 7002000, "CW", 0);
  skim_dup_query_lookup(q, "SILENT1X", 7002000, "CW", 0);
  g_usleep(50 * 1000);
  g_mutex_lock(&s_lock);
  check("unanswered call re-asked at most every 2 s",
        s_requests == asked_before);
  g_mutex_unlock(&s_lock);

  /* Cache: with the mock gone, fresh verdicts still answer from the TTL. */
  g_atomic_int_set(&s_run, 0);
  g_thread_join(server);
  close(s_fd);
  check("cached verdict survives the logbook closing",
        skim_dup_query_lookup(q, "9A1AA", 14025000, "CW", 50) == SKIM_DUP_DUP);
  check("uncached call with no listener → UNKNOWN",
        skim_dup_query_lookup(q, "F5XYZ", 14025000, "CW", 20) ==
            SKIM_DUP_UNKNOWN);
  skim_dup_query_free(q);

  /* Verdict → colour mapping (shared by TCI spots and the pane tags). */
  check("colour: NEW/UNKNOWN bright",
        skim_spot_argb_for_dup(SKIM_DUP_NEW) == SKIM_SPOT_ARGB &&
            skim_spot_argb_for_dup(SKIM_DUP_UNKNOWN) == SKIM_SPOT_ARGB);
  check("colour: DUP/B4 dimmed",
        skim_spot_argb_for_dup(SKIM_DUP_DUP) == SKIM_SPOT_ARGB_DUP &&
            skim_spot_argb_for_dup(SKIM_DUP_B4) == SKIM_SPOT_ARGB_DUP);

  printf("=== %d checks, %d failures ===\n", s_checks, s_failures);
  if (s_failures == 0) {
    printf("PASS — dup verdicts, caching and the colour rule all behave.\n");
  }
  return s_failures ? 1 : 0;
}
