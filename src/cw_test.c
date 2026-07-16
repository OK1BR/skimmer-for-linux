/*
 * skimmer-cw-test — offline gate for the CW decode backend (M3).
 *
 * Synthesizes keyed CW at complex baseband (250 Hz, like a channelizer
 * output): text → ITU element timing → envelope with 5 ms raised-cosine
 * edges → tone at an in-channel offset + AWGN, then checks the decoder:
 *   - clean copy at 15 / 20 / 25 / 35 WPM (exact text),
 *   - WPM, SNR, offset and confidence estimates are sane,
 *   - 12 dB SNR, ±15 % timing jitter (ragged fist) and 10 dB QSB still copy
 *     (small Levenshtein tolerance),
 *   - a mid-stream 18 → 28 WPM speed change re-locks,
 *   - SQUELCH: 20 s of pure noise emits NOT ONE character (RBN-grade rule),
 *   - integration: keyed carrier at 12.018 kHz through the real channelizer,
 *     the right channel decodes the text, a neighbour channel stays silent.
 *
 * Off-air A/B against fldigi / CW Skimmer needs recorded IQ — that is the
 * second half of the M3 gate, run when a capture from the radio exists.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/channelizer.h"
#include "engine/decode_cw.h"

#define RATE 250.0

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/* --- CW synthesis ------------------------------------------------------------ */

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

/* Append `samps` samples of keyed envelope value `on` (with ramps handled by
 * the caller via lerp between elements — we keep it simple: rectangular runs,
 * the 3-tap MA in the decoder plays the role of the shaping). */
static void key_run(GArray *env, double samps, float on, double jitter,
                    GRand *rng) {
  if (jitter > 0) { samps *= 1.0 + g_rand_double_range(rng, -jitter, jitter); }
  guint n = (guint)(samps + 0.5);
  for (guint i = 0; i < n; i++) { g_array_append_val(env, on); }
}

/* text → keyed envelope at `rate`, dit = 1.2/wpm s. Leading "VVV " warms up
 * the decoder's dit bootstrap; trailing 1.5 s of silence flushes the tail.
 * csp_dits/wsp_dits set the operator's char/word spacing (ITU: 3/7 — real
 * fists stretch and squeeze them, which is what the fist model is for). */
static GArray *gen_env_fist(const char *payload, double wpm, double rate,
                            double jitter, GRand *rng, double csp_dits,
                            double wsp_dits) {
  GArray *env = g_array_new(FALSE, FALSE, sizeof(float));
  char   *text = g_strdup_printf("VVV %s", payload);
  double  dit = 1.2 / wpm * rate;
  float   ON = 1.0f, OFF = 0.0f;

  key_run(env, 3 * dit, OFF, 0, rng);
  for (const char *p = text; *p; p++) {
    if (*p == ' ') {
      key_run(env, wsp_dits * dit, OFF, jitter, rng);
      continue;
    }
    const char *m = morse_of(*p);
    if (!m) { continue; }
    for (const char *e = m; *e; e++) {
      key_run(env, *e == '-' ? 3 * dit : dit, ON, jitter, rng);
      if (e[1]) { key_run(env, dit, OFF, jitter, rng); }
    }
    key_run(env, csp_dits * dit, OFF, jitter, rng);
  }
  key_run(env, 1.5 * rate, OFF, 0, rng);
  g_free(text);
  return env;
}

static GArray *gen_env(const char *payload, double wpm, double rate,
                       double jitter, GRand *rng) {
  return gen_env_fist(payload, wpm, rate, jitter, rng, 3.0, 7.0);
}

/* 5 ms raised-cosine keying edges: convolve the rectangular envelope with a
 * normalised Hann kernel. Real transmitters shape their keying — without it
 * the hard edges are key-clicks that leak into neighbouring channels. */
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

/* Gaussian noise via Box-Muller on a seeded GRand. */
static double gauss(GRand *rng) {
  double u1 = 1.0 - g_rand_double(rng), u2 = g_rand_double(rng);
  return sqrt(-2.0 * log(u1)) * cos(2.0 * G_PI * u2);
}

/* The backend under test — the whole suite runs once per backend. */
static const SkimDecodeBackend *g_cw;

/* Run an envelope through the decoder as IQ: tone at `foff` Hz, amplitude
 * 0.5·env, complex AWGN for `snr_db` (per-component σ), optional QSB.
 * Returns the concatenated decoded text; fills last decode info. */
