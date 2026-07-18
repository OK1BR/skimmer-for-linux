/* skimmer-pane-test — offline gate for the phase B hybrid pane
 * (draft → final: the streamed neural reader owns a solid over's pane text,
 * v2's live draft shows until each word firms up).
 *
 *   - SkimPaneLog unit semantics: append / OPEN / SET / CLOSE / head trim,
 *     self-sync of a SET whose OPEN was lost, appends landing BEFORE an
 *     open over region.
 *   - stream positions: push_pos/flush_pos text is bit-identical to read()
 *     and every committed char carries a monotone in-range run index.
 *   - the WHOLE offline pipeline (SKIM_CW_V2=1 + SKIM_CW_READER=blob):
 *     ops arrive, the reader's words commit mid-over (SETs), the over
 *     CLOSEs with the reader text, the composed pane equals it exactly
 *     once (no doubled draft, no newlines), runs are deterministic, the
 *     decode log carries the increments, and the station table is
 *     IDENTICAL with the reader off — nothing of the reader ever reaches
 *     the extractor.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "engine/cw_reader.h"
#include "engine/decode.h"
#include "engine/pane_log.h"
#include "engine/pipeline.h"

static int checks, fails;

static void check(const char *what, gboolean ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
}

/* --- SkimPaneLog units ----------------------------------------------------------- */

static void op(SkimPaneLog *pl, SkimPaneOpKind k, guint erase,
               const char *text, guint flen) {
  SkimPaneOp o = { k, erase, flen, (char *)text, NULL };
  skim_pane_log_apply(pl, &o);
}

static void run_pane_log_units(void) {
  printf("--- SkimPaneLog units ---\n");
  SkimPaneLog *pl = skim_pane_log_new();

  skim_pane_log_append(pl, "OLD ");
  skim_pane_log_append(pl, "CQ TE");                    /* the shown draft   */
  op(pl, SKIM_PANE_OP_OPEN, 5, "CQ TEST", 0);
  check("OPEN takes the draft back",
        strcmp(skim_pane_log_text(pl), "OLD CQ TEST") == 0);
  check("over region spans the new text", skim_pane_log_over_len(pl) == 7);

  op(pl, SKIM_PANE_OP_SET, 0, "CQ TEST OK1BR OK", 8);
  check("SET replaces the region",
        strcmp(skim_pane_log_text(pl), "OLD CQ TEST OK1BR OK") == 0);
  check("final prefix tracked", skim_pane_log_final_len(pl) == 8);

  skim_pane_log_append(pl, "[N]");                     /* neighbour slot     */
  check("append lands BEFORE the open region",
        strcmp(skim_pane_log_text(pl), "OLD [N]CQ TEST OK1BR OK") == 0);

  op(pl, SKIM_PANE_OP_CLOSE, 0, "CQ TEST OK1BR OK1BR", 19);
  check("CLOSE seals with the final text",
        strcmp(skim_pane_log_text(pl), "OLD [N]CQ TEST OK1BR OK1BR") == 0);
  check("no region after CLOSE", skim_pane_log_over_len(pl) == 0);

  skim_pane_log_append(pl, " X");
  check("append after CLOSE is plain",
        g_str_has_suffix(skim_pane_log_text(pl), "OK1BR X"));

  op(pl, SKIM_PANE_OP_SET, 0, "NEW", 3);               /* lost OPEN          */
  check("orphan SET self-syncs at the tail",
        g_str_has_suffix(skim_pane_log_text(pl), "OK1BR XNEW") &&
            skim_pane_log_over_len(pl) == 3);

  const gsize before = skim_pane_log_len(pl);
  skim_pane_log_trim_head(pl, 4);
  check("head trim keeps the region",
        skim_pane_log_len(pl) == before - 4 &&
            skim_pane_log_over_len(pl) == 3 &&
            g_str_has_suffix(skim_pane_log_text(pl), "NEW"));

  skim_pane_log_free(pl);
}

/* --- stream positions -------------------------------------------------------------
 * Perfectly timed runs for a short text; the v3 blob must stream the same
 * chars read() produces, each with a sane, monotone run index. */

