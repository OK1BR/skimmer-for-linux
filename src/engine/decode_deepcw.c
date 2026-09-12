/* decode_deepcw.c — DeepCW neural CW backend (see decode_deepcw.h).
 *
 * Per channel: complex baseband (250 Hz for the CW bank) → 20-point Hann
 * DFT every 4 samples (80 ms window, 16 ms hop — the model was trained on
 * 80 ms / 15 ms; the 6.7 % time stretch sits well inside its speed range,
 * offline-verified 15–35 WPM) → the ±5 bins (±62.5 Hz, the channel) kept
 * per frame in a ring → on the channel's tick: gate (keyed line over the
 * floor) → 65-bin tile, channel bins at 27..37, peak-normalised, log1p →
 * one model run → greedy CTC → commit rule → text out through process().
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_deepcw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ort_shim.h"

/* Model contract (deepcw-engine model.onnx.json): 65 bins × 12.5 Hz,
 * classes 0..41, blank 41, space 40. */
#define DCW_BINS      65
#define DCW_CENTRE    32
#define DCW_KEEP      11              /* channel bins −5..+5              */
#define DCW_HALF      5
#define DCW_CLASSES   42
#define DCW_BLANK     41
#define DCW_SPACE     40
#define DCW_BIN_HZ    12.5
#define DCW_PEAK_MAG  30.0f           /* peak → log1p ≈ 3.4 (training range)*/

static const char DCW_CHARS[DCW_CLASSES] = ",./0123456789?ABCDEFGHIJKLMNOPQRSTUVWXYZ ";

/* Tunables (env, read once). Seconds unless noted. */
typedef struct {
  double win_s;      /* ring length = max pending audio         (10)     */
  double tick_s;     /* inference cadence per channel           (1.6)    */
  double tail_s;     /* tail guard: no commit closer to the end (1.5)    */
  double minconf_s;  /* no commit inside the first seconds      (2.0)    */
  double gate_db;    /* line over floor to run inference        (6)      */
  int    threads;    /* ONNX Runtime intra-op threads           (4)      */
  double space_p;    /* min posterior for a word gap to count   (0 = off)*/
  gboolean sync;     /* SKIM_DEEPCW_SYNC=1: inference inline (replays)   */
  int    workers;    /* async inference threads                 (2)      */
  int    batch;      /* windows per model run on a worker      (32)      */
  int    debug;      /* SKIM_DEEPCW_DEBUG: 1 = ticks, 2 = + frames      */
} DcwTune;

static DcwTune g_tune;
static gsize   g_tune_once;

static double env_d(const char *name, double dflt) {
  const char *v = g_getenv(name);
  return v && v[0] ? g_ascii_strtod(v, NULL) : dflt;
}

static void tune_init(void) {
  if (g_once_init_enter(&g_tune_once)) {
    g_tune.win_s     = env_d("SKIM_DEEPCW_WIN", 10.0);
    g_tune.tick_s    = env_d("SKIM_DEEPCW_TICK", 1.6);
    g_tune.tail_s    = env_d("SKIM_DEEPCW_TAIL", 1.5);
    g_tune.minconf_s = env_d("SKIM_DEEPCW_MINCONF", 2.0);
    g_tune.gate_db   = env_d("SKIM_DEEPCW_GATE_DB", 6.0);
    g_tune.threads   = (int)env_d("SKIM_DEEPCW_THREADS", 4);
    g_tune.space_p   = env_d("SKIM_DEEPCW_SPACE_P", 0.8);
    g_tune.sync      = env_d("SKIM_DEEPCW_SYNC", 0) != 0;
    g_tune.workers   = CLAMP((int)env_d("SKIM_DEEPCW_WORKERS", 2), 1, 8);
    g_tune.batch     = CLAMP((int)env_d("SKIM_DEEPCW_BATCH", 32), 1, 256);
    g_tune.debug     = (int)env_d("SKIM_DEEPCW_DEBUG", 0);
    g_once_init_leave(&g_tune_once, 1);
  }
}

/* ---- runtime + model: one session per process --------------------------- */
/* The session is refcounted: workers hold a ref for the duration of a
 * model run, so a reset (device change) can drop the global ref at any
 * time and the last user frees it. */
typedef struct { SkimOrtSession *s; gint refs; } DcwSess;

static GMutex          g_lock;
static SkimOrt        *g_ort;
static DcwSess        *g_cur;           /* under g_lock                   */
static GError         *g_load_err;
static gboolean        g_tried;
static char           *g_rt_info;
static char           *g_device;        /* set_device(); NULL = env/cpu   */

static DcwSess *sess_acquire(void) {
  g_mutex_lock(&g_lock);
  DcwSess *c = g_cur;
  if (c) { g_atomic_int_inc(&c->refs); }
  g_mutex_unlock(&g_lock);
  return c;
}

static void sess_release(DcwSess *c) {
  if (c && g_atomic_int_dec_and_test(&c->refs)) {
    skim_ort_session_free(c->s);
    g_free(c);
  }
}

