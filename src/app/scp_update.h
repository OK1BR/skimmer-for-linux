/*
 * scp_update — keeps MASTER.SCP current from supercheckpartial.com (gh#15).
 *
 * GLib + libcurl, no GTK (like wf_compose.c), so the whole thing runs in a
 * headless gate against a mock server. Two layers:
 *
 *   skim_scp_update_run()   one check, BLOCKING — worker thread or gate
 *   SkimScpUpdater          the app's handle: a timer + a worker thread;
 *                           the main loop never waits on the network
 *
 * What a check does, in the site's own terms (its Developers page): ask
 * /api/v1/files what exists, fetch only MASTER.SCP and only when it differs
 * from the copy on disk, conditionally (If-None-Match) where an ETag is
 * known, under a User-Agent naming this software — and never more often than
 * the limits below allow, whatever the app is restarted into.
 *
 * What it can NOT do: leave the machine without a dictionary. A download is
 * checked in memory (length, sha256 against the ETag, content that looks
 * like the call list it replaces) and only then renamed over the old file;
 * every failure — no network, a stalled server, an error page — ends in
 * SKIM_SCP_FAILED with the old file untouched.
 */
#pragma once

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* --- limits (persisted in the state file, so a restart does not reset them) */
#define SKIM_SCP_CHECK_SPACING_S    (24 * 3600) /* between completed checks   */
#define SKIM_SCP_ATTEMPT_SPACING_S  3600        /* retry after a failure      */
#define SKIM_SCP_ATTEMPTS_PER_DAY   4           /* anything that hits the net */
#define SKIM_SCP_DOWNLOADS_PER_WEEK 3           /* full file bodies; the site
                                                 * rebuilds twice a week      */
/* --- sanity bars for a downloaded list (measured 2026-09-19: 50 003 calls,
 * 99.86 % of them pass skim_callsign_is_valid, zero junk lines) ------------ */
#define SKIM_SCP_MIN_CALLS          10000       /* a shorter list is broken   */
#define SKIM_SCP_MIN_VALID          0.98        /* share of validating calls  */
#define SKIM_SCP_MAX_JUNK           0.01        /* share of non-call lines    */
#define SKIM_SCP_MIN_VS_PREVIOUS    0.50        /* calls vs the file replaced */
#define SKIM_SCP_MAX_BYTES          (8u << 20)  /* body cap, file and API     */

typedef enum {
  SKIM_SCP_UP_TO_DATE,   /* the server has what is on disk                   */
  SKIM_SCP_UPDATED,      /* a new file passed every check and is in place    */
  SKIM_SCP_NOT_DUE,      /* a limit says "not now" (`requests` tells whether
                          * the file list was asked before it said so)       */
  SKIM_SCP_FAILED,       /* network / server / sanity — old file untouched   */
  SKIM_SCP_CANCELLED,    /* the app went away mid-transfer                   */
} SkimScpResult;

typedef struct {
  SkimScpResult result;
  char   detail[200];    /* one line for the log and About                   */
  char   release[32];    /* UPDATED: the new file's "# Release", "" = none   */
  guint  calls;          /* UPDATED: calls in the new file                   */
  guint  requests;       /* HTTP requests this check sent (0, 1 or 2)        */
  gint64 wait_s;         /* until the limits allow the next attempt          */
} SkimScpOutcome;

typedef struct {
  const char   *dest_path;    /* …/skimmer-for-linux/master.scp              */
  const char   *state_path;   /* NULL = dest_path + ".state"                 */
  const char   *base_url;     /* NULL = the site (skim_scp_env_base_url)     */
  const char   *ca_path;      /* NULL = libcurl's own trust store            */
  const char   *user_agent;   /* NULL = "skimmer-for-linux/<version>"        */
  gboolean      load_dict;    /* UPDATED → skim_callsign_dict_load(dest)     */
  GCancellable *cancel;       /* NULL = not cancellable                      */
  guint         connect_timeout_s;  /* 0 = 10                                */
  guint         total_timeout_s;    /* 0 = 60, per request                   */
  gint64      (*now_s)(gpointer user);  /* NULL = wall clock; gate injects   */
  gpointer      now_user;
} SkimScpConfig;

/* The two things the environment decides — $SKIM_SCP_URL (a mock, a mirror)
 * and the CA bundle an AppImage needs looked up ($SSL_CERT_FILE, else the
 * usual distro paths when $APPIMAGE/$APPDIR is set). Read them on the MAIN
 * thread and hand the strings over: run() itself never calls getenv().
 * NULL = nothing to override. Caller frees. */
char *skim_scp_env_base_url(void);
char *skim_scp_env_ca_path(void);

/* One check, start to finish, blocking for at most two request timeouts.
 * Thread-safe against the engine and the UI (the dictionary swap is). */
void skim_scp_update_run(const SkimScpConfig *cfg, SkimScpOutcome *out);

/* Seconds until the persisted limits allow the next attempt (0 = now), and
 * the time of the last completed check (0 = never) — no network involved. */
gint64 skim_scp_update_wait_s(const SkimScpConfig *cfg);
gint64 skim_scp_update_last_check(const char *dest_path, const char *state_path);

/* --- the app's handle ------------------------------------------------------- */
typedef struct _SkimScpUpdater SkimScpUpdater;

/* Runs on the main context the updater was created on, after every check
 * that sent a request (NOT_DUE timer wake-ups stay silent). */
typedef void (*SkimScpUpdaterCb)(const SkimScpOutcome *out, gpointer user);

SkimScpUpdater *skim_scp_updater_new(const char *dest_path,
                                     SkimScpUpdaterCb cb, gpointer user);
/* Arm: first look `delay_s` from now, then whenever the limits allow. */
void skim_scp_updater_start(SkimScpUpdater *u, guint delay_s);
/* Disarm: timer gone, a transfer in flight is cancelled; no callback after
 * this returns. stop() never waits; free() gives a cancelled transfer at most
 * 0.4 s to leave libcurl (it takes milliseconds) — only when one is in flight. */
void skim_scp_updater_stop(SkimScpUpdater *u);
void skim_scp_updater_free(SkimScpUpdater *u);
gboolean skim_scp_updater_busy(const SkimScpUpdater *u);

G_END_DECLS
