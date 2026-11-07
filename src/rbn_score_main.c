/*
 * skimmer-rbn-score — score a replay's RBN tap against the RBN archive
 * (gh#28: the bench's referee), or two taps against each other.
 *
 *   skimmer-rbn-score [options] <run.rbn.log> <YYYYMMDD.csv> [more.csv …]
 *   skimmer-rbn-score --diff <a.rbn.log> <b.rbn.log>
 *
 * <run.rbn.log> is what skimmer-replay writes: every record the RBN feed
 * would have broadcast. The archive is RBN's public daily file
 * (https://data.reversebeacon.net/rbn_history/<YYYYMMDD>.zip, unzipped):
 *   callsign,de_pfx,de_cont,freq,band,dx,dx_pfx,dx_cont,mode,db,date,speed,tx_mode
 *
 * The unit is call × band × UTC hour (spots of one station within an hour
 * are one unit). Each of OUR units is
 *   confirmed    some skimmer (any continent) spotted the call on that band
 *                in that hour — a real call on the air;
 *   look-alike   not confirmed, but a spotted call within two edits of ours
 *                (or one containing the other) sat within ±1 kHz of our
 *                frequency in that hour — a bust of a real station;
 *   unconfirmed  neither: a station nobody else heard, or a bust whose
 *                original RBN did not hear either.
 * The REFEREE set is the archive's CW units that a skimmer of --cont (EU)
 * — or one of --spotter — heard while we recorded, inside the span the
 * recording covered at that moment (the .meta centre and the .events
 * retunes, minus a 5 kHz edge). Recall = our confirmed units that are in the
 * referee set / the referee set. It is an A/B figure: the referee hears
 * stations our antenna cannot, so only differences between runs over the
 * same recording mean something.
 *
 * --diff compares two taps of the SAME recording by unit: common, only in
 * A, only in B — two replays that differ only by warm-up give the noise
 * floor every later A/B must clear.
 *
 * Spots by the operator's own skimmer (--self, default OK1BR) are left out
 * of the referee.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bands (by frequency, the same table both sides) ----------------------- */

static const struct { double lo, hi; const char *name; } BANDS[] = {
  {  1800,  2000, "160m" }, {  3500,  4000, "80m" }, {  5250,  5450, "60m" },
  {  7000,  7300, "40m" },  { 10100, 10150, "30m" }, { 14000, 14350, "20m" },
  { 18068, 18168, "17m" },  { 21000, 21450, "15m" }, { 24890, 24990, "12m" },
  { 28000, 29700, "10m" },  { 50000, 54000, "6m" },
};

static const char *band_of(double khz) {
  for (guint i = 0; i < G_N_ELEMENTS(BANDS); i++) {
    if (khz >= BANDS[i].lo && khz <= BANDS[i].hi) { return BANDS[i].name; }
  }
  return NULL;
}

/* ---- one spot, either side ------------------------------------------------- */

typedef struct {
  gint64 t;               /* unix seconds UTC */
  double khz;
  char   call[16];
  char   spotter[16];
  gboolean ref;           /* archive row that counts for the referee set   */
} Spot;

static char *unit_key(const char *call, const char *band, gint64 t) {
  return g_strdup_printf("%s|%s|%" G_GINT64_FORMAT, call, band, t / 3600);
}

static gint64 iso_to_unix(const char *s) {
  /* "2026-11-01T18:03:35Z" or "2026-11-01 18:03:35" (UTC) */
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%d-%d-%d%*c%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) { return -1; }
  GDateTime *dt = g_date_time_new_utc(Y, M, D, h, m, sec);
  if (!dt) { return -1; }
  const gint64 u = g_date_time_to_unix(dt);
  g_date_time_unref(dt);
  return u;
}