void skim_decode_deepcw_set_device(const char *device) {
  g_mutex_lock(&g_lock);
  g_free(g_device);
  g_device = device && device[0] ? g_ascii_strdown(device, -1) : NULL;
  g_mutex_unlock(&g_lock);
}

void skim_decode_deepcw_reset(void) {
  g_mutex_lock(&g_lock);
  DcwSess *old = g_cur;
  g_cur = NULL;
  g_tried = FALSE;
  g_clear_error(&g_load_err);
  g_clear_pointer(&g_rt_info, g_free);
  g_mutex_unlock(&g_lock);
  sess_release(old);
}

char *skim_decode_deepcw_model_path(void) {
  const char *env = g_getenv("SKIM_DEEPCW_MODEL");
  if (env && env[0]) { return g_strdup(env); }
  return g_build_filename(g_get_user_data_dir(), "skimmer-for-linux",
                          "models", "deepcw", "model.onnx", NULL);
}

static gboolean ensure_session(GError **error) {
  tune_init();
  g_mutex_lock(&g_lock);
  if (!g_tried) {
    g_tried = TRUE;
    char *model = skim_decode_deepcw_model_path();
    if (!g_file_test(model, G_FILE_TEST_IS_REGULAR)) {
      g_set_error(&g_load_err, SKIM_ORT_ERROR, 10,
                  "model file not found: %s", model);
    } else {
      if (!g_ort) { g_ort = skim_ort_open(NULL, &g_load_err); }
      if (g_ort) {
        const char *envdev = g_getenv("SKIM_DEEPCW_DEVICE");
        const char *dev = g_device ? g_device
                        : (envdev && envdev[0] ? envdev : "cpu");
        SkimOrtSession *ss = skim_ort_session_new(g_ort, model, g_tune.threads,
                                                  dev, &g_load_err);
        if (ss) {
          g_cur = g_new0(DcwSess, 1);
          g_cur->s = ss;
          g_cur->refs = 1;
          g_rt_info = g_strdup_printf("%s via %s, %s", skim_ort_version(g_ort),
                                      skim_ort_library(g_ort),
                                      skim_ort_session_device(ss));
          g_message("deepcw: ONNX Runtime %s, model %s, in '%s' out '%s', "
                    "%d threads, batch %d", g_rt_info, model,
                    skim_ort_session_input_name(ss),
                    skim_ort_session_output_name(ss), g_tune.threads,
                    g_tune.batch);
        }
      }
    }
    g_free(model);
  }
  const gboolean ok = g_cur != NULL;
  if (!ok && error && g_load_err) { *error = g_error_copy(g_load_err); }
  g_mutex_unlock(&g_lock);
  return ok;
}

gboolean skim_decode_deepcw_available(GError **error) {
  return ensure_session(error);
}

const char *skim_decode_deepcw_runtime_info(void) { return g_rt_info; }

/* ---- the commit rule (pure) ---------------------------------------------- */
typedef struct { guint8 cls; guint64 t; float p; } Spike;

/* Greedy CTC collapse of logp[T][42]: one Spike per emitted character at
 * the frame where its run starts, p = posterior there. */
static GArray *ctc_collapse(const float *logp, guint T, guint64 w0) {
  GArray *a = g_array_new(FALSE, FALSE, sizeof(Spike));
  int prev = -1;
  for (guint t = 0; t < T; t++) {
    const float *row = logp + (gsize)t * DCW_CLASSES;
    int best = 0;
    for (int c = 1; c < DCW_CLASSES; c++) { if (row[c] > row[best]) best = c; }
    if (best == DCW_BLANK) { prev = -1; continue; }
    if (best != prev) {
      Spike s = { (guint8)best, w0 + t, expf(row[best]) };
      if (g_tune.debug > 2 && best == DCW_SPACE) {
        g_printerr("deepcw: space p=%.2f at %" G_GUINT64_FORMAT "\n", s.p, s.t);
      }
      g_array_append_val(a, s);
    }
    prev = best;
  }
  return a;
}

/* A word gap the model is unsure about (posterior < space_p) is more often
 * a torn fist than a gap — but only where it would tear a SHORT piece off
 * a token: "OK2B TK" → OK2BTK, "OK1C Z" → OK1CZ (80 m fixture: both
 * halves validated as calls and the panadapter would have shown OK2B),
 * while "TEST TA5ARU" with a weak gap stays two words — dropping every
 * weak gap fused weak stations' text and lost TA5ARU on 20 m. Such a
 * space is neither a split point nor emitted. */
static void drop_weak_tears(GArray *sp, double bar) {
  if (bar <= 0.0) return;
  for (guint i = 0; i < sp->len; i++) {
    Spike *s = &g_array_index(sp, Spike, i);
    if (s->cls != DCW_SPACE || s->p >= bar) continue;
    guint left = 0, right = 0;
    for (gint j = (gint)i - 1; j >= 0 && g_array_index(sp, Spike, j).cls != DCW_SPACE; j--) left++;
    for (guint j = i + 1; j < sp->len && g_array_index(sp, Spike, j).cls != DCW_SPACE; j++) right++;
    if (left > 0 && right > 0 && MIN(left, right) <= 2) {
      g_array_remove_index(sp, i);
      i--;
    }
  }
}

