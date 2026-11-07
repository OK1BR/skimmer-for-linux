/*
 * skimmer-replay — offline decode of a recorded IQ sample (M3 A/B harness).
 *
 *   skimmer-replay <file.cf32> [rate_hz] [center_hz]
 *   skimmer-replay <file.tap>
 *
 * Feeds a cf32 recording (float32 interleaved I/Q, true orientation — what
 * skimmer-tci-probe dumps) through the full engine pipeline offline:
 * channelizer → CW decoders → callsign extractors → station tracker. Rate and
 * centre default from the <file>.meta sidecar the probe writes. Decodes land
 * in <file>.decodes.log stamped with STREAM time, so two runs of different
 * decoder versions over the same sample diff line by line; the run ends with
 * a station table and summary counters for quick before/after comparison.
 *
 * SKIM_REPLAY_HOLDS="t0-t1,t0-t1,…" (stream seconds) replays the operator's
 * own transmissions: the TX flag is raised over each interval exactly as the
 * TCI trx broadcast would, so the pipeline's TX hold (and its grace) runs
 * as it did live. Take the intervals from the "TX hold" lines of the live
 * log of the same session. SKIM_REPLAY_MUTE (same format) zeroes the IQ
 * over its intervals, to the sample — what sdr-for-linux puts on the wire
 * from key-down on (gh#17), so a SYNTHETIC over can be laid anywhere into a
 * recording: mute from the key, raise the flag a poll later.
 * SKIM_REPLAY_FROM / SKIM_REPLAY_TO (stream seconds) replay a slice only;
 * hold and mute times stay absolute.
 *
 * gh#28 bench — a recording by skimmer-iq-record carries a <file>.events
 * sidecar: the DDC centre per frame (retunes), the holes the server left
 * (packet counter) and the radio's TX edges. The replay applies them all,
 * so it sees what the live pipeline saw; SKIM_REPLAY_HOLDS, when given,
 * replaces the sidecar's TX edges. Every record the RBN feed would have
 * broadcast (the feed's own policy: CQ only, score bar, ham bands, dedup,
 * rate budget) lands in <file>.rbn.log, one per line:
 *   <UTC>  <stream s>  <call>  <kHz>  <mode>  <dB>  <wpm>
 * UTC = the .meta start_unix_us + stream time (holes included); "-" for a
 * recording without it. SKIM_REPLAY_OUT=<prefix> writes <prefix>.decodes.log,
 * <prefix>.rbn.log and <prefix>.tap instead — two runs over one recording
 * side by side. SKIM_REPLAY_RBN_MIN overrides the feed's score bar (0.85).
 *
 * The decode tap (<prefix>.tap, decode_tap.h) is every event the stages
 * behind the decoders consumed: each decode at the extractor seam, the
 * extractor resets, the prune ticks. Given a .tap instead of IQ, the replay
 * runs ONLY those stages — extractor, station table, RBN policy — over the
 * recorded decodes and clock, in seconds for an evening, and writes the
 * same <out>.rbn.log (default <file.tap>.rbn.log). It reproduces the IQ
 * replay's RBN tap byte for byte (the rbn-feed gate holds it to that), so a
 * change behind the seam is measured on identical decodes, free of the
 * decoder's run-to-run noise. Decoder, arbitration and frequency-lock
 * changes sit before the seam and need the IQ replay.
 *
 * The same MASTER.SCP the app uses (~/.config/skimmer-for-linux/master.scp)
 * is loaded when present — keep it that way for honest A/B against the app.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/wf_compose.h"                     /* floor-tracker constants (dump) */
#include "engine/callsign.h"
#include "engine/pipeline.h"

#define BLK 2048                       /* frames per feed — the TCI block size */

static guint64 g_fragments;
static GHashTable *g_stations;         /* call → SkimStation* (last state)     */

static void text_cb(double freq_hz, const char *text, gpointer user) {
  (void)freq_hz; (void)text; (void)user;
  g_fragments++;
}