static const struct { char c; const char *m; } MORSE[] = {
  {'A',".-"},   {'B',"-..."}, {'C',"-.-."}, {'D',"-.."},  {'E',"."},
  {'F',"..-."}, {'G',"--."},  {'H',"...."}, {'I',".."},   {'J',".---"},
  {'K',"-.-"},  {'L',".-.."}, {'M',"--"},   {'N',"-."},   {'O',"---"},
  {'P',".--."}, {'Q',"--.-"}, {'R',".-."},  {'S',"..."},  {'T',"-"},
  {'U',"..-"},  {'V',"...-"}, {'W',".--"},  {'X',"-..-"}, {'Y',"-.--"},
  {'Z',"--.."},
  {'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},
  {'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
};

static const char *morse_of(char c) {
  for (guint i = 0; i < G_N_ELEMENTS(MORSE); i++) {
    if (MORSE[i].c == c) { return MORSE[i].m; }
  }
  return NULL;
}

/* text → alternating key/duration runs at `dit` ms. */
static void runs_of(const char *text, double dit, GArray *key, GArray *dur) {
  const gboolean t = TRUE, f = FALSE;
  for (const char *p = text; *p; p++) {
    if (*p == ' ') {
      if (dur->len) {                            /* extend the last gap      */
        double *d = &g_array_index(dur, double, dur->len - 1);
        *d = 7.0 * dit;
      }
      continue;
    }
    const char *m = morse_of(*p);
    if (!m) { continue; }
    for (const char *e = m; *e; e++) {
      const double dm = (*e == '-' ? 3.0 : 1.0) * dit;
      const double dg = (e[1] ? 1.0 : 3.0) * dit;
      g_array_append_val(key, t);
      g_array_append_val(dur, dm);
      g_array_append_val(key, f);
      g_array_append_val(dur, dg);
    }
  }
}

static void run_stream_positions(const char *blob) {
  printf("--- stream positions (v3 blob) ---\n");
  GError *err = NULL;
  SkimCwReader *r = skim_cw_reader_load(blob, &err);
  check("blob loads", r != NULL);
  if (!r) {
    g_clear_error(&err);
    return;
  }
  GArray *key = g_array_new(FALSE, FALSE, sizeof(gboolean));
  GArray *dur = g_array_new(FALSE, FALSE, sizeof(double));
  runs_of("CQ CQ DE OK1BR OK1BR K", 55.0, key, dur);

  char *batch = skim_cw_reader_read(r, (const gboolean *)key->data,
                                    (const double *)dur->data, key->len);

  SkimCwReaderStream *s = skim_cw_reader_stream_new(r);
  check("v3 blob streams", s != NULL);
  GString *txt = g_string_new(NULL);
  GArray  *pos = g_array_new(FALSE, FALSE, sizeof(guint));
  if (s) {
    for (guint i = 0; i < key->len; i++) {
      guint *pp = NULL;
      char  *t = skim_cw_reader_stream_push_pos(
          s, g_array_index(key, gboolean, i), g_array_index(dur, double, i),
          &pp);
      if (t) {
        g_string_append(txt, t);
        g_array_append_vals(pos, pp, (guint)strlen(t));
      }
      g_free(t);
      g_free(pp);
    }
    guint *pp = NULL;
    char  *t = skim_cw_reader_stream_flush_pos(s, &pp);
    if (t) {
      g_string_append(txt, t);
      g_array_append_vals(pos, pp, (guint)strlen(t));
    }
    g_free(t);
    g_free(pp);
    skim_cw_reader_stream_free(s);
  }
  check("stream text == batch text", strcmp(txt->str, batch) == 0);
  check("one position per char", pos->len == txt->len);
  gboolean mono = TRUE, in_range = TRUE;
  for (guint i = 0; i < pos->len; i++) {
    const guint p = g_array_index(pos, guint, i);
    if (p >= key->len) { in_range = FALSE; }
    if (i && p < g_array_index(pos, guint, i - 1)) { mono = FALSE; }
  }
  check("positions monotone", mono);
  check("positions in range", in_range);

  g_string_free(txt, TRUE);
  g_array_free(pos, TRUE);
  g_array_free(key, TRUE);
  g_array_free(dur, TRUE);
  g_free(batch);
  skim_cw_reader_free(r);
}

/* --- integration: offline pipeline, reader streaming --------------------------- */

#define RATE 48000.0

static void key_run(GArray *env, double samps, float on) {
  guint n = (guint)(samps + 0.5);
  for (guint i = 0; i < n; i++) { g_array_append_val(env, on); }
}

static GArray *gen_env(const char *payload, double wpm, double rate) {
  GArray *env = g_array_new(FALSE, FALSE, sizeof(float));
  char   *text = g_strdup_printf("VVV %s", payload);
  double  dit = 1.2 / wpm * rate;
  key_run(env, 3 * dit, 0.0f);
  for (const char *p = text; *p; p++) {
    if (*p == ' ') {
      key_run(env, 7 * dit, 0.0f);
      continue;
    }
    const char *m = morse_of(*p);
    if (!m) { continue; }
    for (const char *e = m; *e; e++) {
      key_run(env, *e == '-' ? 3 * dit : dit, 1.0f);
      if (e[1]) { key_run(env, dit, 0.0f); }
    }
    key_run(env, 3 * dit, 0.0f);
  }
  key_run(env, 2.5 * rate, 0.0f);                /* enough for the break     */
  g_free(text);
  return env;
}

static void shape_env(GArray *env, double rate) {
  guint L = (guint)(rate * 0.005);
  if (L < 3) { return; }
  float *w = g_new(float, L);
  double sum = 0.0;
  for (guint i = 0; i < L; i++) {
    w[i] = 0.5f - 0.5f * (float)cos(2.0 * G_PI * i / (L - 1));
    sum += w[i];
  }
  for (guint i = 0; i < L; i++) { w[i] = (float)(w[i] / sum); }
  float *src = (float *)env->data;
  float *dst = g_new0(float, env->len);
  for (guint n = 0; n < env->len; n++) {
    float acc = 0.0f;
    guint kmax = MIN(L, n + 1);
    for (guint k = 0; k < kmax; k++) { acc += w[k] * src[n - k]; }
    dst[n] = acc;
  }
  memcpy(src, dst, env->len * sizeof(float));
  g_free(dst);
  g_free(w);
}

static double gauss(GRand *rng) {
  double u1 = 1.0 - g_rand_double(rng), u2 = g_rand_double(rng);
  return sqrt(-2.0 * log(u1)) * cos(2.0 * G_PI * u2);
}

static float *synth_one(const GArray *env, double f0, double amp,
                        double snr_db, GRand *rng, guint *out_frames) {
  const guint n = env->len;
  const double sigma = 0.5 / (sqrt(2.0) * pow(10.0, snr_db / 20.0));
  float *iq = g_new(float, 2 * n);
  double ph = 0.0;
  for (guint i = 0; i < n; i++) {
    const double e = g_array_index(env, float, i);
    ph += 2.0 * G_PI * f0 / RATE;
    iq[2 * i]     = (float)(amp * e * cos(ph) + sigma * gauss(rng));
    iq[2 * i + 1] = (float)(amp * e * sin(ph) + sigma * gauss(rng));
  }
  *out_frames = n;
  return iq;
}

typedef struct {
  char   calls[16][24];
  guint  n;
} Seen;

static void station_cb(const SkimStation *st, gpointer user) {
  Seen *seen = user;
  for (guint i = 0; i < seen->n; i++) {
    if (strcmp(seen->calls[i], st->call) == 0) { return; }
  }
  if (seen->n < G_N_ELEMENTS(seen->calls)) {
    g_strlcpy(seen->calls[seen->n], st->call, sizeof(seen->calls[0]));
    seen->n++;
  }
}

typedef struct {
  double       f0;                       /* the carrier's absolute Hz        */
  SkimPaneLog *pl;
  GString     *close_text;
  guint        opens, sets, closes;
  gboolean     bad_erase, bad_final, saw_nl;
} PaneCap;

static void cap_text(double hz, const char *text, gpointer user) {
  PaneCap *c = user;
  if (fabs(hz - c->f0) > 50.0)
    return;
  if (strchr(text, '\n')) { c->saw_nl = TRUE; }
  skim_pane_log_append(c->pl, text);
}

static void cap_over(double hz, SkimPaneOpKind kind, guint erase,
                     const char *text, guint final_len, gpointer user) {
  PaneCap *c = user;
  if (fabs(hz - c->f0) > 50.0)
    return;
  if (kind == SKIM_PANE_OP_OPEN) {
    c->opens++;
    if (erase > skim_pane_log_len(c->pl)) { c->bad_erase = TRUE; }
  } else if (kind == SKIM_PANE_OP_SET) {
    c->sets++;
  } else if (kind == SKIM_PANE_OP_CLOSE) {
    c->closes++;
    g_string_assign(c->close_text, text);
  }
  if (final_len > strlen(text)) { c->bad_final = TRUE; }
  if (strchr(text, '\n')) { c->saw_nl = TRUE; }
  SkimPaneOp o = { kind, erase, final_len, (char *)text, NULL };
  skim_pane_log_apply(c->pl, &o);
}

/* One offline pipeline pass over the IQ; fills the station set and (when
 * cap is non-NULL) the composed pane. */
static void run_pipeline(const float *iq, guint frames, double center,
                         Seen *seen, PaneCap *cap, const char *dlog) {
  SkimPipelineConfig cfg = { 0 };
  cfg.chan_bw_hz = 125.0;
  cfg.decode_log_path = dlog;
  SkimPipeline *p = skim_pipeline_new(&cfg);
  skim_pipeline_set_station_cb(p, station_cb, seen);
  if (cap) {
    skim_pipeline_set_text_cb(p, cap_text, cap);
    skim_pipeline_set_over_cb(p, cap_over, cap);
  }
  GError *err = NULL;
  check("offline pipeline starts", skim_pipeline_start_offline(p, &err));
  for (guint at = 0; at < frames; at += 4800) {
    const guint n = MIN(4800u, frames - at);
    skim_pipeline_feed(p, iq + 2 * at, n, RATE, center);
  }
  skim_pipeline_stop(p);
  skim_pipeline_free(p);
}

static int cmp_str(gconstpointer a, gconstpointer b) {
  return strcmp((const char *)a, (const char *)b);
}

static char *seen_key(const Seen *s) {
  GPtrArray *v = g_ptr_array_new();
  for (guint i = 0; i < s->n; i++) { g_ptr_array_add(v, (gpointer)s->calls[i]); }
  g_ptr_array_sort_values(v, cmp_str);
  GString *k = g_string_new(NULL);
  for (guint i = 0; i < v->len; i++) {
    g_string_append(k, g_ptr_array_index(v, i));
    g_string_append_c(k, '|');
  }
  g_ptr_array_free(v, TRUE);
  return g_string_free(k, FALSE);
}

static guint count_occ(const char *hay, const char *needle) {
  guint n = 0;
  for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) {
    n++;
  }
  return n;
}

/* Concatenate the dlog's aux increments near f0 (kHz match to 0.05). */
static char *dlog_aux_concat(const char *path, double f0) {
  char *body = NULL;
  if (!g_file_get_contents(path, &body, NULL, NULL))
    return g_strdup("");
  GString *out = g_string_new(NULL);
  char **lines = g_strsplit(body, "\n", -1);
  for (char **l = lines; *l; l++) {
    char *aux = strstr(*l, "   aux        |");
    if (!aux)
      continue;
    double khz = 0.0;
    if (sscanf(*l, "%*s %lf", &khz) != 1 ||
        fabs(khz * 1000.0 - f0) > 50.0)
      continue;
    char *txt = aux + strlen("   aux        |");
    char *bar = strrchr(txt, '|');
    if (bar) { g_string_append_len(out, txt, bar - txt); }
  }
  g_strfreev(lines);
  g_free(body);
  return g_string_free(out, FALSE);
}

static void run_integration(const char *blob) {
  printf("--- integration: offline pipeline, streamed reader owns the over ---\n");
  g_setenv("SKIM_CW_V2", "1", TRUE);

  const double center = 14050000.0, foff = 11990.0;
  const double f0 = center + foff;
  GRand *rng = g_rand_new_with_seed(20260718);
  GString *pay = g_string_new(NULL);
  for (guint i = 0; i < 6; i++) {
    if (i) { g_string_append_c(pay, ' '); }
    g_string_append(pay, "CQ TEST OK1BR OK1BR");
  }
  GArray *env = gen_env(pay->str, 24, RATE);
  shape_env(env, RATE);
  guint  frames;
  float *iq = synth_one(env, foff, 0.5, 20.0, rng, &frames);

  /* Baseline: reader OFF. */
  g_unsetenv("SKIM_CW_READER");
  Seen off = { .n = 0 };
  run_pipeline(iq, frames, center, &off, NULL, NULL);
  gboolean got_call = FALSE;
  for (guint i = 0; i < off.n; i++) {
    if (strcmp(off.calls[i], "OK1BR") == 0) { got_call = TRUE; }
  }
  check("baseline (reader off) tracks OK1BR", got_call);

  /* Reader ON, twice — the pane must compose and the runs must agree. */
  g_setenv("SKIM_CW_READER", blob, TRUE);
  char *dlog = g_strdup_printf("%s/skimmer-pane-test-%d.log", g_get_tmp_dir(),
                               (int)getpid());
  char *pane1 = NULL;
  char *close1 = NULL;
  for (guint run = 0; run < 2; run++) {
    Seen on = { .n = 0 };
    PaneCap cap = { .f0 = f0,
                    .pl = skim_pane_log_new(),
                    .close_text = g_string_new(NULL) };
    run_pipeline(iq, frames, center, &on, &cap, run == 0 ? dlog : NULL);
    if (run == 0) {
      check("ops arrived: over OPENed", cap.opens >= 1);
      check("reader words committed mid-over (SETs)", cap.sets >= 3);
      check("over CLOSEd", cap.closes >= 1);
      check("no erase past the shown text", !cap.bad_erase);
      check("final prefix within text", !cap.bad_final);
      check("no newline reached the pane", !cap.saw_nl);
      check("reader text carries the calls",
            count_occ(cap.close_text->str, "OK1BR") >= 8);
      check("no live region after the run",
            skim_pane_log_over_len(cap.pl) == 0);
      const char *pane = skim_pane_log_text(cap.pl);
      check("pane = the reader's over exactly once (no doubled draft)",
            g_str_has_prefix(pane, cap.close_text->str) &&
                skim_pane_log_len(cap.pl) <=
                    cap.close_text->len + 8);
      check("the over separator follows",
            strstr(pane + cap.close_text->len, "\xC2\xB7") != NULL);
      char *aux = dlog_aux_concat(dlog, f0);
      check("decode log increments == the sealed over",
            strcmp(aux, cap.close_text->str) == 0);
      g_free(aux);
      char *key_on = seen_key(&on), *key_off = seen_key(&off);
      check("station table identical to reader-off (extractor isolation)",
            strcmp(key_on, key_off) == 0);
      g_free(key_on);
      g_free(key_off);
      pane1  = g_strdup(pane);
      close1 = g_strdup(cap.close_text->str);
      printf("       over: |%s|\n", close1);
    } else {
      check("deterministic: run 2 pane identical",
            strcmp(pane1, skim_pane_log_text(cap.pl)) == 0);
      check("deterministic: run 2 close identical",
            strcmp(close1, cap.close_text->str) == 0);
    }
    skim_pane_log_free(cap.pl);
    g_string_free(cap.close_text, TRUE);
  }
  unlink(dlog);
  g_free(dlog);
  g_free(pane1);
  g_free(close1);
  g_free(iq);
  g_array_free(env, TRUE);
  g_string_free(pay, TRUE);
  g_rand_free(rng);
  g_unsetenv("SKIM_CW_READER");
  g_unsetenv("SKIM_CW_V2");
}

int main(void) {
  printf("=== hybrid pane gate (phase B, offline) ===\n");
  const char *blob = g_getenv("SKIM_READER_BLOB");
  run_pane_log_units();
  if (blob && g_file_test(blob, G_FILE_TEST_EXISTS)) {
    run_stream_positions(blob);
    run_integration(blob);
  } else {
    printf("--- SKIM_READER_BLOB not set — stream/integration SKIPPED ---\n");
    fails++;                             /* the gate must not silently pass  */
  }
  printf("=== %d checks, %d failures ===\n", checks, fails);
  return fails ? 1 : 0;
}
