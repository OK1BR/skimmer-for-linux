/*
 * skimmer-reader-test — offline gate for the neural CW reader (cw_reader.c).
 *
 * The C forward pass must match the training-side network EXACTLY, or the
 * exported weights are garbage in production. Proven two ways:
 *   - tiny hand-built blobs whose forward result a NAIVE reference
 *     implementation (written independently here) predicts — conv taps
 *     (look−2d, look−d, look), dilation, residual adds, ReLU placement and
 *     the head all verified, for a v3 blob with a causal layer AND a v2
 *     blob that must load as the symmetric look=dil net;
 *   - the blob's baked-in test vector (selftest), which for real exports
 *     comes from torch itself. Set SKIM_READER_BLOB to also gate a trained
 *     export (skipped when unset — gates never depend on training artifacts).
 * Streaming (phase A): push+flush over a v3 blob must reproduce read()
 * byte-for-byte — same runs, same text — including mid-stream prefix
 * consistency, reuse across overs, and the short-over refusal. Plus
 * mechanics: greedy CTC collapse, determinism, degenerate inputs, and
 * truncated-blob rejection.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/cw_reader.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

#define TOY_CONVS 2
#define TOY_CH    2
#define TOY_IN    4                    /* read() insists on 4 features       */
#define TOY_OUT   3
#define TOY_T     8
#define TOY_K     3
#define TOY_EPS   1e-5f

/* Deterministic pseudo-weights — small values keep ReLU half-active. */
static float toy_val(guint seed) {
  return ((float)((seed * 2654435761u) >> 16 & 0xffff) / 65536.0f - 0.5f);
}

/* Toy weights, shared by every toy blob below. */
static float w0[TOY_CH][TOY_IN][TOY_K], w1[TOY_CH][TOY_CH][TOY_K];
static float b[TOY_CONVS][TOY_CH], gm[TOY_CH], bt[TOY_CH];
static float hw[TOY_OUT][TOY_CH], hb[TOY_OUT];
static const guint dil[TOY_CONVS] = { 1, 2 };

/* Naive reference forward: full loops, explicit LayerNorm — deliberately
 * structured differently from cw_reader.c so shared bugs cannot cancel.
 * Layer 0: plain conv (TOY_IN inputs) + ReLU. Layer 1: pre-norm residual
 * block y += relu(conv(layernorm(y))). Taps at (look−2d, look−d, look). */
static void ref_forward(const guint look[TOY_CONVS],
                        const float x[TOY_T][TOY_IN],
                        float y[TOY_T][TOY_OUT]) {
  float act[TOY_T][TOY_CH], nrm[TOY_T][TOY_CH];
  for (guint t = 0; t < TOY_T; t++) {          /* layer 0                    */
    for (guint o = 0; o < TOY_CH; o++) {
      float s = b[0][o];
      for (guint k = 0; k < TOY_K; k++) {
        const gint ts = (gint)t + ((gint)k - 2) * (gint)dil[0] +
                        (gint)look[0];
        if (ts < 0 || ts >= TOY_T)
          continue;
        for (guint i = 0; i < TOY_IN; i++) { s += w0[o][i][k] * x[ts][i]; }
      }
      act[t][o] = s > 0.0f ? s : 0.0f;
    }
  }
  for (guint t = 0; t < TOY_T; t++) {          /* layernorm over channels    */
    float mean = 0.0f, var = 0.0f;
    for (guint i = 0; i < TOY_CH; i++) { mean += act[t][i]; }
    mean /= TOY_CH;
    for (guint i = 0; i < TOY_CH; i++) {
      var += (act[t][i] - mean) * (act[t][i] - mean);
    }
    var /= TOY_CH;
    for (guint i = 0; i < TOY_CH; i++) {
      nrm[t][i] = (act[t][i] - mean) / sqrtf(var + TOY_EPS) * gm[i] + bt[i];
    }
  }
  for (guint t = 0; t < TOY_T; t++) {          /* layer 1, residual          */
    for (guint o = 0; o < TOY_CH; o++) {
      float s = b[1][o];
      for (guint k = 0; k < TOY_K; k++) {
        const gint ts = (gint)t + ((gint)k - 2) * (gint)dil[1] +
                        (gint)look[1];
        if (ts < 0 || ts >= TOY_T)
          continue;
        for (guint i = 0; i < TOY_CH; i++) { s += w1[o][i][k] * nrm[ts][i]; }
      }
      act[t][o] += s > 0.0f ? s : 0.0f;        /* conv reads nrm, so the     */
                                               /* in-place add is safe       */
    }
  }
  for (guint t = 0; t < TOY_T; t++) {
    for (guint o = 0; o < TOY_OUT; o++) {
      float s = hb[o];
      for (guint i = 0; i < TOY_CH; i++) { s += hw[o][i] * act[t][i]; }
      y[t][o] = s;
    }
  }
}

