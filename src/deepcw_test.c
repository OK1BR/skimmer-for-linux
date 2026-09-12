/*
 * skimmer-deepcw-test — offline gate for the DeepCW backend.
 *
 * Sections: (A) the pure commit rule on synthetic CTC matrices; (B) the
 * tile builder through the public vtable on synthetic complex baseband —
 * offset sign (+30 / −30 Hz), level scaling, gate keyed vs noise, dit
 * estimate, rate rejection; (C) the real model on a synthetic keyed tone
 * and on noise — ONLY when SKIM_ORT_LIB / SKIM_DEEPCW_MODEL resolve
 * (exit 77 = meson SKIP otherwise, reason printed).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/decode.h"
#include "engine/decode_deepcw.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

#define NC 42
#define BLANK 41
#define SPACE 40
static int cls_of(char c) {
  static const char CH[NC] = ",./0123456789?ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
  for (int i = 0; i < NC - 1; i++) if (CH[i] == c) return i;
  return -1;
}
static float *logp_new(guint T) {
  float *m = g_new(float, (gsize)T * NC);
  for (guint t = 0; t < T; t++) {
    for (int c = 0; c < NC; c++) m[t * NC + c] = logf(0.1f / 41.0f);
    m[t * NC + BLANK] = logf(0.9f);
  }
  return m;
}
static void spike(float *m, guint t, int cls, float p) {
  for (int c = 0; c < NC; c++) m[t * NC + c] = logf((1.0f - p) / 41.0f);
  m[t * NC + cls] = logf(p);
}
/* Write "text" as spikes: chars every `step` frames from t0; ' ' is a
 * SPACE spike. Returns the frame after the last spike. */
static guint spikes_text(float *m, guint t0, guint step, const char *text) {
  guint t = t0;
  for (const char *s = text; *s; s++, t += step) spike(m, t, cls_of(*s), 0.95f);
  return t;
}

/* ---- synthetic complex baseband --------------------------------------- */
static const char *morse(char c) {
  static const char *M[] = { "A.-", "B-...", "C-.-.", "D-..", "E.", "F..-.", "G--.",
    "H....", "I..", "J.---", "K-.-", "L.-..", "M--", "N-.", "O---", "P.--.", "Q--.-",
    "R.-.", "S...", "T-", "U..-", "V...-", "W.--", "X-..-", "Y-.--", "Z--..",
    "0-----", "1.----", "2..---", "3...--", "4....-", "5.....", "6-....", "7--...",
    "8---..", "9----.", NULL };
  for (int i = 0; M[i]; i++) if (M[i][0] == c) return M[i] + 1;
  return "";
}
/* Keyed envelope for text at wpm, rate Hz; leading/trailing quiet in dits. */
static GArray *keyer(const char *text, double wpm, double rate, int lead, int trail) {
  GArray *e = g_array_new(FALSE, FALSE, sizeof(float));
  const double dit = 1.2 / wpm;
  const float one = 1.0f, zero = 0.0f;
#define ON(n)  for (int _i = 0; _i < (int)llround((n) * dit * rate); _i++) g_array_append_val(e, one)
#define OFF(n) for (int _i = 0; _i < (int)llround((n) * dit * rate); _i++) g_array_append_val(e, zero)
  OFF(lead);
  for (const char *s = text; *s; s++) {
    if (*s == ' ') { OFF(4); continue; }
    const char *m = morse(*s);
    for (; *m; m++) { ON(*m == '.' ? 1 : 3); OFF(1); }
    OFF(2);
  }
  OFF(trail);
#undef ON
#undef OFF
  return e;
}
/* Complex tone at off_hz with the keyed envelope (5 ms raised-cosine edges
 * via a 1-pole smoother), amplitude amp, plus white noise sigma. */
static float *tone_iq(GArray *env, double rate, double off_hz, float amp,
                      float sigma, guint seed) {
  GRand *r = g_rand_new_with_seed(seed);
  float *iq = g_new(float, 2 * env->len);
  double e = 0, a = exp(-1.0 / (0.005 * rate));
  for (guint n = 0; n < env->len; n++) {
    e = a * e + (1 - a) * g_array_index(env, float, n);
    const double ph = 2.0 * G_PI * off_hz * n / rate;
    double ni = 0, nq = 0;
    if (sigma > 0) {
      /* Box–Muller */
      const double u1 = MAX(g_rand_double(r), 1e-12), u2 = g_rand_double(r);
      const double m = sqrt(-2.0 * log(u1));
      ni = sigma * m * cos(2 * G_PI * u2); nq = sigma * m * sin(2 * G_PI * u2);
    }
    iq[2 * n] = (float)(amp * e * cos(ph) + ni);
    iq[2 * n + 1] = (float)(amp * e * sin(ph) + nq);
  }
  g_rand_free(r);
  return iq;
}
/* Run a backend state over iq in 64-frame blocks (the pipeline's drain);
 * collect emitted text into out, return number of hits. */
