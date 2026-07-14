/*
 * skimmer-chan-test — offline gate for the polyphase channelizer (M2).
 *
 * Synthetic-tone checks on a 48 k / 125 Hz bank (M = 384, out 250 Hz):
 *   - geometry: channel count, output rate, offset mapping (±, mirror),
 *   - a tone on a channel centre lands in THAT channel with unit gain,
 *     neighbours < −60 dBc, its mirror channel at the float noise floor,
 *   - phase preservation: a +30 Hz in-channel offset shows up as a +30 Hz
 *     baseband rotation (and −30 Hz as −30 Hz) — the RTTY/PSK requirement,
 *   - a channel-edge tone straddles the two neighbours at ~−6 dB each,
 *   - three simultaneous tones: each recovered at its own amplitude, a far
 *     empty channel stays quiet (isolation under load),
 * and a CPU benchmark on the real M2 geometry (192 k / 125 Hz, M = 1536):
 * the whole CW segment must channelize well under one core.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/channelizer.h"

#define FS   48000.0
#define BW   125.0
#define M    384
#define BLK  1024

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

typedef struct { double freq, amp, phase; } Tone;

/* Push nframes of the tone sum through the bank in BLK chunks. */
static void push_tones(SkimChannelizer *ch, Tone *t, guint ntones, guint nframes) {
  static float buf[2 * BLK];
  while (nframes) {
    guint n = MIN(nframes, (guint)BLK);
    memset(buf, 0, 2 * n * sizeof(float));
    for (guint j = 0; j < ntones; j++) {
      double dph = 2.0 * G_PI * t[j].freq / FS;
      for (guint i = 0; i < n; i++) {
        buf[2 * i]     += (float)(t[j].amp * cos(t[j].phase + dph * i));
        buf[2 * i + 1] += (float)(t[j].amp * sin(t[j].phase + dph * i));
      }
      t[j].phase = fmod(t[j].phase + dph * n, 2.0 * G_PI);
    }
    skim_channelizer_push(ch, buf, n);
    nframes -= n;
  }
}

/* Drain a channel; return frames and fill iq (caller sizes it). */
static guint drain(SkimChannelizer *ch, guint chan, float *iq, guint max) {
  return skim_channelizer_read(ch, chan, iq, max);
}

/* RMS magnitude of the LAST `use` frames of iq[0..n). */
static double rms_tail(const float *iq, guint n, guint use) {
  if (n < use) { use = n; }
  if (!use) { return 0.0; }
  double s = 0.0;
  for (guint i = n - use; i < n; i++) {
    s += (double)iq[2 * i] * iq[2 * i] + (double)iq[2 * i + 1] * iq[2 * i + 1];
  }
  return sqrt(s / use);
}

/* Baseband frequency from the mean phase increment of the tail. */
static double freq_tail(const float *iq, guint n, guint use, double out_rate) {
  if (n < use + 1) { return NAN; }
  double acc = 0.0;
  for (guint i = n - use; i < n; i++) {
    double ar = iq[2 * (i - 1)], ai = iq[2 * (i - 1) + 1];
    double br = iq[2 * i],       bi = iq[2 * i + 1];
    acc += atan2(bi * ar - br * ai, br * ar + bi * ai);
  }
  return acc / use * out_rate / (2.0 * G_PI);
}

static double dbc(double x, double ref) {
  return 20.0 * log10((x + 1e-30) / (ref + 1e-30));
}

