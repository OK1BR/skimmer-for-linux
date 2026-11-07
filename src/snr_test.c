/*
 * skimmer-snr-test — gate for gh#27: the dB of a spot is SNR in 500 Hz.
 *
 * The scale is CW Skimmer's, so that a station's dB from this skimmer
 * stands beside the other skimmers' on RBN: the key-down carrier power
 * over the noise density times 500 Hz. (VE3NEA, quoted by N4ZR on the RBN
 * blog, 2014-03-05: the key-on samples of a 50 Hz channel give the signal,
 * the noise density comes from the flat part of the whole receiver's
 * spectrum, "the noise power in 500 Hz is then computed by multiplying the
 * power density by the bandwidth".)
 *
 * Synthetic stations of KNOWN SNR500 — complex AWGN of density 2σ²/rate at
 * the 48 kHz input, a carrier of power A² — go through the WHOLE offline
 * pipeline (channelizer → decoder → extractor → station table), and the
 * station's snr_db (what the RBN line carries) is held against the truth:
 *   - the channel's noise bandwidth from the taps agrees with white noise
 *     measured through the bank (the one constant of the conversion),
 *   - a sweep 3…30 dB at the channel centre, per backend,
 *   - 12 dB at offsets across the channel (the prototype's skirt).
 * The raw per-decode estimate is read off the decode tap beside the table's
 * value. DeepCW runs when the runtime and the model resolve; otherwise that
 * half is SKIPPED and the v2 half alone gates.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/channelizer.h"
#include "engine/decode_tap.h"
#include "engine/pipeline.h"
#include "engine/station.h"

#define RATE    48000.0
#define CENTER  7020000.0
#define CHAN_BW 125.0
#define SIGMA   0.01                      /* noise σ per I/Q component      */

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/* --- CW synthesis (the cw_test keyer) ------------------------------------- */

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

/* text → keyed envelope at `rate` (ITU 1:3, 3/7 spacing), a lead-in of
 * `lead_s` silence and 1.5 s of tail. */
static GArray *gen_env(const char *text, double wpm, double rate, double lead_s) {
  GArray *env = g_array_new(FALSE, FALSE, sizeof(float));
  const double dit = 1.2 / wpm * rate;
  key_run(env, lead_s * rate, 0.0f);
  for (const char *p = text; *p; p++) {
    if (*p == ' ') { key_run(env, 7 * dit, 0.0f); continue; }
    const char *m = morse_of(*p);
    if (!m) { continue; }
    for (const char *e = m; *e; e++) {
      key_run(env, *e == '-' ? 3 * dit : dit, 1.0f);
      if (e[1]) { key_run(env, dit, 0.0f); }
    }
    key_run(env, 3 * dit, 0.0f);
  }
  key_run(env, 1.5 * rate, 0.0f);
  return env;
}

/* 5 ms raised-cosine keying edges. */
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

static char *rep(const char *payload, guint n) {
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < n; i++) {
    if (i) { g_string_append_c(s, ' '); }
    g_string_append(s, payload);
  }
  return g_string_free(s, FALSE);
}

/* Carrier amplitude for SNR500 against noise σ per component at `rate`:
 * SNR500 = A² / (2σ² · 500 / rate). */
static double amp_for(double snr500) {
  return sqrt(pow(10.0, snr500 / 10.0) * 2.0 * SIGMA * SIGMA * 500.0 / RATE);
}

/* --- the band ---------------------------------------------------------------- */

typedef struct {
  const char *call;
  double      snr500;        /* truth                                        */
  double      off_hz;        /* carrier offset from the band centre          */
  /* what came back */
  guint       n_dec;         /* decodes with text on the tap near the carrier */
  double      sum_dec;       /* Σ raw snr_db of those                         */
  double      min_dec, max_dec;
  guint       n_rep;         /* station-table reports of the call             */
  double      last_tab;      /* the table's snr_db at the last report         */
  double      hz_tab;
} Station;

typedef struct {
  Station *st;
  guint    n;
} Band;

static Station *near_station(Band *b, double hz) {
  Station *best = NULL;
  double   bd = 1e9;
  for (guint i = 0; i < b->n; i++) {
    const double d = fabs(hz - (CENTER + b->st[i].off_hz));
    if (d < bd) { bd = d; best = &b->st[i]; }
  }
  return bd <= 300.0 ? best : NULL;
}

