/*
 * skimmer-rtty-test — offline gate for the RTTY decode backend (M7).
 *
 * Synthesizes 45.45 Bd / 170 Hz-shift Baudot FSK at complex baseband
 * (500 Hz, like the RTTY channelizer output) and checks the decoder:
 *   - ITA2 table truth on HARDCODED bit vectors (R, Y, FIGS-1) — the
 *     synthesis tables are mirrors of the decoder's, so these vectors are
 *     the independent witness against a shared table error,
 *   - clean copy at centre and at ±in-channel offsets (exact text, centre
 *     frequency, baud, confidence, SNR sane),
 *   - figures/letters shifts with unshift-on-space ("599 001" exchanges),
 *   - REVERSED polarity copies via the dual-framer election,
 *   - AWGN and slow QSB still copy (small Levenshtein tolerance),
 *   - selective fade (mark 10 dB down) copies exactly — the ATC test,
 *   - SQUELCH: pure noise, keyed CW, and TWO INDEPENDENT carriers 170 Hz
 *     apart each emit NOT ONE character (RBN-grade precision),
 *   - integration: a worst-case straddler (station centred midway between
 *     two 250 Hz channels) through the real wide-passband channelizer
 *     decodes exactly in a straddling channel; far channels stay mute.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/channelizer.h"
#include "engine/decode_rtty.h"
#include "engine/pipeline.h"

#define RATE  500.0
#define BAUD  45.45
#define HALF  85.0

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/* --- ITA2 encode (mirrors the decoder's tables; the hardcoded bit vectors
 * below are the independent truth) ------------------------------------------ */

#define ITA2_FIGS 0x1B
#define ITA2_LTRS 0x1F
#define ITA2_SP   0x04

static const char ita2_ltrs[32] = {
  0,   'E', '\n', 'A', ' ', 'S', 'I', 'U',
  0,   'D', 'R',  'J', 'N', 'F', 'C', 'K',
  'T', 'Z', 'L',  'W', 'H', 'Y', 'P', 'Q',
  'O', 'B', 'G',  0,   'M', 'X', 'V', 0,
};
static const char ita2_figs[32] = {
  0,   '3', '\n', '-', ' ', 0,   '8', '7',
  0,   '$', '4',  '\'', ',', '!', ':', '(',
  '5', '"', ')',  '2', '#', '6', '0', '1',
  '9', '?', '&',  0,   '.', '/', ';', 0,
};

typedef struct { gboolean mark; double bits; } Sym;

static void sym_add(GArray *sym, gboolean mark, double bits) {
  Sym s = { mark, bits };
  g_array_append_val(sym, s);
}

static void enc_code(GArray *sym, guint code) {
  sym_add(sym, FALSE, 1.0);                         /* start               */
  for (guint k = 0; k < 5; k++) { sym_add(sym, (code >> k) & 1u, 1.0); }
  sym_add(sym, TRUE, 1.5);                          /* stop                */
}

static gint tab_find(const char tab[32], char c) {
  for (guint i = 1; i < 32; i++) {
    if (tab[i] == c && i != ITA2_FIGS && i != ITA2_LTRS)
      return (gint)i;
  }
  return -1;
}

/* Encode text; tracks the RECEIVER-visible shift incl. unshift-on-space. */
static void enc_text(GArray *sym, const char *text) {
  guint shift = 0;
  for (const char *p = text; *p; p++) {
    if (*p == ' ') {
      enc_code(sym, ITA2_SP);
      shift = 0;
      continue;
    }
    gint lc = tab_find(ita2_ltrs, *p);
    gint fc = tab_find(ita2_figs, *p);
    if (lc >= 0) {
      if (shift != 0) { enc_code(sym, ITA2_LTRS); shift = 0; }
      enc_code(sym, (guint)lc);
    } else if (fc >= 0) {
      if (shift != 1) { enc_code(sym, ITA2_FIGS); shift = 1; }
      enc_code(sym, (guint)fc);
    }
  }
}

/* Leading LTRS diddles — the idle a real op keys between overs; they carry
 * both tones (each start bit is space) so the pair finder can acquire. */
static void enc_diddle(GArray *sym, guint n) {
  for (guint i = 0; i < n; i++) { enc_code(sym, ITA2_LTRS); }
}