/* skimmer-replay's tap: UTC \t stream_s \t call \t kHz \t mode \t dB \t wpm */
static GArray *tap_load(const char *path) {
  GArray *a = g_array_new(FALSE, TRUE, sizeof(Spot));
  char *body = NULL;
  GError *err = NULL;
  if (!g_file_get_contents(path, &body, NULL, &err)) {
    g_printerr("%s\n", err->message);
    exit(2);
  }
  char **lines = g_strsplit(body, "\n", -1);
  for (char **l = lines; *l; l++) {
    char **w = g_strsplit(*l, "\t", -1);
    if (g_strv_length(w) >= 4) {
      Spot s = { .t = iso_to_unix(w[0]), .khz = g_ascii_strtod(w[3], NULL) };
      g_strlcpy(s.call, w[2], sizeof(s.call));
      if (s.t >= 0 && band_of(s.khz)) { g_array_append_val(a, s); }
      else if (s.t < 0) {
        g_printerr("%s: a record without UTC (recording without start_unix_us?)\n", path);
        exit(2);
      }
    }
    g_strfreev(w);
  }
  g_strfreev(lines);
  g_free(body);
  return a;
}

/* ---- recording coverage: centre over time (.meta + .events) --------------- */

typedef struct { gint64 t0; double centre_khz; } Seg;

typedef struct {
  gint64  start, end;     /* unix s */
  double  half_khz;       /* rate/2 minus the edge                        */
  GArray *segs;           /* Seg, by t0                                   */
} Cover;

static double meta_num(const char *body, const char *key) {
  const char *hit = strstr(body, key);
  return hit ? g_ascii_strtod(hit + strlen(key), NULL) : 0;
}

static gboolean cover_load(const char *cf32, Cover *cv) {
  char *mp = g_strdup_printf("%s.meta", cf32);
  char *meta = NULL;
  gboolean ok = g_file_get_contents(mp, &meta, NULL, NULL);
  g_free(mp);
  if (!ok) { return FALSE; }
  const double start_us = meta_num(meta, "start_unix_us:");
  const double rate = meta_num(meta, "rate_hz:");
  const double centre = meta_num(meta, "center_hz:");
  const double frames = meta_num(meta, "frames:");
  g_free(meta);
  if (start_us <= 0 || rate <= 0 || centre <= 0) { return FALSE; }
  cv->segs = g_array_new(FALSE, FALSE, sizeof(Seg));
  cv->half_khz = rate / 2000.0 - 5.0;
  cv->start = (gint64)(start_us / 1e6);
  Seg s0 = { cv->start, centre / 1000.0 };
  g_array_append_val(cv->segs, s0);
  char *ep = g_strdup_printf("%s.events", cf32);
  char *ev = NULL;
  guint64 holes = 0, last_frame = 0;
  if (g_file_get_contents(ep, &ev, NULL, NULL)) {
    char **lines = g_strsplit(ev, "\n", -1);
    for (char **l = lines; *l; l++) {
      char **w = g_strsplit(*l, " ", -1);
      if (g_strv_length(w) >= 3) {
        const guint64 fr = g_ascii_strtoull(w[0], NULL, 10);
        last_frame = MAX(last_frame, fr);
        if (!strcmp(w[1], "gap")) { holes += g_ascii_strtoull(w[2], NULL, 10); }
        else if (!strcmp(w[1], "centre")) {
          Seg s = { (gint64)(start_us / 1e6 + (double)(fr + holes) / rate),
                    g_ascii_strtod(w[2], NULL) / 1000.0 };
          /* the recorder's frame-0 line repeats the .meta centre: not a retune */
          const Seg *last = &g_array_index(cv->segs, Seg, cv->segs->len - 1);
          if (s.centre_khz != last->centre_khz) { g_array_append_val(cv->segs, s); }
        }
      }
      g_strfreev(w);
    }
    g_strfreev(lines);
    g_free(ev);
  }
  g_free(ep);
  /* A recording still running has no frames: line yet — the file size does. */
  double fr_total = frames;
  if (fr_total <= 0) {
    GStatBuf st;
    if (g_stat(cf32, &st) == 0) { fr_total = (double)st.st_size / 8.0; }
  }
  cv->end = (gint64)(start_us / 1e6 + (fr_total + (double)holes) / rate);
  (void)last_frame;
  return TRUE;
}

static gboolean cover_has(const Cover *cv, gint64 t, double khz) {
  if (t < cv->start || t > cv->end) { return FALSE; }
  double c = g_array_index(cv->segs, Seg, 0).centre_khz;
  for (guint i = 0; i < cv->segs->len; i++) {
    const Seg *s = &g_array_index(cv->segs, Seg, i);
    if (s->t0 <= t) { c = s->centre_khz; } else { break; }
  }
  return khz >= c - cv->half_khz && khz <= c + cv->half_khz;
}