static char *run_decoder(GArray *env, double foff, double snr_db,
                         double qsb_hz, double qsb_depth_db, GRand *rng,
                         SkimDecode *last) {
  const SkimDecodeBackend *cw = g_cw;
  gpointer st = cw->channel_new(RATE);
  GString *txt = g_string_new(NULL);
  const double A = 0.5;
  const double sigma = A / (sqrt(2.0) * pow(10.0, snr_db / 20.0));
  double ph = 0.0, dph = 2.0 * G_PI * foff / RATE;

  float iq[2 * 64];
  SkimDecode d;
  memset(&d, 0, sizeof(d));
  for (guint at = 0; at < env->len; at += 64) {
    guint n = MIN(64u, env->len - at);
    for (guint i = 0; i < n; i++) {
      double a = A * g_array_index(env, float, at + i);
      if (qsb_hz > 0) {
        double t = (double)(at + i) / RATE;
        double depth = pow(10.0, -qsb_depth_db / 20.0);
        a *= depth + (1.0 - depth) * (0.5 + 0.5 * sin(2.0 * G_PI * qsb_hz * t));
      }
      iq[2 * i]     = (float)(a * cos(ph) + sigma * gauss(rng));
      iq[2 * i + 1] = (float)(a * sin(ph) + sigma * gauss(rng));
      ph += dph;
    }
    if (cw->process(st, iq, n, &d)) {
      g_string_append(txt, d.text);
      if (last) { *last = d; }
    }
  }
  cw->channel_free(st);
  return g_string_free(txt, FALSE);
}

/* --- fuzzy matching ----------------------------------------------------------- */

/* Levenshtein distance of `pat` against the best-matching substring of `txt`
 * (free prefix/suffix — the classic fuzzy substring DP). */
static int fuzzy_dist(const char *txt, const char *pat) {
  size_t n = strlen(txt), m = strlen(pat);
  int *dp = g_new(int, n + 1), *nx = g_new(int, n + 1);
  for (size_t j = 0; j <= n; j++) { dp[j] = 0; }        /* free prefix        */
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
  printf("       %-14s \"%s\"\n", label, got);
}

