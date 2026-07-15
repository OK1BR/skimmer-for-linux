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
#include "station.h"

G_BEGIN_DECLS

typedef struct {
  const char *host;           /* TCI server (default 127.0.0.1)              */
  guint16     port;           /* default 40001                               */
  guint       iq_rate;        /* 48/96/192/384 kHz; 0 = keep device rate     */
  double      chan_bw_hz;     /* channel spacing (default 125 Hz)            */
  const char *dict_path;      /* optional MASTER.SCP; NULL = none            */
  const char *decode_log_path; /* append raw decodes here; NULL = no log     */
} SkimPipelineConfig;

typedef struct _SkimPipeline SkimPipeline;

/* Station/text callbacks fire on the ENGINE thread; state/vfo callbacks may
 * also fire on the network thread (connection loss, radio retune) or the
 * caller's thread (start/stop). Marshal accordingly. */
typedef void (*SkimPipelineStationCb)(const SkimStation *st, gpointer user);
typedef void (*SkimPipelineTextCb)(double freq_hz, const char *text, gpointer user);
typedef void (*SkimPipelineStateCb)(gboolean connected, const char *detail, gpointer user);
/* The radio's tuned frequency (vfo:0,0) changed. */
typedef void (*SkimPipelineVfoCb)(double vfo_hz, gpointer user);

SkimPipeline *skim_pipeline_new(const SkimPipelineConfig *cfg);
void          skim_pipeline_free(SkimPipeline *p);

void skim_pipeline_set_station_cb(SkimPipeline *p, SkimPipelineStationCb cb, gpointer user);
void skim_pipeline_set_text_cb(SkimPipeline *p, SkimPipelineTextCb cb, gpointer user);
void skim_pipeline_set_state_cb(SkimPipeline *p, SkimPipelineStateCb cb, gpointer user);
void skim_pipeline_set_vfo_cb(SkimPipeline *p, SkimPipelineVfoCb cb, gpointer user);

/* Connect + start decoding. Blocks for the TCI handshake. */
gboolean skim_pipeline_start(SkimPipeline *p, GError **error);
void     skim_pipeline_stop(SkimPipeline *p);

/* The radio's tuned frequency (0 until the first vfo broadcast lands). */
double skim_pipeline_vfo_hz(const SkimPipeline *p);

/* Tune the radio to freq_hz — only ever on an explicit user action (a no-op
 * while disconnected). The vfo broadcast confirms the retune. */
void   skim_pipeline_tune(SkimPipeline *p, double freq_hz);

/* Counters for the status line / gates. */
guint64 skim_pipeline_frames(const SkimPipeline *p);
guint64 skim_pipeline_spots(const SkimPipeline *p);
guint   skim_pipeline_stations(const SkimPipeline *p);
guint64 skim_pipeline_dropped_blocks(const SkimPipeline *p);

G_END_DECLS

#endif /* SKIMMER_PIPELINE_H */
