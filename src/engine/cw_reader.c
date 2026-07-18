/* cw_reader.c — neural CW reader: dilated-TCN forward pass + CTC greedy
 * decode in plain C. See cw_reader.h for the role and ml/export_c.py for the
 * blob format this loads.
 *
 * The forward pass mirrors ml/train_ctc.py::Reader exactly: n_conv dilated
 * 1-D convolutions (kernel 3, ReLU) with residual adds from the second layer
 * on, then a 1×1 head. Kernel taps sit at (look−2d, look−d, look) relative
 * to the output timestep — `look` is the layer's bounded right context
 * (CWRD v3; v2 blobs are the symmetric look=d net). Features are computed
 * here the same way training did.
 *
 * Streaming (v3): the bounded lookahead makes the net runnable one run at a
 * time — per-layer rings hold just enough input history, each push advances
 * every layer as far as its lookahead allows, and logits become final
 * sum(look) runs behind the newest input. Same float ops in the same order
 * as the batch pass, so stream output is bit-identical to read().
 *
 * Cost: O(T · ch² · 3 · n_conv) either way — a 60 s over (~500 runs) is
 * ~170 MFLOP total, ~330 kFLOP per pushed run when streamed. Engine thread.
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
  guint  in_ch, dil, look;             /* look = right context (v2: = dil)   */
  float *gamma, *beta;                 /* pre-norm LayerNorm; NULL = layer 0 */
  float *w;                            /* [ch][in_ch][KERNEL]                */
  float *b;                            /* [ch]                               */
} Conv;

struct _SkimCwReader {
  guint  ver;                          /* blob version (2 or 3)              */
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
  guint alen = 0;
  gboolean ok = rd_bytes(f, magic, 4) && memcmp(magic, "CWRD", 4) == 0 &&
                rd_u32(f, &r->ver) && (r->ver == 2 || r->ver == 3) &&
                rd_u32(f, &r->n_conv) &&
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
           (r->ver == 2 || rd_u32(f, &c->look)) &&
           rd_u32(f, &has_norm) && c->in_ch <= 1024 && c->dil >= 1 &&
           c->dil <= 4096 && has_norm <= 1;
      if (r->ver == 2) { c->look = c->dil; }     /* symmetric legacy net     */
      ok = ok && c->look <= 2 * c->dil;
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
                "cw_reader: %s is not a valid CWRD v2/v3 blob", path);
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
        const gint ts = (gint)t + ((gint)k - 2) * (gint)c->dil +
                        (gint)c->look;
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

/* --- features --------------------------------------------------------------------
 * v2 nets normalize by the WHOLE-over mark median (batch only); v3 nets by a
 * CAUSAL windowed median — the last MED_WIN marks up to and including run i,
 * exactly what the live stream maintains (mirrors ml/train_ctc.py::features,
 * incl. the med=100 fallback before the first mark). */
#define MED_WIN 31

typedef struct {
  double win[MED_WIN];                 /* ring of recent mark durs (>= 1.0)  */
  guint  n, pos;
} FeatMed;

static double featmed_push(FeatMed *m, gboolean key, double d) {
  if (key) {
    m->win[m->pos] = d;
    m->pos = (m->pos + 1) % MED_WIN;
    if (m->n < MED_WIN) { m->n++; }
  }
  if (!m->n)
    return 100.0;
  double tmp[MED_WIN];
  memcpy(tmp, m->win, sizeof(double) * m->n);   /* ring order is irrelevant */
  qsort(tmp, m->n, sizeof(double), cmp_double);
  return m->n % 2 ? tmp[m->n / 2]
                  : 0.5 * (tmp[m->n / 2 - 1] + tmp[m->n / 2]);
}