guint skim_deepcw_commit(const float *logp, guint T, guint64 w0,
                         guint64 *committed, guint tail, guint minconf,
                         gboolean force, GString *out, double *conf) {
  const guint64 w1 = w0 + T;
  tune_init();
  GArray *sp = ctc_collapse(logp, T, w0);
  drop_weak_tears(sp, g_tune.space_p);
  guint n = 0;
  double psum = 0.0;
  /* Nothing at all: the window is silence (or the model is unsure) —
   * advance the cursor to the tail guard so a quiet channel never grows
   * a long pending span (the next over then starts ≤ tail before it). */
  if (sp->len == 0) {
    if (w1 > *committed + tail) { *committed = w1 - tail; }
    g_array_free(sp, TRUE);
    if (conf) { *conf = 0.0; }
    return 0;
  }
  guint64 upto = 0;                    /* commit chars with t ≤ upto       */
  gboolean have = FALSE;
  if (w1 > tail && w0 + minconf <= w1 - tail) {
    for (gint i = (gint)sp->len - 1; i >= 0; i--) {
      const Spike *s = &g_array_index(sp, Spike, i);
      if (s->cls == DCW_SPACE && s->t >= w0 + minconf && s->t <= w1 - tail) {
        upto = s->t;
        have = TRUE;
        break;
      }
    }
  }
  if (!have && force && w1 > tail) {
    upto = w1 - tail;
    have = TRUE;
  }
  if (have) {
    for (guint i = 0; i < sp->len; i++) {
      const Spike *s = &g_array_index(sp, Spike, i);
      if (s->t > upto || s->t < *committed) continue;
      /* A commit ends at a word gap and the next window starts after it:
       * the model then tends to open with another space — squeeze runs. */
      if (s->cls == DCW_SPACE && (out->len == 0 || out->str[out->len - 1] == ' '))
        continue;
      g_string_append_c(out, DCW_CHARS[s->cls]);
      psum += s->p;
      n++;
    }
    *committed = upto + 1;
  }
  g_array_free(sp, TRUE);
  if (conf) { *conf = n ? psum / n : 0.0; }
  return n;
}

/* ---- per-channel state ---------------------------------------------------- */
typedef struct {
  double  rate;
  guint   N, hop;
  float  *win;                  /* Hann N                                  */
  float  *tw_re, *tw_im;        /* [DCW_KEEP][N]                           */
  float  *sring;                /* complex sample ring, N frames           */
  guint   spos, sfill, since;
  float  *ring;                 /* [ring_frames][DCW_KEEP] magnitudes      */
  guint   ring_frames;
  guint64 frames_abs;
  guint64 committed;
  guint   tick_frames, tick_phase, tail_f, minconf_f;
  double  lvl_ema, off_ema;
  double  env_lo;               /* channel noise floor (inner bins, EMA)   */
  double  win_peak;             /* strongest line magnitude in the window  */
  double  snr_db, wpm;
  gboolean gate;
  double  ratio_db, duty;
  guint   ticks;
  GString *out;
  double  out_conf;
  double  freq_hz;
  gboolean dead;
  gboolean nomodel;
  float  *tile;                 /* scratch [ring_frames][DCW_BINS]         */
  /* async: one job in flight per channel; the result waits in the mailbox
   * for the engine thread, which runs the commit rule (deterministic order,
   * no locking around the text path). refs keeps the state alive while a
   * job references it; channel_free of a referenced state defers. */
  gint    refs;
  gboolean dying;
  gboolean inflight;
  float  *res_logp;             /* mailbox: logp[To][42] or NULL           */
  guint   res_T;
  guint64 res_w0;
  gboolean res_force;
} DcwState;

typedef struct {
  DcwState *st;
  float    *tile;               /* [T][DCW_BINS]                           */
  guint     T;
  guint64   w0;
  gboolean  force;
} DcwJob;

static GAsyncQueue *g_jobs;     /* DcwJob*                                 */
static GMutex       g_res_lock; /* guards every state's mailbox            */
static gsize        g_workers_once;
static gboolean     g_behind_warned;

static void state_unref(DcwState *st);

/* One window through the model (inline path). */
static void dcw_infer(const float *tile, guint T, float **logp, guint *To) {
  const int64_t dims[4] = { 1, 1, (int64_t)T, DCW_BINS };
  int64_t od[8]; int ond = 0;
  GError *err = NULL;
  *logp = NULL; *To = 0;
  DcwSess *c = sess_acquire();
  if (!c) return;
  if (!skim_ort_run(c->s, tile, dims, 4, logp, od, &ond, &err)) {
    static gboolean warned;
    if (!warned) { warned = TRUE; g_warning("deepcw: inference failed: %s", err->message); }
    g_clear_error(&err);
    sess_release(c);
    return;
  }
  sess_release(c);
  *To = (ond == 3 && od[2] == DCW_CLASSES) ? (guint)MIN((int64_t)T, od[1]) : 0;
}

static void job_deliver(DcwJob *job, float *logp, guint To) {
  DcwState *st = job->st;
  g_mutex_lock(&g_res_lock);
  g_free(st->res_logp);
  st->res_logp = logp; st->res_T = To; st->res_w0 = job->w0;
  st->res_force = job->force;
  g_mutex_unlock(&g_res_lock);
}

