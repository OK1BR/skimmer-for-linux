/*
 * skimmer-split-test — offline gate for the per-channel tone splitter.
 *
 * Two CW stations inside ONE 125 Hz channel beat in the envelope and mutate
 * both calls (live-caught 2026-07-15, the 14036 slot). The splitter must:
 *   - NOT split a lone hard-keyed carrier against its own keying sidebands
 *     (50 % duty puts the first pair only ~4 dB under the line),
 *   - split Δf = 50 Hz and Δf = 30 Hz pairs — both texts copy, slot mixes
 *     land on the true carriers, no contested flag,
 *   - flag Δf = 15 Hz as CONTESTED (below the keying bandwidth no linear
 *     filter separates them) and never split,
 *   - collapse when one station leaves (TTL) and re-engage on its return,
 *   - integration: two stations in one channel through the WHOLE offline
 *     pipeline with SKIM_TONE_SPLIT=1 — both callsigns reach the station
 *     tracker, no beat mutations do.
 *
 * Synthesis helpers follow src/cw_test.c (same house style).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/decode_cw.h"
#include "engine/pipeline.h"
#include "engine/tone_split.h"

#define RATE 250.0

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/* --- CW synthesis (src/cw_test.c pattern) -------------------------------------- */

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

static void key_run(GArray *env, double samps, float on) {
  guint n = (guint)(samps + 0.5);
  for (guint i = 0; i < n; i++) { g_array_append_val(env, on); }
}

/* text → keyed envelope at `rate`, dit = 1.2/wpm s, leading "VVV " warm-up. */
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
  key_run(env, 1.5 * rate, 0.0f);
  g_free(text);
  return env;
}

/* 5 ms raised-cosine keying edges (real transmitters shape their keying). */
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

/* Repeat "payload" n times, space-joined. */
static char *rep(const char *payload, guint n) {
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < n; i++) {
    if (i) { g_string_append_c(s, ' '); }
    g_string_append(s, payload);
  }
  return g_string_free(s, FALSE);
}

/* Zero-pad (or truncate) an envelope to exactly len samples. */
static void env_fit(GArray *env, guint len) {
  if (env->len > len) {
    g_array_set_size(env, len);
  } else {
    const float z = 0.0f;
    while (env->len < len) { g_array_append_val(env, z); }
  }
}

/* One keyed carrier whose frequency starts +chirp_hz high at every key-down
 * and settles within ~15 ms (a chirpy TX): the keying sidebands go
 * ASYMMETRIC, which is what defeats the symmetric mirror test (live-caught
 * 2026-07-16 — EA1EYL's drifty keying grew a phantom slot on his own lower
 * sidebands). Whatever spurious lines this paints, they exist only WHILE
 * the carrier keys — the OFF-gate must refuse them slots and clear the
 * contested flag once it has data. */
static float *synth_chirpy(const GArray *env, double f0, double amp,
                           double chirp_hz, double snr_db, GRand *rng,
                           guint *out_frames) {
  const guint n = env->len;
  const double sigma = 0.5 / (sqrt(2.0) * pow(10.0, snr_db / 20.0));
  float *iq = g_new(float, 2 * n);
  double ph = 0.0;
  double mark_age = 1e9;
  for (guint i = 0; i < n; i++) {
    const double e = g_array_index(env, float, i);
    mark_age = e > 0.5 ? mark_age + 1.0 : 0.0;
    const double f = f0 + chirp_hz * exp(-mark_age / 3.0);
    ph += 2.0 * G_PI * f / RATE;
    iq[2 * i]     = (float)(amp * e * cos(ph) + sigma * gauss(rng));
    iq[2 * i + 1] = (float)(amp * e * sin(ph) + sigma * gauss(rng));
  }
  *out_frames = n;
  return iq;
}

/* Two independently keyed carriers + complex AWGN (σ per component against
 * the 0.5 reference amplitude, like cw_test). Returns interleaved IQ. */
