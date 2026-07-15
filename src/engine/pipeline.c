/* pipeline.c — the engine assembled (M5, docs/SCOPE.md).
 *
 * Threading: the TCI client's LWS thread copies each IQ block into a
 * GAsyncQueue (bounded — overload drops whole blocks and counts them,
 * never stalls the socket). The engine thread drains the queue, feeds the
 * channelizer, walks every channel through its CW decoder and callsign
 * extractor, folds hits into the station table (ghost dedup) and offers
 * validated stations to the spot feeder. Station/text/state callbacks fire
 * on the engine thread — the UI marshals.
 *
 * The channelizer (and the per-channel decoder/extractor arrays) are built
 * lazily from the FIRST block's sample rate, and rebuilt if the device rate
 * ever changes mid-run (iq_samplerate is radio state — another client can
 * switch it under us).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "pipeline.h"

#include <string.h>

#include "callsign.h"
#include "channelizer.h"
#include "decode_cw.h"
#include "spot_out.h"
#include "tci_client.h"

#define QUEUE_MAX      64                     /* blocks in flight            */
#define DRAIN_FRAMES   64                     /* per-channel read chunk      */
#define PRUNE_EVERY_US (2 * G_USEC_PER_SEC)
#define STATION_TTL_US (600u * G_USEC_PER_SEC)

typedef struct {
  float  *iq;
  guint   nframes;
  double  rate;
  double  center_hz;
} IqBlock;

struct _SkimPipeline {
  SkimPipelineConfig cfg;
  char              *host;

  SkimTciClient    *tci;
  SkimChannelizer  *bank;
  double            bank_rate;
  gpointer         *dec;                       /* per-channel decoder state  */
  SkimCallsignExtractor **ext;
  guint             nchan;

  SkimStationTable *stations;
  SkimSpotOut      *spots;

  GAsyncQueue      *queue;                     /* IqBlock*                   */
  GThread          *thread;
  volatile gint     run;
  gint64            last_prune;

  SkimPipelineStationCb station_cb;
  gpointer              station_user;
  SkimPipelineTextCb    text_cb;
  gpointer              text_user;
  SkimPipelineStateCb   state_cb;
  gpointer              state_user;

  volatile guint64 frames;
  volatile guint64 dropped;
  guint64          spots_total;                /* survives stop()            */
};

/* ---- construction ------------------------------------------------------------ */

SkimPipeline *skim_pipeline_new(const SkimPipelineConfig *cfg) {
  SkimPipeline *p = g_new0(SkimPipeline, 1);
  p->cfg = *cfg;
  p->host = g_strdup(cfg->host ? cfg->host : "127.0.0.1");
  p->cfg.host = p->host;
  if (p->cfg.chan_bw_hz <= 0) { p->cfg.chan_bw_hz = 125.0; }
  p->stations = skim_station_table_new();
  p->queue    = g_async_queue_new();
  if (p->cfg.dict_path) {
    GError *err = NULL;
    if (!skim_callsign_dict_load(p->cfg.dict_path, &err)) {
      g_warning("pipeline: dictionary %s not loaded: %s", p->cfg.dict_path,
                err ? err->message : "?");
      g_clear_error(&err);
    }
  }
  return p;
}

static void bank_teardown(SkimPipeline *p) {
  const SkimDecodeBackend *cw = skim_decode_cw();
  for (guint c = 0; c < p->nchan; c++) {
    if (p->dec) { cw->channel_free(p->dec[c]); }
    if (p->ext) { skim_callsign_extractor_free(p->ext[c]); }
  }
  g_free(p->dec);
  g_free(p->ext);
  p->dec = NULL;
  p->ext = NULL;
  p->nchan = 0;
  g_clear_pointer(&p->bank, skim_channelizer_free);
}

void skim_pipeline_free(SkimPipeline *p) {
  if (!p)
    return;
  skim_pipeline_stop(p);
  bank_teardown(p);
  skim_station_table_free(p->stations);
  g_clear_pointer(&p->spots, skim_spot_out_free);
  IqBlock *b;
  while ((b = g_async_queue_try_pop(p->queue)) != NULL) {
    g_free(b->iq);
    g_free(b);
  }
  g_async_queue_unref(p->queue);
  g_free(p->host);
  g_free(p);
}

void skim_pipeline_set_station_cb(SkimPipeline *p, SkimPipelineStationCb cb, gpointer user) {
  p->station_cb = cb;
  p->station_user = user;
}
void skim_pipeline_set_text_cb(SkimPipeline *p, SkimPipelineTextCb cb, gpointer user) {
  p->text_cb = cb;
  p->text_user = user;
}
void skim_pipeline_set_state_cb(SkimPipeline *p, SkimPipelineStateCb cb, gpointer user) {
  p->state_cb = cb;
  p->state_user = user;
}

/* ---- LWS-thread side: queue the block ---------------------------------------- */

static void iq_cb(const float *iq, guint nframes, double rate, double center,
                  gpointer user) {
  SkimPipeline *p = user;
  if (g_async_queue_length(p->queue) >= QUEUE_MAX) {
    p->dropped++;                              /* overload: drop, don't stall */
    return;
  }
  IqBlock *b = g_new(IqBlock, 1);
  b->iq = g_memdup2(iq, (gsize)nframes * 2 * sizeof(float));
  b->nframes   = nframes;
  b->rate      = rate;
  b->center_hz = center;
  g_async_queue_push(p->queue, b);
}

/* ---- engine thread ------------------------------------------------------------ */