/* The whole classic suite, parametrized by backend. */
static void run_suite(const SkimDecodeBackend *cw) {
  g_cw = cw;
  printf("--- backend: %s ---\n", cw->name);
  GRand *rng = g_rand_new_with_seed(20260715);         /* deterministic */
  SkimDecode last;

  /* -- clean copy across speeds -------------------------------------------- */
  static const double SPEEDS[] = { 15, 20, 25, 35 };
  const char *PAYLOAD = "CQ TEST DE OK1BR OK1BR K";
  for (guint s = 0; s < G_N_ELEMENTS(SPEEDS); s++) {
    GArray *env = gen_env(PAYLOAD, SPEEDS[s], RATE, 0.0, rng);
    char *got = run_decoder(env, 18.0, 30.0, 0, 0, rng, &last);
    char what[64];
    g_snprintf(what, sizeof(what), "clean copy at %.0f WPM (exact)", SPEEDS[s]);
    int d = fuzzy_dist(got, PAYLOAD);
    if (d) { show("decoded:", got); }
    check(what, d == 0);
    if (SPEEDS[s] == 20) {
      check("WPM estimate ≈ 20 (±2)", fabs(last.speed - 20.0) < 2.0);
      if (last.snr_db <= 20.0) {
        printf("       (snr_db = %.1f)\n", last.snr_db);
      }
      check("SNR estimate > 20 dB on a clean trace", last.snr_db > 20.0);
      check("tone offset ≈ +18 Hz (±5)", fabs(last.freq_offset_hz - 18.0) < 5.0);
      check("confidence high on clean keying", last.confidence > 0.7);
    }
    g_array_free(env, TRUE);
    g_free(got);
  }

  /* -- 12 dB SNR ------------------------------------------------------------- */
  GArray *env = gen_env(PAYLOAD, 20, RATE, 0.0, rng);
  char *got = run_decoder(env, 18.0, 12.0, 0, 0, rng, &last);
  show("12 dB SNR:", got);
  check("copies at 12 dB SNR (≤2 errors)", fuzzy_dist(got, PAYLOAD) <= 2);
  g_free(got);

  /* -- ragged fist: ±15 % element jitter -------------------------------------- */
  GArray *envj = gen_env(PAYLOAD, 20, RATE, 0.15, rng);
  got = run_decoder(envj, 18.0, 30.0, 0, 0, rng, &last);
  show("15 % jitter:", got);
  check("copies a ±15 % ragged fist (≤2 errors)", fuzzy_dist(got, PAYLOAD) <= 2);
  check("confidence drops on the ragged fist", last.confidence < 0.9);
  g_array_free(envj, TRUE);
  g_free(got);

  /* -- QSB: 10 dB at 0.4 Hz ---------------------------------------------------- */
  got = run_decoder(env, 18.0, 30.0, 0.4, 10.0, rng, &last);
  show("10 dB QSB:", got);
  check("copies through 10 dB / 0.4 Hz QSB (≤2 errors)",
        fuzzy_dist(got, PAYLOAD) <= 2);
  g_array_free(env, TRUE);
  g_free(got);

  /* -- speed change mid-stream: 18 → 28 WPM ------------------------------------ */
  {
    GArray *a = gen_env("VVV VVV DE OK1BR", 18, RATE, 0, rng);
    GArray *b = gen_env("VVV VVV DE OK1BR", 28, RATE, 0, rng);
    g_array_append_vals(a, b->data, b->len);
    got = run_decoder(a, 18.0, 30.0, 0, 0, rng, &last);
    show("18→28 WPM:", got);
    printf("       (final speed %.1f WPM)\n", last.speed);
    /* Both payload copies must be present; the second proves the re-lock. */
    check("re-locks after a mid-stream 18 → 28 WPM change",
          fuzzy_dist(got, "DE OK1BR") <= 1 &&
          fabs(last.speed - 28.0) < 3.0);
    g_array_free(a, TRUE);
    g_array_free(b, TRUE);
    g_free(got);
  }

  /* -- SQUELCH: pure noise must emit nothing ----------------------------------- */
  {
    GArray *quiet = g_array_new(FALSE, FALSE, sizeof(float));
    float z = 0.0f;
    for (guint i = 0; i < (guint)(20 * RATE); i++) {
      g_array_append_val(quiet, z);
    }
    got = run_decoder(quiet, 0.0, 30.0, 0, 0, rng, NULL);
    check("20 s of pure noise decodes to NOTHING (RBN rule)", got[0] == '\0');
    if (got[0]) { show("noise gave:", got); }
    g_array_free(quiet, TRUE);
    g_free(got);
  }

  /* -- integration: through the real channelizer ------------------------------- */
  {
    SkimChannelizer *bank = skim_channelizer_new(48000.0, 125.0);
    gpointer st_hit  = cw->channel_new(skim_channelizer_out_rate(bank));
    gpointer st_miss = cw->channel_new(skim_channelizer_out_rate(bank));
    GString *hit = g_string_new(NULL), *miss = g_string_new(NULL);

    /* The quiet channel sits 6.7 kHz off the carrier: shaped keying still
     * splatters measurably a few hundred Hz out (a REAL near-channel signal
     * the decoder rightly copies — adjacent-channel ghosts of strong
     * stations are the M5 station tracker's dedup job, not the decoder's),
     * but far channels must hold only noise and stay mute. */
    GArray *e48 = gen_env("DE OK1BR", 20, 48000.0, 0, rng);
    shape_env(e48, 48000.0);
    double ph = 0.0, dph = 2.0 * G_PI * 12018.0 / 48000.0;
    float blk[2 * 1024], chan[2 * 512];
    SkimDecode d;
    for (guint at = 0; at < e48->len; at += 1024) {
      guint n = MIN(1024u, e48->len - at);
      for (guint i = 0; i < n; i++) {
        double a = 0.5 * g_array_index(e48, float, at + i);
        blk[2 * i]     = (float)(a * cos(ph) + 0.01 * gauss(rng));
        blk[2 * i + 1] = (float)(a * sin(ph) + 0.01 * gauss(rng));
        ph += dph;
      }
      skim_channelizer_push(bank, blk, n);
      guint g;
      while ((g = skim_channelizer_read(bank, 96, chan, 512)) > 0) {
        if (cw->process(st_hit, chan, g, &d)) { g_string_append(hit, d.text); }
      }
      while ((g = skim_channelizer_read(bank, 150, chan, 512)) > 0) {
        if (cw->process(st_miss, chan, g, &d)) { g_string_append(miss, d.text); }
      }
    }
    show("via bank ch96:", hit->str);
    check("channelizer → decoder end-to-end copies the text",
          fuzzy_dist(hit->str, "DE OK1BR") <= 1);
    if (miss->len) { show("ch150 gave:", miss->str); }
    check("a noise-only channel through the bank stays silent",
          miss->len == 0);
    cw->channel_free(st_hit);
    cw->channel_free(st_miss);
    g_string_free(hit, TRUE);
    g_string_free(miss, TRUE);
    g_array_free(e48, TRUE);
    skim_channelizer_free(bank);
  }

  g_rand_free(rng);
}

