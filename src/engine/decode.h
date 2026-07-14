/* decode.h — decode backend interface (mode-agnostic).
 *
 * A backend receives one narrow, decimated COMPLEX baseband channel (interleaved
 * float I/Q, as produced by the channelizer) and emits decoded characters with a
 * confidence, the channel's audio/carrier offset, and a speed (WPM for CW, baud
 * for RTTY/PSK). CW, RTTY and PSK31/63 are each a backend implementing this.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_DECODE_H
#define SKIMMER_DECODE_H

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_DECODE_TEXT_MAX 64

/* One decode event on a channel. */
typedef struct {
  char    text[SKIM_DECODE_TEXT_MAX]; /* newly decoded characters (UTF-8/ASCII) */
  double  confidence;                 /* 0.0 .. 1.0 */
  double  freq_offset_hz;             /* signal offset within the channel */
  double  speed;                      /* WPM (CW) or baud (RTTY/PSK) */
  double  snr_db;                     /* estimated SNR of the trace */
} SkimDecode;

typedef struct _SkimDecodeBackend SkimDecodeBackend;

/* A decode backend: a vtable + opaque per-channel state factory. */
struct _SkimDecodeBackend {
  const char *name;                   /* "cw", "rtty", "psk31" ... */

  /* Allocate per-channel decoder state for a channel sampled at sample_rate. */
  gpointer (*channel_new)(double sample_rate);
  void     (*channel_free)(gpointer state);

  /* Feed one block of interleaved complex baseband (nframes I/Q pairs).
   * Returns TRUE and fills *out when characters were decoded this block. */
  gboolean (*process)(gpointer state, const float *iq, guint nframes,
                      SkimDecode *out);
};

G_END_DECLS

#endif /* SKIMMER_DECODE_H */