int main(void) {
  printf("=== channelizer gate (offline, synthetic tones) ===\n");
  static float out[8192 * 2], out2[8192 * 2];

  /* -- geometry ------------------------------------------------------------ */
  SkimChannelizer *ch = skim_channelizer_new(FS, BW);
  check("bank constructs (48 k / 125 Hz)", ch != NULL);
  if (!ch) { printf("FAIL\n"); return 1; }
  check("count = 384", skim_channelizer_count(ch) == M);
  check("out_rate = 250 Hz", skim_channelizer_out_rate(ch) == 250.0);
  check("offset map: ch1 = +125, ch96 = +12 kHz, ch288 = −12 kHz, ch383 = −125",
        skim_channelizer_offset_hz(ch, 1) == 125.0 &&
        skim_channelizer_offset_hz(ch, 96) == 12000.0 &&
        skim_channelizer_offset_hz(ch, 288) == -12000.0 &&
        skim_channelizer_offset_hz(ch, 383) == -125.0);
  check("bad geometry rejected (odd/fractional M)",
        skim_channelizer_new(48000.0, 130.0) == NULL &&
        skim_channelizer_new(48000.0, 96000.0 / 383.0) == NULL);

  /* -- centred tone: +12 kHz, amp 0.5 → channel 96 ------------------------- */
  Tone t1[] = { { 12000.0, 0.5, 0.0 } };
  push_tones(ch, t1, 1, 4 * 48000);            /* 4 s → ~1000 frames/channel */
  guint n96 = drain(ch, 96, out, 8192);
  check("channel 96 produces output (~1000 frames)", n96 > 900 && n96 <= 1024);
  double a96 = rms_tail(out, n96, 500);
  check("unit channel gain: |y| ≈ 0.5 on the centre channel",
        a96 > 0.475 && a96 < 0.525);
  guint n95 = drain(ch, 95, out2, 8192);
  double a95 = rms_tail(out2, n95, 500);
  guint n97 = drain(ch, 97, out2, 8192);
  double a97 = rms_tail(out2, n97, 500);
  printf("       neighbours: ch95 %.1f dBc, ch97 %.1f dBc\n",
         dbc(a95, a96), dbc(a97, a96));
  check("adjacent channels < −60 dBc", dbc(a95, a96) < -60 && dbc(a97, a96) < -60);
  guint nmir = drain(ch, 288, out2, 8192);
  double amir = rms_tail(out2, nmir, 500);
  printf("       mirror ch288 (−12 kHz): %.1f dBc\n", dbc(amir, a96));
  check("mirror channel < −80 dBc (no orientation/index leak)",
        dbc(amir, a96) < -80);
  check("baseband ≈ DC on a centred tone (|f| < 1 Hz)",
        fabs(freq_tail(out, n96, 400, 250.0)) < 1.0);
  skim_channelizer_free(ch);

  /* -- phase preservation: ±30 Hz in-channel offsets ------------------------ */
  ch = skim_channelizer_new(FS, BW);
  Tone t2[] = { { 12030.0, 0.5, 0.0 } };
  push_tones(ch, t2, 1, 4 * 48000);
  guint n = drain(ch, 96, out, 8192);
  double f = freq_tail(out, n, 400, 250.0);
  printf("       +30 Hz offset measured as %+.2f Hz\n", f);
  check("+30 Hz offset → +30 Hz baseband rotation", fabs(f - 30.0) < 1.0);
  skim_channelizer_free(ch);

  ch = skim_channelizer_new(FS, BW);
  Tone t3[] = { { -11970.0, 0.5, 0.0 } };      /* ch288 centre −12 k, +30    */
  push_tones(ch, t3, 1, 4 * 48000);
  n = drain(ch, 288, out, 8192);
  f = freq_tail(out, n, 400, 250.0);
  printf("       −12 kHz + 30 Hz measured as %+.2f Hz in ch288\n", f);
  check("below-centre channel keeps phase too (+30 Hz)", fabs(f - 30.0) < 1.0);
  skim_channelizer_free(ch);

  /* -- channel-edge tone straddles both neighbours -------------------------- */
  ch = skim_channelizer_new(FS, BW);
  Tone t4[] = { { 12062.5, 0.5, 0.0 } };
  push_tones(ch, t4, 1, 4 * 48000);
  n96 = drain(ch, 96, out, 8192);
  a96 = rms_tail(out, n96, 500);
  n97 = drain(ch, 97, out2, 8192);
  a97 = rms_tail(out2, n97, 500);
  printf("       edge tone: ch96 %.1f dBc, ch97 %.1f dBc (of 0.5)\n",
         dbc(a96, 0.5), dbc(a97, 0.5));
  check("edge tone lands ~−6 dB in BOTH straddling channels",
        dbc(a96, 0.5) > -9 && dbc(a96, 0.5) < -3 &&
        dbc(a97, 0.5) > -9 && dbc(a97, 0.5) < -3);
  skim_channelizer_free(ch);

  /* -- three tones at once --------------------------------------------------- */
  ch = skim_channelizer_new(FS, BW);
  Tone t5[] = { { -6250.0, 0.3, 0.0 },         /* ch 334 (= −50)             */
                {  1250.0, 0.2, 1.0 },         /* ch 10                      */
                { 18750.0, 0.1, 2.0 } };       /* ch 150                     */
  push_tones(ch, t5, 3, 4 * 48000);
  guint na = drain(ch, 334, out, 8192);
  double aa = rms_tail(out, na, 500);
  guint nb = drain(ch, 10, out2, 8192);
  double ab = rms_tail(out2, nb, 500);
  guint nc = drain(ch, 150, out2, 8192);
  double ac = rms_tail(out2, nc, 500);
  guint nq = drain(ch, 200, out2, 8192);
  double aq = rms_tail(out2, nq, 500);
  check("three tones recovered at their amplitudes (±5 %)",
        fabs(aa - 0.3) < 0.015 && fabs(ab - 0.2) < 0.01 && fabs(ac - 0.1) < 0.005);
  printf("       quiet channel 200 under load: %.1f dBc of strongest\n",
         dbc(aq, 0.3));
  check("an empty channel stays < −70 dBc under 3-tone load", dbc(aq, 0.3) < -70);
  skim_channelizer_free(ch);

  /* -- CPU: the real M2 geometry, 192 k / 125 Hz (M = 1536) ----------------- */
  ch = skim_channelizer_new(192000.0, BW);
  check("real geometry constructs (192 k / 125 Hz, M = 1536)", ch != NULL);
  if (ch) {
    static float blk[2 * 1920];
    double ph = 0.0, dph = 2.0 * G_PI * 12345.0 / 192000.0;
    /* 20 s: also long enough (5000 hops) to overflow the 4096-frame rings,
     * which is what the dropped-counter check below relies on. */
    const int SECS = 20;
    gint64 t0 = g_get_monotonic_time();
    for (int s = 0; s < SECS * 100; s++) {     /* 10 ms blocks               */
      for (int i = 0; i < 1920; i++) {
        blk[2 * i]     = (float)(0.4 * cos(ph));
        blk[2 * i + 1] = (float)(0.4 * sin(ph));
        ph += dph;
      }
      skim_channelizer_push(ch, blk, 1920);
    }
    double secs = (double)(g_get_monotonic_time() - t0) / G_USEC_PER_SEC;
    double ratio = secs / SECS;
    printf("       %d s of 192 k IQ channelized in %.2f s → %.1f %% of one core\n",
           SECS, secs, 100.0 * ratio);
    check("CPU: whole segment well under one core (< 50 %)", ratio < 0.5);
    check("dropped counter counts unread overwrites",
          skim_channelizer_dropped(ch) > 0);
    skim_channelizer_free(ch);
  } else {
    checks += 3; fails += 3;
  }

  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — isolation, phase and CPU all behave.");
  return fails ? 1 : 0;
}