static void tap_cb(const SkimTapEvent *ev, gpointer user) {
  Band *b = user;
  if (ev->kind != SKIM_TAP_DECODE || !ev->d.text[0] || ev->contested)
    return;
  Station *s = near_station(b, ev->sig_hz);
  if (!s)
    return;
  if (!s->n_dec) { s->min_dec = s->max_dec = ev->d.snr_db; }
  s->min_dec = MIN(s->min_dec, ev->d.snr_db);
  s->max_dec = MAX(s->max_dec, ev->d.snr_db);
  s->sum_dec += ev->d.snr_db;
  s->n_dec++;
}

static void station_cb(const SkimStation *st, gpointer user) {
  Band *b = user;
  for (guint i = 0; i < b->n; i++) {
    if (strcmp(b->st[i].call, st->call) == 0) {
      b->st[i].n_rep++;
      b->st[i].last_tab = st->snr_db;
      b->st[i].hz_tab   = st->freq_hz;
      return;
    }
  }
}

/* Synthesize the band — every station keys "CQ TEST DE <call> <call> K"
 * `reps` times at `wpm`, staggered — and run it through the offline
 * pipeline on the backend `engine` ("cw-v2" / "deepcw"). Returns the engine
 * the pipeline really ran (DeepCW falls back to v2 without the model). */
static const char *run_band(Band *b, const char *engine, double wpm, guint reps,
                            GRand *rng) {
  g_setenv("SKIM_CW_ENGINE", engine, TRUE);
  guint frames = 0;
  GArray **envs = g_new0(GArray *, b->n);
  for (guint i = 0; i < b->n; i++) {
    char *pl = g_strdup_printf("CQ TEST DE %s %s K", b->st[i].call, b->st[i].call);
    char *txt = rep(pl, reps);
    envs[i] = gen_env(txt, wpm, RATE, 0.5 + 0.7 * i);
    shape_env(envs[i], RATE);
    frames = MAX(frames, envs[i]->len);
    g_free(txt);
    g_free(pl);
  }
  float *iq = g_new(float, 2 * (gsize)frames);
  double *ph = g_new0(double, b->n), *amp = g_new0(double, b->n);
  for (guint i = 0; i < b->n; i++) { amp[i] = amp_for(b->st[i].snr500); }
  for (guint n = 0; n < frames; n++) {
    double re = SIGMA * gauss(rng), im = SIGMA * gauss(rng);
    for (guint i = 0; i < b->n; i++) {
      const double e = n < envs[i]->len ? g_array_index(envs[i], float, n) : 0.0;
      if (e > 0) { re += amp[i] * e * cos(ph[i]); im += amp[i] * e * sin(ph[i]); }
      ph[i] += 2.0 * G_PI * b->st[i].off_hz / RATE;
      if (ph[i] > G_PI) { ph[i] -= 2.0 * G_PI; }
    }
    iq[2 * n] = (float)re;
    iq[2 * n + 1] = (float)im;
  }
  for (guint i = 0; i < b->n; i++) { g_array_free(envs[i], TRUE); }
  g_free(envs); g_free(ph); g_free(amp);

  SkimPipelineConfig cfg = { 0 };
  cfg.chan_bw_hz = CHAN_BW;
  cfg.decode_tap = tap_cb;
  cfg.decode_tap_user = b;
  SkimPipeline *p = skim_pipeline_new(&cfg);
  skim_pipeline_set_station_cb(p, station_cb, b);
  GError *err = NULL;
  if (!skim_pipeline_start_offline(p, &err)) {
    printf("  pipeline: %s\n", err ? err->message : "?");
    g_clear_error(&err);
  }
  const char *ran = g_intern_string(skim_pipeline_cw_engine_name(p));
  for (guint at = 0; at < frames; at += 4800) {
    const guint n = MIN(4800u, frames - at);
    skim_pipeline_feed(p, iq + 2 * at, n, RATE, CENTER);
  }
  skim_pipeline_stop(p);
  skim_pipeline_free(p);
  g_free(iq);
  return ran;
}