static void bank_build(SkimPipeline *p, double rate) {
  bank_teardown(p);
  p->bank = skim_channelizer_new(rate, p->cfg.chan_bw_hz);
  if (!p->bank) {
    g_warning("pipeline: no channelizer for %.0f Hz / %.0f Hz", rate,
              p->cfg.chan_bw_hz);
    return;
  }
  p->bank_rate = rate;
  p->nchan = skim_channelizer_count(p->bank);
  p->dec = g_new0(gpointer, p->nchan);
  p->ext = g_new0(SkimCallsignExtractor *, p->nchan);
  const SkimDecodeBackend *cw = skim_decode_cw();
  const double out_rate = skim_channelizer_out_rate(p->bank);
  for (guint c = 0; c < p->nchan; c++) {
    p->dec[c] = cw->channel_new(out_rate);
    p->ext[c] = skim_callsign_extractor_new();
  }
  if (p->state_cb) {
    char detail[128];
    g_snprintf(detail, sizeof(detail), "%u channels × %.0f Hz @ %.0f kHz IQ",
               p->nchan, p->cfg.chan_bw_hz, rate / 1000.0);
    p->state_cb(TRUE, detail, p->state_user);
  }
}

static void process_block(SkimPipeline *p, IqBlock *b) {
  if (!p->bank || p->bank_rate != b->rate) { bank_build(p, b->rate); }
  if (!p->bank)
    return;
  skim_channelizer_push(p->bank, b->iq, b->nframes);
  p->frames += b->nframes;

  const SkimDecodeBackend *cw = skim_decode_cw();
  float buf[DRAIN_FRAMES * 2];
  SkimDecode d;
  for (guint c = 0; c < p->nchan; c++) {
    guint n;
    while ((n = skim_channelizer_read(p->bank, c, buf, DRAIN_FRAMES)) > 0) {
      if (!cw->process(p->dec[c], buf, n, &d))
        continue;
      const double chan_hz =
          b->center_hz + skim_channelizer_offset_hz(p->bank, c);
      const double sig_hz = chan_hz + d.freq_offset_hz;
      if (p->text_cb) { p->text_cb(sig_hz, d.text, p->text_user); }

      skim_callsign_extractor_feed(p->ext[c], d.text);
      char call[24];
      double score = skim_callsign_extractor_best(p->ext[c], call, sizeof(call));
      if (score <= 0)
        continue;

      SkimStation st;
      memset(&st, 0, sizeof(st));
      g_strlcpy(st.call, call, sizeof(st.call));
      g_strlcpy(st.mode, "CW", sizeof(st.mode));
      st.freq_hz    = sig_hz;
      st.speed      = d.speed;
      st.snr_db     = d.snr_db;
      st.score      = score;
      st.last_heard = g_get_monotonic_time();
      st.first_heard = st.last_heard;
      const SkimStation *merged = skim_station_table_report(p->stations, &st);
      if (p->station_cb) { p->station_cb(merged, p->station_user); }
      /* The merged record's frequency is the ghost-deduped one. */
      skim_spot_out_emit(p->spots, merged->call, merged->mode,
                         merged->freq_hz, merged->snr_db);
    }
  }
}

static gpointer engine_thread(gpointer data) {
  SkimPipeline *p = data;
  while (g_atomic_int_get(&p->run)) {
    IqBlock *b = g_async_queue_timeout_pop(p->queue, 100 * 1000);
    if (b) {
      process_block(p, b);
      g_free(b->iq);
      g_free(b);
    }
    const gint64 now = g_get_monotonic_time();
    if (now - p->last_prune > PRUNE_EVERY_US) {
      p->last_prune = now;
      skim_station_table_prune(p->stations, STATION_TTL_US);
    }
  }
  return NULL;
}

/* ---- start/stop ----------------------------------------------------------------- */

gboolean skim_pipeline_start(SkimPipeline *p, GError **error) {
  if (p->thread) {
    g_set_error(error, g_quark_from_static_string("skim-pipeline"), 1,
                "pipeline already running");
    return FALSE;
  }
  p->tci = skim_tci_client_new(p->cfg.host, p->cfg.port);
  skim_tci_client_set_iq_cb(p->tci, iq_cb, p);
  if (!skim_tci_client_start(p->tci, p->cfg.iq_rate, error)) {
    g_clear_pointer(&p->tci, skim_tci_client_free);
    return FALSE;
  }
  p->spots = skim_spot_out_new(p->tci, NULL, 0);
  g_atomic_int_set(&p->run, 1);
  p->last_prune = g_get_monotonic_time();
  p->thread = g_thread_new("skim-engine", engine_thread, p);
  if (p->state_cb) {
    char detail[160];
    g_snprintf(detail, sizeof(detail), "%s — dds %.0f Hz",
               skim_tci_client_device(p->tci),
               skim_tci_client_center_hz(p->tci));
    p->state_cb(TRUE, detail, p->state_user);
  }
  return TRUE;
}

void skim_pipeline_stop(SkimPipeline *p) {
  if (!p->thread)
    return;
  g_atomic_int_set(&p->run, 0);
  g_thread_join(p->thread);
  p->thread = NULL;
  if (p->spots) { p->spots_total = skim_spot_out_count(p->spots); }
  g_clear_pointer(&p->tci, skim_tci_client_free);
  g_clear_pointer(&p->spots, skim_spot_out_free);
  if (p->state_cb) { p->state_cb(FALSE, "disconnected", p->state_user); }
}

/* ---- counters -------------------------------------------------------------------- */

guint64 skim_pipeline_frames(const SkimPipeline *p) { return p->frames; }
guint64 skim_pipeline_spots(const SkimPipeline *p) {
  return p->spots ? skim_spot_out_count(p->spots) : p->spots_total;
}
guint skim_pipeline_stations(const SkimPipeline *p) {
  return skim_station_table_size(p->stations);
}
guint64 skim_pipeline_dropped_blocks(const SkimPipeline *p) { return p->dropped; }