static void station_cb(const SkimStation *st, gpointer user) {
  (void)user;
  SkimStation *copy = g_hash_table_lookup(g_stations, st->call);
  if (!copy) {
    copy = g_new0(SkimStation, 1);
    g_hash_table_insert(g_stations, g_strdup(st->call), copy);
  }
  *copy = *st;
}

static void gone_cb(const SkimStation *st, gpointer user) {
  (void)user;
  g_hash_table_remove(g_stations, st->call);
}

/* Pull "key: value" doubles out of the probe's .meta sidecar. */
static double meta_get(const char *path, const char *key) {
  char *body = NULL;
  double v = 0;
  if (g_file_get_contents(path, &body, NULL, NULL)) {
    char *hit = strstr(body, key);
    if (hit) { v = g_ascii_strtod(hit + strlen(key), NULL); }
    g_free(body);
  }
  return v;
}

static void spectrum_cb(const guint8 *row, guint nbins, double center_hz,
                        double bin_hz, gpointer user) {
  (void)user;
  static guint rows;
  static double t_per_row;
  /* The view's floor tracker replayed on the rows (wf_compose.h constants):
   * what the colour map would hang off, and how far a run of rows drags it. */
  static double trk;
  static guint  trk_min = 255;
  if (!t_per_row) { t_per_row = 1.0 / (bin_hz * nbins) * (nbins / 4.0); }
  guint pk = 0;
  guint hist[256] = { 0 };
  for (guint i = 0; i < nbins; i++) {
    if (row[i] > row[pk]) { pk = i; }
    hist[row[i]]++;
  }
  guint cum = 0, fl = 0;
  const guint target = nbins * SKIM_WF_FLOOR_PCT / 100;
  for (guint b = 0; b < 256; b++) { cum += hist[b]; if (cum >= target) { fl = b; break; } }
  trk = rows ? trk + SKIM_WF_FLOOR_SMOOTH * ((double)fl - trk) : (double)fl;
  trk_min = MIN(trk_min, fl);
  rows++;
  const guint per_sec = (guint)(1.0 / t_per_row + 0.5);
  if (rows % MAX(per_sec, 1u)) { return; }
  fprintf(stderr, "spectrum: row %6u peak %.1f kHz (byte %u, floor %u, +%.1f dB)"
          " tracked floor %.1f dBFS, lowest row floor %.0f dBFS\n",
          rows, (center_hz + ((double)pk - nbins / 2.0) * bin_hz) / 1000.0,
          row[pk], fl, (double)row[pk] - fl,
          trk - SKIM_WF_DB_OFFSET, (double)trk_min - SKIM_WF_DB_OFFSET);
  trk_min = 255;
}

/* SKIM_REPLAY_HOLDS → [t0, t1) pairs in stream seconds. */
static GArray *holds_parse(const char *spec) {
  GArray *a = g_array_new(FALSE, FALSE, sizeof(double));
  if (!spec || !spec[0]) { return a; }
  char **parts = g_strsplit(spec, ",", -1);
  for (char **q = parts; *q; q++) {
    char *dash = strchr(*q, '-');
    if (!dash) { continue; }
    const double t0 = g_ascii_strtod(*q, NULL);
    const double t1 = g_ascii_strtod(dash + 1, NULL);
    if (t1 > t0) { g_array_append_val(a, t0); g_array_append_val(a, t1); }
  }
  g_strfreev(parts);
  return a;
}

static gboolean holds_tx_at(const GArray *a, double t) {
  for (guint i = 0; i + 1 < a->len; i += 2) {
    if (t >= g_array_index(a, double, i) && t < g_array_index(a, double, i + 1)) {
      return TRUE;
    }
  }
  return FALSE;
}

/* ---- gh#28: the .events sidecar (skimmer-iq-record) ------------------------- */

typedef enum { EV_CENTRE, EV_GAP, EV_TX, EV_CLOSED } EvKind;
typedef struct {
  guint64 frame;                       /* applies from this file frame on    */
  EvKind  kind;
  double  v;
} Ev;

/* "<frame> centre <hz>" / "<frame> gap <n>" / "<frame> tx 0|1" /
 * "<frame> closed" — in file order, which is frame order. */