int main(void) {
  printf("=== CW decode gate (offline, synthetic) ===\n");
  run_suite(skim_decode_cw());
  run_suite(skim_decode_cw_v2());

  /* -- QSB mutation cases: what v2 exists for ------------------------------------
   * Deep slow QSB eats the weak elements of a character — the live-caught
   * 9A170NT case ("7" --... losing its trailing dits to a fade → "G" --.).
   * v1's binary threshold drops the faded dits outright; v2's soft Viterbi
   * must carry them. v1 runs uncounted here, printed for comparison. */
  {
    GRand *rng = g_rand_new_with_seed(20260715);
    const char *PAYLOAD = "CQ TEST 9A170NT 9A170NT K";
    SkimDecode last;

    GArray *env = gen_env(PAYLOAD, 25, RATE, 0.0, rng);
    g_cw = skim_decode_cw();
    char *v1 = run_decoder(env, 18.0, 30.0, 0.31, 16.0, rng, &last);
    if (g_getenv("QSB_TRACE")) { g_setenv("SKIM_CW2_TRACE", "1", TRUE); }
    g_cw = skim_decode_cw_v2();
    char *v2 = run_decoder(env, 18.0, 30.0, 0.31, 16.0, rng, &last);
    g_unsetenv("SKIM_CW2_TRACE");
    printf("--- QSB mutation: 16 dB @ 0.31 Hz, 25 WPM ---\n");
    show("v1 (info):", v1);
    show("v2:", v2);
    printf("       v1 dist %d, v2 dist %d\n",
           fuzzy_dist(v1, PAYLOAD), fuzzy_dist(v2, PAYLOAD));
    check("v2 copies through element-eating QSB (≤2 errors)",
          fuzzy_dist(v2, PAYLOAD) <= 2);
    check("v2 is not worse than v1 on the QSB trace",
          fuzzy_dist(v2, PAYLOAD) <= fuzzy_dist(v1, PAYLOAD));
    g_free(v1);
    g_free(v2);
    g_array_free(env, TRUE);

    /* Sub-dit flutter dropouts — the in-dash notch that splits a dash in
     * two (v1's admitted limit). 35 ms at −18 dB every 700 ms: shorter
     * than any real inter-element space, so a decoder CAN tell it apart.
     * (A dropout a full dit long is amplitude-indistinguishable from real
     * keying — no envelope decoder recovers that.) */
    env = gen_env(PAYLOAD, 22, RATE, 0.0, rng);
    for (guint at = 0; at < env->len; at++) {
      double tsec = (double)at / RATE;
      double ph = fmod(tsec, 0.7);
      if (ph < 0.035) {
        float *v = &g_array_index(env, float, at);
        *v *= 0.126f;                          /* −18 dB notch               */
      }
    }
    g_cw = skim_decode_cw();
    v1 = run_decoder(env, 18.0, 30.0, 0, 0, rng, &last);
    g_cw = skim_decode_cw_v2();
    v2 = run_decoder(env, 18.0, 30.0, 0, 0, rng, &last);
    printf("--- flutter notches: 35 ms / -18 dB every 0.7 s, 22 WPM ---\n");
    show("v1 (info):", v1);
    show("v2:", v2);
    printf("       v1 dist %d, v2 dist %d\n",
           fuzzy_dist(v1, PAYLOAD), fuzzy_dist(v2, PAYLOAD));
    check("v2 copies through in-dash dropout notches (≤2 errors)",
          fuzzy_dist(v2, PAYLOAD) <= 2);
    g_free(v1);
    g_free(v2);
    g_array_free(env, TRUE);
    g_rand_free(rng);
  }

  /* -- fist model: spacing is personal ---------------------------------------------
   * EA1EYL (live 2026-07-16) stretches his char gaps to ~5 dits when
   * calling CQ — the fixed 3/7-dit priors filed the C–Q gap as a WORD
   * space, "C Q" matched no marker and the spot never fired. v2 must learn
   * the operator's own two space centres and put "CQ" back together; the
   * counter-case guards the other direction (a tight runner's 5-dit word
   * gaps must NOT collapse into char gaps once the model adapts). v1 keeps
   * its fixed 5.5-dit boundary and is printed for info only. */
  {
    GRand *rng = g_rand_new_with_seed(20260716);
    SkimDecode last;

    const char *CALL = "CQ CQ DE EA1EYL EA1EYL K";
    char *loop = g_strdup_printf("%s %s %s", CALL, CALL, CALL);
    GArray *env = gen_env_fist(loop, 24, RATE, 0.05, rng, 5.1, 9.8);
    g_cw = skim_decode_cw();
    char *v1 = run_decoder(env, 12.0, 25.0, 0, 0, rng, &last);
    g_cw = skim_decode_cw_v2();
    char *v2 = run_decoder(env, 12.0, 25.0, 0, 0, rng, &last);
    printf("--- stretched fist: char gaps 5.1 dits, word gaps 9.8 (24 WPM) ---\n");
    show("v1 (info):", v1);
    show("v2:", v2);
    printf("       v1 dist %d, v2 dist %d\n",
           fuzzy_dist(v1, CALL), fuzzy_dist(v2, CALL));
    check("v2 learns the stretched fist — a call copies with CQ intact",
          fuzzy_dist(v2, CALL) <= 1);
    g_free(v1);
    g_free(v2);
    g_array_free(env, TRUE);
    g_free(loop);

    const char *RUN = "TEST OK1BR OK1BR TEST OK1BR OK1BR NR 5";
    env = gen_env_fist(RUN, 28, RATE, 0.05, rng, 3.0, 5.0);
    g_cw = skim_decode_cw_v2();
    v2 = run_decoder(env, 12.0, 25.0, 0, 0, rng, &last);
    printf("--- tight runner: word gaps squeezed to 5.0 dits (28 WPM) ---\n");
    show("v2:", v2);
    check("tight runner's 5-dit word gaps stay WORDS (no merges)",
          fuzzy_dist(v2, RUN) <= 1);
    g_free(v2);
    g_array_free(env, TRUE);
    g_rand_free(rng);
  }

  /* -- mid-stream speed DROP: the stale-clock carrier trap ------------------------
   * An operator falling from ~34 to ~11 WPM makes every slow dah ~9× the
   * stale dit — the old "8 dits = carrier" rule swallowed the whole slow
   * transmission and the clock never saw a commit to relearn from
   * (live-caught 2026-07-16, 7028.9). The 0.8 s carrier floor must keep
   * the slow dahs committing so the boosted clock can snap down. */
  {
    GRand *rng = g_rand_new_with_seed(20260717);
    SkimDecode last;
    const char *SLOW = "CQ CQ DE OK1WW OK1WW PSE K";

    GArray *env = gen_env("CQ CQ DE OK1WW OK1WW K", 34, RATE, 0.03, rng);
    GArray *tail = gen_env(SLOW, 11, RATE, 0.03, rng);
    g_array_append_vals(env, tail->data, tail->len);
    g_array_free(tail, TRUE);

    g_cw = skim_decode_cw();
    char *v1 = run_decoder(env, 14.0, 28.0, 0, 0, rng, &last);
    g_cw = skim_decode_cw_v2();
    char *v2 = run_decoder(env, 14.0, 28.0, 0, 0, rng, &last);
    printf("--- speed drop: 34 -> 11 WPM mid-stream ---\n");
    show("v1 (info):", v1);
    show("v2:", v2);
    printf("       v1 dist %d, v2 dist %d\n",
           fuzzy_dist(v1, SLOW), fuzzy_dist(v2, SLOW));
    check("v2 survives a 3x speed drop (slow copy ≤2 errors)",
          fuzzy_dist(v2, SLOW) <= 2);
    check("v1 survives a 3x speed drop (slow copy ≤2 errors)",
          fuzzy_dist(v1, SLOW) <= 2);
    g_free(v1);
    g_free(v2);
    g_array_free(env, TRUE);
    g_rand_free(rng);
  }

  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — both CW backends copy, adapt and stay "
                          "quiet on noise; v2 rides out QSB and learns the "
                          "operator's fist.");
  return fails ? 1 : 0;
}
