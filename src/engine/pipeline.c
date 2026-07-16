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

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "callsign.h"
#include "channelizer.h"
#include "decode_cw.h"
#include "spot_out.h"
#include "tci_client.h"
#include "tone_split.h"

#define QUEUE_MAX      64                     /* blocks in flight            */
#define DRAIN_FRAMES   64                     /* per-channel read chunk      */
#define PRUNE_EVERY_US (2 * G_USEC_PER_SEC)
/* RBN policy: the network keeps its own history, so re-announce sparsely
 * (the panadapter's 180 s is about keeping labels alive — not needed here)
 * and only re-spot a move that is a real QSY, not estimate convergence. */
#define RBN_MIN_SCORE_DEFAULT 0.85
#define RBN_RESPOT_S          600
#define RBN_QSY_HZ            100.0
#define RBN_MAX_PER_S         5
/* A station silent this long leaves the table (and its panadapter label is
 * SPOT_DELETEd). 120 s rides out one side of a QSO; the old 600 s kept a
 * contest band map full of stations long gone (Richard, 2026-07-15). */
#define STATION_TTL_US (120u * G_USEC_PER_SEC)
/* Per-signal frequency lock. The per-decode tone estimate breathes (noise
 * pulls a band-edge estimate toward the channel centre, arbitration swaps
 * between overlapped channels) and every consumer downstream — decode log,
 * traffic-monitor windows, tracker, spots — saw the wobble (live-caught
 * 2026-07-15: "decodes keep sliding by an IQ offset"). A dispatched channel
 * LOCKS its frequency: estimates within the window only nudge it (slow
 * convergence onto the true carrier), and a neighbouring channel that takes
 * over arbitration ADOPTS the existing lock — one signal, one frequency. */
#define FLOCK_TTL_US   (30 * G_USEC_PER_SEC)  /* quiet this long → unlock    */
#define FLOCK_WIN_HZ   40.0                    /* same signal within this     */
#define FLOCK_ADOPT_HZ 60.0                    /* neighbour lock, same tone   */

typedef struct {
  double hz;                                   /* 0 = unlocked                */
  gint64 at;                                   /* last decode using the lock  */
} FreqLock;

/* Tone slots (SKIM_TONE_SPLIT=1, default OFF until the live session confirms
 * it). Every per-channel array (dec/ext/lvl/flock/sgen) is slot-major, sized
 * nchan × SKIM_TONE_SPLIT_MAX. Slot 0 is the channel itself — with the
 * splitter unarmed the walk touches only slot 0 and the path is the old one
 * exactly. Higher slots come and go with the channel's splitter topology;
 * their decoders spawn lazily and reset whenever the slot generation moves. */
#define NSLOT SKIM_TONE_SPLIT_MAX
#define SL(c, s) ((c) * NSLOT + (s))

typedef struct {
  float  *iq;
  guint   nframes;
  double  rate;
  double  center_hz;
} IqBlock;

/* One slot's decode collected in pass 1 of a block (dispatch happens in
 * pass 2, once every channel's level is known for ghost arbitration). */
typedef struct {
  guint      chan;
  guint      slot;
  double     eff_off;   /* slot mix + decode offset = in-channel offset      */
  gboolean   contested; /* slot band held >1 carrier when this decoded       */
  SkimDecode d;
  char      *aux;       /* display-only text (take_aux_text) — shown, logged,
                         * NEVER fed to the extractor (hallucination guard)  */
} Hit;

struct _SkimPipeline {
  SkimPipelineConfig cfg;
  char              *host;

  SkimTciClient    *tci;
  SkimChannelizer  *bank;
  double            bank_rate;
  double            center_hz;                 /* last block's dds centre    */
  gpointer         *dec;                       /* per-slot decoder state     */
  SkimCallsignExtractor **ext;
  SkimToneSplit   **split;                     /* per-channel; NULL = unarmed */
  guint            *sgen;                      /* slot generations last seen */
  gboolean          use_split;
  guint             nchan;

