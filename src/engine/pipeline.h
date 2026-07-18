/* pipeline.h — the headless engine assembled end to end.
 *
 * TCI client → polyphase channelizer → per-channel CW decoders → callsign
 * extractors → station tracker → spot feeder. Runs on its own engine thread
 * (the TCI client's LWS thread only queues IQ blocks). GLib-only — the GTK
 * app sits on top via callbacks and must marshal them to its main loop
 * itself (they fire on the engine thread).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_PIPELINE_H
#define SKIMMER_PIPELINE_H

#include <glib.h>
#include "decode.h"
#include "rbn_feed.h"
#include "station.h"

G_BEGIN_DECLS

typedef struct {
  const char *host;           /* TCI server (default 127.0.0.1)              */
  guint16     port;           /* default 40001                               */
  guint       iq_rate;        /* 48/96/192/384 kHz; 0 = keep device rate     */
  double      chan_bw_hz;     /* channel spacing (default 125 Hz)            */
  const char *dict_path;      /* optional MASTER.SCP; NULL = none            */
  const char *decode_log_path; /* append raw decodes here; NULL = no log     */

  /* M6 — RBN telnet feed (borrowed; the app owns it so aggregator sessions
   * survive TCI reconnects). NULL = no RBN. The feed is ALWAYS CQ-only and
   * gated at rbn_min_score (0 = default 0.85): stricter than the 0.70 that
   * puts a label on the local panadapter — the network wants certainty. */
  SkimRbnFeed *rbn;
  double       rbn_min_score;
} SkimPipelineConfig;

typedef struct _SkimPipeline SkimPipeline;

/* Station/text callbacks fire on the ENGINE thread; state/vfo callbacks may
 * also fire on the network thread (connection loss, radio retune) or the
 * caller's thread (start/stop). Marshal accordingly. */
typedef void (*SkimPipelineStationCb)(const SkimStation *st, gpointer user);
/* A station left the tracker (TTL, or its frequency was taken over). */
typedef void (*SkimPipelineStationGoneCb)(const SkimStation *st, gpointer user);
typedef void (*SkimPipelineTextCb)(double freq_hz, const char *text, gpointer user);
/* Phase B hybrid pane op (OPEN/SET/CLOSE — APPENDs ride the text cb): the
 * live over region at freq_hz becomes `text`; its first final_len bytes are
 * reader-final, the rest live draft. OPEN takes back `erase` bytes of
 * already-delivered draft first. Display only — never the extractor. */
typedef void (*SkimPipelineOverCb)(double freq_hz, SkimPaneOpKind kind,
                                   guint erase, const char *text,
                                   guint final_len, gpointer user);
typedef void (*SkimPipelineStateCb)(gboolean connected, const char *detail, gpointer user);
/* The radio's tuned frequency (vfo:0,0) changed. */
typedef void (*SkimPipelineVfoCb)(double vfo_hz, gpointer user);

SkimPipeline *skim_pipeline_new(const SkimPipelineConfig *cfg);
void          skim_pipeline_free(SkimPipeline *p);

void skim_pipeline_set_station_cb(SkimPipeline *p, SkimPipelineStationCb cb, gpointer user);
void skim_pipeline_set_station_gone_cb(SkimPipeline *p, SkimPipelineStationGoneCb cb, gpointer user);
void skim_pipeline_set_text_cb(SkimPipeline *p, SkimPipelineTextCb cb, gpointer user);
void skim_pipeline_set_over_cb(SkimPipeline *p, SkimPipelineOverCb cb, gpointer user);
void skim_pipeline_set_state_cb(SkimPipeline *p, SkimPipelineStateCb cb, gpointer user);
void skim_pipeline_set_vfo_cb(SkimPipeline *p, SkimPipelineVfoCb cb, gpointer user);

/* Connect + start decoding. Blocks for the TCI handshake. */
gboolean skim_pipeline_start(SkimPipeline *p, GError **error);
void     skim_pipeline_stop(SkimPipeline *p);

/* Offline mode (the .cf32 replayer / M3 A/B gate): no TCI, no engine thread —
 * the caller feeds IQ synchronously and callbacks fire on the caller's
 * thread. The decode log is stamped with STREAM time (deterministic across
 * runs). host/port/iq_rate in the config are ignored. */
gboolean skim_pipeline_start_offline(SkimPipeline *p, GError **error);
void     skim_pipeline_feed(SkimPipeline *p, const float *iq, guint nframes,
                            double rate, double center_hz);

/* The radio's tuned frequency (0 until the first vfo broadcast lands). */
double skim_pipeline_vfo_hz(const SkimPipeline *p);

/* Tune the radio to freq_hz — only ever on an explicit user action (a no-op
 * while disconnected). The vfo broadcast confirms the retune. */
void   skim_pipeline_tune(SkimPipeline *p, double freq_hz);

/* Spot policy: when TRUE, only stations heard CALLING (CQ/TEST/QRZ context)
 * reach the spot sinks — S&P answers stay off the panadapter/RBN. The
 * station list still tracks everything. Runtime-switchable, thread-safe. */
void   skim_pipeline_set_spot_cq_only(SkimPipeline *p, gboolean cq_only);

/* Snap outgoing spot frequencies (panadapter AND telnet feed) to a grid;
 * 0/1 = exact. Applies live. */
void   skim_pipeline_set_spot_round_hz(SkimPipeline *p, guint hz);

/* Counters for the status line / gates. */
guint64 skim_pipeline_frames(const SkimPipeline *p);
guint64 skim_pipeline_spots(const SkimPipeline *p);
guint64 skim_pipeline_rbn_spots(const SkimPipeline *p);
guint   skim_pipeline_stations(const SkimPipeline *p);
guint64 skim_pipeline_dropped_blocks(const SkimPipeline *p);

G_END_DECLS

#endif /* SKIMMER_PIPELINE_H */