static void print_band(const Band *b, const char *title) {
  printf("  %s\n", title);
  printf("  %-8s %6s %7s | %5s %6s %6s %6s | %4s %6s %6s\n", "call", "true",
         "off Hz", "ndec", "mean", "min", "max", "nrep", "table", "err");
  for (guint i = 0; i < b->n; i++) {
    const Station *s = &b->st[i];
    if (s->n_rep) {
      printf("  %-8s %6.1f %+7.1f | %5u %6.1f %6.1f %6.1f | %4u %6.1f %+6.1f\n",
             s->call, s->snr500, s->off_hz - CHAN_BW * round(s->off_hz / CHAN_BW),
             s->n_dec, s->n_dec ? s->sum_dec / s->n_dec : 0.0, s->min_dec,
             s->max_dec, s->n_rep, s->last_tab, s->last_tab - s->snr500);
    } else {
      printf("  %-8s %6.1f %+7.1f | %5u %6.1f %6.1f %6.1f | %4s %6s %6s\n",
             s->call, s->snr500, s->off_hz - CHAN_BW * round(s->off_hz / CHAN_BW),
             s->n_dec, s->n_dec ? s->sum_dec / s->n_dec : 0.0, s->min_dec,
             s->max_dec, "-", "-", "-");
    }
  }
}

/* --- the suite per backend --------------------------------------------------- */

/* Every station in its own channel, 1 kHz apart, on its channel's centre
 * (channel c sits at c·125 Hz from the band centre; 2 kHz = channel 16). */
static void sweep(const char *engine, double tol_db) {
  static const double SNRS[] = { 3, 6, 10, 15, 20, 25, 30 };
  static const char *CALLS[] = { "OK1BR", "DL1ABC", "9A5K", "SM0ONR",
                                 "F5IN", "HB9CAT", "EA3XY" };
  Station st[G_N_ELEMENTS(SNRS)];
  memset(st, 0, sizeof(st));
  for (guint i = 0; i < G_N_ELEMENTS(SNRS); i++) {
    st[i].call = CALLS[i];
    st[i].snr500 = SNRS[i];
    st[i].off_hz = 2000.0 + 1000.0 * i;            /* channel 16 + 8·i   */
  }
  Band b = { st, G_N_ELEMENTS(SNRS) };
  GRand *rng = g_rand_new_with_seed(20261107);
  const char *ran = run_band(&b, engine, 25.0, 6, rng);
  g_rand_free(rng);
  char title[96];
  g_snprintf(title, sizeof(title), "sweep at the channel centre — backend %s", ran);
  print_band(&b, title);
  for (guint i = 0; i < b.n; i++) {
    const Station *s = &st[i];
    char what[128];
    if (s->snr500 < 6.0) {
      /* 3 dB in 500 Hz is under v2's squelch on the wide path (OK1HRA's
       * sweep: 50 % of calls at 2.4 dB) — reported, not demanded. */
      if (s->n_rep) {
        g_snprintf(what, sizeof(what), "%s: %.0f dB station read within %.1f dB (%.1f)",
                   ran, s->snr500, tol_db + 1.0, s->last_tab);
        check(what, fabs(s->last_tab - s->snr500) <= tol_db + 1.0);
      }
      continue;
    }
    g_snprintf(what, sizeof(what), "%s: %.0f dB station reaches the table", ran, s->snr500);
    check(what, s->n_rep > 0);
    if (!s->n_rep) { continue; }
    g_snprintf(what, sizeof(what), "%s: %.0f dB station's dB within %.1f dB (table %.1f)",
               ran, s->snr500, tol_db, s->last_tab);
    check(what, fabs(s->last_tab - s->snr500) <= tol_db);
  }
}

/* One SNR, carriers spread across the channel: the prototype is −6 dB at
 * the channel edge (±62.5 Hz) and the neighbour channel owns the tone
 * from there on, so the skirt may cost up to a few dB of signal. */