  SkimStationTable *stations;
  SkimSpotOut      *spots;
  SkimSpotOut      *rbn_spots;                 /* RBN policy → cfg.rbn feed  */
  double            rbn_min;

  GArray           *hits;                      /* Hit — one block's decodes  */
  double           *lvl;                       /* per-channel level snapshot */
  FreqLock         *flock;                     /* per-channel frequency lock */
  guint64           ghosts;                    /* decodes suppressed         */

  GAsyncQueue      *queue;                     /* IqBlock*                   */
  GThread          *thread;
  volatile gint     run;
  gint64            last_prune;
  gboolean          offline;                   /* fed by skim_pipeline_feed  */
  volatile gint     cq_only;                   /* spot only CALLING stations */

  char             *dlog_path;                 /* decode log (engine thread) */
  FILE             *dlog;

  SkimPipelineStationCb station_cb;
  gpointer              station_user;
  SkimPipelineStationGoneCb gone_cb;
  gpointer                  gone_user;
  SkimPipelineTextCb    text_cb;
  gpointer              text_user;
  SkimPipelineStateCb   state_cb;
  gpointer              state_user;
  SkimPipelineVfoCb     vfo_cb;
  gpointer              vfo_user;

  volatile guint64 frames;
  volatile guint64 dropped;
  guint64          spots_total;                /* survives stop()            */
};

/* ---- construction ------------------------------------------------------------ */

/* CW decoder pick: v1 (classical, live-proven) stays the default; the
 * Viterbi v2 arms with SKIM_CW_V2=1 until the replay A/B flips it. One
 * process = one backend (per-channel states are not mixable). */
static const SkimDecodeBackend *cw_backend(void) {
  return g_getenv("SKIM_CW_V2") ? skim_decode_cw_v2() : skim_decode_cw();
}

static void station_gone_fwd(const SkimStation *st, gpointer user);

/* RBN spot_out sink → the telnet feed (user = the borrowed SkimRbnFeed). */
static void rbn_sink_fwd(const char *call, const char *mode, double freq_hz,
                         double snr_db, double speed, gpointer user) {
  skim_rbn_feed_spot(user, call, mode, freq_hz, snr_db, speed);
}

SkimPipeline *skim_pipeline_new(const SkimPipelineConfig *cfg) {
  SkimPipeline *p = g_new0(SkimPipeline, 1);
  p->cfg = *cfg;
  p->host = g_strdup(cfg->host ? cfg->host : "127.0.0.1");
  p->cfg.host = p->host;
  p->dlog_path = g_strdup(cfg->decode_log_path);
  if (p->cfg.chan_bw_hz <= 0) { p->cfg.chan_bw_hz = 125.0; }
  p->use_split = g_getenv("SKIM_TONE_SPLIT") != NULL;
  p->stations = skim_station_table_new();
  skim_station_table_set_gone_cb(p->stations, station_gone_fwd, p);
  p->queue    = g_async_queue_new();
  p->hits     = g_array_new(FALSE, FALSE, sizeof(Hit));
  if (p->cfg.rbn) {
    /* Lives for the pipeline's whole life (not per-connection like the TCI
     * sink): the dedup memo rides out reconnects, no re-spot burst. */
    p->rbn_spots = skim_spot_out_new(NULL);
    skim_spot_out_set_policy(p->rbn_spots, RBN_RESPOT_S, RBN_QSY_HZ,
                             RBN_MAX_PER_S);
    skim_spot_out_set_sink(p->rbn_spots, rbn_sink_fwd, p->cfg.rbn);
    p->rbn_min = p->cfg.rbn_min_score > 0 ? p->cfg.rbn_min_score
                                          : RBN_MIN_SCORE_DEFAULT;
  }
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
  const SkimDecodeBackend *cw = cw_backend();
  for (guint i = 0; i < p->nchan * NSLOT; i++) {
    if (p->dec && p->dec[i]) { cw->channel_free(p->dec[i]); }
    if (p->ext && p->ext[i]) { skim_callsign_extractor_free(p->ext[i]); }
  }
  if (p->split) {
    for (guint c = 0; c < p->nchan; c++) { skim_tone_split_free(p->split[c]); }
  }
  g_free(p->dec);
  g_free(p->ext);
  g_free(p->split);
  g_free(p->sgen);
  g_free(p->lvl);
  g_free(p->flock);
  p->dec = NULL;
  p->ext = NULL;
  p->split = NULL;
  p->sgen = NULL;
  p->lvl = NULL;
  p->flock = NULL;
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
  g_clear_pointer(&p->rbn_spots, skim_spot_out_free);
  IqBlock *b;
  while ((b = g_async_queue_try_pop(p->queue)) != NULL) {
    g_free(b->iq);
    g_free(b);
  }
  g_async_queue_unref(p->queue);
  g_array_free(p->hits, TRUE);
  g_free(p->host);
  g_free(p->dlog_path);
  g_free(p);
}