static void job_free(DcwJob *job) {
  state_unref(job->st);
  g_free(job->tile);
  g_free(job);
}

/* A batch of windows through the model: every queued job that is ready
 * goes in ONE run, zero-padded at the end to the longest window (padding
 * is dead air after the over — the commit rule reads only each window's
 * own frames). One launch for N channels is what a GPU wants; on the CPU
 * it costs the same per channel as N single runs. */
static void worker_batch(GPtrArray *jobs) {
  guint Tmax = 0;
  for (guint i = 0; i < jobs->len; i++) {
    const DcwJob *j = g_ptr_array_index(jobs, i);
    Tmax = MAX(Tmax, j->T);
  }
  const guint N = jobs->len;
  float *in = g_new0(float, (gsize)N * Tmax * DCW_BINS);
  for (guint i = 0; i < N; i++) {
    const DcwJob *j = g_ptr_array_index(jobs, i);
    memcpy(in + (gsize)i * Tmax * DCW_BINS, j->tile,
           (gsize)j->T * DCW_BINS * sizeof(float));
  }
  const int64_t dims[4] = { (int64_t)N, 1, (int64_t)Tmax, DCW_BINS };
  int64_t od[8]; int ond = 0;
  float *out = NULL;
  GError *err = NULL;
  DcwSess *c = sess_acquire();
  gboolean ok = c && skim_ort_run(c->s, in, dims, 4, &out, od, &ond, &err);
  sess_release(c);
  g_free(in);
  if (!ok) {
    static gboolean warned;
    if (!warned && err) { warned = TRUE; g_warning("deepcw: inference failed: %s", err->message); }
    g_clear_error(&err);
    return;
  }
  const gboolean shape_ok = ond == 3 && od[0] == (int64_t)N &&
                            od[2] == DCW_CLASSES;
  for (guint i = 0; i < N; i++) {
    DcwJob *j = g_ptr_array_index(jobs, i);
    if (!shape_ok || g_atomic_int_get(&j->st->dying)) continue;
    const guint To = (guint)MIN((int64_t)j->T, od[1]);
    float *logp = g_memdup2(out + (gsize)i * od[1] * DCW_CLASSES,
                            (gsize)To * DCW_CLASSES * sizeof(float));
    job_deliver(j, logp, To);
  }
  g_free(out);
}

static gpointer worker_main(gpointer data) {
  (void)data;
  GPtrArray *batch = g_ptr_array_new();
  for (;;) {
    DcwJob *first = g_async_queue_pop(g_jobs);
    g_ptr_array_set_size(batch, 0);
    g_ptr_array_add(batch, first);
    DcwJob *more;
    while (batch->len < (guint)g_tune.batch &&
           (more = g_async_queue_try_pop(g_jobs)) != NULL) {
      g_ptr_array_add(batch, more);
    }
    /* drop windows whose channel died meanwhile */
    for (guint i = 0; i < batch->len; ) {
      DcwJob *j = g_ptr_array_index(batch, i);
      if (g_atomic_int_get(&j->st->dying)) { job_free(j); g_ptr_array_remove_index_fast(batch, i); }
      else { i++; }
    }
    if (batch->len) { worker_batch(batch); }
    for (guint i = 0; i < batch->len; i++) { job_free(g_ptr_array_index(batch, i)); }
  }
  return NULL;
}

static void workers_ensure(void) {
  if (g_once_init_enter(&g_workers_once)) {
    g_jobs = g_async_queue_new();
    for (int i = 0; i < g_tune.workers; i++) {
      g_thread_new("deepcw-infer", worker_main, NULL);
    }
    g_once_init_leave(&g_workers_once, 1);
  }
}

static guint g_reg;                     /* channel registration counter   */
/* Band-wide noise floor: an EMA over EVERY channel's per-frame inner-bin
 * floor (engine thread only; one pipeline per process). A station's own
 * channel cannot measure the noise under it — a 37 dB station's inner bins
 * hold its keying leakage and its SNR read 12 dB while its −12 dB spur read
 * 14 dB (80 m fixture: the table parked OK1DOL on the spur). Against the
 * band floor the two rank as they should. */
static double g_band_floor;

