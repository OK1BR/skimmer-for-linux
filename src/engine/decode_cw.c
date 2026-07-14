/* decode_cw.c — CW decode backend. M0: skeleton (no decoding yet).
 * Real envelope/timing/Morse logic lands in M3 (docs/SCOPE.md).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_cw.h"

typedef struct {
  double sample_rate;
  /* M3: envelope state, adaptive threshold, element timer, WPM estimate. */
} CwState;

static gpointer cw_channel_new(double sample_rate) {
  CwState *st = g_new0(CwState, 1);
  st->sample_rate = sample_rate;
  return st;
}

static void cw_channel_free(gpointer state) {
  g_free(state);
}

static gboolean cw_process(gpointer state, const float *iq, guint nframes,
                           SkimDecode *out) {
  (void)state; (void)iq; (void)nframes; (void)out;
  /* M3: demodulate the tone envelope and emit Morse. */
  return FALSE;
}

const SkimDecodeBackend *skim_decode_cw(void) {
  static const SkimDecodeBackend backend = {
    .name         = "cw",
    .channel_new  = cw_channel_new,
    .channel_free = cw_channel_free,
    .process      = cw_process,
  };
  return &backend;
}