void skim_pipeline_set_station_cb(SkimPipeline *p, SkimPipelineStationCb cb, gpointer user) {
  p->station_cb = cb;
  p->station_user = user;
}
void skim_pipeline_set_station_gone_cb(SkimPipeline *p,
                                       SkimPipelineStationGoneCb cb,
                                       gpointer user) {
  p->gone_cb = cb;
  p->gone_user = user;
}

/* Station-table removal (engine thread) → panadapter label + owner. Dead
 * spots used to hang on the radio for the server's whole 10-minute TTL. */
static void station_gone_fwd(const SkimStation *st, gpointer user) {
  SkimPipeline *p = user;
  if (p->spots) { skim_spot_out_delete(p->spots, st->call); }
  /* No delete on the cluster wire — just forget the memo, so a comeback
   * after the TTL re-spots to the RBN at once. */
  if (p->rbn_spots) { skim_spot_out_delete(p->rbn_spots, st->call); }
  if (p->gone_cb) { p->gone_cb(st, p->gone_user); }
}
void skim_pipeline_set_text_cb(SkimPipeline *p, SkimPipelineTextCb cb, gpointer user) {
  p->text_cb = cb;
  p->text_user = user;
}
void skim_pipeline_set_state_cb(SkimPipeline *p, SkimPipelineStateCb cb, gpointer user) {
  p->state_cb = cb;
  p->state_user = user;
}
void skim_pipeline_set_vfo_cb(SkimPipeline *p, SkimPipelineVfoCb cb, gpointer user) {
  p->vfo_cb = cb;
  p->vfo_user = user;
}

/* ---- network-thread side: retune + connection loss ---------------------------- */

static void vfo_fwd_cb(double vfo_hz, gpointer user) {
  SkimPipeline *p = user;
  if (p->vfo_cb) { p->vfo_cb(vfo_hz, p->vfo_user); }
}

static void closed_cb(gpointer user) {
  SkimPipeline *p = user;
  /* The owner reacts (stops the pipeline from ITS thread — never from here:
   * stop() joins threads and would deadlock inside the LWS callback). */
  if (p->state_cb) { p->state_cb(FALSE, "connection lost", p->state_user); }
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
  p->dec = g_new0(gpointer, p->nchan * NSLOT);
  p->ext = g_new0(SkimCallsignExtractor *, p->nchan * NSLOT);
  p->sgen = g_new0(guint, p->nchan * NSLOT);
  p->lvl = g_new0(double, p->nchan * NSLOT);
  p->flock = g_new0(FreqLock, p->nchan * NSLOT);
  if (p->use_split) { p->split = g_new0(SkimToneSplit *, p->nchan); }
  p->center_hz = 0;                            /* fresh states — no flush    */
  const SkimDecodeBackend *cw = cw_backend();
  const double out_rate = skim_channelizer_out_rate(p->bank);
  for (guint c = 0; c < p->nchan; c++) {
    p->dec[SL(c, 0)] = cw->channel_new(out_rate);
    p->ext[SL(c, 0)] = skim_callsign_extractor_new();
    if (p->split) {
      p->split[c] = skim_tone_split_new(out_rate);
      p->sgen[SL(c, 0)] = skim_tone_split_slot_gen(p->split[c], 0);
    }
  }
  if (p->state_cb) {
    char detail[128];
    g_snprintf(detail, sizeof(detail), "%u channels × %.0f Hz @ %.0f kHz IQ",
               p->nchan, p->cfg.chan_bw_hz, rate / 1000.0);
    p->state_cb(TRUE, detail, p->state_user);
  }
}