static GArray *events_load(const char *path) {
  GArray *a = g_array_new(FALSE, FALSE, sizeof(Ev));
  char *body = NULL;
  if (!g_file_get_contents(path, &body, NULL, NULL)) { return a; }
  char **lines = g_strsplit(body, "\n", -1);
  for (char **l = lines; *l; l++) {
    char **w = g_strsplit_set(g_strstrip(*l), " \t", -1);
    if (g_strv_length(w) >= 2) {
      Ev e = { .frame = g_ascii_strtoull(w[0], NULL, 10) };
      const double v = w[2] ? g_ascii_strtod(w[2], NULL) : 0;
      gboolean ok = TRUE;
      if (!strcmp(w[1], "centre"))      { e.kind = EV_CENTRE; e.v = v; ok = v > 0; }
      else if (!strcmp(w[1], "gap"))    { e.kind = EV_GAP;    e.v = v; }
      else if (!strcmp(w[1], "tx"))     { e.kind = EV_TX;     e.v = v; }
      else if (!strcmp(w[1], "closed")) { e.kind = EV_CLOSED; }
      else { ok = FALSE; }
      if (ok) { g_array_append_val(a, e); }
    }
    g_strfreev(w);
  }
  g_strfreev(lines);
  g_free(body);
  return a;
}

/* ---- gh#28: the RBN tap ------------------------------------------------------ */

static FILE   *g_rbn;
static double  g_rate;
static double  g_start_unix_us;        /* .meta start_unix_us; 0 = unknown   */
static guint64 g_origin;               /* recording frame (holes counted) at
                                        * the pipeline's stream frame 0 — a
                                        * slice start, or the tap's header  */
static SkimPipeline *g_pipe;
static guint64 g_rbn_n;

/* The RBN feed policy knobs (gh#24), for an A/B of two policies over one
 * tap: SKIM_REPLAY_RBN_MIN the score bar (0.85), SKIM_REPLAY_RBN_READS the
 * readings a call needs (2; 1 = any single reading), SKIM_REPLAY_RBN_SETTLE
 * the hold before its first line in seconds (8; -1 = none) and
 * SKIM_REPLAY_RBN_SETTLE_NODICT the hold of a call MASTER.SCP does not know
 * (-1 = the same as SETTLE). READS=1 with SETTLE=-1 is the feed as it was
 * before gh#24. SKIM_REPLAY_RBN_FRESH (gh#26) is the window in seconds
 * within which a call must have been READ for its record to go out (120;
 * -1 = none). */
static void rbn_policy_from_env(SkimPipelineConfig *cfg) {
  const char *e;
  if ((e = g_getenv("SKIM_REPLAY_RBN_MIN")) && e[0]) {
    cfg->rbn_min_score = g_ascii_strtod(e, NULL);
  }
  if ((e = g_getenv("SKIM_REPLAY_RBN_READS")) && e[0]) {
    cfg->rbn_min_hearings = (guint)MAX(1, atoi(e));
  }
  if ((e = g_getenv("SKIM_REPLAY_RBN_SETTLE")) && e[0]) {
    const double v = g_ascii_strtod(e, NULL);
    cfg->rbn_settle_s = v > 0 ? v : -1;
  }
  if ((e = g_getenv("SKIM_REPLAY_RBN_SETTLE_NODICT")) && e[0]) {
    const double v = g_ascii_strtod(e, NULL);
    cfg->rbn_settle_nodict_s = v > 0 ? v : -1;
  }
  if ((e = g_getenv("SKIM_REPLAY_RBN_FRESH")) && e[0]) {
    const double v = g_ascii_strtod(e, NULL);
    cfg->rbn_fresh_s = v > 0 ? v : -1;
  }
}

static void print_rbn_policy(const SkimPipeline *p, const char *rlog,
                             const char *note) {
  double bar, settle, settle_nd, fresh;
  guint reads;
  skim_pipeline_rbn_policy(p, &bar, &reads, &settle, &settle_nd, &fresh);
  printf("rbn tap: %s (bar %.2f, %u reading%s or MASTER.SCP, hold %.0f s, "
         "%.0f s for a call MASTER.SCP does not know, read within %.0f s)%s\n",
         rlog, bar, reads, reads == 1 ? "" : "s", settle, settle_nd, fresh,
         note);
}