/* Render symbols to continuous-phase FSK IQ. amp_m/amp_s set the per-tone
 * amplitudes (selective fade), reverse swaps the tones (wrong sideband). */
static GArray *render(const GArray *sym, double rate, double center,
                      gboolean reverse, double amp_m, double amp_s) {
  GArray *iq = g_array_new(FALSE, FALSE, sizeof(float));
  double phase = 0.0, t_acc = 0.0;
  guint emitted = 0;
  for (guint i = 0; i < sym->len; i++) {
    const Sym *s = &g_array_index(sym, Sym, i);
    const gboolean hi = reverse ? !s->mark : s->mark;
    const double f   = center + (hi ? HALF : -HALF);
    const double amp = s->mark ? amp_m : amp_s;
    const double dph = 2.0 * G_PI * f / rate;
    t_acc += s->bits * rate / BAUD;
    while ((double)emitted < t_acc) {
      float re = (float)(amp * cos(phase));
      float im = (float)(amp * sin(phase));
      g_array_append_val(iq, re);
      g_array_append_val(iq, im);
      phase = fmod(phase + dph, 2.0 * G_PI);
      emitted++;
    }
  }
  return iq;
}

/* --- helpers ---------------------------------------------------------------- */

static double rand_gauss(GRand *rng) {
  double u1 = g_rand_double_range(rng, 1e-12, 1.0);
  double u2 = g_rand_double(rng);
  return sqrt(-2.0 * log(u1)) * cos(2.0 * G_PI * u2);
}

static void add_noise(GArray *iq, double sigma, GRand *rng) {
  for (guint i = 0; i < iq->len; i++) {
    g_array_index(iq, float, i) += (float)(sigma * rand_gauss(rng));
  }
}

static void add_tail(GArray *iq, double secs, double sigma, GRand *rng) {
  guint n = (guint)(secs * RATE);
  for (guint i = 0; i < 2 * n; i++) {
    float v = (float)(sigma * rand_gauss(rng));
    g_array_append_val(iq, v);
  }
}

/* Run the whole IQ through a fresh RTTY channel state; returns the decoded
 * text (caller frees) and the last decode event in *last (optional). */
static char *run_rtty(const GArray *iq, double rate, SkimDecode *last) {
  const SkimDecodeBackend *be = skim_decode_rtty();
  gpointer st = be->channel_new(rate);
  GString *txt = g_string_new(NULL);
  SkimDecode d;
  const float *p = (const float *)iq->data;
  guint nframes = iq->len / 2;
  for (guint at = 0; at < nframes; at += 64) {
    guint n = MIN(64u, nframes - at);
    if (be->process(st, p + 2 * at, n, &d)) {
      g_string_append(txt, d.text);
      /* keep the last PAYLOAD event — a lone break mark rides the release,
       * whose stats no longer describe the over */
      if (last && strcmp(d.text, "\xC2\xB7 ") != 0) { *last = d; }
    }
  }
  be->channel_free(st);
  return g_string_free(txt, FALSE);
}

/* Strip over-break marks ("· ") and outer whitespace for comparison. */
static char *tidy(const char *s) {
  GString *o = g_string_new(NULL);
  for (const char *p = s; *p;) {
    if ((guchar)p[0] == 0xC2 && (guchar)p[1] == 0xB7) {
      p += 2;
      continue;
    }
    g_string_append_c(o, *p++);
  }
  char *r = g_string_free(o, FALSE);
  g_strstrip(r);
  /* collapse double spaces left by the stripped marks */
  char *w = r, *q = r;
  gboolean sp = FALSE;
  for (; *q; q++) {
    if (*q == ' ') {
      if (sp) { continue; }
      sp = TRUE;
    } else {
      sp = FALSE;
    }
    *w++ = *q;
  }
  *w = '\0';
  return r;
}

/* Station-callback collector for the offline pipeline section: keeps the
 * LATEST report per call. */
static SkimStation seen[16];
static guint n_seen;

static void collect_station(const SkimStation *st, gpointer user) {
  (void)user;
  for (guint i = 0; i < n_seen; i++) {
    if (g_strcmp0(seen[i].call, st->call) == 0) {
      seen[i] = *st;
      return;
    }
  }
  if (n_seen < G_N_ELEMENTS(seen)) { seen[n_seen++] = *st; }
}