static void put_u32(FILE *f, guint v) {
  guint8 b4[4] = { v & 0xff, v >> 8 & 0xff, v >> 16 & 0xff, v >> 24 & 0xff };
  fwrite(b4, 1, 4, f);
}

/* Write a toy blob; ver 3 carries the look fields, ver 2 omits them (the
 * loader must then imply look=dil). Test vector: x + ref logits y. */
static char *write_toy(guint ver, const guint look[TOY_CONVS],
                       const float x[TOY_T][TOY_IN],
                       const float y[TOY_T][TOY_OUT], const char *name) {
  char *path = g_build_filename(g_get_tmp_dir(), name, NULL);
  FILE *f = fopen(path, "wb");
  fwrite("CWRD", 1, 4, f);
  put_u32(f, ver);
  put_u32(f, TOY_CONVS);
  put_u32(f, TOY_CH);
  put_u32(f, TOY_OUT);
  put_u32(f, TOY_IN);
  put_u32(f, TOY_OUT - 1);
  fwrite("AB", 1, 2, f);
  put_u32(f, TOY_IN);                          /* layer 0: no norm           */
  put_u32(f, dil[0]);
  if (ver >= 3) { put_u32(f, look[0]); }
  put_u32(f, 0);
  fwrite(w0, sizeof(float), TOY_CH * TOY_IN * TOY_K, f);
  fwrite(b[0], sizeof(float), TOY_CH, f);
  put_u32(f, TOY_CH);                          /* layer 1: pre-norm block    */
  put_u32(f, dil[1]);
  if (ver >= 3) { put_u32(f, look[1]); }
  put_u32(f, 1);
  fwrite(gm, sizeof(float), TOY_CH, f);
  fwrite(bt, sizeof(float), TOY_CH, f);
  fwrite(w1, sizeof(float), TOY_CH * TOY_CH * TOY_K, f);
  fwrite(b[1], sizeof(float), TOY_CH, f);
  fwrite(hw, sizeof(float), TOY_OUT * TOY_CH, f);
  fwrite(hb, sizeof(float), TOY_OUT, f);
  put_u32(f, TOY_T);
  fwrite(x, sizeof(float), TOY_T * TOY_IN, f);
  fwrite(y, sizeof(float), TOY_T * TOY_OUT, f);
  fclose(f);
  return path;
}

/* Pseudo-random alternating run sequence (marks/spaces, ham-ish durs). */
static void gen_runs(guint seed, guint n, gboolean *key, double *dur) {
  for (guint i = 0; i < n; i++) {
    key[i] = i % 2 == 0;                       /* start on a mark            */
    const float u = toy_val(seed + 31 * i) + 0.5f;   /* 0..1                 */
    dur[i] = key[i] ? (u < 0.6f ? 60.0 : 180.0) + 20.0 * u
                    : (u < 0.5f ? 60.0 : (u < 0.8f ? 200.0 : 500.0)) +
                          15.0 * u;
  }
}

/* Concatenate everything the stream commits for one over: push all runs,
 * then flush; optionally capture the text accumulated BEFORE the flush. */
static char *stream_over(SkimCwReaderStream *s, const gboolean *key,
                         const double *dur, guint n, char **pre_flush) {
  GString *all = g_string_new(NULL);
  for (guint i = 0; i < n; i++) {
    char *t = skim_cw_reader_stream_push(s, key[i], dur[i]);
    if (t) {
      g_string_append(all, t);
      g_free(t);
    }
  }
  if (pre_flush) { *pre_flush = g_strdup(all->str); }
  char *tail = skim_cw_reader_stream_flush(s);
  if (tail) {
    g_string_append(all, tail);
    g_free(tail);
  }
  return g_string_free(all, FALSE);
}