static gpointer dcw_channel_new(double rate) {
  tune_init();
  DcwState *st = g_new0(DcwState, 1);
  st->rate = rate;
  st->out = g_string_new(NULL);
  st->refs = 1;
  const double nf = rate / DCW_BIN_HZ;
  const guint N = (guint)llround(nf);
  if (fabs(nf - N) > 1e-6 || N < 8) {
    static gboolean warned;
    if (!warned) {
      warned = TRUE;
      g_warning("deepcw: channel rate %.1f Hz is no multiple of 12.5 Hz "
                "(the CW bank runs 250 Hz) — channel decodes nothing", rate);
    }
    st->dead = TRUE;
    return st;
  }
  /* No runtime/model: the DSP half still runs (level, offset, gate — the
   * gate can prove it without a model); only inference is skipped. The
   * pipeline never picks this backend in that case (it falls back to v2),
   * so this is the test harness's path. */
  GError *err = NULL;
  st->nomodel = !ensure_session(&err);
  if (st->nomodel) {
    static gboolean warned2;
    if (!warned2) {
      warned2 = TRUE;
      g_warning("deepcw: %s — channels track but decode nothing", err->message);
    }
    g_clear_error(&err);
  }
  st->N = N;
  st->hop = MAX((guint)llround(rate * 0.016), 1u);   /* 16 ms: 4 at 250 Hz */
  st->win = g_new(float, N);
  st->tw_re = g_new(float, DCW_KEEP * N);
  st->tw_im = g_new(float, DCW_KEEP * N);
  for (guint n = 0; n < N; n++) {
    st->win[n] = 0.5f - 0.5f * cosf(2.0f * (float)G_PI * (float)n / (float)N);
    for (gint k = -DCW_HALF; k <= DCW_HALF; k++) {
      const double a = 2.0 * G_PI * (double)k * (double)n / (double)N;
      st->tw_re[(k + DCW_HALF) * N + n] = (float)cos(a);
      st->tw_im[(k + DCW_HALF) * N + n] = (float)-sin(a);
    }
  }
  st->sring = g_new0(float, 2 * N);
  const double fps = rate / st->hop;                 /* 62.5 frames/s     */
  st->ring_frames = MAX((guint)llround(g_tune.win_s * fps), 16u);
  st->ring = g_new0(float, (gsize)st->ring_frames * DCW_KEEP);
  st->tile = g_new0(float, (gsize)st->ring_frames * DCW_BINS);
  st->tick_frames = MAX((guint)llround(g_tune.tick_s * fps), 1u);
  st->tick_phase  = (g_reg++ * 7u) % st->tick_frames;
  st->tail_f      = (guint)llround(g_tune.tail_s * fps);
  st->minconf_f   = (guint)llround(g_tune.minconf_s * fps);
  return st;
}

static void state_unref(DcwState *st) {
  if (!g_atomic_int_dec_and_test(&st->refs)) return;
  g_free(st->win); g_free(st->tw_re); g_free(st->tw_im);
  g_free(st->sring); g_free(st->ring); g_free(st->tile);
  g_free(st->res_logp);
  g_string_free(st->out, TRUE);
  g_free(st);
}

static void dcw_channel_free(gpointer state) {
  DcwState *st = state;
  if (!st) return;
  g_atomic_int_set(&st->dying, TRUE);
  state_unref(st);                 /* a job in flight drops the last ref  */
}

static inline float *ring_row(DcwState *st, guint64 abs_frame) {
  return st->ring + (gsize)(abs_frame % st->ring_frames) * DCW_KEEP;
}

/* Gate + WPM + SNR over abs frames [w0, w1). */
static void window_stats(DcwState *st, guint64 w0, guint64 w1) {
  const guint T = (guint)(w1 - w0);
  double prof[DCW_KEEP] = { 0 };
  for (guint64 f = w0; f < w1; f++) {
    const float *r = ring_row(st, f);
    for (guint b = 0; b < DCW_KEEP; b++) prof[b] += r[b];
  }
  guint p = 0;
  for (guint b = 1; b < DCW_KEEP; b++) if (prof[b] > prof[p]) p = b;
  double floor_sum = 0; guint floor_n = 0;
  for (guint b = 0; b < DCW_KEEP; b++) {
    if ((gint)b - (gint)p >= 3 || (gint)p - (gint)b >= 3) { floor_sum += prof[b]; floor_n++; }
  }
  const double line = prof[p] / T;
  const double floorv = floor_n ? floor_sum / floor_n / T : 1e-12;
  st->ratio_db = 20.0 * log10(MAX(line, 1e-12) / MAX(floorv, 1e-12));
  /* keyed duty on the line bin: threshold halfway between its max and the
   * floor; on/off run lengths give a dit estimate (shortest cluster). */
  double mx = 0;
  for (guint64 f = w0; f < w1; f++) mx = MAX(mx, ring_row(st, f)[p]);
  const double thr = 0.5 * (mx + floorv);
  st->win_peak = mx;
  guint on = 0, run = 0, runs[64], nr = 0;
  for (guint64 f = w0; f < w1; f++) {
    const gboolean k = ring_row(st, f)[p] > thr;
    if (k) { on++; run++; }
    else if (run) { if (nr < 64) runs[nr++] = run; run = 0; }
  }
  st->duty = (double)on / T;
  /* WPM only from a window that is really keyed (a quiet tail with noise
   * runs reads as 50+ WPM); otherwise the last estimate stands. */
  const gboolean keyed_enough = st->duty >= 0.15 && st->duty <= 0.85 && nr >= 5;
  if (g_tune.debug > 1) {
    g_printerr("deepcw: stats T=%u p=%u line %.4f floor %.4f mx %.4f thr %.4f runs %u:",
               T, p, line, floorv, mx, thr, nr);
    for (guint i = 0; i < MIN(nr, 12u); i++) g_printerr(" %u", runs[i]);
    g_printerr("\n");
  }
  if (keyed_enough) {
    /* Dit = the lower mode of the on-run histogram, 1-frame glitches
     * excluded (they read as 50+ WPM); refine with its ±1 neighbours. */
    guint hist[16] = { 0 };
    for (guint i = 0; i < nr; i++) if (runs[i] >= 2 && runs[i] < 16) hist[runs[i]]++;
    /* Dah-heavy fists (OK1XC: O K X C) put the histogram's peak on the
     * DAH (7–9 frames at 25–30 WPM) and read 10 WPM — the dit is the
     * FIRST run length that is well populated, not the most populated. */
    guint mode = 0;
    for (guint L = 2; L < 9; L++) if (hist[L] > hist[mode]) mode = L;
    guint dit = 0;
    for (guint L = 2; L < 9; L++) {
      if (hist[L] >= 3 && hist[L] * 5 >= hist[mode] * 2) { dit = L; break; }
    }
    if (dit) {
      double s = 0; guint c = 0;
      for (guint L = dit - 1; L <= dit + 1 && L < 16; L++) { s += (double)L * hist[L]; c += hist[L]; }
      const double dit_s = (s / c) * st->hop / st->rate;
      const double wpm = 1.2 / dit_s;
      if (wpm <= 60.0 && wpm >= 5.0) st->wpm = wpm;
      if (g_tune.debug > 1) {
        g_printerr("deepcw: dit %u (mode %u) frames, refined %.2f → %.1f wpm\n", dit, mode, s / c, st->wpm);
      }
    }
  }
  st->gate = st->ratio_db >= g_tune.gate_db && st->duty >= 0.03 && st->duty <= 0.97;
  /* Peak over the per-bin floor, minus 10 dB: a 12.5 Hz bin holds a tenth
   * of the 125 Hz channel's noise v2 measures against — the two engines'
   * dB columns should mean roughly the same thing (approximate: leakage
   * caps the readable SNR of very strong stations around 30 dB). */
  /* SNR of the trace = the window's strongest moment (the decaying peak
   * hold read −2…39 dB on one station between ticks after pauses). */
  st->snr_db = 20.0 * log10(MAX(st->win_peak, 1e-12) /
                            MAX(MIN(st->env_lo, g_band_floor), 1e-12)) - 10.0;
}

