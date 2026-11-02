/* decode_tap.h — the decode tap: what the decoders handed the extractors.
 *
 * gh#28 bench. A replay over IQ decodes for minutes; the stages BEHIND the
 * decoders — callsign extractor, station table, spot/RBN policy — run in
 * seconds. The tap records every event those stages consumed, so a change
 * to any of them replays from the tap, without decoding again and without
 * the decoder's run-to-run noise (docs/BENCH.md, "Noise floor"): both
 * sides of such an A/B read the very same decodes.
 *
 * The cut sits in the pipeline's dispatch, AFTER ghost arbitration and the
 * frequency lock (those read neighbouring decoder state and belong to the
 * decoder side) and BEFORE the extractor is fed. A tap replay is exact
 * only when it also sees the events that touch post-decoder state without
 * a decode: the bank geometry, the extractor resets (a slot reborn, a
 * centre change) and the station-table prune ticks (their time decides
 * whether a comeback merges into a record or re-spots after a gone).
 *
 * One event = one text line, tab-separated, C locale, doubles round-trip
 * exact (%.17g), the decoded text last and g_strescape()d. Lines starting
 * with '#' are comments (the writer's header). GLib-only.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_DECODE_TAP_H
#define SKIMMER_DECODE_TAP_H

#include <glib.h>
#include "decode.h"

G_BEGIN_DECLS

typedef enum {
  SKIM_TAP_BANK = 0,   /* channel bank built: rate, nchan, spacing          */
  SKIM_TAP_DECODE,     /* one decode reached the extractor seam             */
  SKIM_TAP_RESET,      /* extractor (chan, slot) reset — the slot was reborn */
  SKIM_TAP_FLUSH,      /* centre change: every extractor reset              */
  SKIM_TAP_PRUNE,      /* the station table was pruned at this time         */
} SkimTapKind;

typedef struct {
  SkimTapKind kind;
  guint64     frames;      /* the pipeline's stream frames at the event
                            * (fed frames + holes since it started)         */
  gint64      now_us;      /* the pipeline's clock at the event: stream time
                            * offline, monotonic live — every TTL and dedup
                            * window ticks on it                            */
  double      rate;        /* BANK: IQ rate (Hz)                            */
  guint       nchan;       /* BANK: channels in the bank                    */
  double      chan_bw_hz;  /* BANK: channel spacing                         */
  guint       chan;        /* DECODE, RESET                                 */
  guint       slot;        /* DECODE, RESET (decode lane within the channel) */
  double      sig_hz;      /* DECODE: the locked signal frequency           */
  gboolean    contested;   /* DECODE: slot held >1 carrier — shown, not fed */
  SkimDecode  d;           /* DECODE: text, confidence, speed, snr_db       */
} SkimTapEvent;

/* The pipeline's tap sink (SkimPipelineConfig.decode_tap): one call per
 * event, on the thread that runs the dispatch. ev is valid for the call. */
typedef void (*SkimTapCb)(const SkimTapEvent *ev, gpointer user);

/* The event as one line, no trailing newline. Caller g_frees. */
char    *skim_tap_event_to_line(const SkimTapEvent *ev);

/* Parse one line; FALSE (ev untouched) for a comment, a blank or a line
 * that is not an event. A DECODE's text is unescaped into ev->d.text,
 * truncated to SKIM_DECODE_TEXT_MAX − 1 bytes like the decoders' own. */
gboolean skim_tap_line_parse(const char *line, SkimTapEvent *ev);

G_END_DECLS

#endif /* SKIMMER_DECODE_TAP_H */
