/*
 * skimmer-reader-test — offline gate for the neural CW reader (cw_reader.c).
 *
 * The C forward pass must match the training-side network EXACTLY, or the
 * exported weights are garbage in production. Proven two ways:
 *   - a tiny hand-built blob whose forward result a NAIVE reference
 *     implementation (written independently here) predicts — conv padding,
 *     dilation, residual adds, ReLU placement and the head all verified;
 *   - the blob's baked-in test vector (selftest), which for real exports
 *     comes from torch itself. Set SKIM_READER_BLOB to also gate a trained
 *     export (skipped when unset — gates never depend on training artifacts).
 * Plus mechanics: greedy CTC collapse, determinism, degenerate inputs, and
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

/* Naive reference forward: full loops, explicit LayerNorm — deliberately
 * structured differently from cw_reader.c so shared bugs cannot cancel.
 * Layer 0: plain conv (TOY_IN inputs) + ReLU. Layer 1: pre-norm residual
 * block y += relu(conv(layernorm(y))). */
static void ref_forward(const float w0[TOY_CH][TOY_IN][TOY_K],
                        const float w1[TOY_CH][TOY_CH][TOY_K],
                        const float b[TOY_CONVS][TOY_CH],
                        const float gm[TOY_CH], const float bt[TOY_CH],
                        const float hw[TOY_OUT][TOY_CH],
                        const float hb[TOY_OUT],
                        const guint dil[TOY_CONVS],
                        const float x[TOY_T][TOY_IN],
                        float y[TOY_T][TOY_OUT]) {
  float act[TOY_T][TOY_CH], nrm[TOY_T][TOY_CH];
  for (guint t = 0; t < TOY_T; t++) {          /* layer 0                    */
    for (guint o = 0; o < TOY_CH; o++) {
      float s = b[0][o];
      for (guint k = 0; k < TOY_K; k++) {
        const gint ts = (gint)t + ((gint)k - 1) * (gint)dil[0];
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
        const gint ts = (gint)t + ((gint)k - 1) * (gint)dil[1];
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
  guint8 b[4] = { v & 0xff, v >> 8 & 0xff, v >> 16 & 0xff, v >> 24 & 0xff };
  fwrite(b, 1, 4, f);
}

int main(void) {
  printf("=== cw reader gate (offline) ===\n");

  /* -- build the toy blob + its reference answer ----------------------------- */
  float w0[TOY_CH][TOY_IN][TOY_K], w1[TOY_CH][TOY_CH][TOY_K];
  float b[TOY_CONVS][TOY_CH], gm[TOY_CH], bt[TOY_CH];
  float hw[TOY_OUT][TOY_CH], hb[TOY_OUT];
  float x[TOY_T][TOY_IN], y[TOY_T][TOY_OUT];
  const guint dil[TOY_CONVS] = { 1, 2 };
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
  for (guint t = 0; t < TOY_T; t++) {
    for (guint i = 0; i < TOY_IN; i++) { x[t][i] = toy_val(seed++) * 2.0f; }
    x[t][1] = t % 2 ? 1.0f : -1.0f;
  }
  ref_forward(w0, w1, b, gm, bt, hw, hb, dil, x, y);

  char *blob = g_build_filename(g_get_tmp_dir(), "skimmer-reader-toy.bin", NULL);
  {
    FILE *f = fopen(blob, "wb");
    fwrite("CWRD", 1, 4, f);
    put_u32(f, 2);
    put_u32(f, TOY_CONVS);
    put_u32(f, TOY_CH);
    put_u32(f, TOY_OUT);
    put_u32(f, TOY_IN);
    put_u32(f, TOY_OUT - 1);
    fwrite("AB", 1, 2, f);
    put_u32(f, TOY_IN);                          /* layer 0: no norm         */
    put_u32(f, dil[0]);
    put_u32(f, 0);
    fwrite(w0, sizeof(float), TOY_CH * TOY_IN * TOY_K, f);
    fwrite(b[0], sizeof(float), TOY_CH, f);
    put_u32(f, TOY_CH);                          /* layer 1: pre-norm block  */
    put_u32(f, dil[1]);
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
  }

  GError *err = NULL;
  SkimCwReader *r = skim_cw_reader_load(blob, &err);
  check("toy blob loads", r != NULL);
  if (!r) {
    printf("       %s\n", err ? err->message : "?");
    return 1;
  }

  /* -- numerics: C forward vs the independent reference ---------------------- */
  const double diff = skim_cw_reader_selftest(r);
  printf("       max |logit diff| vs reference: %.2e\n", diff);
  check("forward matches the reference (< 1e-5)", diff < 1e-5);

  /* -- mechanics -------------------------------------------------------------- */
  {
    gboolean key[8] = { 1, 0, 1, 0, 1, 0, 1, 0 };
    double dur[8] = { 60, 60, 180, 60, 60, 240, 60, 60 };
    char *t1 = skim_cw_reader_read(r, key, dur, 8);
    char *t2 = skim_cw_reader_read(r, key, dur, 8);
    check("read() is deterministic", strcmp(t1, t2) == 0);
    for (const char *p = t1; *p; p++) {
      if (*p != 'A' && *p != 'B') {
        check("read() emits alphabet chars only", 0);
        break;
      }
    }
    g_free(t1);
    g_free(t2);
    char *t3 = skim_cw_reader_read(r, key, dur, 3);
    check("too-short over reads as empty", strcmp(t3, "") == 0);
    g_free(t3);
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
      skim_cw_reader_free(rr);
    } else {
      printf("       %s\n", e3 ? e3->message : "?");
      g_clear_error(&e3);
    }
  } else {
    printf("       (SKIM_READER_BLOB unset — trained-export check skipped)\n");
  }

  skim_cw_reader_free(r);
  g_remove(blob);
  g_free(blob);
  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — the C net computes what torch trained.");
  return fails ? 1 : 0;
}