/* Stamped with the PIPELINE's stream frames (the block end the record was
 * dispatched at) plus the origin — the one clock a tap replay has too, so
 * both paths stamp a record identically. */
static void rbn_tap_cb(const char *call, const char *mode, double freq_hz,
                       double snr_db, double speed, gpointer user) {
  (void)user;
  const double t = (double)(g_origin + skim_pipeline_frames(g_pipe)) / g_rate;
  char utc[32] = "-";
  if (g_start_unix_us > 0) {
    const double abs_s = g_start_unix_us / 1e6 + t;
    GDateTime *dt = g_date_time_new_from_unix_utc((gint64)abs_s);
    char *s = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%SZ");
    g_strlcpy(utc, s, sizeof(utc));
    g_free(s);
    g_date_time_unref(dt);
  }
  char khz[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(khz, sizeof(khz), "%.2f", freq_hz / 1000.0);
  fprintf(g_rbn, "%s\t%.1f\t%s\t%s\t%s\t%.0f\t%.0f\n", utc, t, call, khz,
          mode, snr_db, speed);
  g_rbn_n++;
}

/* ---- gh#28: the decode tap ----------------------------------------------------- */

static FILE   *g_tap;
static guint64 g_tap_n[5];             /* events written, per SkimTapKind    */

static void tap_write_cb(const SkimTapEvent *ev, gpointer user) {
  (void)user;
  char *line = skim_tap_event_to_line(ev);
  if (line) {
    fputs(line, g_tap);
    fputc('\n', g_tap);
    g_free(line);
    if ((guint)ev->kind < G_N_ELEMENTS(g_tap_n)) { g_tap_n[ev->kind]++; }
  }
}

static int by_freq(gconstpointer a, gconstpointer b) {
  const SkimStation *sa = *(const SkimStation *const *)a;
  const SkimStation *sb = *(const SkimStation *const *)b;
  return (sa->freq_hz > sb->freq_hz) - (sa->freq_hz < sb->freq_hz);
}

/* Station table, sorted by frequency — the band as the decoder saw it.
 * Returns the row count. */
static guint print_stations(gboolean rtty) {
  GPtrArray *rows = g_ptr_array_new();
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, g_stations);
  while (g_hash_table_iter_next(&it, &key, &val)) { g_ptr_array_add(rows, val); }
  g_ptr_array_sort(rows, by_freq);
  printf("\n%-12s %10s %5s %5s %7s %6s %3s\n",
         "call", "kHz", rtty ? "bd" : "wpm", "dB", "reports", "score", "cq");
  for (guint i = 0; i < rows->len; i++) {
    const SkimStation *st = g_ptr_array_index(rows, i);
    char khz[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(khz, sizeof(khz), "%.2f", st->freq_hz / 1000.0);
    printf("%-12s %10s %5.0f %5.0f %7u %6.2f %3s\n", st->call, khz, st->speed,
           st->snr_db, st->reports, st->score, st->cq ? "CQ" : "");
  }
  const guint n = rows->len;
  g_ptr_array_free(rows, TRUE);
  return n;
}

static char *dict_path_default(void) {
  char *dict = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                "master.scp", NULL);
  if (!g_file_test(dict, G_FILE_TEST_EXISTS)) { g_clear_pointer(&dict, g_free); }
  return dict;
}

/* ---- tap replay: skimmer-replay <file.tap> ------------------------------------ */