int main(void) {
  printf("=== cw reader gate (offline) ===\n");

  /* -- toy weights ------------------------------------------------------------ */
  guint seed = 1;
  for (guint o = 0; o < TOY_CH; o++) {
    b[0][o] = toy_val(seed++) * 0.1f;
    for (guint i = 0; i < TOY_IN; i++) {
      for (guint k = 0; k < TOY_K; k++) { w0[o][i][k] = toy_val(seed++); }
    }
  }
  for (guint o = 0; o < TOY_CH; o++) {
    b[1][o] = toy_val(seed++) * 0.1f;
    gm[o] = 1.0f + toy_val(seed++) * 0.2f;
    bt[o] = toy_val(seed++) * 0.1f;
    for (guint i = 0; i < TOY_CH; i++) {
      for (guint k = 0; k < TOY_K; k++) { w1[o][i][k] = toy_val(seed++); }
    }
  }
  for (guint o = 0; o < TOY_OUT; o++) {
    hb[o] = toy_val(seed++) * 0.1f;
    for (guint i = 0; i < TOY_CH; i++) { hw[o][i] = toy_val(seed++); }
  }
  float x[TOY_T][TOY_IN], y3[TOY_T][TOY_OUT], y2[TOY_T][TOY_OUT];
  for (guint t = 0; t < TOY_T; t++) {
    for (guint i = 0; i < TOY_IN; i++) { x[t][i] = toy_val(seed++) * 2.0f; }
    x[t][1] = t % 2 ? 1.0f : -1.0f;
  }

  /* -- v3 blob: layer 0 symmetric (look=dil), layer 1 causal (look=0) --------- */
  const guint look3[TOY_CONVS] = { 1, 0 };
  const guint look2[TOY_CONVS] = { 1, 2 };     /* what a v2 load must imply  */
  ref_forward(look3, x, y3);
  ref_forward(look2, x, y2);
  char *blob3 = write_toy(3, look3, x, y3, "skimmer-reader-toy3.bin");
  char *blob2 = write_toy(2, NULL, x, y2, "skimmer-reader-toy2.bin");

  GError *err = NULL;
  SkimCwReader *r3 = skim_cw_reader_load(blob3, &err);
  check("v3 toy blob loads", r3 != NULL);
  if (!r3) {
    printf("       %s\n", err ? err->message : "?");
    return 1;
  }

  /* -- numerics: C forward vs the independent reference ----------------------- */
  const double diff3 = skim_cw_reader_selftest(r3);
  printf("       v3 max |logit diff| vs reference: %.2e\n", diff3);
  check("v3 forward matches the reference (< 1e-5)", diff3 < 1e-5);

  SkimCwReader *r2 = skim_cw_reader_load(blob2, &err);
  check("v2 toy blob loads (back-compat)", r2 != NULL);
  if (r2) {
    const double diff2 = skim_cw_reader_selftest(r2);
    printf("       v2 max |logit diff| vs symmetric reference: %.2e\n",
           diff2);
    check("v2 loads as the symmetric look=dil net (< 1e-5)", diff2 < 1e-5);
  }

  /* -- batch mechanics --------------------------------------------------------- */
  {
    gboolean key[8] = { 1, 0, 1, 0, 1, 0, 1, 0 };
    double dur[8] = { 60, 60, 180, 60, 60, 240, 60, 60 };
    char *t1 = skim_cw_reader_read(r3, key, dur, 8);
    char *t2 = skim_cw_reader_read(r3, key, dur, 8);
    check("read() is deterministic", strcmp(t1, t2) == 0);
    for (const char *p = t1; *p; p++) {
      if (*p != 'A' && *p != 'B') {
        check("read() emits alphabet chars only", 0);
        break;
      }
    }
    g_free(t1);
    g_free(t2);
    char *t3 = skim_cw_reader_read(r3, key, dur, 3);
    check("too-short over reads as empty", strcmp(t3, "") == 0);
    g_free(t3);
  }

  /* -- streaming == batch ------------------------------------------------------ */
  {
    SkimCwReaderStream *s = skim_cw_reader_stream_new(r3);
    check("stream opens on a v3 blob", s != NULL);
    check("stream refuses a v2 blob",
          r2 && skim_cw_reader_stream_new(r2) == NULL);

    enum { N = 120 };
    gboolean key[N];
    double dur[N];
    gen_runs(7, N, key, dur);
    char *batch = skim_cw_reader_read(r3, key, dur, N);
    char *pre = NULL;
    char *stream = stream_over(s, key, dur, N, &pre);
    printf("       over1: batch \"%s\"\n       stream \"%s\" (pre-flush "
           "\"%s\")\n", batch, stream, pre);
    check("stream output == batch output", strcmp(batch, stream) == 0);
    check("stream commits text before the flush", pre[0] != '\0');
    check("pre-flush text is a batch prefix",
          strncmp(batch, pre, strlen(pre)) == 0);
    g_free(pre);
    g_free(stream);
    g_free(batch);

    /* the SAME stream must serve the next over — flush() resets it        */
    gen_runs(99, N, key, dur);
    batch = skim_cw_reader_read(r3, key, dur, N);
    stream = stream_over(s, key, dur, N, NULL);
    check("stream reused across overs == batch", strcmp(batch, stream) == 0);
    g_free(stream);
    g_free(batch);

    /* a short over must refuse exactly like read()                        */
    char *t = skim_cw_reader_stream_push(s, TRUE, 60.0);
    check("short over: no commit from push", t == NULL);
    g_free(t);
    skim_cw_reader_stream_push(s, FALSE, 60.0);
    skim_cw_reader_stream_push(s, TRUE, 180.0);
    t = skim_cw_reader_stream_flush(s);
    check("short over: flush yields nothing", t == NULL);
    g_free(t);

    /* ...and the refusal must not poison the following over               */
    gen_runs(23, N, key, dur);
    batch = skim_cw_reader_read(r3, key, dur, N);
    stream = stream_over(s, key, dur, N, NULL);
    check("stream after a refused over == batch",
          strcmp(batch, stream) == 0);
    g_free(stream);
    g_free(batch);
    skim_cw_reader_stream_free(s);
  }

  /* -- corrupt blobs die cleanly ---------------------------------------------- */
  {
    char *bad = g_build_filename(g_get_tmp_dir(), "skimmer-reader-bad.bin",
                                 NULL);
    g_file_set_contents(bad, "CWRDgarbage", 11, NULL);
    GError *e2 = NULL;
    SkimCwReader *rb = skim_cw_reader_load(bad, &e2);
    check("truncated blob is rejected", rb == NULL && e2 != NULL);
    g_clear_error(&e2);
    g_remove(bad);
    g_free(bad);
  }

  /* -- optional: a real trained export (torch-baked test vector) -------------- */
  const char *real = g_getenv("SKIM_READER_BLOB");
  if (real) {
    GError *e3 = NULL;
    SkimCwReader *rr = skim_cw_reader_load(real, &e3);
    check("trained blob loads", rr != NULL);
    if (rr) {
      const double d2 = skim_cw_reader_selftest(rr);
      printf("       trained blob max |logit diff| vs torch: %.2e\n", d2);
      check("C forward matches torch (< 1e-3)", d2 < 1e-3);
      SkimCwReaderStream *s = skim_cw_reader_stream_new(rr);
      if (s) {                                 /* v3 export: stream gate     */
        enum { N = 240 };
        gboolean key[N];
        double dur[N];
        gen_runs(41, N, key, dur);
        char *batch = skim_cw_reader_read(rr, key, dur, N);
        char *stream = stream_over(s, key, dur, N, NULL);
        check("trained blob: stream == batch", strcmp(batch, stream) == 0);
        g_free(stream);
        g_free(batch);
        skim_cw_reader_stream_free(s);
      } else {
        printf("       (v2 export — stream check skipped)\n");
      }
      skim_cw_reader_free(rr);
    } else {
      printf("       %s\n", e3 ? e3->message : "?");
      g_clear_error(&e3);
    }
  } else {
    printf("       (SKIM_READER_BLOB unset — trained-export check skipped)\n");
  }

  skim_cw_reader_free(r3);
  skim_cw_reader_free(r2);
  g_remove(blob3);
  g_free(blob3);
  g_remove(blob2);
  g_free(blob2);
  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — the C net computes what torch trained, "
                          "streamed and batch alike.");
  return fails ? 1 : 0;
}