static const SkimStation *seen_find(const char *call) {
  for (guint i = 0; i < n_seen; i++) {
    if (g_strcmp0(seen[i].call, call) == 0)
      return &seen[i];
  }
  return NULL;
}

static guint lev(const char *a, const char *b) {
  guint la = (guint)strlen(a), lb = (guint)strlen(b);
  guint *row = g_new(guint, lb + 1);
  for (guint j = 0; j <= lb; j++) { row[j] = j; }
  for (guint i = 1; i <= la; i++) {
    guint prev = row[0];
    row[0] = i;
    for (guint j = 1; j <= lb; j++) {
      guint cur = row[j];
      guint sub = prev + (a[i - 1] == b[j - 1] ? 0 : 1);
      row[j] = MIN(MIN(row[j] + 1, row[j - 1] + 1), sub);
      prev = cur;
    }
  }
  guint d = row[lb];
  g_free(row);
  return d;
}

/* --- main ------------------------------------------------------------------- */

int main(void) {
  printf("=== RTTY decode gate (offline, synthetic FSK) ===\n");
  GRand *rng = g_rand_new_with_seed(42);
  SkimDecode d;

  /* -- ITA2 truth: hardcoded bit vectors ------------------------------------ */
  /* 'R' = 01010, 'Y' = 10101, FIGS = 11011 then '1' = Q = 11101 — first
   * transmitted bit written FIRST here, encoded LSB-first by hand. */
  {
    GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym, 8);
    static const struct { const char *bits; } vec[] = {
      { "01010" }, { "10101" },                    /* R Y                    */
      { "11011" }, { "11101" },                    /* FIGS 1                 */
    };
    for (guint v = 0; v < G_N_ELEMENTS(vec); v++) {
      sym_add(sym, FALSE, 1.0);
      for (guint k = 0; k < 5; k++) {
        sym_add(sym, vec[v].bits[k] == '1', 1.0);
      }
      sym_add(sym, TRUE, 1.5);
    }
    enc_diddle(sym, 4);
    GArray *iq = render(sym, RATE, 0.0, FALSE, 1.0, 1.0);
    add_tail(iq, 4.0, 0.001, rng);
    add_noise(iq, 0.001, rng);
    char *txt = run_rtty(iq, RATE, NULL);
    char *t = tidy(txt);
    printf("       vectors decoded: \"%s\"\n", t);
    check("hardcoded ITA2 vectors → \"RY1\"", g_strcmp0(t, "RY1") == 0);
    g_free(txt);
    g_free(t);
    g_array_free(iq, TRUE);
    g_array_free(sym, TRUE);
  }

  /* -- clean copy at centre -------------------------------------------------- */
  const char *CQ = "CQ TEST DE OK1BR OK1BR CQ";
  {
    GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym, 10);
    enc_text(sym, CQ);
    enc_diddle(sym, 2);
    GArray *iq = render(sym, RATE, 0.0, FALSE, 1.0, 1.0);
    add_tail(iq, 4.0, 0.001, rng);
    add_noise(iq, 0.001, rng);
    char *txt = run_rtty(iq, RATE, &d);
    char *t = tidy(txt);
    printf("       clean: \"%s\"\n", t);
    check("clean copy exact", g_strcmp0(t, CQ) == 0);
    printf("       centre %+.1f Hz, baud %.2f, conf %.2f, snr %.0f dB\n",
           d.freq_offset_hz, d.speed, d.confidence, d.snr_db);
    check("centre ≈ 0 Hz (±6)", fabs(d.freq_offset_hz) < 6.0);
    check("baud = 45.45", fabs(d.speed - 45.45) < 0.01);
    check("confidence ≥ 0.8", d.confidence >= 0.8);
    check("SNR estimate ≥ 10 dB", d.snr_db >= 10.0);
    g_free(txt);
    g_free(t);
    g_array_free(iq, TRUE);
    g_array_free(sym, TRUE);
  }

  /* -- in-channel offsets ---------------------------------------------------- */
  {
    static const double offs[] = { 100.0, -120.0 };
    for (guint o = 0; o < G_N_ELEMENTS(offs); o++) {
      GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
      enc_diddle(sym, 10);
      enc_text(sym, CQ);
      enc_diddle(sym, 2);
      GArray *iq = render(sym, RATE, offs[o], FALSE, 1.0, 1.0);
      add_tail(iq, 4.0, 0.001, rng);
      add_noise(iq, 0.001, rng);
      char *txt = run_rtty(iq, RATE, &d);
      char *t = tidy(txt);
      char what[96];
      g_snprintf(what, sizeof(what), "copy exact at %+.0f Hz offset (centre ±6)",
                 offs[o]);
      printf("       %+.0f Hz: \"%s\" centre %+.1f\n", offs[o], t,
             d.freq_offset_hz);
      check(what, g_strcmp0(t, CQ) == 0 &&
                  fabs(d.freq_offset_hz - offs[o]) < 6.0);
      g_free(txt);
      g_free(t);
      g_array_free(iq, TRUE);
      g_array_free(sym, TRUE);
    }
  }

  /* -- figures + unshift-on-space -------------------------------------------- */
  {
    const char *EX = "UR 599 001 001 BK";
    GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym, 10);
    enc_text(sym, EX);
    enc_diddle(sym, 2);
    GArray *iq = render(sym, RATE, 0.0, FALSE, 1.0, 1.0);
    add_tail(iq, 4.0, 0.001, rng);
    add_noise(iq, 0.001, rng);
    char *txt = run_rtty(iq, RATE, NULL);
    char *t = tidy(txt);
    printf("       figures: \"%s\"\n", t);
    check("figures/letters shifts + UOS exact", g_strcmp0(t, EX) == 0);
    g_free(txt);
    g_free(t);
    g_array_free(iq, TRUE);
    g_array_free(sym, TRUE);
  }

  /* -- reversed polarity ------------------------------------------------------ */
  {
    GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym, 14);                /* election proves on the diddles     */
    enc_text(sym, CQ);
    enc_diddle(sym, 2);
    GArray *iq = render(sym, RATE, 0.0, TRUE, 1.0, 1.0);
    add_tail(iq, 4.0, 0.001, rng);
    add_noise(iq, 0.001, rng);
    char *txt = run_rtty(iq, RATE, NULL);
    char *t = tidy(txt);
    printf("       reversed: \"%s\"\n", t);
    check("REVERSED signal copies exact (polarity election)",
          g_strcmp0(t, CQ) == 0);
    g_free(txt);
    g_free(t);
    g_array_free(iq, TRUE);
    g_array_free(sym, TRUE);
  }

  /* -- AWGN ------------------------------------------------------------------- */
  {
    GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym, 10);
    enc_text(sym, CQ);
    enc_diddle(sym, 2);
    GArray *iq = render(sym, RATE, 40.0, FALSE, 1.0, 1.0);
    add_tail(iq, 4.0, 0.35, rng);
    add_noise(iq, 0.35, rng);
    char *txt = run_rtty(iq, RATE, &d);
    char *t = tidy(txt);
    guint dist = lev(t, CQ);
    printf("       AWGN σ0.35: \"%s\" (dist %u, snr %.0f dB)\n", t, dist,
           d.snr_db);
    check("noisy copy ≤ 2 errors", dist <= 2);
    g_free(txt);
    g_free(t);
    g_array_free(iq, TRUE);
    g_array_free(sym, TRUE);
  }

  /* -- selective fade: mark 10 dB down (the ATC case) ------------------------- */
  {
    GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym, 10);
    enc_text(sym, CQ);
    enc_diddle(sym, 2);
    GArray *iq = render(sym, RATE, 0.0, FALSE, 0.316, 1.0);
    add_tail(iq, 4.0, 0.02, rng);
    add_noise(iq, 0.02, rng);
    char *txt = run_rtty(iq, RATE, NULL);
    char *t = tidy(txt);
    printf("       fade −10 dB mark: \"%s\"\n", t);
    check("selective fade (mark −10 dB) copies exact", g_strcmp0(t, CQ) == 0);
    g_free(txt);
    g_free(t);
    g_array_free(iq, TRUE);
    g_array_free(sym, TRUE);
  }

  /* -- slow QSB ---------------------------------------------------------------- */
  {
    GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym, 10);
    enc_text(sym, CQ);
    enc_diddle(sym, 2);
    GArray *iq = render(sym, RATE, 0.0, FALSE, 1.0, 1.0);
    for (guint i = 0; i < iq->len / 2; i++) {      /* 10 dB dip @ 0.3 Hz     */
      double g = 1.0 - 0.684 * (0.5 - 0.5 * cos(2.0 * G_PI * 0.3 * i / RATE));
      g_array_index(iq, float, 2 * i)     *= (float)g;
      g_array_index(iq, float, 2 * i + 1) *= (float)g;
    }
    add_tail(iq, 4.0, 0.05, rng);
    add_noise(iq, 0.05, rng);
    char *txt = run_rtty(iq, RATE, NULL);
    char *t = tidy(txt);
    guint dist = lev(t, CQ);
    printf("       QSB 10 dB @ 0.3 Hz: \"%s\" (dist %u)\n", t, dist);
    check("slow QSB copy ≤ 2 errors", dist <= 2);
    g_free(txt);
    g_free(t);
    g_array_free(iq, TRUE);
    g_array_free(sym, TRUE);
  }

  /* -- squelch: pure noise ----------------------------------------------------- */
  {
    GArray *iq = g_array_new(FALSE, FALSE, sizeof(float));
    add_tail(iq, 20.0, 1.0, rng);
    char *txt = run_rtty(iq, RATE, NULL);
    check("20 s of pure noise emits NOTHING", txt[0] == '\0');
    g_free(txt);
    g_array_free(iq, TRUE);
  }

  /* -- squelch: keyed CW (a lone on/off tone must never pass) ------------------ */
  {
    GArray *iq = g_array_new(FALSE, FALSE, sizeof(float));
    double phase = 0.0;
    const double dph = 2.0 * G_PI * 85.0 / RATE;   /* sits ON the mark slot  */
    const double dit = RATE * 1.2 / 25.0;          /* 25 WPM                 */
    guint n = (guint)(20.0 * RATE);
    guint t = 0;
    gboolean on = TRUE;
    guint run = (guint)dit;
    for (guint i = 0; i < n; i++) {
      float re = 0, im = 0;
      if (on) {
        re = (float)cos(phase);
        im = (float)sin(phase);
      }
      phase = fmod(phase + dph, 2.0 * G_PI);
      g_array_append_val(iq, re);
      g_array_append_val(iq, im);
      if (++t >= run) {
        t = 0;
        on = !on;
        run = (guint)(dit * (1 + g_rand_int_range(rng, 0, 3)));
      }
    }
    add_noise(iq, 0.05, rng);
    char *txt = run_rtty(iq, RATE, NULL);
    check("20 s of keyed CW emits NOTHING", txt[0] == '\0');
    g_free(txt);
    g_array_free(iq, TRUE);
  }

  /* -- squelch: two INDEPENDENT keyed carriers 170 Hz apart --------------------- */
  {
    GArray *iq = g_array_new(FALSE, FALSE, sizeof(float));
    double ph1 = 0.0, ph2 = 1.0;
    const double dp1 = 2.0 * G_PI * 85.0 / RATE;
    const double dp2 = 2.0 * G_PI * -85.0 / RATE;
    const double dit1 = RATE * 1.2 / 24.0, dit2 = RATE * 1.2 / 31.0;
    guint n = (guint)(20.0 * RATE);
    guint t1 = 0, t2 = 0, r1 = (guint)dit1, r2 = (guint)(dit2 * 3);
    gboolean on1 = TRUE, on2 = FALSE;
    for (guint i = 0; i < n; i++) {
      float re = 0, im = 0;
      if (on1) { re += (float)cos(ph1); im += (float)sin(ph1); }
      if (on2) { re += (float)cos(ph2); im += (float)sin(ph2); }
      ph1 = fmod(ph1 + dp1, 2.0 * G_PI);
      ph2 = fmod(ph2 + dp2, 2.0 * G_PI);
      g_array_append_val(iq, re);
      g_array_append_val(iq, im);
      if (++t1 >= r1) { t1 = 0; on1 = !on1; r1 = (guint)(dit1 * (1 + g_rand_int_range(rng, 0, 3))); }
      if (++t2 >= r2) { t2 = 0; on2 = !on2; r2 = (guint)(dit2 * (1 + g_rand_int_range(rng, 0, 3))); }
    }
    add_noise(iq, 0.05, rng);
    char *txt = run_rtty(iq, RATE, NULL);
    if (txt[0]) { printf("       two-carrier leak: \"%s\"\n", txt); }
    check("two independent CW carriers 170 Hz apart emit NOTHING",
          txt[0] == '\0');
    g_free(txt);
    g_array_free(iq, TRUE);
  }

  /* -- integration: worst-case straddler through the real channelizer ---------- */
  {
    /* Station centred at +2625 Hz = exactly midway between channel 10
     * (+2500) and channel 11 (+2750) of the 48 k / 250 Hz wide-passband
     * bank — tones land at ±(125∓85) in both channels. */
    SkimChannelizer *ch = skim_channelizer_new_ex(48000.0, 250.0, 225.0, 16);
    check("RTTY bank constructs", ch != NULL);
    if (ch) {
      GArray *sym = g_array_new(FALSE, FALSE, sizeof(Sym));
      enc_diddle(sym, 10);
      enc_text(sym, CQ);
      enc_diddle(sym, 2);
      GArray *iq = render(sym, 48000.0, 2625.0, FALSE, 0.5, 0.5);
      add_tail(iq, 4.0, 0.002, rng);
      add_noise(iq, 0.002, rng);

      const SkimDecodeBackend *be = skim_decode_rtty();
      const double out_rate = skim_channelizer_out_rate(ch);
      gpointer st10 = be->channel_new(out_rate);
      gpointer st11 = be->channel_new(out_rate);
      gpointer st30 = be->channel_new(out_rate);
      GString *t10 = g_string_new(NULL), *t11 = g_string_new(NULL),
              *t30 = g_string_new(NULL);
      double c10 = 0, c11 = 0;
      const float *p = (const float *)iq->data;
      guint nframes = iq->len / 2;
      float buf[64 * 2];
      for (guint at = 0; at < nframes; at += 4800) {
        skim_channelizer_push(ch, p + 2 * at, MIN(4800u, nframes - at));
        guint m;
        while ((m = skim_channelizer_read(ch, 10, buf, 64)) > 0) {
          if (be->process(st10, buf, m, &d)) {
            g_string_append(t10, d.text);
            c10 = d.freq_offset_hz;
          }
        }
        while ((m = skim_channelizer_read(ch, 11, buf, 64)) > 0) {
          if (be->process(st11, buf, m, &d)) {
            g_string_append(t11, d.text);
            c11 = d.freq_offset_hz;
          }
        }
        while ((m = skim_channelizer_read(ch, 30, buf, 64)) > 0) {
          if (be->process(st30, buf, m, &d)) { g_string_append(t30, d.text); }
        }
      }
      char *s10 = tidy(t10->str), *s11 = tidy(t11->str);
      printf("       ch10: \"%s\" (centre %+.1f)\n", s10, c10);
      printf("       ch11: \"%s\" (centre %+.1f)\n", s11, c11);
      gboolean ok10 = g_strcmp0(s10, CQ) == 0;
      gboolean ok11 = g_strcmp0(s11, CQ) == 0;
      check("straddler copies exact in a straddling channel", ok10 || ok11);
      /* Whatever a straddling channel says must be the truth or silence —
       * a mutated second copy would breed a phantom callsign. */
      check("no mutated second copy",
            (s10[0] == '\0' || ok10) && (s11[0] == '\0' || ok11));
      gboolean abs_ok =
          (ok10 && fabs(2500.0 + c10 - 2625.0) < 8.0) ||
          (ok11 && fabs(2750.0 + c11 - 2625.0) < 8.0);
      check("absolute frequency recovered (±8 Hz)", abs_ok);
      check("a far channel stays MUTE", t30->str[0] == '\0');
      g_free(s10);
      g_free(s11);
      g_string_free(t10, TRUE);
      g_string_free(t11, TRUE);
      g_string_free(t30, TRUE);
      be->channel_free(st10);
      be->channel_free(st11);
      be->channel_free(st30);
      g_array_free(iq, TRUE);
      g_array_free(sym, TRUE);
      skim_channelizer_free(ch);
    } else {
      checks += 4; fails += 4;
    }
  }

  /* -- the WHOLE offline pipeline in RTTY mode --------------------------------- */
  {
    /* Two CQing RTTY stations on a 48 kHz band: OK1BR on the worst-case
     * channel straddle (+2625), DL1ABC at −3010. The pipeline must table
     * exactly those two calls, mode RTTY, on the right absolute Hz —
     * bank geometry, backend pick, ghost/same-tone arbitration and the
     * extractor over ITA2 text all in one pass. */
    const double CENTER = 14085000.0;
    GArray *sym1 = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym1, 10);
    enc_text(sym1, "CQ TEST DE OK1BR OK1BR CQ");
    enc_text(sym1, " CQ TEST DE OK1BR OK1BR CQ");
    enc_diddle(sym1, 2);
    GArray *sym2 = g_array_new(FALSE, FALSE, sizeof(Sym));
    enc_diddle(sym2, 14);
    enc_text(sym2, "CQ TEST DE DL1ABC DL1ABC CQ");
    enc_text(sym2, " CQ TEST DE DL1ABC DL1ABC CQ");
    enc_diddle(sym2, 2);
    GArray *iq1 = render(sym1, 48000.0, 2625.0, FALSE, 0.4, 0.4);
    GArray *iq2 = render(sym2, 48000.0, -3010.0, FALSE, 0.25, 0.25);
    GArray *band = iq1->len >= iq2->len ? iq1 : iq2;
    GArray *othr = band == iq1 ? iq2 : iq1;
    for (guint i = 0; i < othr->len; i++) {
      g_array_index(band, float, i) += g_array_index(othr, float, i);
    }
    add_tail(band, 3.0, 0.002, rng);
    add_noise(band, 0.002, rng);

    SkimPipelineConfig cfg = { 0 };
    cfg.mode = SKIM_PIPELINE_MODE_RTTY;
    SkimPipeline *pl = skim_pipeline_new(&cfg);
    skim_pipeline_set_station_cb(pl, collect_station, NULL);
    GError *err = NULL;
    check("offline RTTY pipeline starts", skim_pipeline_start_offline(pl, &err));
    const float *p = (const float *)band->data;
    guint nframes = band->len / 2;
    for (guint at = 0; at < nframes; at += 4800) {
      skim_pipeline_feed(pl, p + 2 * at, MIN(4800u, nframes - at), 48000.0,
                         CENTER);
    }
    guint nst = skim_pipeline_stations(pl);
    printf("       stations tabled: %u (seen %u calls)\n", nst, n_seen);
    check("exactly the two stations tabled (no phantoms)",
          nst == 2 && n_seen == 2);
    const SkimStation *ok = seen_find("OK1BR");
    const SkimStation *dl = seen_find("DL1ABC");
    if (ok) {
      printf("       OK1BR: %s %.1f Hz cq=%d\n", ok->mode, ok->freq_hz, ok->cq);
    }
    if (dl) {
      printf("       DL1ABC: %s %.1f Hz cq=%d\n", dl->mode, dl->freq_hz, dl->cq);
    }
    check("OK1BR tabled as RTTY on 14087625 ± 10 Hz",
          ok && g_strcmp0(ok->mode, "RTTY") == 0 &&
          fabs(ok->freq_hz - (CENTER + 2625.0)) < 10.0 && ok->cq);
    check("DL1ABC tabled as RTTY on 14081990 ± 10 Hz",
          dl && g_strcmp0(dl->mode, "RTTY") == 0 &&
          fabs(dl->freq_hz - (CENTER - 3010.0)) < 10.0 && dl->cq);
    skim_pipeline_stop(pl);
    skim_pipeline_free(pl);
    g_array_free(iq1, TRUE);
    g_array_free(iq2, TRUE);
    g_array_free(sym1, TRUE);
    g_array_free(sym2, TRUE);
  }

  g_rand_free(rng);
  printf("\n=== %d checks, %d failures ===\n%s\n", checks, fails,
         fails ? "FAIL" : "PASS — RTTY decodes, the squelch holds.");
  return fails ? 1 : 0;
}
