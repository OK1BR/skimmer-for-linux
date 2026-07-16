/* cw_reader.c — neural CW reader: dilated-TCN forward pass + CTC greedy
 * decode in plain C. See cw_reader.h for the role and ml/export_c.py for the
 * blob format this loads.
 *
 * The forward pass mirrors ml/train_ctc.py::Reader exactly: n_conv dilated
 * 1-D convolutions (kernel 3, zero padding, ReLU) with residual adds from the
 * second layer on, then a 1×1 head. Features are computed here the same way
 * training did: [log(dur / median mark dur), key ? +1 : −1].
 *
 * Cost: O(T · ch² · 3 · n_conv) — a 60 s over (~500 runs) is ~170 MFLOP,
 * one-shot per over end, engine thread. No allocations in the hot loop
 * beyond the two ping-pong activation buffers.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "cw_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KERNEL 3

#define LN_EPS 1e-5f

typedef struct {
  guint  in_ch, dil;
  float *gamma, *beta;                 /* pre-norm LayerNorm; NULL = layer 0 */
  float *w;                            /* [ch][in_ch][KERNEL]                */
  float *b;                            /* [ch]                               */
} Conv;

struct _SkimCwReader {
  guint  n_conv, ch, n_out, in_dim;
  char  *alphabet;                     /* n_out-1 chars, index 1..n_out-1    */
  Conv  *conv;
  float *head_w;                       /* [n_out][ch]                        */
  float *head_b;                       /* [n_out]                            */
  guint  test_t;                       /* baked-in test vector               */
  float *test_x;                       /* [test_t][in_dim]                   */
  float *test_y;                       /* [test_t][n_out]                    */
};

static gboolean rd_bytes(FILE *f, void *dst, gsize n) {
  return fread(dst, 1, n, f) == n;
}

static gboolean rd_u32(FILE *f, guint *v) {
  guint8 b[4];
  if (!rd_bytes(f, b, 4))
    return FALSE;
  *v = (guint)b[0] | (guint)b[1] << 8 | (guint)b[2] << 16 | (guint)b[3] << 24;
  return TRUE;
}

void skim_cw_reader_free(SkimCwReader *r) {
  if (!r)
    return;
  if (r->conv) {
    for (guint i = 0; i < r->n_conv; i++) {
      g_free(r->conv[i].gamma);
      g_free(r->conv[i].beta);
      g_free(r->conv[i].w);
      g_free(r->conv[i].b);
    }
    g_free(r->conv);
  }
  g_free(r->alphabet);
  g_free(r->head_w);
  g_free(r->head_b);
  g_free(r->test_x);
  g_free(r->test_y);
  g_free(r);
}