/* Ghost arbitration: a decode from channel c is suppressed when a channel
 * within ±2 spacings holds a clearly stronger signal — the splatter of a
 * strong station decodes several channels away and pollutes the extractors
 * with clipped garbage (31 % of all fragments on the 2026-07-15 contest
 * sample). A +6 dB neighbour kills unconditionally; a +3 dB DIRECT
 * neighbour kills only when the tone sits off-centre in our channel (true
 * in-channel stations stay near their own centre, leakage sits at the
 * passband edge) — that margin protects a genuinely weaker station one
 * channel away from a big gun. */
static gboolean ghost_suppressed(SkimPipeline *p, const double *lvl, guint c,
                                 guint slot, double eff_off) {
  const SkimDecodeBackend *cw = cw_backend();
  const gint M = (gint)p->nchan;
  const gint k = (c <= p->nchan / 2) ? (gint)c : (gint)c - M;
  const double mine = lvl[SL(c, slot)];
  for (gint s = -2; s <= 2; s++) {
    if (s == 0)
      continue;
    const gint kn = k + s;
    const guint cn = (guint)((kn % M + M) % M);
    /* The neighbour's strongest slot: "does that channel hold a clearly
     * stronger signal" is a per-channel question either way. */
    double nb_lvl = 0.0;
    for (guint j = 0; j < NSLOT; j++) { nb_lvl = MAX(nb_lvl, lvl[SL(cn, j)]); }
    const double ratio = nb_lvl / MAX(mine, 1e-12);
    if (ratio >= 2.0) { return TRUE; }                     /* +6 dB          */
    if (ABS(s) == 1 && ratio >= 1.41 &&                     /* +3 dB          */
        fabs(eff_off) > 0.24 * p->cfg.chan_bw_hz) {
      return TRUE;
    }
    /* Same-tone tie-break: a signal midway between two overlapping channels
     * decodes in BOTH at near-equal level (the ±3 dB rule cannot pick a
     * winner) and every character comes out twice (live-caught 2026-07-15,
     * IT9IQN doubling in the tuned pane). When a DIRECT neighbour tracks
     * the same physical tone in ANY of its slots, the channel whose centre
     * is closer keeps it; a dead tie falls to the lower channel. */
    if (ABS(s) == 1 && ratio > 0.71 && cw->tone_offset_hz) {
      const double my_hz = skim_channelizer_offset_hz(p->bank, c) + eff_off;
      const guint nns = (p->split && p->split[cn])
                            ? skim_tone_split_slots(p->split[cn]) : 1;
      for (guint j = 0; j < nns; j++) {
        if (!p->dec[SL(cn, j)])
          continue;
        const double nb_off =
            ((p->split && p->split[cn])
                 ? skim_tone_split_slot_hz(p->split[cn], j) : 0.0) +
            cw->tone_offset_hz(p->dec[SL(cn, j)]);
        const double nb_hz = skim_channelizer_offset_hz(p->bank, cn) + nb_off;
        if (fabs(my_hz - nb_hz) < 60.0) {
          const double my_dist = fabs(eff_off);
          const double nb_dist = fabs(nb_off);
          if (my_dist > nb_dist + 5.0) { return TRUE; }
          if (fabs(my_dist - nb_dist) <= 5.0 && kn < k) { return TRUE; }
        }
      }
    }
  }
  return FALSE;
}

/* A slot whose generation moved is a NEW stream: fresh decoder, cleared
 * candidates, no frequency pin. Slot decoders spawn lazily here. */