/* ---- the archive ----------------------------------------------------------- */

static void archive_load(const char *path, GArray *out, const Cover *cv,
                         const char *cont, char **spotters, const char *self,
                         gint64 t_lo, gint64 t_hi) {
  FILE *f = fopen(path, "r");
  if (!f) { g_printerr("cannot open %s\n", path); exit(2); }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *fld[13];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, ",\r\n", &save); tok && n < 13;
         tok = strtok_r(NULL, ",\r\n", &save)) {
      fld[n++] = tok;
    }
    /* empty fields collapse under strtok — a short row is not a CW spot row */
    if (n < 13 || strcmp(fld[12], "CW") != 0) { continue; }
    if (strcmp(fld[8], "CQ") != 0 && strcmp(fld[8], "DX") != 0) { continue; }
    if (self && self[0] && g_str_has_prefix(fld[0], self)) { continue; }
    const gint64 t = iso_to_unix(fld[10]);
    if (t < t_lo || t > t_hi) { continue; }
    Spot s = { .t = t, .khz = g_ascii_strtod(fld[3], NULL) };
    if (!band_of(s.khz)) { continue; }
    g_strlcpy(s.call, fld[5], sizeof(s.call));
    g_strlcpy(s.spotter, fld[0], sizeof(s.spotter));
    gboolean near;
    if (spotters) {
      near = FALSE;
      for (char **q = spotters; *q; q++) { if (!strcmp(*q, fld[0])) { near = TRUE; } }
    } else {
      near = !cont || !cont[0] || strcmp(fld[2], cont) == 0;
    }
    s.ref = near && cover_has(cv, t, s.khz);
    g_array_append_val(out, s);
  }
  fclose(f);
}

/* ---- look-alike ------------------------------------------------------------ */

static guint lev(const char *a, const char *b) {
  const guint la = strlen(a), lb = strlen(b);
  guint row[32];
  if (lb >= G_N_ELEMENTS(row)) { return 99; }
  for (guint j = 0; j <= lb; j++) { row[j] = j; }
  for (guint i = 1; i <= la; i++) {
    guint prev = row[0];
    row[0] = i;
    for (guint j = 1; j <= lb; j++) {
      const guint cur = row[j];
      const guint sub = prev + (a[i - 1] != b[j - 1]);
      row[j] = MIN(MIN(row[j] + 1, row[j - 1] + 1), sub);
      prev = cur;
    }
  }
  return row[lb];
}

static gboolean looks_alike(const char *ours, const char *theirs) {
  if (!strcmp(ours, theirs)) { return FALSE; }
  if (strlen(ours) >= 3 && strlen(theirs) >= 3 &&
      (strstr(ours, theirs) || strstr(theirs, ours))) {
    return TRUE;
  }
  return lev(ours, theirs) <= 2;
}

/* ---- per band-hour tally --------------------------------------------------- */

typedef struct { guint ours, conf, alike, unconf, ref, hit; } Tally;

static Tally *tally_get(GHashTable *h, const char *band, gint64 hour) {
  char *k = g_strdup_printf("%s %02" G_GINT64_FORMAT "Z", band, hour % 24);
  Tally *t = g_hash_table_lookup(h, k);
  if (!t) { t = g_new0(Tally, 1); g_hash_table_insert(h, k, t); }
  else { g_free(k); }
  return t;
}

static void tally_print(const char *label, const Tally *t) {
  printf("%-10s %6u %6u %6u %6u   %6u %6u  %6.1f %%  %6.1f %%\n", label, t->ours,
         t->conf, t->alike, t->unconf, t->ref, t->hit,
         t->ref ? 100.0 * t->hit / t->ref : 0.0,
         t->ours ? 100.0 * t->alike / t->ours : 0.0);
}

/* ---- --diff ---------------------------------------------------------------- */

static GHashTable *units_of(GArray *a) {
  GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < a->len; i++) {
    const Spot *s = &g_array_index(a, Spot, i);
    g_hash_table_add(h, unit_key(s->call, band_of(s->khz), s->t));
  }
  return h;
}