static float *synth_two(const GArray *ea, double fa, double aa,
                        const GArray *eb, double fb, double ab, double rate,
                        double snr_db, GRand *rng, guint *out_frames) {
  const guint n = MAX(ea->len, eb->len);
  const double sigma = 0.5 / (sqrt(2.0) * pow(10.0, snr_db / 20.0));
  float *iq = g_new(float, 2 * n);
  double pa = 0.0, pb = 0.0;
  const double da = 2.0 * G_PI * fa / rate, db = 2.0 * G_PI * fb / rate;
  for (guint i = 0; i < n; i++) {
    const double va = aa * (i < ea->len ? g_array_index(ea, float, i) : 0.0f);
    const double vb = ab * (i < eb->len ? g_array_index(eb, float, i) : 0.0f);
    iq[2 * i]     = (float)(va * cos(pa) + vb * cos(pb) + sigma * gauss(rng));
    iq[2 * i + 1] = (float)(va * sin(pa) + vb * sin(pb) + sigma * gauss(rng));
    pa += da;
    pb += db;
  }
  *out_frames = n;
  return iq;
}

/* --- fuzzy matching (src/cw_test.c) --------------------------------------------- */

static int fuzzy_dist(const char *txt, const char *pat) {
  size_t n = strlen(txt), m = strlen(pat);
  int *dp = g_new(int, n + 1), *nx = g_new(int, n + 1);
  for (size_t j = 0; j <= n; j++) { dp[j] = 0; }
  for (size_t i = 1; i <= m; i++) {
    nx[0] = (int)i;
    for (size_t j = 1; j <= n; j++) {
      int sub = dp[j - 1] + (pat[i - 1] != txt[j - 1]);
      nx[j] = MIN(sub, MIN(dp[j] + 1, nx[j - 1] + 1));
    }
    memcpy(dp, nx, (n + 1) * sizeof(int));
  }
  int best = dp[0];
  for (size_t j = 1; j <= n; j++) { best = MIN(best, dp[j]); }
  g_free(dp);
  g_free(nx);
  return best;
}

static void show(const char *label, const char *got) {
  printf("       %-12s \"%s\"\n", label, got);
}

/* --- splitter driver (mirrors the pipeline's slot glue) -------------------------- */

typedef struct {
  GString *txt[SKIM_TONE_SPLIT_MAX];  /* per final slot index                */
  double   hz[SKIM_TONE_SPLIT_MAX];   /* last slot mix                       */
  guint    nslots;                    /* at end of run                       */
  guint    max_slots;
  double   t_split;                   /* first slots>1 (s), <0 = never       */
  gboolean contested_seen;
  guint8   slots_at[512];             /* topology per second (capped)        */
  guint8   contested_at[512];         /* any slot contested that second      */
  guint8   focus_at[512];             /* 1 slot AND a nonzero mix = FOCUS    */
} SplitRun;

/* TRUE if any second in [from, to) had a contested slot. */
static gboolean contested_between(const SplitRun *r, guint from, guint to) {
  for (guint s = from; s < MIN(to, (guint)G_N_ELEMENTS(r->contested_at)); s++) {
    if (r->contested_at[s]) { return TRUE; }
  }
  return FALSE;
}