static void slot_sync(SkimPipeline *p, guint c, guint s,
                      const SkimDecodeBackend *cw) {
  const guint g = skim_tone_split_slot_gen(p->split[c], s);
  const guint i = SL(c, s);
  if (p->sgen[i] == g && p->dec[i])
    return;
  if (g_getenv("SKIM_TS_DEBUG")) {      /* absolute view the splitter lacks  */
    g_printerr("pipeline: ch %u @ %.0f Hz slot %u gen %u->%u mix %+.1f Hz\n",
               c, p->center_hz + skim_channelizer_offset_hz(p->bank, c), s,
               p->sgen[i], g, skim_tone_split_slot_hz(p->split[c], s));
  }
  if (p->dec[i]) { cw->channel_free(p->dec[i]); }
  p->dec[i] = cw->channel_new(skim_channelizer_out_rate(p->bank));
  if (p->ext[i]) {
    skim_callsign_extractor_reset(p->ext[i]);
  } else {
    p->ext[i] = skim_callsign_extractor_new();
  }
  memset(&p->flock[i], 0, sizeof(FreqLock));
  p->sgen[i] = g;
}

static void process_block(SkimPipeline *p, IqBlock *b) {
  if (!p->bank || p->bank_rate != b->rate) { bank_build(p, b->rate); }
  if (!p->bank)
    return;

  /* A dds/centre change (band switch, panadapter re-centre) invalidates
   * every channel's ABSOLUTE meaning: the decoders' trackers describe
   * signals that are no longer there, the extractors still hold the OLD
   * band's callsign candidates — they age by TRAFFIC, not time, so the
   * first decodes on the new band re-announced 20 m calls and the QSY
   * logic teleported their spots onto 40 m (live-caught 2026-07-15).
   * Flush all per-channel state; the station table keeps the old band's
   * records (really heard — they age out via their own TTL). */
  if (b->center_hz != p->center_hz) {
    if (p->center_hz != 0) {
      const SkimDecodeBackend *cwf = cw_backend();
      const double out_rate = skim_channelizer_out_rate(p->bank);
      for (guint c = 0; c < p->nchan; c++) {
        for (guint s = 0; s < NSLOT; s++) {
          const guint i = SL(c, s);
          if (p->dec[i]) { cwf->channel_free(p->dec[i]); }
          p->dec[i] = (s == 0) ? cwf->channel_new(out_rate) : NULL;
          if (p->ext[i]) { skim_callsign_extractor_reset(p->ext[i]); }
          p->sgen[i] = 0;
        }
        if (p->split) {                /* fresh detection: old carriers gone */
          skim_tone_split_free(p->split[c]);
          p->split[c] = skim_tone_split_new(out_rate);
          p->sgen[SL(c, 0)] = skim_tone_split_slot_gen(p->split[c], 0);
        }
      }
      memset(p->flock, 0, p->nchan * NSLOT * sizeof(FreqLock));
    }
    p->center_hz = b->center_hz;
  }

  skim_channelizer_push(p->bank, b->iq, b->nframes);
  p->frames += b->nframes;

  const SkimDecodeBackend *cw = cw_backend();
  float buf[DRAIN_FRAMES * 2];
  SkimDecode d;

  /* Pass 1: drain every channel, collect its decodes (dispatch waits until
   * all levels for this block are known — arbitration needs the neighbours).
   * An armed splitter sits between the channel and the decoders: samples go
   * through it and come back out per slot (verbatim while passthrough). */
  g_array_set_size(p->hits, 0);
  for (guint c = 0; c < p->nchan; c++) {
    SkimToneSplit *sp = p->split ? p->split[c] : NULL;
    const double chan_hz = cw->set_freq
        ? b->center_hz + skim_channelizer_offset_hz(p->bank, c) : 0.0;
    if (cw->set_freq && !sp) { cw->set_freq(p->dec[SL(c, 0)], chan_hz); }
    guint n;
    while ((n = skim_channelizer_read(p->bank, c, buf, DRAIN_FRAMES)) > 0) {
      if (!sp) {
        const gboolean got = cw->process(p->dec[SL(c, 0)], buf, n, &d);
        char *aux = cw->take_aux_text
                        ? cw->take_aux_text(p->dec[SL(c, 0)]) : NULL;
        if (!got && !aux)
          continue;
        if (!got) { memset(&d, 0, sizeof(d)); }
        Hit h = { .chan = c, .slot = 0, .eff_off = d.freq_offset_hz,
                  .contested = FALSE, .d = d, .aux = aux };
        g_array_append_val(p->hits, h);
        continue;
      }
      skim_tone_split_push(sp, buf, n);
      const guint ns = skim_tone_split_slots(sp);
      for (guint s = 0; s < ns; s++) {
        slot_sync(p, c, s, cw);
        if (cw->set_freq) {
          cw->set_freq(p->dec[SL(c, s)],
                       chan_hz + skim_tone_split_slot_hz(sp, s));
        }
        float sbuf[DRAIN_FRAMES * 2];
        guint m;
        while ((m = skim_tone_split_read(sp, s, sbuf, DRAIN_FRAMES)) > 0) {
          const gboolean got = cw->process(p->dec[SL(c, s)], sbuf, m, &d);
          char *aux = cw->take_aux_text
                          ? cw->take_aux_text(p->dec[SL(c, s)]) : NULL;
          if (!got && !aux)
            continue;
          if (!got) { memset(&d, 0, sizeof(d)); }
          Hit h = { .chan = c, .slot = s,
                    .eff_off = skim_tone_split_slot_hz(sp, s) +
                               d.freq_offset_hz,
                    .contested = skim_tone_split_slot_contested(sp, s),
                    .d = d, .aux = aux };
          g_array_append_val(p->hits, h);
        }
      }
    }
    for (guint s = 0; s < NSLOT; s++) {
      const gboolean live = (s == 0) || (sp && s < skim_tone_split_slots(sp));
      p->lvl[SL(c, s)] = (live && p->dec[SL(c, s)] && cw->level)
                             ? cw->level(p->dec[SL(c, s)]) : 0.0;
    }
  }

  /* Pass 2: arbitrate and dispatch the survivors. */
  for (guint i = 0; i < p->hits->len; i++) {
    const Hit *h = &g_array_index(p->hits, Hit, i);
    const guint c = h->chan;
    d = h->d;
    if (cw->level && ghost_suppressed(p, p->lvl, c, h->slot, h->eff_off)) {
      p->ghosts++;
      g_free(h->aux);
      continue;
    }
    {
      const double chan_hz =
          b->center_hz + skim_channelizer_offset_hz(p->bank, c);
      const double raw_hz = chan_hz + h->eff_off;

      /* Frequency lock (see FLOCK_* above): pin the signal, follow the
       * carrier only slowly, adopt a neighbour's lock on a channel swap.
       * Adoption takes the CLOSEST live lock — a split channel's slots sit
       * ≥20 Hz apart, so its window tightens or the default 60 Hz would
       * steal the OTHER carrier's pin. */
      FreqLock *L = &p->flock[SL(c, h->slot)];
      const gint64 tnow = g_get_monotonic_time();
      if (L->hz > 0 && tnow - L->at > FLOCK_TTL_US) { L->hz = 0; }
      if (L->hz > 0 && fabs(raw_hz - L->hz) <= FLOCK_WIN_HZ) {
        L->hz += 0.1 * (raw_hz - L->hz);
      } else {
        L->hz = raw_hz;
        const double win = (p->split && p->split[c] &&
                            skim_tone_split_slots(p->split[c]) > 1)
                               ? 15.0 : FLOCK_ADOPT_HZ;
        const gint M = (gint)p->nchan;
        const gint k = (c <= p->nchan / 2) ? (gint)c : (gint)c - M;
        double best = win;
        for (gint s = -2; s <= 2; s++) {
          if (s == 0)
            continue;
          const guint cn = (guint)(((k + s) % M + M) % M);
          for (guint j = 0; j < NSLOT; j++) {
            const FreqLock *N = &p->flock[SL(cn, j)];
            if (N->hz > 0 && tnow - N->at <= FLOCK_TTL_US &&
                fabs(N->hz - raw_hz) <= best) {
              best  = fabs(N->hz - raw_hz);
              L->hz = N->hz;                   /* same tone, keep its pin    */
            }
          }
        }
      }
      L->at = tnow;
      const double sig_hz = L->hz;
      if (p->dlog) {
        char tbuf[24];
        if (p->offline) {                    /* stream time — deterministic  */
          guint sec = (guint)((double)p->frames / MAX(b->rate, 1.0));
          g_snprintf(tbuf, sizeof(tbuf), "%02u:%02u:%02u", sec / 3600,
                     (sec / 60) % 60, sec % 60);
        } else {
          GDateTime *now = g_date_time_new_now_local();
          char *ts = g_date_time_format(now, "%H:%M:%S");
          g_strlcpy(tbuf, ts, sizeof(tbuf));
          g_free(ts);
          g_date_time_unref(now);
        }
        char khz[G_ASCII_DTOSTR_BUF_SIZE];   /* C locale — parseable dot     */
        g_ascii_formatd(khz, sizeof(khz), "%.2f", sig_hz / 1000.0);
        if (d.text[0]) {
          fprintf(p->dlog, "%s %9s %2.0fwpm %3.0fdB |%s|\n",
                  tbuf, khz, d.speed, d.snr_db, d.text);
        }
        if (h->aux) {
          fprintf(p->dlog, "%s %9s   aux        |%s|\n", tbuf, khz, h->aux);
        }
        fflush(p->dlog);
      }
      if (p->text_cb && d.text[0]) { p->text_cb(sig_hz, d.text, p->text_user); }
      if (h->aux) {
        /* Display-only: pane + log, NEVER the extractor (a lexically primed
         * re-read hallucinates plausible calls from babble — the phantom
         * EI55ISI station, live-caught 2026-07-16). Own line in the pane. */
        if (p->text_cb) {
          char *line = g_strdup_printf("\n%s\n", h->aux);
          p->text_cb(sig_hz, line, p->text_user);
          g_free(line);
        }
        g_free(h->aux);
      }
      if (!d.text[0])
        continue;

      /* Two carriers beating inside one slot garble the text — it still
       * shows (log, monitor panes), but it must not breed callsign
       * candidates: beat mutations validate often enough to reach the
       * spot path (live-caught 2026-07-15, the 14036 slot). */
      if (h->contested)
        continue;

      skim_callsign_extractor_feed(p->ext[SL(c, h->slot)], d.text);
      char call[24];
      gboolean cq = FALSE;
      double score = skim_callsign_extractor_best_ex(p->ext[SL(c, h->slot)],
                                                     call, sizeof(call), &cq);
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
      st.cq         = cq;
      st.last_heard = g_get_monotonic_time();
      st.first_heard = st.last_heard;
      const SkimStation *merged = skim_station_table_report(p->stations, &st);
      if (p->station_cb) { p->station_cb(merged, p->station_user); }
      /* The merged record's frequency is the ghost-deduped one. CQ-only
       * policy: only a station heard CALLING may reach the spot sinks —
       * an S&P answer does not own the frequency (RBN etiquette). */
      if (p->spots &&
          (!g_atomic_int_get(&p->cq_only) || merged->cq)) {
        skim_spot_out_emit(p->spots, merged->call, merged->mode,
                           merged->freq_hz, merged->snr_db, merged->speed);
      }
      /* RBN etiquette is stricter than the panadapter: only a CALLING
       * station (regardless of the local CQ-only switch), and only once
       * the best score seen clears the RBN threshold. */
      if (p->rbn_spots && merged->cq && merged->score >= p->rbn_min) {
        skim_spot_out_emit(p->rbn_spots, merged->call, merged->mode,
                           merged->freq_hz, merged->snr_db, merged->speed);
      }
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
  skim_tci_client_set_vfo_cb(p->tci, vfo_fwd_cb, p);
  skim_tci_client_set_closed_cb(p->tci, closed_cb, p);
  if (!skim_tci_client_start(p->tci, p->cfg.iq_rate, error)) {
    g_clear_pointer(&p->tci, skim_tci_client_free);
    return FALSE;
  }
  p->spots = skim_spot_out_new(p->tci);
  if (p->dlog_path) {
    p->dlog = fopen(p->dlog_path, "a");
    if (p->dlog) {
      GDateTime *now = g_date_time_new_now_local();
      char *ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
      fprintf(p->dlog, "--- session %s — %s, dds %.0f Hz ---\n", ts,
              skim_tci_client_device(p->tci), skim_tci_client_center_hz(p->tci));
      fflush(p->dlog);
      g_free(ts);
      g_date_time_unref(now);
    } else {
      g_warning("pipeline: decode log %s not writable", p->dlog_path);
    }
  }
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

gboolean skim_pipeline_start_offline(SkimPipeline *p, GError **error) {
  if (p->thread || p->offline) {
    g_set_error(error, g_quark_from_static_string("skim-pipeline"), 1,
                "pipeline already running");
    return FALSE;
  }
  p->offline = TRUE;
  if (p->dlog_path) {
    p->dlog = fopen(p->dlog_path, "a");
    if (p->dlog) {
      fprintf(p->dlog, "--- offline replay session ---\n");
    } else {
      g_warning("pipeline: decode log %s not writable", p->dlog_path);
    }
  }
  return TRUE;
}

void skim_pipeline_feed(SkimPipeline *p, const float *iq, guint nframes,
                        double rate, double center_hz) {
  g_return_if_fail(p->offline);
  IqBlock b = {
    .iq = (float *)iq,                 /* process_block never writes into it */
    .nframes   = nframes,
    .rate      = rate,
    .center_hz = center_hz,
  };
  process_block(p, &b);
}

void skim_pipeline_stop(SkimPipeline *p) {
  if (p->offline) {
    p->offline = FALSE;
    if (p->dlog) {
      fclose(p->dlog);
      p->dlog = NULL;
    }
    return;
  }
  if (!p->thread)
    return;
  g_atomic_int_set(&p->run, 0);
  g_thread_join(p->thread);
  p->thread = NULL;
  if (p->spots) { p->spots_total = skim_spot_out_count(p->spots); }
  g_clear_pointer(&p->tci, skim_tci_client_free);
  g_clear_pointer(&p->spots, skim_spot_out_free);
  if (p->dlog) {                    /* engine thread is joined — safe here   */
    fclose(p->dlog);
    p->dlog = NULL;
  }
  if (p->state_cb) { p->state_cb(FALSE, "disconnected", p->state_user); }
}

/* ---- counters -------------------------------------------------------------------- */

double skim_pipeline_vfo_hz(const SkimPipeline *p) {
  return p->tci ? skim_tci_client_vfo_hz(p->tci) : 0;
}

void skim_pipeline_tune(SkimPipeline *p, double freq_hz) {
  if (p->tci) { skim_tci_client_tune(p->tci, freq_hz); }
}

void skim_pipeline_set_spot_cq_only(SkimPipeline *p, gboolean cq_only) {
  g_atomic_int_set(&p->cq_only, cq_only ? 1 : 0);
}

guint64 skim_pipeline_frames(const SkimPipeline *p) { return p->frames; }
guint64 skim_pipeline_spots(const SkimPipeline *p) {
  return p->spots ? skim_spot_out_count(p->spots) : p->spots_total;
}
guint64 skim_pipeline_rbn_spots(const SkimPipeline *p) {
  return p->rbn_spots ? skim_spot_out_count(p->rbn_spots) : 0;
}
guint skim_pipeline_stations(const SkimPipeline *p) {
  return skim_station_table_size(p->stations);
}
guint64 skim_pipeline_dropped_blocks(const SkimPipeline *p) { return p->dropped; }