static int diff_main(const char *pa, const char *pb, gboolean list) {
  GArray *a = tap_load(pa), *b = tap_load(pb);
  /* Compare from the time both runs cover — a warm-up shift starts later.
   * The END is not trimmed: both runs end with the recording, and a feed
   * policy that holds a call (gh#24) releases the last one AFTER the other
   * run's last record — trimming there listed it as lost (DM1BM, 2026-11-07). */
  gint64 lo = G_MININT64;
  GArray *tmp[2] = { a, b };
  for (int k = 0; k < 2; k++) {
    gint64 mn = G_MAXINT64;
    for (guint i = 0; i < tmp[k]->len; i++) {
      mn = MIN(mn, g_array_index(tmp[k], Spot, i).t);
    }
    lo = MAX(lo, mn);
  }
  for (int k = 0; k < 2; k++) {
    GArray *f = g_array_new(FALSE, FALSE, sizeof(Spot));
    for (guint i = 0; i < tmp[k]->len; i++) {
      const Spot *s = &g_array_index(tmp[k], Spot, i);
      if (s->t >= lo) { g_array_append_val(f, *s); }
    }
    g_array_free(tmp[k], TRUE);
    tmp[k] = f;
  }
  GHashTable *ua = units_of(tmp[0]), *ub = units_of(tmp[1]);
  guint common = 0, only_a = 0, only_b = 0;
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, ua);
  while (g_hash_table_iter_next(&it, &key, NULL)) {
    if (g_hash_table_contains(ub, key)) { common++; }
    else { only_a++; if (list) { printf("  only A  %s\n", (char *)key); } }
  }
  g_hash_table_iter_init(&it, ub);
  while (g_hash_table_iter_next(&it, &key, NULL)) {
    if (!g_hash_table_contains(ua, key)) {
      only_b++;
      if (list) { printf("  only B  %s\n", (char *)key); }
    }
  }
  const guint uni = common + only_a + only_b;
  printf("units (call × band × hour) from the common start: A %u, B %u, "
         "common %u, only A %u, only B %u — %.1f %% of the union differ\n",
         g_hash_table_size(ua), g_hash_table_size(ub), common, only_a, only_b,
         uni ? 100.0 * (only_a + only_b) / uni : 0.0);
  printf("records: A %u, B %u\n", tmp[0]->len, tmp[1]->len);
  return 0;
}