static void run_split(const float *iq, guint frames,
                      const SkimDecodeBackend *cw, double focus_fc,
                      SplitRun *r) {
  SkimToneSplit *ts = skim_tone_split_new(RATE);
  if (focus_fc > 0) { skim_tone_split_set_focus(ts, focus_fc); }
  gpointer dec[SKIM_TONE_SPLIT_MAX] = { NULL };
  guint    gen[SKIM_TONE_SPLIT_MAX] = { 0 };
  memset(r, 0, sizeof(*r));
  r->t_split = -1.0;
  for (guint s = 0; s < SKIM_TONE_SPLIT_MAX; s++) {
    r->txt[s] = g_string_new(NULL);
  }

  float out[64 * 2];
  SkimDecode d;
  for (guint at = 0; at < frames; at += 64) {
    const guint n = MIN(64u, frames - at);
    skim_tone_split_push(ts, iq + 2 * at, n);
    const guint ns = skim_tone_split_slots(ts);
    if (ns > 1 && r->t_split < 0) { r->t_split = at / RATE; }
    r->max_slots = MAX(r->max_slots, ns);
    const guint sec = (guint)(at / RATE);
    if (sec < G_N_ELEMENTS(r->slots_at)) {
      r->slots_at[sec] = (guint8)ns;
      r->focus_at[sec] =
          (ns == 1 && skim_tone_split_slot_hz(ts, 0) != 0.0) ? 1 : 0;
    }
    for (guint s = 0; s < ns; s++) {
      const guint g = skim_tone_split_slot_gen(ts, s);
      if (g != gen[s] || !dec[s]) {           /* the pipeline's slot_sync    */
        if (dec[s]) { cw->channel_free(dec[s]); }
        dec[s] = cw->channel_new(RATE);
        g_string_truncate(r->txt[s], 0);
        gen[s] = g;
      }
      if (skim_tone_split_slot_contested(ts, s)) {
        r->contested_seen = TRUE;
        if (sec < G_N_ELEMENTS(r->contested_at)) { r->contested_at[sec] = 1; }
      }
      guint m;
      while ((m = skim_tone_split_read(ts, s, out, 64)) > 0) {
        if (cw->process(dec[s], out, m, &d)) {
          g_string_append(r->txt[s], d.text);
        }
      }
      r->hz[s] = skim_tone_split_slot_hz(ts, s);
    }
  }
  r->nslots = skim_tone_split_slots(ts);
  for (guint s = 0; s < SKIM_TONE_SPLIT_MAX; s++) {
    if (dec[s]) { cw->channel_free(dec[s]); }
  }
  skim_tone_split_free(ts);
}

static void run_free(SplitRun *r) {
  for (guint s = 0; s < SKIM_TONE_SPLIT_MAX; s++) {
    g_string_free(r->txt[s], TRUE);
  }
}

/* The final slot nearest an expected carrier offset. -1 = none within 6 Hz. */
static gint slot_near(const SplitRun *r, double hz) {
  gint best = -1;
  double bd = 6.0;
  for (guint s = 0; s < r->nslots; s++) {
    if (fabs(r->hz[s] - hz) < bd) {
      bd = fabs(r->hz[s] - hz);
      best = (gint)s;
    }
  }
  return best;
}

/* --- suite (runs once per decode backend) ---------------------------------------- */