static void offsets(const char *engine, double snr500, double tol_db) {
  static const double OFFS[] = { 0, 15, 30, 45, 55 };
  static const char *CALLS[] = { "OK1BR", "DL1ABC", "9A5K", "SM0ONR", "F5IN" };
  Station st[G_N_ELEMENTS(OFFS)];
  memset(st, 0, sizeof(st));
  for (guint i = 0; i < G_N_ELEMENTS(OFFS); i++) {
    st[i].call = CALLS[i];
    st[i].snr500 = snr500;
    st[i].off_hz = 2000.0 + 1000.0 * i + OFFS[i];
  }
  Band b = { st, G_N_ELEMENTS(OFFS) };
  GRand *rng = g_rand_new_with_seed(20261108);
  const char *ran = run_band(&b, engine, 25.0, 6, rng);
  g_rand_free(rng);
  char title[96];
  g_snprintf(title, sizeof(title), "%.0f dB across the channel — backend %s", snr500, ran);
  print_band(&b, title);
  for (guint i = 0; i < b.n; i++) {
    const Station *s = &st[i];
    char what[128];
    g_snprintf(what, sizeof(what), "%s: %.0f dB at %+.0f Hz reaches the table", ran,
               snr500, OFFS[i]);
    check(what, s->n_rep > 0);
    if (!s->n_rep) { continue; }
    g_snprintf(what, sizeof(what), "%s: %.0f dB at %+.0f Hz within %.1f dB (table %.1f)",
               ran, snr500, OFFS[i], tol_db, s->last_tab);
    check(what, fabs(s->last_tab - snr500) <= tol_db);
  }
}

int main(void) {
  printf("=== SNR scale gate (gh#27: the dB of a spot is SNR in 500 Hz) ===\n");

  /* -- the channel's noise bandwidth: taps vs white noise through the bank --- */
  {
    SkimChannelizer *bank = skim_channelizer_new(RATE, CHAN_BW);
    const double enbw = skim_channelizer_enbw_hz(bank);
    GRand *rng = g_rand_new_with_seed(1);
    float blk[2 * 4800], out[2 * 512];
    double pwr = 0.0; guint64 n = 0;
    for (guint s = 0; s < 600; s++) {                 /* 60 s of white noise */
      for (guint i = 0; i < 4800; i++) {
        blk[2 * i] = (float)(SIGMA * gauss(rng));
        blk[2 * i + 1] = (float)(SIGMA * gauss(rng));
      }
      skim_channelizer_push(bank, blk, 4800);
      guint g;
      while ((g = skim_channelizer_read(bank, 50, out, 512)) > 0) {
        if (s < 10) { continue; }                     /* prototype warm-up  */
        for (guint i = 0; i < g; i++) {
          pwr += (double)out[2 * i] * out[2 * i] + (double)out[2 * i + 1] * out[2 * i + 1];
        }
        n += g;
      }
    }
    /* output noise power = N0 · ENBW with N0 = 2σ²/rate */
    const double meas = (pwr / (double)n) / (2.0 * SIGMA * SIGMA / RATE);
    printf("  channel ENBW: taps %.1f Hz, white noise through the bank %.1f Hz "
           "(%.2f of the %.0f Hz spacing)\n", enbw, meas, enbw / CHAN_BW, CHAN_BW);
    check("channel ENBW from the taps within 3 % of the noise measured through the bank",
          fabs(meas - enbw) <= 0.03 * enbw);
    check("channel ENBW between 0.7 and 1.0 of the spacing", enbw > 0.7 * CHAN_BW && enbw < CHAN_BW);
    g_rand_free(rng);
    skim_channelizer_free(bank);
  }

  /* -- v2 ---------------------------------------------------------------------- */
  printf("--- backend cw-v2 ---\n");
  sweep("v2", 2.0);
  offsets("v2", 12.0, 2.0);

  /* -- DeepCW (when the runtime and model resolve) ------------------------------ */
  printf("--- backend deepcw ---\n");
  {
    Station probe = { .call = "OK1BR", .snr500 = 20.0, .off_hz = 2000.0 };
    Band b = { &probe, 1 };
    GRand *rng = g_rand_new_with_seed(3);
    const char *ran = run_band(&b, "deepcw", 25.0, 1, rng);
    g_rand_free(rng);
    if (strcmp(ran, "deepcw") != 0) {
      printf("  SKIP deepcw: the pipeline ran %s (no ONNX Runtime or model)\n", ran);
    } else {
      sweep("deepcw", 2.0);
      offsets("deepcw", 12.0, 2.0);
    }
  }

  printf("=== %d checks, %d failed ===\n", checks, fails);
  return fails ? 1 : 0;
}