static int replay_tap(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }
  const char *out_env = g_getenv("SKIM_REPLAY_OUT");
  const char *out = out_env && out_env[0] ? out_env : path;
  char *rlog = g_strdup_printf("%s.rbn.log", out);
  g_rbn = fopen(rlog, "w");
  if (!g_rbn) {
    fprintf(stderr, "cannot write %s\n", rlog);
    return 2;
  }

  /* The header: "# key value" lines up to the first event. */
  char *line = NULL;
  size_t cap = 0;
  ssize_t len;
  gboolean rtty = FALSE;
  char *source = NULL, *engine = NULL, *dict_rel = NULL;
  g_rate = 0;
  g_origin = 0;
  g_start_unix_us = 0;
  long body_at = 0;
  while ((len = getline(&line, &cap, f)) > 0) {
    if (line[0] != '#') { break; }
    char **w = g_strsplit_set(g_strstrip(line + 1), " \t", 3);
    const guint n = g_strv_length(w);
    if (n >= 2) {
      if (!strcmp(w[0], "rate"))               { g_rate = g_ascii_strtod(w[1], NULL); }
      else if (!strcmp(w[0], "origin"))        { g_origin = g_ascii_strtoull(w[1], NULL, 10); }
      else if (!strcmp(w[0], "start_unix_us")) { g_start_unix_us = g_ascii_strtod(w[1], NULL); }
      else if (!strcmp(w[0], "mode"))          { rtty = g_ascii_strcasecmp(w[1], "RTTY") == 0; }
      else if (!strcmp(w[0], "source"))        { source = g_strdup(w[1]); }
      else if (!strcmp(w[0], "engine"))        { engine = g_strdup(w[1]); }
      else if (!strcmp(w[0], "dict"))          { dict_rel = g_strdup(w[1]); }
    }
    g_strfreev(w);
    body_at = ftell(f);
  }
  if (g_rate <= 0) {
    fprintf(stderr, "%s: no '# rate' in the header — not a decode tap\n", path);
    return 2;
  }
  fseek(f, body_at, SEEK_SET);

  char *dict = dict_path_default();
  SkimPipelineConfig cfg = {
    .mode = rtty ? SKIM_PIPELINE_MODE_RTTY : SKIM_PIPELINE_MODE_CW,
    .dict_path = dict,
    .rbn_tap = rbn_tap_cb,
  };
  rbn_policy_from_env(&cfg);
  SkimPipeline *p = skim_pipeline_new(&cfg);
  g_pipe = p;
  char *rel_now = skim_callsign_dict_release();
  printf("=== skimmer-replay %s — decode tap of %s (%s, %s), %.0f Hz, "
         "dict %s ===\n",
         path, source ? source : "?", engine ? engine : "?",
         rtty ? "RTTY" : "CW", g_rate,
         dict ? (rel_now ? rel_now : "yes") : "NO");
  if (dict_rel && g_strcmp0(dict_rel, rel_now ? rel_now : "none") != 0) {
    printf("WARNING: the tap was taken with MASTER.SCP %s, this run loads %s — "
           "the extractor's dictionary bonus differs, the result is not the "
           "IQ replay's\n", dict_rel, rel_now ? rel_now : "none");
  }
  g_free(rel_now);
  g_stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  skim_pipeline_set_station_cb(p, station_cb, NULL);
  skim_pipeline_set_station_gone_cb(p, gone_cb, NULL);
  GError *err = NULL;
  if (!skim_pipeline_start_tap(p, &err)) {
    fprintf(stderr, "FAIL — %s\n", err ? err->message : "?");
    g_clear_error(&err);
    return 1;
  }
  print_rbn_policy(p, rlog,
      g_start_unix_us > 0 ? "" : " — no start_unix_us in the header, UTC unknown");

  const gint64 t0 = g_get_monotonic_time();
  guint64 n_ev = 0, n_bad = 0, n_dec = 0;
  guint64 frames_first = 0, frames_last = 0;
  SkimTapEvent ev;
  while ((len = getline(&line, &cap, f)) > 0) {
    if (!skim_tap_line_parse(line, &ev)) {
      if (line[0] != '#' && line[0] != '\n') { n_bad++; }
      continue;
    }
    if (!n_ev) { frames_first = ev.frames; }
    frames_last = ev.frames;
    n_ev++;
    n_dec += ev.kind == SKIM_TAP_DECODE;
    skim_pipeline_feed_tap(p, &ev);
  }
  free(line);
  fclose(f);
  const double wall = (double)(g_get_monotonic_time() - t0) / G_USEC_PER_SEC;
  const double stream = (double)(frames_last - frames_first) / g_rate;
  skim_pipeline_stop(p);

  const guint nst = print_stations(rtty);
  printf("\nsummary: %.1f s stream in %.1f s wall (%.0fx), %" G_GUINT64_FORMAT
         " tap events (%" G_GUINT64_FORMAT " decodes%s), %u stations\n",
         stream, wall, stream / MAX(wall, 0.001), n_ev, n_dec,
         n_bad ? ", some lines unreadable" : "", nst);
  if (n_bad) {
    printf("WARNING: %" G_GUINT64_FORMAT " lines did not parse\n", n_bad);
  }
  printf("rbn tap: %" G_GUINT64_FORMAT " records → %s\n", g_rbn_n, rlog);
  fclose(g_rbn);
  g_free(rlog);
  g_free(source);
  g_free(engine);
  g_free(dict_rel);
  g_free(dict);
  g_hash_table_destroy(g_stations);
  skim_pipeline_free(p);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: skimmer-replay <file.cf32> [rate_hz] [center_hz]\n"
                    "       skimmer-replay <file.tap>\n");
    return 2;
  }
  const char *path = argv[1];
  if (g_str_has_suffix(path, ".tap")) { return replay_tap(path); }

  char *meta = g_strdup_printf("%s.meta", path);
  double rate   = argc > 2 ? g_ascii_strtod(argv[2], NULL)
                           : meta_get(meta, "rate_hz:");
  double center = argc > 3 ? g_ascii_strtod(argv[3], NULL)
                           : meta_get(meta, "center_hz:");
  g_free(meta);
  if (rate <= 0 || center <= 0) {
    fprintf(stderr, "no rate/centre — need the .meta sidecar or CLI args\n");
    return 2;
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }

  char *dict = dict_path_default();
  const char *out_env = g_getenv("SKIM_REPLAY_OUT");
  const char *out = out_env && out_env[0] ? out_env : path;
  char *dlog = g_strdup_printf("%s.decodes.log", out);
  remove(dlog);                        /* fresh log — runs must diff cleanly  */
  char *rlog = g_strdup_printf("%s.rbn.log", out);
  g_rbn = fopen(rlog, "w");            /* fresh too                           */
  if (!g_rbn) {
    fprintf(stderr, "cannot write %s\n", rlog);
    return 2;
  }
  char *tlog = g_strdup_printf("%s.tap", out);
  g_tap = fopen(tlog, "w");
  if (!g_tap) {
    fprintf(stderr, "cannot write %s\n", tlog);
    return 2;
  }
  g_rate = rate;
  {
    char *m = g_strdup_printf("%s.meta", path);
    g_start_unix_us = meta_get(m, "start_unix_us:");
    g_free(m);
  }
  /* SKIM_MODE=rtty replays through the RTTY backend + wide bank (the same
   * config path the app's Mode preference takes); default CW. */
  const char *menv = g_getenv("SKIM_MODE");
  const gboolean rtty = menv && g_ascii_strcasecmp(menv, "rtty") == 0;
  SkimPipelineConfig cfg = {
    .server = NULL,
    .iq_rate = 0,
    .mode = rtty ? SKIM_PIPELINE_MODE_RTTY : SKIM_PIPELINE_MODE_CW,
    .chan_bw_hz = 0,                   /* mode default: 125 Hz CW, 250 RTTY  */
    .dict_path = dict,
    .decode_log_path = dlog,
    .rbn_tap = rbn_tap_cb,
    .decode_tap = tap_write_cb,
  };
  rbn_policy_from_env(&cfg);
  /* SKIM_CW_ENGINE=v1|v2|deepcw picks the CW engine (the app's "CW engine"
   * preference takes the same config field); the pipeline resolves the
   * DeepCW availability itself and falls back to v2 with a warning. A
   * replay runs DeepCW inference INLINE (deterministic, stream order) —
   * the app uses the async workers. */
  g_setenv("SKIM_DEEPCW_SYNC", "1", FALSE);
  SkimPipeline *p = skim_pipeline_new(&cfg);
  g_pipe = p;
  printf("=== skimmer-replay %s — %.0f Hz, centre %.0f Hz, %s, engine %s, "
         "dict %s ===\n",
         path, rate, center, rtty ? "RTTY" : "CW",
         skim_pipeline_cw_engine_name(p), cfg.dict_path ? "yes" : "NO");
  g_stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  skim_pipeline_set_text_cb(p, text_cb, NULL);
  skim_pipeline_set_station_cb(p, station_cb, NULL);
  skim_pipeline_set_station_gone_cb(p, gone_cb, NULL);
  /* SKIM_SPECTRUM_DUMP=1: run the M8 spectrum tap too and print the
   * strongest bin's ABSOLUTE frequency about once a second — the real-air
   * orientation check (a known station must land on its own kHz). */
  if (g_getenv("SKIM_SPECTRUM_DUMP")) {
    skim_pipeline_set_spectrum_cb(p, spectrum_cb, NULL);
    skim_pipeline_set_spectrum_enabled(p, TRUE);
  }

  GError *err = NULL;
  if (!skim_pipeline_start_offline(p, &err)) {
    fprintf(stderr, "FAIL — %s\n", err ? err->message : "?");
    g_clear_error(&err);
    return 1;
  }

  GArray *holds = holds_parse(g_getenv("SKIM_REPLAY_HOLDS"));
  GArray *mutes = holds_parse(g_getenv("SKIM_REPLAY_MUTE"));
  if (holds->len) { printf("TX holds replayed: %u\n", holds->len / 2); }
  if (mutes->len) { printf("muted intervals: %u\n", mutes->len / 2); }
  char *evpath = g_strdup_printf("%s.events", path);
  GArray *ev = events_load(evpath);
  g_free(evpath);
  /* SKIM_REPLAY_HOLDS, when given, replaces the sidecar's TX edges. */
  const gboolean ev_tx = holds->len == 0;
  {
    guint nc = 0, ng = 0, nt = 0;
    for (guint i = 0; i < ev->len; i++) {
      const EvKind k = g_array_index(ev, Ev, i).kind;
      nc += k == EV_CENTRE; ng += k == EV_GAP; nt += k == EV_TX;
    }
    if (ev->len) {
      printf("events: %u centre, %u gap, %u tx%s\n", nc, ng, nt,
             ev_tx ? "" : " (tx replaced by SKIM_REPLAY_HOLDS)");
    }
  }
  print_rbn_policy(p, rlog,
      g_start_unix_us > 0 ? "" : " — no start_unix_us in .meta, UTC unknown");

  gint64 t0 = g_get_monotonic_time();
  static float buf[BLK * 2];
  guint64 frames = 0;
  size_t n;
  {
    const char *from = g_getenv("SKIM_REPLAY_FROM");
    if (from && from[0]) {
      frames = (guint64)(g_ascii_strtod(from, NULL) * rate) / BLK * BLK;
      fseeko(f, (off_t)frames * 2 * (off_t)sizeof(float), SEEK_SET);
    }
  }
  const char *to_env = g_getenv("SKIM_REPLAY_TO");
  const guint64 frames_to = to_env && to_env[0]
      ? (guint64)(g_ascii_strtod(to_env, NULL) * rate) : G_MAXUINT64;
  const guint64 frames_from = frames;
  /* Events up to the slice start set the state it starts in (centre, TX);
   * a hole before it only moves the absolute clock. */
  guint ei = 0;
  gboolean ev_tx_on = FALSE;
  guint64 holes = 0;                   /* hole frames before `frames`         */
  for (; ei < ev->len && g_array_index(ev, Ev, ei).frame <= frames; ei++) {
    const Ev *e = &g_array_index(ev, Ev, ei);
    if (e->kind == EV_CENTRE) { center = e->v; }
    else if (e->kind == EV_TX) { ev_tx_on = e->v != 0; }
    else if (e->kind == EV_GAP) { holes += (guint64)e->v; }
  }
  g_origin = frames_from + holes;
  {
    /* The tap's header: what a tap replay needs to stamp and configure
     * itself, and what it must match (the dictionary) to reproduce this run. */
    char *rel = skim_callsign_dict_release();
    char rbuf[G_ASCII_DTOSTR_BUF_SIZE], sbuf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(rbuf, sizeof(rbuf), "%.17g", rate);
    g_ascii_formatd(sbuf, sizeof(sbuf), "%.17g", g_start_unix_us);
    fprintf(g_tap,
            "# skimmer-for-linux decode tap v1\n"
            "# source %s\n"
            "# engine %s\n"
            "# mode %s\n"
            "# rate %s\n"
            "# origin %" G_GUINT64_FORMAT "\n"
            "# start_unix_us %s\n"
            "# dict %s\n",
            path, skim_pipeline_cw_engine_name(p), rtty ? "RTTY" : "CW", rbuf,
            g_origin, sbuf, cfg.dict_path ? (rel ? rel : "yes") : "none");
    g_free(rel);
  }
  for (;;) {
    for (; ei < ev->len && g_array_index(ev, Ev, ei).frame <= frames; ei++) {
      const Ev *e = &g_array_index(ev, Ev, ei);
      if (e->kind == EV_CENTRE) { center = e->v; }
      else if (e->kind == EV_TX) { ev_tx_on = e->v != 0; }
      else if (e->kind == EV_GAP) {
        holes += (guint64)e->v;
        skim_pipeline_feed_gap(p, (guint)e->v);
      }
    }
    /* A block never straddles an event: live, the client split it there. */
    guint64 want = BLK;
    if (ei < ev->len) { want = MIN(want, g_array_index(ev, Ev, ei).frame - frames); }
    if (frames_to != G_MAXUINT64) { want = MIN(want, frames_to - MIN(frames, frames_to)); }
    if (!want || (n = fread(buf, 2 * sizeof(float), (size_t)want, f)) == 0) { break; }
    if (holds->len) {
      skim_pipeline_set_tx_hold(p, holds_tx_at(holds, (double)frames / rate));
    } else if (ev_tx) {
      skim_pipeline_set_tx_hold(p, ev_tx_on);
    }
    for (guint m = 0; m + 1 < mutes->len; m += 2) {
      const double a = g_array_index(mutes, double, m) * rate - (double)frames;
      const double z = g_array_index(mutes, double, m + 1) * rate - (double)frames;
      if (z <= 0 || a >= (double)n) { continue; }
      const gsize i0 = a > 0 ? (gsize)a : 0, i1 = z < (double)n ? (gsize)z : n;
      memset(buf + 2 * i0, 0, (i1 - i0) * 2 * sizeof(float));
    }
    skim_pipeline_feed(p, buf, (guint)n, rate, center);
    frames += n;
  }
  g_array_free(ev, TRUE);
  fclose(f);
  g_array_free(holds, TRUE);
  g_array_free(mutes, TRUE);
  double wall   = (double)(g_get_monotonic_time() - t0) / G_USEC_PER_SEC;
  double stream = (double)(frames - frames_from) / rate;

  skim_pipeline_stop(p);

  const guint nst = print_stations(rtty);

  printf("\nsummary: %.1f s stream in %.1f s wall (%.1fx), %" G_GUINT64_FORMAT
         " frames, %" G_GUINT64_FORMAT " fragments, %u stations\n",
         stream, wall, stream / MAX(wall, 0.001), frames, g_fragments, nst);
  printf("decode log: %s\n", dlog);
  printf("rbn tap: %" G_GUINT64_FORMAT " records → %s\n", g_rbn_n, rlog);
  printf("decode tap: %" G_GUINT64_FORMAT " decodes, %" G_GUINT64_FORMAT
         " resets, %" G_GUINT64_FORMAT " flushes, %" G_GUINT64_FORMAT
         " prunes → %s\n",
         g_tap_n[SKIM_TAP_DECODE], g_tap_n[SKIM_TAP_RESET],
         g_tap_n[SKIM_TAP_FLUSH], g_tap_n[SKIM_TAP_PRUNE], tlog);
  fclose(g_rbn);
  fclose(g_tap);
  g_free(rlog);
  g_free(tlog);

  g_hash_table_destroy(g_stations);
  skim_pipeline_free(p);
  g_free(dict);
  g_free(dlog);
  return 0;
}