static guint run_state(const SkimDecodeBackend *be, gpointer st, const float *iq,
                       guint nframes, GString *out, double *last_conf) {
  guint hits = 0;
  SkimDecode d;
  for (guint i = 0; i < nframes; i += 64) {
    const guint n = MIN(64u, nframes - i);
    if (be->process(st, iq + 2 * i, n, &d)) {
      hits++;
      g_string_append(out, d.text);
      if (last_conf) *last_conf = d.confidence;
    }
  }
  return hits;
}

int main(void) {
  const double RATE = 250.0;
  printf("=== skimmer-deepcw-test ===\n");

  /* ---- (A) commit rule ------------------------------------------------- */
  printf("[A] commit rule (sliding window, frame cursor)\n");
  {
    const int E = cls_of('E');
    guint T = 400; float *m = logp_new(T);
    spikes_text(m, 10, 10, "CQ");        /* C@10 Q@20                     */
    spike(m, 100, SPACE, 0.95f);
    spikes_text(m, 150, 10, "TEST");     /* 150..180                      */
    spike(m, 200, SPACE, 0.95f);
    spikes_text(m, 340, 10, "DE");       /* 340, 350: inside the tail     */
    GString *out = g_string_new(NULL); guint64 cur = 0; gboolean lsp = TRUE; double conf = 0;
    guint n = skim_deepcw_commit(m, T, 0, &cur, 70, 4, &lsp, out, &conf);
    check("commits every spike ≥ tail before the end, nothing inside the tail",
          n == 8 && strcmp(out->str, "CQ TEST ") == 0 && cur == 200);
    check("confidence = spike posterior", fabs(conf - 0.95) < 0.02);
    g_free(m);
    /* the same window re-read with every spike jittered +2 frames */
    m = logp_new(T);
    spikes_text(m, 12, 10, "CQ"); spike(m, 102, SPACE, 0.95f);
    spikes_text(m, 152, 10, "TEST"); spike(m, 201, SPACE, 0.95f);
    spikes_text(m, 342, 10, "DE");
    n = skim_deepcw_commit(m, T, 0, &cur, 70, 4, &lsp, out, &conf);
    check("a re-read with spikes jittered +2 frames re-emits nothing",
          n == 0 && strcmp(out->str, "CQ TEST ") == 0 && cur == 200);
    g_free(m);
    /* the window slid by 100 frames: DE is now clear of the tail, OK1BR new */
    m = logp_new(T);
    spikes_text(m, 340 - 100, 10, "DE"); spike(m, 360 - 100, SPACE, 0.9f);
    spikes_text(m, 380 - 100, 10, "OK1BR");            /* abs 380..420     */
    n = skim_deepcw_commit(m, T, 100, &cur, 70, 4, &lsp, out, &conf);
    check("next tick commits the characters beyond the cursor",
          n == 8 && strcmp(out->str, "CQ TEST DE OK1BR") == 0 && cur == 420);
    g_free(m);
    /* a word gap two frames after the last character passes on frame
     * order (no margin for spaces); its double is squeezed */
    m = logp_new(T);
    spikes_text(m, 340 - 100, 10, "DE"); spike(m, 360 - 100, SPACE, 0.9f);
    spikes_text(m, 380 - 100, 10, "OK1BR");
    spike(m, 422 - 100, SPACE, 0.9f); spike(m, 424 - 100, SPACE, 0.9f);
    n = skim_deepcw_commit(m, T, 100, &cur, 70, 4, &lsp, out, &conf);
    check("a gap right after the last character passes once, its double is squeezed",
          n == 1 && strcmp(out->str, "CQ TEST DE OK1BR ") == 0 && cur == 422);
    g_free(m);
    /* silence */
    m = logp_new(200); g_string_truncate(out, 0); cur = 0; lsp = TRUE;
    n = skim_deepcw_commit(m, 200, 0, &cur, 50, 4, &lsp, out, &conf);
    check("silence → nothing emitted, cursor untouched", n == 0 && cur == 0 && out->len == 0);
    g_free(m);
    /* CTC collapse: E E E in a run = one E; separated by blanks = more */
    m = logp_new(100); g_string_truncate(out, 0); cur = 0; lsp = TRUE;
    spike(m, 10, E, 0.9f); spike(m, 11, E, 0.9f); spike(m, 12, E, 0.9f);
    spike(m, 20, E, 0.9f); spike(m, 30, E, 0.9f); spike(m, 40, SPACE, 0.9f);
    n = skim_deepcw_commit(m, 100, 0, &cur, 10, 4, &lsp, out, &conf);
    check("CTC collapse: run = one char, blank-separated = more", n == 4 && strcmp(out->str, "EEE ") == 0);
    check("nothing below the cursor is re-emitted",
          skim_deepcw_commit(m, 100, 0, &cur, 10, 4, &lsp, out, &conf) == 0);
    g_free(m);
    /* weak word gaps: torn short piece is glued, a weak gap between two
     * real words stays (default bar 0.8, SKIM_DEEPCW_SPACE_P unset) */
    T = 400; m = logp_new(T); cur = 0; lsp = TRUE; g_string_truncate(out, 0);
    guint tt = spikes_text(m, 10, 10, "OK2B");
    spike(m, tt, SPACE, 0.55f);
    tt = spikes_text(m, tt + 10, 10, "TK");
    spike(m, tt, SPACE, 0.55f);
    tt = spikes_text(m, tt + 10, 10, "TEST");
    spike(m, tt, SPACE, 0.97f);
    n = skim_deepcw_commit(m, T, 0, &cur, 50, 4, &lsp, out, &conf);
    check("weak gap before a 2-char piece is glued (OK2B TK → OK2BTK), weak gap between words kept",
          strcmp(out->str, "OK2BTK TEST ") == 0);
    g_free(m); g_string_free(out, TRUE);
  }

  /* ---- (B) tile builder / vtable ---------------------------------------- */
  printf("[B] tile builder + gate (no model needed)\n");
  const SkimDecodeBackend *be = skim_decode_deepcw();
  {
    gpointer bad = be->channel_new(333.0);
    SkimDeepcwDebug dbg; skim_decode_deepcw_debug(bad, &dbg);
    check("rate 333 Hz (no integer 12.5 Hz DFT) → dead state", dbg.dead);
    be->channel_free(bad);

    GArray *env = keyer("CQ CQ DE OK1BR K", 25.0, RATE, 8, 8);
    const guint nf = env->len;
    float *iq_p = tone_iq(env, RATE, +30.0, 0.1f, 0.003f, 1);
    float *iq_m = tone_iq(env, RATE, -30.0, 0.1f, 0.003f, 2);
    float *iq_2 = tone_iq(env, RATE, +30.0, 0.2f, 0.003f, 3);
    gpointer sp = be->channel_new(RATE), sm = be->channel_new(RATE), s2 = be->channel_new(RATE);
    GString *o = g_string_new(NULL);
    run_state(be, sp, iq_p, nf, o, NULL);
    run_state(be, sm, iq_m, nf, o, NULL);
    run_state(be, s2, iq_2, nf, o, NULL);
    const double op = be->tone_offset_hz(sp), om = be->tone_offset_hz(sm);
    printf("      offsets: +30 → %.1f Hz, −30 → %.1f Hz; levels %.3f / %.3f\n",
           op, om, be->level(sp), be->level(s2));
    check("+30 Hz tone → offset +30 ± 4 Hz (sign = v2's, above centre positive)", fabs(op - 30.0) < 4.0);
    check("−30 Hz tone → offset −30 ± 4 Hz", fabs(om + 30.0) < 4.0);
    const double lr = be->level(s2) / MAX(be->level(sp), 1e-9);
    check("level scales with amplitude (×2 → 1.7..2.3)", lr > 1.7 && lr < 2.3);
    skim_decode_deepcw_debug(sp, &dbg);
    printf("      keyed: ticks %u gate %d ratio %.1f dB duty %.2f wpm %.1f\n",
           dbg.ticks, dbg.gate, dbg.ratio_db, dbg.duty, dbg.wpm);
    check("keyed tone: gate OPEN at the last tick", dbg.ticks > 0 && dbg.gate);
    check("keyed tone: line ≥ 6 dB over the floor", dbg.ratio_db >= 6.0);
    check("dit estimate lands within ±35 % of 25 WPM", dbg.wpm > 16.0 && dbg.wpm < 34.0);
    be->channel_free(sp); be->channel_free(sm); be->channel_free(s2);
    g_free(iq_p); g_free(iq_m); g_free(iq_2);
    /* noise only */
    GArray *env0 = g_array_new(FALSE, FALSE, sizeof(float));
    const float z = 0.0f;
    for (guint i = 0; i < 8 * (guint)RATE; i++) g_array_append_val(env0, z);
    float *iq_n = tone_iq(env0, RATE, 0.0, 0.0f, 0.01f, 4);
    gpointer sn = be->channel_new(RATE);
    run_state(be, sn, iq_n, env0->len, o, NULL);
    skim_decode_deepcw_debug(sn, &dbg);
    printf("      noise: ticks %u gate %d ratio %.1f dB duty %.2f\n", dbg.ticks, dbg.gate, dbg.ratio_db, dbg.duty);
    check("noise only: gate CLOSED", dbg.ticks > 0 && !dbg.gate);
    check("noise only: nothing ever committed (cursor never moved)", dbg.committed == 0);
    be->channel_free(sn); g_free(iq_n); g_array_free(env0, TRUE);
    check("no model → no text emitted from the DSP half alone", o->len == 0 || skim_decode_deepcw_available(NULL));
    g_string_free(o, TRUE); g_array_free(env, TRUE);
  }

  /* ---- (C) the model ---------------------------------------------------- */
  printf("[C] model\n");
  GError *err = NULL;
  if (!skim_decode_deepcw_available(&err)) {
    printf("  SKIP — %s\n  (set SKIM_ORT_LIB and SKIM_DEEPCW_MODEL to run this section)\n",
           err ? err->message : "?");
    g_clear_error(&err);
    printf("=== %d checks, %d failed (model section skipped) ===\n", checks, fails);
    return fails ? 1 : 77;
  }
  printf("  runtime %s\n", skim_decode_deepcw_runtime_info());
  const gboolean sync_env = g_getenv("SKIM_DEEPCW_SYNC") != NULL;
  {
    /* Device selection: ask for CUDA. With the CPU-only runtime (or no
     * GPU) the session must fall back and SAY so; with onnxruntime-cuda
     * it runs on CUDA:0. Either way the model must still read. Then back
     * to the CPU through reset — the way the Preferences row switches. */
    skim_decode_deepcw_reset();
    skim_decode_deepcw_set_device("cuda");
    GError *e2 = NULL;
    check("session reloads after reset with device=cuda requested", skim_decode_deepcw_available(&e2));
    const char *info = skim_decode_deepcw_runtime_info();
    printf("      device=cuda → %s\n", info ? info : "(none)");
    check("runtime_info names CUDA:0 or the fallback reason",
          info && (strstr(info, "CUDA:0") || strstr(info, "cuda unavailable")));
    g_clear_error(&e2);
    skim_decode_deepcw_reset();
    skim_decode_deepcw_set_device("cpu");
    check("reset + device=cpu reloads on the CPU",
          skim_decode_deepcw_available(NULL) &&
          skim_decode_deepcw_runtime_info() && strstr(skim_decode_deepcw_runtime_info(), ", CPU"));
  }
  {
    /* Inline (the replay path): a burst feed, deterministic text. */
    GArray *env = keyer("CQ CQ DE OK1BR OK1BR K CQ CQ DE OK1BR OK1BR K", 25.0, RATE, 40, 200);
    float *iq = tone_iq(env, RATE, +20.0, 0.05f, 0.004f, 5);
    gpointer st = be->channel_new(RATE);
    GString *o = g_string_new(NULL); double conf = 0;
    guint hits = 0;
    SkimDeepcwDebug dbg;
    if (sync_env) {
      hits = run_state(be, st, iq, env->len, o, &conf);
    } else {
      /* Async (the app's path): pace the feed like a live channel would
       * arrive (a 64-frame block ≈ 256 ms of audio; 2 ms wall per block
       * is 128× realtime — the workers must keep up), then drain the
       * mailbox until the last window has come back. */
      SkimDecode d;
      for (guint i = 0; i < env->len; i += 64) {
        const guint n = MIN(64u, env->len - i);
        if (be->process(st, iq + 2 * i, n, &d)) { hits++; g_string_append(o, d.text); conf = d.confidence; }
        g_usleep(2000);
      }
      for (int k = 0; k < 500; k++) {
        if (be->process(st, iq, 0, &d)) { hits++; g_string_append(o, d.text); conf = d.confidence; }
        skim_decode_deepcw_debug(st, &dbg);
        if (!dbg.inflight && o->len > 20) break;
        g_usleep(10000);
      }
    }
    skim_decode_deepcw_debug(st, &dbg);
    printf("      %s text |%s| hits %u ticks %u last conf %.2f wpm %.0f\n",
           sync_env ? "sync " : "async", o->str, hits, dbg.ticks, conf, dbg.wpm);
    check("model reads the call (text contains OK1BR)", strstr(o->str, "OK1BR") != NULL);
    check("commit seams carry single spaces", strstr(o->str, "  ") == NULL);
    check("model reads CQ", strstr(o->str, "CQ") != NULL);
    check("no doubled call from re-decoding committed audio", strstr(o->str, "OK1BROK1BR") == NULL &&
          strstr(o->str, "OK1BR OK1BR OK1BR") == NULL);
    check("committed text carries a confidence", conf > 0.5);
    check("a state freed with a window in flight does not crash (deferred free)",
          (be->channel_free(st), TRUE));
    g_free(iq); g_array_free(env, TRUE); g_string_free(o, TRUE);
    /* three channels fed in lockstep through the async path: the worker
     * batches whatever is queued, zero-padding to the longest window —
     * each channel must still read ITS OWN text. */
    if (!sync_env) {
      const char *texts[3] = { "CQ CQ DE OK1BR OK1BR K", "TEST DL1ABC DL1ABC 5NN", "VVV DE SP9XYZ SP9XYZ K" };
      const double offs[3] = { +20.0, -25.0, +40.0 };
      const double wpms[3] = { 25.0, 30.0, 20.0 };
      GArray *envs[3]; float *iqs[3]; gpointer sts[3]; GString *os[3]; guint len = 0;
      for (int k = 0; k < 3; k++) {
        envs[k] = keyer(texts[k], wpms[k], RATE, 40, 200);
        iqs[k] = tone_iq(envs[k], RATE, offs[k], 0.05f, 0.004f, 10 + k);
        sts[k] = be->channel_new(RATE); os[k] = g_string_new(NULL);
        len = MAX(len, envs[k]->len);
      }
      SkimDecode d;
      for (guint i = 0; i < len; i += 64) {
        for (int k = 0; k < 3; k++) {
          if (i >= envs[k]->len) continue;
          const guint n = MIN(64u, envs[k]->len - i);
          if (be->process(sts[k], iqs[k] + 2 * i, n, &d)) g_string_append(os[k], d.text);
        }
        g_usleep(2000);
      }
      for (int r = 0; r < 300; r++) {
        gboolean busy = FALSE;
        for (int k = 0; k < 3; k++) {
          if (be->process(sts[k], iqs[k], 0, &d)) g_string_append(os[k], d.text);
          SkimDeepcwDebug dg; skim_decode_deepcw_debug(sts[k], &dg); busy |= dg.inflight;
        }
        if (!busy && os[0]->len > 20 && os[1]->len > 10 && os[2]->len > 10) break;
        g_usleep(10000);
      }
      for (int k = 0; k < 3; k++) printf("      batch ch%d |%s|\n", k, os[k]->str);
      check("batched channel 0 reads OK1BR", strstr(os[0]->str, "OK1BR") != NULL);
      check("batched channel 1 reads DL1ABC", strstr(os[1]->str, "DL1ABC") != NULL);
      check("batched channel 2 reads SP9XYZ", strstr(os[2]->str, "SP9XYZ") != NULL);
      check("no cross-talk between batched channels",
            !strstr(os[0]->str, "DL1ABC") && !strstr(os[1]->str, "OK1BR") && !strstr(os[2]->str, "OK1BR"));
      for (int k = 0; k < 3; k++) { be->channel_free(sts[k]); g_free(iqs[k]); g_array_free(envs[k], TRUE); g_string_free(os[k], TRUE); }
    }
    /* noise only, 20 s: no phantom text */
    GArray *env0 = g_array_new(FALSE, FALSE, sizeof(float)); const float z = 0.0f;
    for (guint i = 0; i < 20 * (guint)RATE; i++) g_array_append_val(env0, z);
    float *iq_n = tone_iq(env0, RATE, 0.0, 0.0f, 0.01f, 6);
    gpointer sn = be->channel_new(RATE); o = g_string_new(NULL);
    run_state(be, sn, iq_n, env0->len, o, NULL);
    check("noise only through the model: no text", o->len == 0);
    be->channel_free(sn); g_free(iq_n); g_array_free(env0, TRUE); g_string_free(o, TRUE);
  }
  printf("=== %d checks, %d failed ===\n", checks, fails);
  return fails ? 1 : 0;
}