/* Commit a finished inference (engine thread only): the rule, then the
 * confidence and WPM bookkeeping of the newly pending text. */
static void dcw_apply(DcwState *st, const float *logp, guint To, guint64 w0,
                      gboolean force) {
  double conf = 0;
  const gsize before = st->out->len;
  const guint64 cursor0 = MAX(st->committed, w0);
  const guint n = skim_deepcw_commit(logp, To, w0, &st->committed, st->tail_f,
                                     st->minconf_f, force, st->out, &conf);
  if (n) {
    /* Confidence of the pending emission: length-weighted merge. */
    const gsize tot = st->out->len;
    st->out_conf = tot ? (st->out_conf * before + conf * n) / tot : conf;
    /* WPM from the character rate of the committed span (PARIS: 10 dit
     * units per character incl. its gap → WPM ≈ 12 × chars/s) — the
     * on-run histogram read dah-heavy fists (OK1XC) at a third of their
     * speed; the model's own segmentation is fist-shape independent. */
    const guint64 span = st->committed > cursor0 ? st->committed - cursor0 : 0;
    guint letters = 0;
    for (gsize i = before; i < tot; i++) if (st->out->str[i] != ' ') letters++;
    if (letters >= 6 && span >= 2 * st->rate / st->hop) {
      const double wpm = 12.0 * letters / ((double)span * st->hop / st->rate);
      if (wpm >= 5.0 && wpm <= 60.0) st->wpm = wpm;
    }
  }
  if (g_tune.debug) {
    g_printerr("deepcw: %.2f kHz T=%u ratio %.1f dB duty %.2f wpm %.0f "
               "commit %u |%s| cursor %" G_GUINT64_FORMAT "%s\n",
               st->freq_hz / 1000.0, To, st->ratio_db, st->duty, st->wpm, n,
               st->out->str + before, st->committed, force ? " FORCE" : "");
  }
}