SkimCwReader *skim_cw_reader_load(const char *path, GError **error) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                "cw_reader: cannot open %s", path);
    return NULL;
  }
  SkimCwReader *r = g_new0(SkimCwReader, 1);
  char magic[4];
  guint ver = 0, alen = 0;
  gboolean ok = rd_bytes(f, magic, 4) && memcmp(magic, "CWRD", 4) == 0 &&
                rd_u32(f, &ver) && ver == 2 && rd_u32(f, &r->n_conv) &&
                rd_u32(f, &r->ch) && rd_u32(f, &r->n_out) &&
                rd_u32(f, &r->in_dim) && rd_u32(f, &alen) &&
                alen == r->n_out - 1 && r->n_conv >= 1 && r->n_conv <= 64 &&
                r->ch <= 1024 && r->in_dim >= 1 && r->in_dim <= 64;
  if (ok) {
    r->alphabet = g_malloc0(alen + 1);
    ok = rd_bytes(f, r->alphabet, alen);
  }
  if (ok) {
    r->conv = g_new0(Conv, r->n_conv);
    for (guint i = 0; ok && i < r->n_conv; i++) {
      Conv *c = &r->conv[i];
      guint has_norm = 0;
      ok = rd_u32(f, &c->in_ch) && rd_u32(f, &c->dil) &&
           rd_u32(f, &has_norm) && c->in_ch <= 1024 && c->dil >= 1 &&
           c->dil <= 4096 && has_norm <= 1;
      if (!ok)
        break;
      if (has_norm) {
        c->gamma = g_new(float, c->in_ch);
        c->beta  = g_new(float, c->in_ch);
        ok = rd_bytes(f, c->gamma, sizeof(float) * c->in_ch) &&
             rd_bytes(f, c->beta, sizeof(float) * c->in_ch);
        if (!ok)
          break;
      }
      c->w = g_new(float, (gsize)r->ch * c->in_ch * KERNEL);
      c->b = g_new(float, r->ch);
      ok = rd_bytes(f, c->w, sizeof(float) * r->ch * c->in_ch * KERNEL) &&
           rd_bytes(f, c->b, sizeof(float) * r->ch);
    }
  }
  if (ok) {
    r->head_w = g_new(float, (gsize)r->n_out * r->ch);
    r->head_b = g_new(float, r->n_out);
    ok = rd_bytes(f, r->head_w, sizeof(float) * r->n_out * r->ch) &&
         rd_bytes(f, r->head_b, sizeof(float) * r->n_out);
  }
  if (ok && rd_u32(f, &r->test_t) && r->test_t > 0 && r->test_t <= 65536) {
    r->test_x = g_new(float, (gsize)r->test_t * r->in_dim);
    r->test_y = g_new(float, (gsize)r->test_t * r->n_out);
    ok = rd_bytes(f, r->test_x, sizeof(float) * r->test_t * r->in_dim) &&
         rd_bytes(f, r->test_y, sizeof(float) * r->test_t * r->n_out);
  } else {
    ok = FALSE;
  }
  fclose(f);
  if (!ok) {
    skim_cw_reader_free(r);
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                "cw_reader: %s is not a valid CWRD v1 blob", path);
    return NULL;
  }
  return r;
}

/* Forward pass over features x[T][in_dim]; returns logits[T][n_out]
 * (g_free). Mirrors ml/train_ctc.py::Reader: layer 0 is a plain conv+ReLU,
 * every later layer is a pre-norm residual block
 * y += relu(conv(layernorm(y))). */
static float *forward(const SkimCwReader *r, const float *x, guint T) {
  const guint ch = r->ch;
  float *cur = g_new0(float, (gsize)T * ch);
  float *nxt = g_new0(float, (gsize)T * ch);
  float *nrm = g_new0(float, (gsize)T * ch);    /* normed conv input        */
  for (guint li = 0; li < r->n_conv; li++) {
    const Conv *c = &r->conv[li];
    const guint in_ch = c->in_ch;
    const float *in = li == 0 ? x : cur;
    if (c->gamma) {
      for (guint t = 0; t < T; t++) {           /* per-timestep LayerNorm   */
        const float *ip = in + (gsize)t * in_ch;
        float *np = nrm + (gsize)t * in_ch;
        float mean = 0.0f, var = 0.0f;
        for (guint i = 0; i < in_ch; i++) { mean += ip[i]; }
        mean /= (float)in_ch;
        for (guint i = 0; i < in_ch; i++) {
          const float d = ip[i] - mean;
          var += d * d;
        }
        var /= (float)in_ch;
        const float inv = 1.0f / sqrtf(var + LN_EPS);
        for (guint i = 0; i < in_ch; i++) {
          np[i] = (ip[i] - mean) * inv * c->gamma[i] + c->beta[i];
        }
      }
      in = nrm;
    }
    for (guint t = 0; t < T; t++) {
      float *out = nxt + (gsize)t * ch;
      for (guint o = 0; o < ch; o++) { out[o] = c->b[o]; }
      for (guint k = 0; k < KERNEL; k++) {
        const gint ts = (gint)t + ((gint)k - 1) * (gint)c->dil;
        if (ts < 0 || ts >= (gint)T)
          continue;
        const float *ip = in + (gsize)ts * in_ch;
        const float *wp = c->w + (gsize)k;      /* w[o][i][k] stride KERNEL  */
        for (guint o = 0; o < ch; o++) {
          const float *wo = wp + (gsize)o * in_ch * KERNEL;
          float s = 0.0f;
          for (guint i = 0; i < in_ch; i++) { s += wo[i * KERNEL] * ip[i]; }
          out[o] += s;
        }
      }
      for (guint o = 0; o < ch; o++) {          /* relu + residual           */
        const float v = out[o] > 0.0f ? out[o] : 0.0f;
        out[o] = c->gamma ? cur[(gsize)t * ch + o] + v : v;
      }
    }
    float *tmp = cur;
    cur = nxt;
    nxt = tmp;
  }
  g_free(nrm);
  float *logits = g_new(float, (gsize)T * r->n_out);
  for (guint t = 0; t < T; t++) {
    const float *ip = cur + (gsize)t * ch;
    float *op = logits + (gsize)t * r->n_out;
    for (guint o = 0; o < r->n_out; o++) {
      const float *wo = r->head_w + (gsize)o * ch;
      float s = r->head_b[o];
      for (guint i = 0; i < ch; i++) { s += wo[i] * ip[i]; }
      op[o] = s;
    }
  }
  g_free(cur);
  g_free(nxt);
  return logits;
}

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