static void run_suite(const SkimDecodeBackend *cw) {
  printf("--- backend: %s ---\n", cw->name);
  GRand *rng = g_rand_new_with_seed(20260716);          /* deterministic */

  { /* 1. a lone HARD-KEYED carrier must not split against its sidebands */
    printf(" [1] lone hard-keyed carrier (sideband immunity)\n");
    char   *p = rep("CQ CQ DE OK1BR OK1BR K", 3);
    GArray *ea = gen_env(p, 28, RATE);                  /* no shaping: hard  */
    GArray *eb = g_array_new(FALSE, FALSE, sizeof(float));
    guint   frames;
    float  *iq = synth_two(ea, 15.0, 0.5, eb, 0.0, 0.0, RATE, 25, rng,
                           &frames);
    SplitRun r;
    run_split(iq, frames, cw, 0.0, &r);
    show("slot0", r.txt[0]->str);
    check("never split", r.max_slots == 1);
    check("never contested", !r.contested_seen);
    check("passthrough copies exactly", fuzzy_dist(r.txt[0]->str, "OK1BR OK1BR") == 0);
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(p);
  }

  { /* 1b. chirpy lone carrier — asymmetric sidebands defeat the mirror
     *     test; the OFF-gate must refuse them anyway (live 2026-07-16). */
    printf(" [1b] chirpy lone carrier (asymmetric sidebands)\n");
    char   *p = rep("CQ CQ DE OK1BR OK1BR K", 5);
    GArray *ea = gen_env(p, 21, RATE);
    guint   frames;
    float  *iq = synth_chirpy(ea, 37.0, 0.5, 6.0, 25, rng, &frames);
    SplitRun r;
    run_split(iq, frames, cw, 0.0, &r);
    show("slot0", r.txt[0]->str);
    check("chirpy: never split", r.max_slots == 1);
    check("chirpy: contested clears once the OFF-gate has data",
          !contested_between(&r, 35, 512));
    check("chirpy: copies exactly", fuzzy_dist(r.txt[0]->str, "OK1BR OK1BR K") == 0);
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_free(p);
  }

  { /* 2. Δf = 50 Hz — the bread-and-butter split. The OFF-gate needs ~8
     *    dark windows of the primary before it can VERIFY the second
     *    carrier (word gaps supply them), so engage lands ~20-25 s in;
     *    until then the channel reads contested — candidates hold. */
    printf(" [2] two stations, Δf = 50 Hz (−20 / +30)\n");
    char   *pa = rep("CQ TEST OK1BR OK1BR", 6);
    char   *pb = rep("CQ DE 9A5K 9A5K K", 7);
    GArray *ea = gen_env(pa, 22, RATE);
    GArray *eb = gen_env(pb, 27, RATE);
    shape_env(ea, RATE);
    shape_env(eb, RATE);
    guint  frames;
    float *iq = synth_two(ea, -20.0, 0.5, eb, 30.0, 0.35, RATE, 20, rng,
                          &frames);
    SplitRun r;
    run_split(iq, frames, cw, 0.0, &r);
    check("split engaged", r.t_split >= 0);
    check("engaged within 30 s", r.t_split >= 0 && r.t_split <= 30.0);
    check("two slots at end", r.nslots == 2);
    check("not contested once split", r.t_split >= 0 &&
          !contested_between(&r, (guint)r.t_split + 3, 512));
    const gint sa = slot_near(&r, -20.0), sb = slot_near(&r, 30.0);
    check("slot mixes on both carriers (±6 Hz)", sa >= 0 && sb >= 0 && sa != sb);
    if (sa >= 0 && sb >= 0) {
      show("slot A", r.txt[sa]->str);
      show("slot B", r.txt[sb]->str);
      check("station A copies (≤1 err)",
            fuzzy_dist(r.txt[sa]->str, "CQ TEST OK1BR OK1BR") <= 1);
      check("station B copies (≤1 err)",
            fuzzy_dist(r.txt[sb]->str, "CQ DE 9A5K 9A5K K") <= 1);
    }
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(pa);
    g_free(pb);
  }

  { /* 3. Δf = 30 Hz — narrow cutoffs, soft edges, still two copies */
    printf(" [3] two stations, Δf = 30 Hz (−5 / +25)\n");
    char   *pa = rep("CQ TEST OK1BR OK1BR", 6);
    char   *pb = rep("CQ DE 9A5K 9A5K K", 8);
    GArray *ea = gen_env(pa, 20, RATE);
    GArray *eb = gen_env(pb, 25, RATE);
    shape_env(ea, RATE);
    shape_env(eb, RATE);
    guint  frames;
    float *iq = synth_two(ea, -5.0, 0.5, eb, 25.0, 0.4, RATE, 18, rng,
                          &frames);
    SplitRun r;
    run_split(iq, frames, cw, 0.0, &r);
    check("split engaged", r.t_split >= 0);
    check("two slots at end", r.nslots == 2);
    const gint sa = slot_near(&r, -5.0), sb = slot_near(&r, 25.0);
    check("slot mixes on both carriers (±6 Hz)", sa >= 0 && sb >= 0 && sa != sb);
    if (sa >= 0 && sb >= 0) {
      show("slot A", r.txt[sa]->str);
      show("slot B", r.txt[sb]->str);
      check("station A copies (≤2 err)",
            fuzzy_dist(r.txt[sa]->str, "CQ TEST OK1BR OK1BR") <= 2);
      check("station B copies (≤2 err)",
            fuzzy_dist(r.txt[sb]->str, "CQ DE 9A5K 9A5K K") <= 2);
    }
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(pa);
    g_free(pb);
  }

  { /* 4. Δf = 15 Hz — below the keying bandwidth: contested, never split */
    printf(" [4] two stations, Δf = 15 Hz (0 / +15) → contested\n");
    char   *pa = rep("CQ TEST OK1BR OK1BR", 4);
    char   *pb = rep("CQ DE 9A5K 9A5K K", 6);
    GArray *ea = gen_env(pa, 18, RATE);
    GArray *eb = gen_env(pb, 28, RATE);
    shape_env(ea, RATE);
    shape_env(eb, RATE);
    guint  frames;
    float *iq = synth_two(ea, 0.0, 0.5, eb, 15.0, 0.45, RATE, 20, rng,
                          &frames);
    SplitRun r;
    run_split(iq, frames, cw, 0.0, &r);
    check("never split", r.max_slots == 1);
    check("contested flagged", r.contested_seen);
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(pa);
    g_free(pb);
  }

  { /* 4b. Δf = 21 Hz — the 20–22 Hz dead zone (live-caught 2026-07-16 on
     *     14014.4: neither contested nor split, beat garbage flowed free).
     *     One bar now: anything below the engage threshold is CONTESTED. */
    printf(" [4b] two stations, Δf = 21 Hz (0 / +21) → contested (dead zone)\n");
    char   *pa = rep("CQ TEST OK1BR OK1BR", 4);
    char   *pb = rep("CQ DE 9A5K 9A5K K", 6);
    GArray *ea = gen_env(pa, 22, RATE);
    GArray *eb = gen_env(pb, 38, RATE);
    shape_env(ea, RATE);
    shape_env(eb, RATE);
    guint  frames;
    float *iq = synth_two(ea, 0.0, 0.5, eb, 21.0, 0.45, RATE, 20, rng,
                          &frames);
    SplitRun r;
    run_split(iq, frames, cw, 0.0, &r);
    check("dead zone: never split", r.max_slots == 1);
    check("dead zone: contested flagged", r.contested_seen);
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(pa);
    g_free(pb);
  }

  { /* 5. collapse on TTL, re-engage on return (a QSO pair with a long over) */
    printf(" [5] slot TTL: B leaves for 105 s, comes back\n");
    char   *pa = rep("CQ TEST OK1BR OK1BR", 23);        /* A runs throughout */
    GArray *ea = gen_env(pa, 24, RATE);
    shape_env(ea, RATE);
    char   *pb1 = rep("CQ DE 9A5K 9A5K K", 7);
    GArray *eb = gen_env(pb1, 26, RATE);                /* ~50 s of B        */
    shape_env(eb, RATE);
    env_fit(eb, (guint)(50.0 * RATE));
    env_fit(eb, eb->len + (guint)(105.0 * RATE));       /* 105 s silence     */
    GArray *eb2 = gen_env(pb1, 26, RATE);               /* B returns @155 s  */
    shape_env(eb2, RATE);
    g_array_append_vals(eb, eb2->data, eb2->len);
    g_array_free(eb2, TRUE);
    env_fit(ea, eb->len);
    guint  frames;
    float *iq = synth_two(ea, -18.0, 0.5, eb, 22.0, 0.4, RATE, 20, rng,
                          &frames);
    SplitRun r;
    run_split(iq, frames, cw, 0.0, &r);
    check("split engaged (once the OFF-gate warmed)",
          r.t_split >= 0 && r.t_split <= 30.0);
    /* B falls silent at ~50 s; TTL 90 s → collapse lands ≈ 141–143 s. */
    check("still split at 135 s", r.slots_at[135] == 2);
    check("collapsed by 152 s", r.slots_at[152] == 1);
    /* B keys again at ~155 s; the gate is warm → re-engage in seconds. */
    check("re-engaged by 168 s", r.slots_at[168] == 2);
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(pa);
    g_free(pb1);
  }

  { /* 6. FOCUS: a weak lone carrier — the narrow slot buys the dB that the
     *    wide 250 Hz envelope wastes on noise. Same IQ through both paths:
     *    focus must copy what the passthrough loses. */
    printf(" [6] FOCUS: weak lone carrier at +37 Hz (wide vs narrow)\n");
    char   *p = rep("CQ DE OK1BR OK1BR K", 8);
    GArray *ea = gen_env(p, 24, RATE);
    shape_env(ea, RATE);
    GArray *eb = g_array_new(FALSE, FALSE, sizeof(float));
    guint   frames;
    float  *iq = synth_two(ea, 37.0, 0.5, eb, 0.0, 0.0, RATE, 6, rng,
                           &frames);
    SplitRun rw, rf;
    run_split(iq, frames, cw, 0.0, &rw);              /* wide passthrough    */
    run_split(iq, frames, cw, 25.0, &rf);             /* focus armed         */
    show("wide", rw.txt[0]->str);
    show("focus", rf.txt[0]->str);
    const int dw = fuzzy_dist(rw.txt[0]->str, "OK1BR OK1BR K");
    const int df = fuzzy_dist(rf.txt[0]->str, "OK1BR OK1BR K");
    check("unarmed run stays passthrough", rw.hz[0] == 0.0);
    check("focus engaged on the carrier (±6 Hz)", fabs(rf.hz[0] - 37.0) < 6.0);
    check("focus never splits a lone carrier", rf.max_slots == 1);
    check("focus copies (≤1 err)", df <= 1);
    check("focus strictly better than wide", df < dw);
    run_free(&rw);
    run_free(&rf);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(p);
  }

  { /* 6b. FOCUS release: the station leaves; past the TTL the channel must
     *     return to the verbatim passthrough (fresh detection for whoever
     *     shows up next — possibly elsewhere in the channel). */
    printf(" [6b] FOCUS TTL: station leaves for 110 s\n");
    char   *p = rep("CQ DE OK1BR OK1BR K", 4);
    GArray *ea = gen_env(p, 24, RATE);
    shape_env(ea, RATE);
    env_fit(ea, ea->len + (guint)(110.0 * RATE));     /* long silence        */
    GArray *eb = g_array_new(FALSE, FALSE, sizeof(float));
    guint   frames;
    float  *iq = synth_two(ea, 37.0, 0.5, eb, 0.0, 0.0, RATE, 20, rng,
                           &frames);
    SplitRun r;
    run_split(iq, frames, cw, 25.0, &r);
    check("focus engaged while keying", r.focus_at[20] == 1);
    check("focus released after TTL (passthrough back)", r.hz[0] == 0.0);
    check("one slot throughout", r.max_slots == 1);
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(p);
  }

  { /* 7. FOCUS → SPLIT: a second station appears next to a focused one; the
     *    focus slot must keep its stream (no reset) while a real split
     *    spawns for the newcomer. */
    printf(" [7] FOCUS → SPLIT: B (+30 Hz) joins a focused A (−20 Hz) at 40 s\n");
    char   *pa = rep("CQ TEST OK1BR OK1BR", 12);
    GArray *ea = gen_env(pa, 22, RATE);
    shape_env(ea, RATE);
    char   *pb = rep("CQ DE 9A5K 9A5K K", 8);
    GArray *eb = g_array_new(FALSE, FALSE, sizeof(float));
    key_run(eb, 40.0 * RATE, 0.0f);                   /* B silent until 40 s */
    GArray *eb1 = gen_env(pb, 27, RATE);
    shape_env(eb1, RATE);
    g_array_append_vals(eb, eb1->data, eb1->len);
    g_array_free(eb1, TRUE);
    env_fit(ea, eb->len);
    guint  frames;
    float *iq = synth_two(ea, -20.0, 0.5, eb, 30.0, 0.4, RATE, 20, rng,
                          &frames);
    SplitRun r;
    run_split(iq, frames, cw, 25.0, &r);
    check("focused on A before B keys", r.focus_at[30] == 1);
    check("split engaged after B appears", r.t_split >= 40.0 && r.t_split <= 75.0);
    check("two slots at end", r.nslots == 2);
    const gint sa = slot_near(&r, -20.0), sb = slot_near(&r, 30.0);
    check("slot mixes on both carriers (±6 Hz)", sa >= 0 && sb >= 0 && sa != sb);
    if (sa >= 0 && sb >= 0) {
      show("slot A", r.txt[sa]->str);
      show("slot B", r.txt[sb]->str);
      check("A rode through the transition (≤1 err)",
            fuzzy_dist(r.txt[sa]->str, "CQ TEST OK1BR OK1BR") <= 1);
      check("B copies (≤1 err)",
            fuzzy_dist(r.txt[sb]->str, "CQ DE 9A5K 9A5K K") <= 1);
    }
    run_free(&r);
    g_free(iq);
    g_array_free(ea, TRUE);
    g_array_free(eb, TRUE);
    g_free(pa);
    g_free(pb);
  }

  g_rand_free(rng);
}