int main(int argc, char **argv) {
  char *rec = NULL, *cont = g_strdup("EU"), *spot_list = NULL, *self = g_strdup("OK1BR");
  gboolean diff = FALSE, list = FALSE;
  GOptionEntry entries[] = {
    { "recording", 'r', 0, G_OPTION_ARG_FILENAME, &rec,
      "the .cf32 the tap was replayed from (its .meta/.events give the span)", "CF32" },
    { "cont", 'c', 0, G_OPTION_ARG_STRING, &cont,
      "referee: skimmers on this continent (default EU; \"\" = all)", "CONT" },
    { "spotter", 's', 0, G_OPTION_ARG_STRING, &spot_list,
      "referee: only these skimmers (comma list, overrides --cont)", "CALLS" },
    { "self", 0, 0, G_OPTION_ARG_STRING, &self,
      "own skimmer prefix left out of the referee (default OK1BR)", "CALL" },
    { "diff", 'd', 0, G_OPTION_ARG_NONE, &diff, "compare two taps instead", NULL },
    { "list", 'l', 0, G_OPTION_ARG_NONE, &list,
      "list the look-alike and unconfirmed units (and --diff's differences)", NULL },
    G_OPTION_ENTRY_NULL
  };
  GOptionContext *oc = g_option_context_new(
      "<run.rbn.log> <rbn.csv>… | --diff <a.rbn.log> <b.rbn.log>");
  g_option_context_add_main_entries(oc, entries, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(oc, &argc, &argv, &err) || argc < 3) {
    g_printerr("%s\n", err ? err->message : g_option_context_get_help(oc, TRUE, NULL));
    return 2;
  }
  g_option_context_free(oc);
  if (diff) { return diff_main(argv[1], argv[2], list); }
  if (!rec) {
    g_printerr("--recording <file.cf32> is required: the referee set is limited "
               "to the span and time the recording covered\n");
    return 2;
  }

  Cover cv = { 0 };
  if (!cover_load(rec, &cv)) {
    g_printerr("%s: no usable .meta (start_unix_us, rate_hz, center_hz)\n", rec);
    return 2;
  }
  GArray *ours = tap_load(argv[1]);
  GArray *arch = g_array_new(FALSE, TRUE, sizeof(Spot));
  char **spotters = spot_list && spot_list[0] ? g_strsplit(spot_list, ",", -1) : NULL;
  /* confirmation looks a little past the recording's ends: an hour unit */
  for (int i = 2; i < argc; i++) {
    archive_load(argv[i], arch, &cv, cont, spotters, self,
                 cv.start - 3600, cv.end + 3600);
  }

  /* Index the archive by unit: confirmed units (any spotter) + referee units. */
  GHashTable *conf = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *ref = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < arch->len; i++) {
    const Spot *s = &g_array_index(arch, Spot, i);
    char *k = unit_key(s->call, band_of(s->khz), s->t);
    if (s->ref) { g_hash_table_add(ref, g_strdup(k)); }
    g_hash_table_add(conf, k);
  }

  /* Our units: first record of each wins (its frequency). */
  GHashTable *mine = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *tal = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  Tally total = { 0 };
  for (guint i = 0; i < ours->len; i++) {
    const Spot *s = &g_array_index(ours, Spot, i);
    const char *band = band_of(s->khz);
    char *k = unit_key(s->call, band, s->t);
    if (g_hash_table_contains(mine, k)) { g_free(k); continue; }
    g_hash_table_add(mine, k);
    Tally *t = tally_get(tal, band, s->t / 3600);
    t->ours++; total.ours++;
    if (g_hash_table_contains(conf, k)) { t->conf++; total.conf++; continue; }
    const char *alike = NULL;
    for (guint j = 0; j < arch->len && !alike; j++) {
      const Spot *r = &g_array_index(arch, Spot, j);
      if (r->t / 3600 == s->t / 3600 && ABS(r->khz - s->khz) <= 1.0 &&
          looks_alike(s->call, r->call)) {
        alike = r->call;
      }
    }
    if (alike) {
      t->alike++; total.alike++;
      if (list) { printf("  look-alike  %-10s %9.2f kHz %s  ~ %s\n", s->call, s->khz,
                         band, alike); }
    } else {
      t->unconf++; total.unconf++;
      if (list) {
        GDateTime *dt = g_date_time_new_from_unix_utc(s->t);
        char *ts = g_date_time_format(dt, "%H:%M:%SZ");
        printf("  unconfirmed %-10s %9.2f kHz %s  %s\n", s->call, s->khz, band, ts);
        g_free(ts);
        g_date_time_unref(dt);
      }
    }
  }
  /* The referee set, and which of it we have. */
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, ref);
  while (g_hash_table_iter_next(&it, &key, NULL)) {
    char **p = g_strsplit(key, "|", 3);
    Tally *t = tally_get(tal, p[1], g_ascii_strtoll(p[2], NULL, 10));
    t->ref++; total.ref++;
    if (g_hash_table_contains(mine, key)) { t->hit++; total.hit++; }
    g_strfreev(p);
  }

  GDateTime *d0 = g_date_time_new_from_unix_utc(cv.start);
  GDateTime *d1 = g_date_time_new_from_unix_utc(cv.end);
  char *s0 = g_date_time_format(d0, "%Y-%m-%d %H:%M:%SZ");
  char *s1 = g_date_time_format(d1, "%H:%M:%SZ");
  printf("recording %s … %s, %u retune%s; referee = %s; own skimmer %s* left out\n",
         s0, s1, cv.segs->len - 1, cv.segs->len == 2 ? "" : "s",
         spotters ? spot_list : cont && cont[0] ? cont : "all skimmers",
         self ? self : "-");
  printf("recall is an A/B figure: the referee hears what our antenna cannot\n\n");
  printf("%-10s %6s %6s %6s %6s   %6s %6s  %8s  %8s\n", "band-hour", "ours", "conf",
         "alike", "unconf", "ref", "hit", "recall", "alike");
  GList *keys = g_list_sort(g_hash_table_get_keys(tal), (GCompareFunc)strcmp);
  for (GList *l = keys; l; l = l->next) {
    tally_print(l->data, g_hash_table_lookup(tal, l->data));
  }
  g_list_free(keys);
  tally_print("total", &total);
  g_free(s0); g_free(s1);
  g_date_time_unref(d0); g_date_time_unref(d1);
  return 0;
}