static void dcw_tick(DcwState *st) {
  if (st->inflight) return;          /* async: one window at a time         */
  const guint64 w1 = st->frames_abs;
  const guint64 oldest = w1 > st->ring_frames ? w1 - st->ring_frames : 0;
  const guint64 w0 = MAX(oldest, st->committed);
  if (w1 <= w0) return;
  const guint T = (guint)(w1 - w0);
  if (T < st->minconf_f) return;
  window_stats(st, w0, w1);
  st->ticks++;
  if (!st->gate || st->nomodel) {
    if (w1 > st->committed + st->tail_f) st->committed = w1 - st->tail_f;
    if (g_tune.debug) {
      g_printerr("deepcw: %.2f kHz T=%u closed ratio %.1f dB duty %.2f\n",
                 st->freq_hz / 1000.0, T, st->ratio_db, st->duty);
    }
    return;
  }
  /* Tile: channel bins at 27..37, peak-normalised, log1p; rest zero. */
  float mx = 0.0f;
  for (guint64 f = w0; f < w1; f++) {
    const float *r = ring_row(st, f);
    for (guint b = 0; b < DCW_KEEP; b++) mx = MAX(mx, r[b]);
  }
  const float g = mx > 0 ? DCW_PEAK_MAG / mx : 1.0f;
  memset(st->tile, 0, (gsize)T * DCW_BINS * sizeof(float));
  for (guint t = 0; t < T; t++) {
    const float *r = ring_row(st, w0 + t);
    float *row = st->tile + (gsize)t * DCW_BINS + (DCW_CENTRE - DCW_HALF);
    for (guint b = 0; b < DCW_KEEP; b++) row[b] = log1pf(r[b] * g);
  }
  const gboolean force = (w1 - w0) >= st->ring_frames - st->tick_frames;
  if (g_tune.sync) {
    float *logp = NULL; guint To = 0;
    dcw_infer(st->tile, T, &logp, &To);
    if (To) { dcw_apply(st, logp, To, w0, force); }
    g_free(logp);
    return;
  }
  /* Async: hand the window to a worker; the mailbox comes back through
   * dcw_process on the engine thread. One job per channel in flight; a
   * queue that keeps growing means the workers cannot keep up — skip the
   * tick (the channel retries next tick) and say so once. */
  workers_ensure();
  if (g_async_queue_length(g_jobs) > 256) {
    if (!g_behind_warned) {
      g_behind_warned = TRUE;
      g_warning("deepcw: inference queue > 256 jobs — workers fall behind, "
                "ticks skipped (SKIM_DEEPCW_WORKERS=%d)", g_tune.workers);
    }
    return;
  }
  DcwJob *job = g_new0(DcwJob, 1);
  job->st = st;
  job->T = T;
  job->w0 = w0;
  job->force = force;
  job->tile = g_memdup2(st->tile, (gsize)T * DCW_BINS * sizeof(float));
  g_atomic_int_inc(&st->refs);
  st->inflight = TRUE;
  g_async_queue_push(g_jobs, job);
}