char *skim_cw_reader_read(const SkimCwReader *r, const gboolean *key_mark,
                          const double *dur_ms, guint n) {
  if (!r || n < 4 || r->in_dim != 4)
    return g_strdup("");
  float *x = g_new(float, (gsize)n * 4);
  if (r->ver == 2) {
    double *marks = g_new(double, n);
    guint nm = 0;
    for (guint i = 0; i < n; i++) {
      if (key_mark[i]) { marks[nm++] = MAX(dur_ms[i], 1.0); }
    }
    if (nm == 0) {
      g_free(marks);
      g_free(x);
      return g_strdup("");
    }
    qsort(marks, nm, sizeof(double), cmp_double);
    const double med = nm % 2 ? marks[nm / 2]
                              : 0.5 * (marks[nm / 2 - 1] + marks[nm / 2]);
    g_free(marks);
    for (guint i = 0; i < n; i++) {
      const double d = MAX(dur_ms[i], 1.0);
      x[4 * i] = (float)log(d / MAX(med, 1.0));
    }
  } else {
    FeatMed med = { { 0 }, 0, 0 };
    for (guint i = 0; i < n; i++) {
      const double d = MAX(dur_ms[i], 1.0);
      x[4 * i] = (float)log(d / featmed_push(&med, key_mark[i], d));
    }
  }
  for (guint i = 0; i < n; i++) {
    const double d = MAX(dur_ms[i], 1.0);
    const double dp = i > 0 ? MAX(dur_ms[i - 1], 1.0) : d;
    const double dn = i + 1 < n ? MAX(dur_ms[i + 1], 1.0) : d;
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

/* --- streaming (v3) --------------------------------------------------------------
 * Per-layer rings of INPUT activations; each push lets every layer advance
 * as far as its bounded lookahead allows, so logits (and greedy commits)
 * become final sum(look) runs behind the newest input. stream_step() redoes
 * the batch forward's float ops in the identical order per timestep —
 * push+flush output is bit-equal to read() over the same runs. */

struct _SkimCwReaderStream {
  const SkimCwReader *r;
  guint    lag;                        /* sum of look over all layers        */
  /* features */
  FeatMed  med;
  guint64  n_in;                       /* runs accepted so far               */
  gboolean have_pend;                  /* newest run waits for its f3        */
  float    pend_f[3];                  /* f0, f1, f2 of that run             */
  double   pend_dur;
  /* per-layer input rings: row ts lives at (ts % cap[l]) × in_ch */
  float  **ring;
  guint   *cap;
  gint64  *in_avail;                   /* newest input timestep (-1 = none)  */
  gint64  *out_done;                   /* newest produced output (-1)        */
  /* CTC greedy */
  guint    prev_id;
  GString *text;                       /* committed, not yet handed out      */
  GArray  *pos;                        /* guint run index per text char      */
  GArray  *marg;                       /* float logit margin per text char   */
};

SkimCwReaderStream *skim_cw_reader_stream_new(const SkimCwReader *r) {
  if (!r || r->ver != 3 || r->in_dim != 4)
    return NULL;
  SkimCwReaderStream *s = g_new0(SkimCwReaderStream, 1);
  s->r = r;
  for (guint l = 0; l < r->n_conv; l++) { s->lag += r->conv[l].look; }
  s->ring     = g_new0(float *, r->n_conv);
  s->cap      = g_new(guint, r->n_conv);
  s->in_avail = g_new(gint64, r->n_conv);
  s->out_done = g_new(gint64, r->n_conv);
  for (guint l = 0; l < r->n_conv; l++) {
    /* the taps' 2d+1-deep window plus the flush backlog (≤ lag rows)       */
    s->cap[l]  = 2 * r->conv[l].dil + 1 + s->lag;
    s->ring[l] = g_new(float, (gsize)s->cap[l] * r->conv[l].in_ch);
    s->in_avail[l] = s->out_done[l] = -1;
  }
  s->text = g_string_new(NULL);
  s->pos  = g_array_new(FALSE, FALSE, sizeof(guint));
  s->marg = g_array_new(FALSE, FALSE, sizeof(float));
  return s;
}

void skim_cw_reader_stream_free(SkimCwReaderStream *s) {
  if (!s)
    return;
  for (guint l = 0; l < s->r->n_conv; l++) { g_free(s->ring[l]); }
  g_free(s->ring);
  g_free(s->cap);
  g_free(s->in_avail);
  g_free(s->out_done);
  g_string_free(s->text, TRUE);
  g_array_free(s->pos, TRUE);
  g_array_free(s->marg, TRUE);
  g_free(s);
}

static const float *st_row(const SkimCwReaderStream *s, guint l, gint64 ts) {
  return s->ring[l] +
         (gsize)(ts % (gint64)s->cap[l]) * s->r->conv[l].in_ch;
}

/* One output timestep t of layer l — forward()'s float ops in the same
 * order, taps read from the ring; ts outside [0, in_max] are skipped (the
 * batch zero-pad edges). out[] gets the post-residual activation. */
static void stream_step(const SkimCwReaderStream *s, guint l, gint64 t,
                        float *out) {
  const Conv *c = &s->r->conv[l];
  const guint ch = s->r->ch, in_ch = c->in_ch;
  const gint64 in_max = s->in_avail[l];
  float nrm[1024];                     /* in_ch ≤ 1024 (loader bound)        */
  for (guint o = 0; o < ch; o++) { out[o] = c->b[o]; }
  for (guint k = 0; k < KERNEL; k++) {
    const gint64 ts = t + ((gint)k - 2) * (gint)c->dil + (gint)c->look;
    if (ts < 0 || ts > in_max)
      continue;
    const float *ip = st_row(s, l, ts);
    if (c->gamma) {                    /* pre-norm, recomputed per tap       */
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
        nrm[i] = (ip[i] - mean) * inv * c->gamma[i] + c->beta[i];
      }
    }
    const float *rp = c->gamma ? nrm : ip;
    const float *wp = c->w + (gsize)k;
    for (guint o = 0; o < ch; o++) {
      const float *wo = wp + (gsize)o * in_ch * KERNEL;
      float sum = 0.0f;
      for (guint i = 0; i < in_ch; i++) { sum += wo[i * KERNEL] * rp[i]; }
      out[o] += sum;
    }
  }
  const float *raw = st_row(s, l, t);  /* still ringed: t ≥ in_max − 2d      */
  for (guint o = 0; o < ch; o++) {
    const float v = out[o] > 0.0f ? out[o] : 0.0f;
    out[o] = c->gamma ? raw[o] + v : v;
  }
}

/* Head + greedy commit for one final activation row; t is the output
 * timestep (== input run index) the char is attributed to. The margin —
 * winning logit minus runner-up at the commit frame — is the net's own
 * confidence: sharp on clean reads, near zero where classes fight (gap
 * fusions, babble). The pane's per-word gate rides on it. */
static void stream_head(SkimCwReaderStream *s, const float *act, gint64 t) {
  const SkimCwReader *r = s->r;
  guint best = 0;
  float bv = 0.0f, second = 0.0f;
  for (guint o = 0; o < r->n_out; o++) {
    const float *wo = r->head_w + (gsize)o * r->ch;
    float v = r->head_b[o];
    for (guint i = 0; i < r->ch; i++) { v += wo[i] * act[i]; }
    if (o == 0 || v > bv) {            /* first-max, as the batch argmax     */
      if (o) { second = bv; }
      best = o;
      bv = v;
    } else if (o == 1 || v > second) {
      second = v;
    }
  }
  if (best != 0 && best != s->prev_id) {
    g_string_append_c(s->text, r->alphabet[best - 1]);
    const guint p = (guint)t;
    const float m = bv - second;
    g_array_append_val(s->pos, p);
    g_array_append_val(s->marg, m);
  }
  s->prev_id = best;
}

/* Append one finalized feature row and advance every layer as far as its
 * lookahead allows; final-layer rows fall through head+greedy into text. */
static void stream_feed(SkimCwReaderStream *s, const float *feat) {
  const SkimCwReader *r = s->r;
  float out[1024];                     /* ch ≤ 1024 (loader bound)           */
  s->in_avail[0]++;
  memcpy(s->ring[0] +
             (gsize)(s->in_avail[0] % (gint64)s->cap[0]) * r->in_dim,
         feat, sizeof(float) * r->in_dim);
  for (guint l = 0; l < r->n_conv; l++) {
    while (s->out_done[l] <= s->in_avail[l] - 1 - (gint64)r->conv[l].look) {
      const gint64 t = s->out_done[l] + 1;
      stream_step(s, l, t, out);
      s->out_done[l] = t;
      if (l + 1 < r->n_conv) {
        s->in_avail[l + 1]++;          /* == t                               */
        memcpy(s->ring[l + 1] +
                   (gsize)(t % (gint64)s->cap[l + 1]) * r->ch,
               out, sizeof(float) * r->ch);
      } else {
        stream_head(s, out, t);
      }
    }
  }
}

/* read() refuses overs shorter than 4 runs; the stream must match, so text
 * is withheld until the 4th run arrives (irrelevant with the real ~22-run
 * lag, decisive for the gate's toy nets). pos (optional out) receives the
 * per-char run indices, parallel to the returned string. */
static char *stream_drain(SkimCwReaderStream *s, guint **pos, float **marg) {
  if (pos) { *pos = NULL; }
  if (marg) { *marg = NULL; }
  if (s->n_in < 4 || !s->text->len)
    return NULL;
  if (pos) {
    *pos = g_new(guint, s->text->len);
    memcpy(*pos, s->pos->data, sizeof(guint) * s->text->len);
  }
  if (marg) {
    *marg = g_new(float, s->text->len);
    memcpy(*marg, s->marg->data, sizeof(float) * s->text->len);
  }
  char *out = g_string_free(s->text, FALSE);
  s->text = g_string_new(NULL);
  g_array_set_size(s->pos, 0);
  g_array_set_size(s->marg, 0);
  return out;
}

char *skim_cw_reader_stream_push_pos(SkimCwReaderStream *s, gboolean key_mark,
                                     double dur_ms, guint **pos,
                                     float **marg) {
  if (!s) {
    if (pos) { *pos = NULL; }
    if (marg) { *marg = NULL; }
    return NULL;
  }
  const double d = MAX(dur_ms, 1.0);
  if (s->have_pend) {                  /* the newcomer supplies pend's f3    */
    const float f[4] = { s->pend_f[0], s->pend_f[1], s->pend_f[2],
                         (float)CLAMP(log(s->pend_dur / d), -3.0, 3.0) };
    stream_feed(s, f);
  }
  const double dp = s->have_pend ? s->pend_dur : d;
  s->pend_f[0] = (float)log(d / featmed_push(&s->med, key_mark, d));
  s->pend_f[1] = key_mark ? 1.0f : -1.0f;
  s->pend_f[2] = (float)CLAMP(log(d / dp), -3.0, 3.0);
  s->pend_dur  = d;
  s->have_pend = TRUE;
  s->n_in++;
  return stream_drain(s, pos, marg);
}

char *skim_cw_reader_stream_push(SkimCwReaderStream *s, gboolean key_mark,
                                 double dur_ms) {
  return skim_cw_reader_stream_push_pos(s, key_mark, dur_ms, NULL, NULL);
}

char *skim_cw_reader_stream_flush_pos(SkimCwReaderStream *s, guint **pos,
                                      float **marg) {
  if (pos) { *pos = NULL; }
  if (marg) { *marg = NULL; }
  if (!s)
    return NULL;
  const SkimCwReader *r = s->r;
  if (s->have_pend) {                  /* last run: f3 = 0 (batch: dn = d)   */
    const float f[4] = { s->pend_f[0], s->pend_f[1], s->pend_f[2], 0.0f };
    stream_feed(s, f);
  }
  const gint64 T = s->in_avail[0] + 1;
  char *txt = NULL;
  if (T >= 4) {
    /* right edge: finish every layer to T−1 in order — each layer's input
     * is complete by the time its turn comes; taps past it are skipped     */
    float out[1024];
    for (guint l = 0; l < r->n_conv; l++) {
      while (s->out_done[l] < T - 1) {
        const gint64 t = s->out_done[l] + 1;
        stream_step(s, l, t, out);
        s->out_done[l] = t;
        if (l + 1 < r->n_conv) {
          s->in_avail[l + 1]++;
          memcpy(s->ring[l + 1] +
                     (gsize)(t % (gint64)s->cap[l + 1]) * r->ch,
                 out, sizeof(float) * r->ch);
        } else {
          stream_head(s, out, t);
        }
      }
    }
    if (s->text->len) {
      if (pos) {
        *pos = g_new(guint, s->text->len);
        memcpy(*pos, s->pos->data, sizeof(guint) * s->text->len);
      }
      if (marg) {
        *marg = g_new(float, s->text->len);
        memcpy(*marg, s->marg->data, sizeof(float) * s->text->len);
      }
      txt = g_string_free(s->text, FALSE);
      s->text = g_string_new(NULL);
    }
  }
  /* reset for the next over (rings need no wipe: ts < 0 taps are skipped
   * and every valid row is rewritten before it is read) */
  for (guint l = 0; l < r->n_conv; l++) {
    s->in_avail[l] = s->out_done[l] = -1;
  }
  memset(&s->med, 0, sizeof(s->med));
  s->n_in = 0;
  s->have_pend = FALSE;
  s->prev_id = 0;
  g_string_truncate(s->text, 0);
  g_array_set_size(s->pos, 0);
  g_array_set_size(s->marg, 0);
  return txt;
}

char *skim_cw_reader_stream_flush(SkimCwReaderStream *s) {
  return skim_cw_reader_stream_flush_pos(s, NULL, NULL);
}
