/*
 * skimmer-replay — offline decode of a recorded IQ sample (M3 A/B harness).
 *
 *   skimmer-replay <file.cf32> [rate_hz] [center_hz]
 *
 * Feeds a cf32 recording (float32 interleaved I/Q, true orientation — what
 * skimmer-tci-probe dumps) through the full engine pipeline offline:
 * channelizer → CW decoders → callsign extractors → station tracker. Rate and
 * centre default from the <file>.meta sidecar the probe writes. Decodes land
 * in <file>.decodes.log stamped with STREAM time, so two runs of different
 * decoder versions over the same sample diff line by line; the run ends with
 * a station table and summary counters for quick before/after comparison.
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

static int by_freq(gconstpointer a, gconstpointer b) {
  const SkimStation *sa = *(const SkimStation *const *)a;
  const SkimStation *sb = *(const SkimStation *const *)b;
  return (sa->freq_hz > sb->freq_hz) - (sa->freq_hz < sb->freq_hz);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: skimmer-replay <file.cf32> [rate_hz] [center_hz]\n");
    return 2;
  }
  const char *path = argv[1];

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

  char *dict = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                "master.scp", NULL);
  char *dlog = g_strdup_printf("%s.decodes.log", path);
  remove(dlog);                        /* fresh log — runs must diff cleanly  */
  SkimPipelineConfig cfg = {
    .host = NULL,
    .port = 0,
    .iq_rate = 0,
    .chan_bw_hz = 125.0,
    .dict_path = g_file_test(dict, G_FILE_TEST_EXISTS) ? dict : NULL,
    .decode_log_path = dlog,
  };
  printf("=== skimmer-replay %s — %.0f Hz, centre %.0f Hz, dict %s ===\n",
         path, rate, center, cfg.dict_path ? "yes" : "NO");

  SkimPipeline *p = skim_pipeline_new(&cfg);
  g_free(dict);
  g_stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  skim_pipeline_set_text_cb(p, text_cb, NULL);
  skim_pipeline_set_station_cb(p, station_cb, NULL);
  skim_pipeline_set_station_gone_cb(p, gone_cb, NULL);

  GError *err = NULL;
  if (!skim_pipeline_start_offline(p, &err)) {
    fprintf(stderr, "FAIL — %s\n", err ? err->message : "?");
    g_clear_error(&err);
    return 1;
  }

  gint64 t0 = g_get_monotonic_time();
  static float buf[BLK * 2];
  guint64 frames = 0;
  size_t n;
  while ((n = fread(buf, 2 * sizeof(float), BLK, f)) > 0) {
    skim_pipeline_feed(p, buf, (guint)n, rate, center);
    frames += n;
  }
  fclose(f);
  double wall   = (double)(g_get_monotonic_time() - t0) / G_USEC_PER_SEC;
  double stream = (double)frames / rate;

  skim_pipeline_stop(p);

  /* Station table, sorted by frequency — the band as the decoder saw it. */
  GPtrArray *rows = g_ptr_array_new();
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, g_stations);
  while (g_hash_table_iter_next(&it, &key, &val)) { g_ptr_array_add(rows, val); }
  g_ptr_array_sort(rows, by_freq);
  printf("\n%-12s %10s %5s %5s %7s %6s %3s\n",
         "call", "kHz", "wpm", "dB", "reports", "score", "cq");
  for (guint i = 0; i < rows->len; i++) {
    const SkimStation *st = g_ptr_array_index(rows, i);
    char khz[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(khz, sizeof(khz), "%.2f", st->freq_hz / 1000.0);
    printf("%-12s %10s %5.0f %5.0f %7u %6.2f %3s\n", st->call, khz, st->speed,
           st->snr_db, st->reports, st->score, st->cq ? "CQ" : "");
  }

  printf("\nsummary: %.1f s stream in %.1f s wall (%.1fx), %" G_GUINT64_FORMAT
         " frames, %" G_GUINT64_FORMAT " fragments, %u stations\n",
         stream, wall, stream / MAX(wall, 0.001), frames, g_fragments,
         rows->len);
  printf("decode log: %s\n", dlog);

  g_ptr_array_free(rows, TRUE);
  g_hash_table_destroy(g_stations);
  skim_pipeline_free(p);
  g_free(dlog);
  return 0;
}