char *skim_cw_reader_read(const SkimCwReader *r, const gboolean *key_mark,
                          const double *dur_ms, guint n) {
  if (!r || n < 4 || r->in_dim != 4)
    return g_strdup("");
  /* Features exactly as in training: log(dur / median mark), key ±1, and
   * clamped log ratios to the neighbouring runs. */
  double *marks = g_new(double, n);
  guint nm = 0;
  for (guint i = 0; i < n; i++) {
    if (key_mark[i]) { marks[nm++] = MAX(dur_ms[i], 1.0); }
  }
  if (nm == 0) {
    g_free(marks);
    return g_strdup("");
  }
  qsort(marks, nm, sizeof(double), cmp_double);
  const double med = nm % 2 ? marks[nm / 2]
                            : 0.5 * (marks[nm / 2 - 1] + marks[nm / 2]);
  g_free(marks);
  float *x = g_new(float, (gsize)n * 4);
  for (guint i = 0; i < n; i++) {
    const double d = MAX(dur_ms[i], 1.0);
    const double dp = i > 0 ? MAX(dur_ms[i - 1], 1.0) : d;
    const double dn = i + 1 < n ? MAX(dur_ms[i + 1], 1.0) : d;
    x[4 * i]     = (float)log(d / MAX(med, 1.0));
    x[4 * i + 1] = key_mark[i] ? 1.0f : -1.0f;
    x[4 * i + 2] = (float)CLAMP(log(d / dp), -3.0, 3.0);
    x[4 * i + 3] = (float)CLAMP(log(d / dn), -3.0, 3.0);
  }
  float *logits = forward(r, x, n);
  g_free(x);

  GString *out = g_string_new(NULL);
  guint prev = 0;
  for (guint t = 0; t < n; t++) {
    const float *op = logits + (gsize)t * r->n_out;
    guint best = 0;
    for (guint o = 1; o < r->n_out; o++) {
      if (op[o] > op[best]) { best = o; }
    }
    if (best != 0 && best != prev) {
      g_string_append_c(out, r->alphabet[best - 1]);
    }
    prev = best;
  }
  g_free(logits);
  return g_string_free(out, FALSE);
}

double skim_cw_reader_selftest(const SkimCwReader *r) {
  float *logits = forward(r, r->test_x, r->test_t);
  double worst = 0.0;
  for (gsize i = 0; i < (gsize)r->test_t * r->n_out; i++) {
    const double d = fabs((double)logits[i] - (double)r->test_y[i]);
    worst = MAX(worst, d);
  }
  g_free(logits);
  return worst;
}
