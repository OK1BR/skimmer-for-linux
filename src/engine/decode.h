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
  gboolean pane_own;                  /* backend owns the PANE view of this
                                       * call's text: the pipeline must not
                                       * append text[] to the pane (the pane
                                       * ops carry it); extractor/spot/log
                                       * paths are unaffected */
} SkimDecode;

/* --- pane ops (phase B: draft → final hybrid pane) --------------------------
 * While the neural reader streams on a solid channel, the backend COMPOSES
 * the pane view of the current over: v2's per-element draft shows live and
 * every reader word-commit rewrites it in place. The ops are full-state
 * (SET carries the over's whole text), so a dropped op self-heals at the
 * next one. Display only — nothing here may ever reach the extractor. */
typedef enum {
  SKIM_PANE_OP_APPEND = 0,  /* plain pane append (e.g. the over separator)   */
  SKIM_PANE_OP_OPEN,        /* erase `erase` bytes of already-shown draft,
                             * open the over region, append text             */
  SKIM_PANE_OP_SET,         /* replace the open over region with text        */
  SKIM_PANE_OP_CLOSE,       /* final replace; the region seals               */
} SkimPaneOpKind;

typedef struct {
  SkimPaneOpKind kind;
  guint  erase;             /* OPEN only: bytes of shown draft to take back  */
  guint  final_len;         /* prefix of text[] that is reader-final (bytes);
                             * the rest is live draft (dim in the UI)        */
  char  *text;              /* g_free; over text, or the APPEND payload      */
  char  *fresh;             /* g_free, may be NULL: newly final chars — the
                             * decode log's increment (never re-logs a SET)  */
} SkimPaneOp;

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

  /* Current peak signal level, linear envelope units (comparable across
   * channels of one bank). The pipeline arbitrates adjacent-channel ghosts
   * with it: only the strongest channel of a splatter group may report.
   * Optional (NULL = backend cannot tell). */
  double (*level)(gpointer state);

  /* Current in-channel tone offset estimate (Hz, EMA) — lets the pipeline
   * recognise that two OVERLAPPING channels are tracking the same physical
   * tone (a signal midway decodes in both at equal level and doubles every
   * character). Optional. */
  double (*tone_offset_hz)(gpointer state);

  /* Absolute frequency label for this channel/slot (Hz) — the pipeline
   * refreshes it each block before process(). Diagnostics only (run dumps
   * carry real frequencies); the decode path never depends on it. Optional. */
  void (*set_freq)(gpointer state, double freq_hz);

  /* Pending DISPLAY-ONLY text (returns it and clears; caller g_frees; NULL
   * when none). The pipeline shows it (pane, decode log) but must NEVER
   * feed it to the callsign extractor: aux text may come from a lexically
   * primed model that can hallucinate plausible callsigns from noise —
   * live-caught 2026-07-16, a phantom "EI55ISI" station row minted from a
   * neural re-read of channel babble. Optional. */
  char *(*take_aux_text)(gpointer state);

  /* Pop one pending pane op (fills *op, returns TRUE) or FALSE when the
   * queue is empty. The caller owns op->text/op->fresh. Same hallucination
   * rule as aux text: pane + decode log only, NEVER the extractor.
   * Optional. */
  gboolean (*take_pane_op)(gpointer state, SkimPaneOp *op);
};

G_END_DECLS

#endif /* SKIMMER_DECODE_H */