/* --- integration: the whole offline pipeline with the splitter armed ------------- */

typedef struct {
  char   calls[16][24];
  double hz[16];
  guint  n;
} Seen;

static void station_cb(const SkimStation *st, gpointer user) {
  Seen *seen = user;
  for (guint i = 0; i < seen->n; i++) {
    if (strcmp(seen->calls[i], st->call) == 0) {
      seen->hz[i] = st->freq_hz;
      return;
    }
  }
  if (seen->n < G_N_ELEMENTS(seen->calls)) {
    g_strlcpy(seen->calls[seen->n], st->call, sizeof(seen->calls[0]));
    seen->hz[seen->n] = st->freq_hz;
    seen->n++;
  }
}

/* Two stations in one channel through the whole pipeline. Runs once armed
 * via SKIM_TONE_SPLIT and once via SKIM_TONE_FOCUS — focus implies the
 * splitter, and a genuine pair must still split with only focus armed. */
static void run_integration(const char *env) {
  printf("--- integration: offline pipeline, %s=1 ---\n", env);
  g_setenv(env, "1", TRUE);

  const double rate = 48000.0, center = 14050000.0;
  /* Channel 96 sits at +12 kHz; carriers at −10 Hz and +40 Hz inside it. */
  const double fa = 11990.0, fb = 12040.0;
  GRand *rng = g_rand_new_with_seed(20260716);
  char   *pa = rep("CQ TEST OK1BR OK1BR", 6);
  char   *pb = rep("CQ DE 9A5K 9A5K K", 8);
  GArray *ea = gen_env(pa, 22, rate);
  GArray *eb = gen_env(pb, 27, rate);
  shape_env(ea, rate);
  shape_env(eb, rate);
  guint  frames;
  float *iq = synth_two(ea, fa, 0.5, eb, fb, 0.35, rate, 20, rng, &frames);

  SkimPipelineConfig cfg = { 0 };
  cfg.chan_bw_hz = 125.0;
  SkimPipeline *p = skim_pipeline_new(&cfg);
  Seen seen = { .n = 0 };
  skim_pipeline_set_station_cb(p, station_cb, &seen);
  GError *err = NULL;
  check("offline pipeline starts", skim_pipeline_start_offline(p, &err));
  for (guint at = 0; at < frames; at += 4800) {
    const guint n = MIN(4800u, frames - at);
    skim_pipeline_feed(p, iq + 2 * at, n, rate, center);
  }
  skim_pipeline_stop(p);

  gboolean got_a = FALSE, got_b = FALSE, mutant = FALSE;
  for (guint i = 0; i < seen.n; i++) {
    printf("       station %-10s %.1f Hz\n", seen.calls[i], seen.hz[i]);
    if (strcmp(seen.calls[i], "OK1BR") == 0 &&
        fabs(seen.hz[i] - (center + fa)) < 20.0) {
      got_a = TRUE;
    } else if (strcmp(seen.calls[i], "9A5K") == 0 &&
               fabs(seen.hz[i] - (center + fb)) < 20.0) {
      got_b = TRUE;
    } else if (!g_str_has_prefix("OK1BR", seen.calls[i]) &&
               !g_str_has_prefix("9A5K", seen.calls[i])) {
      mutant = TRUE;                /* truncations fold away; mixes must not */
    }
  }
  check("OK1BR tracked on its carrier", got_a);
  check("9A5K tracked on its carrier", got_b);
  check("no beat mutations reached the tracker", !mutant);

  skim_pipeline_free(p);
  g_free(iq);
  g_array_free(ea, TRUE);
  g_array_free(eb, TRUE);
  g_free(pa);
  g_free(pb);
  g_rand_free(rng);
  g_unsetenv(env);
}