static void dcw_frame(DcwState *st) {
  /* DFT of the last N samples (oldest first) × Hann → bins −5..+5. */
  float *row = ring_row(st, st->frames_abs);
  float mags[DCW_KEEP];
  for (guint b = 0; b < DCW_KEEP; b++) {
    const float *tr = st->tw_re + b * st->N, *ti = st->tw_im + b * st->N;
    float re = 0, im = 0;
    guint idx = st->spos;                          /* oldest sample        */
    for (guint n = 0; n < st->N; n++) {
      const float w = st->win[n];
      const float x = st->sring[2 * idx] * w, y = st->sring[2 * idx + 1] * w;
      re += x * tr[n] - y * ti[n];
      im += x * ti[n] + y * tr[n];
      idx = (idx + 1 == st->N) ? 0 : idx + 1;
    }
    mags[b] = sqrtf(re * re + im * im);
    row[b] = mags[b];
  }
  if (g_tune.debug > 1 && st->frames_abs % 50 == 30) {
    g_printerr("deepcw: frame %" G_GUINT64_FORMAT " mags", st->frames_abs);
    for (guint b = 0; b < DCW_KEEP; b++) g_printerr(" %.4f", mags[b]);
    g_printerr("\n");
  }
  /* Level (raw units, EMA on the peak bin) and tone offset (centroid on
   * mark frames only — v2's rule: a noisy phase drags the pin). */
  guint p = 0; float fl = 0;
  for (guint b = 1; b < DCW_KEEP; b++) if (mags[b] > mags[p]) p = b;
  {
    /* Noise floor of this frame = the INNER bins (|b| ≤ 3 = ±37.5 Hz,
     * inside the channelizer passband) at least 2 bins off the line. The
     * outer bins sit on the filter roll-off and read tens of dB low, which
     * inflated every SNR and let a −12 dB spur outrank its station. */
    float sum = 0; guint n = 0;
    for (guint b = DCW_HALF - 3; b <= DCW_HALF + 3; b++) {
      if ((gint)b - (gint)p >= 2 || (gint)p - (gint)b >= 2) { sum += mags[b]; n++; }
    }
    fl = n ? sum / n : mags[p];
  }
  /* Level = peak hold with a ~2 s decay (a word gap must not drop it to
   * the noise: the ghost arbitration compares channels at every block).
   * Offset updates only on frames near that peak — noise frames between
   * marks would drag the estimate to the centre (gate-caught: 30 → 14 Hz). */
  const double decay = exp(-(double)st->hop / (2.0 * st->rate));
  st->lvl_ema = MAX((double)mags[p], st->lvl_ema * decay);
  /* Noise floor: what the line bin shows between marks — falls at once,
   * rises slowly (v2's env_lo idea). SNR = peak hold over it, so a 35 dB
   * station and its −20 dB image no longer read the same (the tile's
   * line/floor ratio saturates on the signal's own keying sidebands). */
  /* The line bin's own minima are useless for a strong fast station: an
   * 80 ms window never reaches the noise inside a 48 ms element gap, so a
   * 37 dB station read 29 dB and its −12 dB spur 35 dB (80 m fixture) —
   * the table then parked the station on the spur. The floor is the
   * quietest the OTHER bins have been lately instead: fast down, ~8 s up. */
  if (st->env_lo <= 0.0) { st->env_lo = fl; }
  else { st->env_lo += 0.01 * (fl - st->env_lo); }
  if (g_band_floor <= 0.0) { g_band_floor = fl; }
  else { g_band_floor += 2e-6 * (fl - g_band_floor); }   /* ~10 s over the bank */
  if (mags[p] > 4.0f * fl && mags[p] > 0.5 * st->lvl_ema &&
      p > 0 && p < DCW_KEEP - 1) {
    const double l = log((double)MAX(mags[p - 1], 1e-9f));
    const double c = log((double)MAX(mags[p], 1e-9f));
    const double r = log((double)MAX(mags[p + 1], 1e-9f));
    const double den = l - 2.0 * c + r;
    const double delta = den != 0.0 ? CLAMP(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
    const double off = ((double)p - DCW_HALF + delta) * DCW_BIN_HZ;
    st->off_ema += 0.05 * (off - st->off_ema);
  }
  st->frames_abs++;
  if ((st->frames_abs + st->tick_phase) % st->tick_frames == 0) { dcw_tick(st); }
}

static gboolean dcw_process(gpointer state, const float *iq, guint nframes,
                            SkimDecode *out) {
  DcwState *st = state;
  memset(out, 0, sizeof(*out));
  if (st->dead) return FALSE;
  if (st->inflight) {
    float *logp = NULL; guint To = 0; guint64 w0 = 0; gboolean force = FALSE;
    g_mutex_lock(&g_res_lock);
    if (st->res_logp) {
      logp = st->res_logp; To = st->res_T; w0 = st->res_w0; force = st->res_force;
      st->res_logp = NULL;
    }
    g_mutex_unlock(&g_res_lock);
    if (logp) {
      st->inflight = FALSE;
      if (To) { dcw_apply(st, logp, To, w0, force); }
      g_free(logp);
    }
  }
  for (guint i = 0; i < nframes; i++) {
    /* write position = oldest slot once the ring is full */
    const guint w = (st->spos + st->sfill) % st->N;
    st->sring[2 * w] = iq[2 * i];
    st->sring[2 * w + 1] = iq[2 * i + 1];
    if (st->sfill < st->N) { st->sfill++; }
    else { st->spos = (st->spos + 1) % st->N; }
    if (st->sfill == st->N && ++st->since >= st->hop) {
      st->since = 0;
      dcw_frame(st);
    }
  }
  if (st->out->len == 0) return FALSE;
  /* One WORD per call (the drain runs every ~256 ms per channel): the
   * extractor and the station table then see the same cadence of hits
   * they were tuned on with v2's per-character stream, instead of one
   * whole-over hit per commit. */
  gsize n = st->out->len;
  {
    const char *sp = strchr(st->out->str, ' ');
    if (sp) { n = (gsize)(sp - st->out->str) + 1; }
  }
  n = MIN(n, (gsize)SKIM_DECODE_TEXT_MAX - 1);
  memcpy(out->text, st->out->str, n);
  out->text[n] = '\0';
  g_string_erase(st->out, 0, (gssize)n);
  out->confidence     = CLAMP(st->out_conf, 0.0, 1.0);
  out->freq_offset_hz = st->off_ema;
  out->speed          = st->wpm;
  out->snr_db         = st->snr_db;
  out->pane_own       = FALSE;
  return TRUE;
}

static double dcw_level(gpointer state) { return ((DcwState *)state)->lvl_ema; }
static double dcw_tone_offset_hz(gpointer state) { return ((DcwState *)state)->off_ema; }
static void dcw_set_freq(gpointer state, double freq_hz) { ((DcwState *)state)->freq_hz = freq_hz; }

/* TX hold: the channel missed audio — start the window afresh, keep the
 * level/offset tracking and any text already committed. */
static void dcw_resync(gpointer state) {
  DcwState *st = state;
  if (st->dead) return;
  st->sfill = 0; st->spos = 0; st->since = 0;
  st->committed = st->frames_abs;
}

void skim_decode_deepcw_debug(gpointer state, SkimDeepcwDebug *dbg) {
  const DcwState *st = state;
  memset(dbg, 0, sizeof(*dbg));
  dbg->gate = st->gate; dbg->ratio_db = st->ratio_db; dbg->duty = st->duty;
  dbg->wpm = st->wpm; dbg->frames_abs = st->frames_abs;
  dbg->committed = st->committed; dbg->ticks = st->ticks; dbg->dead = st->dead;
  dbg->inflight = st->inflight;
}

const SkimDecodeBackend *skim_decode_deepcw(void) {
  static const SkimDecodeBackend backend = {
    .name           = "deepcw",
    .channel_new    = dcw_channel_new,
    .channel_free   = dcw_channel_free,
    .process        = dcw_process,
    .level          = dcw_level,
    .tone_offset_hz = dcw_tone_offset_hz,
    .set_freq       = dcw_set_freq,
    .take_aux_text  = NULL,
    .take_pane_op   = NULL,
    .resync         = dcw_resync,
  };
  return &backend;
}