/* A WEAK lone station end-to-end: with focus armed the tracker gets the
 * callsign on the right absolute frequency; the wide baseline loses it at
 * this SNR (that asymmetry IS the milestone). Same IQ both runs. */
static void run_integration_weak(void) {
  printf("--- integration: weak lone station, baseline vs SKIM_TONE_FOCUS=1 ---\n");
  const double rate = 48000.0, center = 14050000.0;
  const double fa = 12037.0;                   /* channel 96, +37 Hz inside  */
  GRand *rng = g_rand_new_with_seed(20260719);
  char   *pa = rep("CQ DE OK1BR OK1BR K", 10);
  GArray *ea = gen_env(pa, 24, rate);
  shape_env(ea, rate);
  GArray *eb = g_array_new(FALSE, FALSE, sizeof(float));
  guint  frames;
  float *iq = synth_two(ea, fa, 0.5, eb, 0.0, 0.0, rate, -17, rng, &frames);

  gboolean tracked[2] = { FALSE, FALSE };      /* [0] baseline, [1] focus    */
  for (guint arm = 0; arm < 2; arm++) {
    if (arm) { g_setenv("SKIM_TONE_FOCUS", "1", TRUE); }
    SkimPipelineConfig cfg = { 0 };
    cfg.chan_bw_hz = 125.0;
    SkimPipeline *p = skim_pipeline_new(&cfg);
    Seen seen = { .n = 0 };
    skim_pipeline_set_station_cb(p, station_cb, &seen);
    GError *err = NULL;
    if (!skim_pipeline_start_offline(p, &err)) { g_clear_error(&err); }
    for (guint at = 0; at < frames; at += 4800) {
      const guint n = MIN(4800u, frames - at);
      skim_pipeline_feed(p, iq + 2 * at, n, rate, center);
    }
    skim_pipeline_stop(p);
    for (guint i = 0; i < seen.n; i++) {
      printf("       %-8s station %-10s %.1f Hz\n", arm ? "focus" : "wide",
             seen.calls[i], seen.hz[i]);
      if (strcmp(seen.calls[i], "OK1BR") == 0 &&
          fabs(seen.hz[i] - (center + fa)) < 20.0) {
        tracked[arm] = TRUE;
      }
    }
    skim_pipeline_free(p);
    if (arm) { g_unsetenv("SKIM_TONE_FOCUS"); }
  }
  check("focus: weak station tracked on its carrier", tracked[1]);
  check("wide baseline loses it at this SNR", !tracked[0]);

  g_free(iq);
  g_array_free(ea, TRUE);
  g_array_free(eb, TRUE);
  g_free(pa);
  g_rand_free(rng);
}

int main(void) {
  printf("=== tone splitter gate (offline, synthetic) ===\n");
  run_suite(skim_decode_cw());
  run_suite(skim_decode_cw_v2());
  run_integration("SKIM_TONE_SPLIT");
  run_integration("SKIM_TONE_FOCUS");
  run_integration_weak();
  printf("=== %d checks, %d failures ===\n", checks, fails);
  return fails ? 1 : 0;
}
