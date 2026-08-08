/* spot_out.c — spot output policy + sinks (M5, docs/SCOPE.md).
 *
 * Per-call dedup: a spot goes out when the call is new, when it moved by
 * more than the QSY threshold (a real frequency change must repaint the
 * label), or when the re-spot interval elapsed (sdr-for-linux ages spots
 * out after 10 min — re-announcing keeps live stations on the panadapter;
 * we re-announce at 180 s only while the station is still being heard,
 * because emit() is only called on fresh decode events).
 *
 * A token-bucket rate limiter caps the outgoing flood globally (band
 * openings in a contest can validate tens of calls per second).
 *
 * Sinks: the TCI client (labels on the radio panadapter) and an optional
 * callback (the RBN telnet feed, offline gates). The pipeline runs one
 * instance per sink so each feed keeps its own dedup memo and budget.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "spot_out.h"

#include <math.h>

typedef struct {
  double freq_hz;
  gint64 at;
  char   mode[8];                     /* last emission, for recolour resend  */
  double snr_db;
  double out_hz;                      /* the REPORTED value on the label     */
  gint   out_rh;                      /* grid it was quantised to            */
} SpotMemo;

struct _SkimSpotOut {
  SkimTciClient *tci;                          /* not owned                  */

  guint   respot_s;
  double  qsy_hz;
  guint   max_per_s;

  GHashTable *memo;                            /* call → SpotMemo (owned)    */
  double      tokens;
  gint64      token_at;

  volatile gint round_hz;              /* outgoing frequency grid (0=exact) */
  SkimSpotSink sink;
  gpointer     sink_user;
  guint64      emitted;

  SkimNowFn now_fn;                            /* NULL → monotonic           */
  gpointer  now_user;

  SkimDupQuery *dupq;                          /* not owned; NULL = no tint  */
};

static gint64 so_now(const SkimSpotOut *s) {
  return s->now_fn ? s->now_fn(s->now_user) : g_get_monotonic_time();
}

SkimSpotOut *skim_spot_out_new(SkimTciClient *tci) {
  SkimSpotOut *s = g_new0(SkimSpotOut, 1);
  s->tci       = tci;
  s->respot_s  = 180;
  s->qsy_hz    = 30.0;    /* re-spot as the frequency estimate converges —
                           * the label follows onto the true carrier         */
  s->max_per_s = 5;
  s->tokens    = 5.0;
  s->token_at  = g_get_monotonic_time();
  s->memo = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  return s;
}

void skim_spot_out_free(SkimSpotOut *s) {
  if (!s)
    return;
  g_hash_table_destroy(s->memo);
  g_free(s);
}

void skim_spot_out_set_dup_query(SkimSpotOut *s, SkimDupQuery *q) {
  s->dupq = q;
}

void skim_spot_out_set_policy(SkimSpotOut *s, guint respot_s,
                              double qsy_hz, guint max_per_s) {
  s->respot_s  = respot_s;
  s->qsy_hz    = qsy_hz;
  s->max_per_s = MAX(max_per_s, 1u);
}

void skim_spot_out_set_sink(SkimSpotOut *s, SkimSpotSink sink, gpointer user) {
  s->sink      = sink;
  s->sink_user = user;
}

void skim_spot_out_set_round_hz(SkimSpotOut *s, guint hz) {
  g_atomic_int_set(&s->round_hz, (gint)hz);
}

void skim_spot_out_set_clock(SkimSpotOut *s, SkimNowFn now_fn, gpointer user) {
  s->now_fn   = now_fn;
  s->now_user = user;
  s->token_at = so_now(s);         /* never mix clocks in the bucket delta   */
}

gboolean skim_spot_out_emit(SkimSpotOut *s, const char *call, const char *mode,
                            double freq_hz, double snr_db, double speed) {
  if (!call || !call[0])
    return FALSE;
  const gint64 now = so_now(s);
  if (now < s->token_at) {         /* clock re-based (offline start): the    */
    s->token_at = now;             /* wall-time reference would freeze the   */
  }                                /* bucket on a huge negative delta        */

  SpotMemo *m = g_hash_table_lookup(s->memo, call);
  if (m && now >= m->at &&
      fabs(m->freq_hz - freq_hz) < s->qsy_hz &&
      now - m->at < (gint64)s->respot_s * G_USEC_PER_SEC) {
    return FALSE;                              /* fresh enough already       */
  }

  /* Token bucket. */
  s->tokens += (double)(now - s->token_at) / G_USEC_PER_SEC * s->max_per_s;
  s->tokens  = MIN(s->tokens, (double)s->max_per_s);
  s->token_at = now;
  if (s->tokens < 1.0)
    return FALSE;
  s->tokens -= 1.0;

  if (!m) {
    m = g_new0(SpotMemo, 1);
    g_hash_table_insert(s->memo, g_strdup(call), m);
  }
  m->freq_hz = freq_hz;                        /* raw — QSY policy input     */
  const gint rh = g_atomic_int_get(&s->round_hz);
  /* Hysteresis on the REPORTED value (grid only; Exact follows the raw
   * estimate): a re-announce re-sends the value already on the label unless
   * the raw estimate left the reported cell clearly — ¾ of a step, so a
   * few-Hz jitter across a grid boundary can never alternate the label.
   * A grid change (out_rh differs) re-quantises at once. Principle from
   * s53zo's DeepCW fork (EMA vs reported frequency); no code copied. */
  double out_hz;
  if (rh > 1) {
    out_hz = (m->out_rh == rh && fabs(freq_hz - m->out_hz) <= 0.75 * rh)
                 ? m->out_hz
                 : round(freq_hz / rh) * rh;
  } else {
    out_hz = freq_hz;
  }
  m->out_hz  = out_hz;
  m->out_rh  = rh;
  m->at      = now;
  g_strlcpy(m->mode, mode ? mode : "CW", sizeof(m->mode));
  m->snr_db  = snr_db;

  if (s->tci) {
    char text[48];
    g_snprintf(text, sizeof(text), "%.0f dB", snr_db);
    const SkimDupVerdict v = s->dupq
        ? skim_dup_query_lookup(s->dupq, call, freq_hz, mode, 5)
        : SKIM_DUP_UNKNOWN;
    skim_tci_client_spot(s->tci, call, mode, out_hz,
                         skim_spot_argb_for_dup(v), text);
  }
  if (s->sink) { s->sink(call, mode, out_hz, snr_db, speed, s->sink_user); }
  s->emitted++;
  return TRUE;
}

void skim_spot_out_recolour(SkimSpotOut *s, const char *call,
                            SkimDupVerdict verdict) {
  if (!s->tci || !call || !call[0])
    return;
  SpotMemo *m = g_hash_table_lookup(s->memo, call);
  /* Only a label that is plausibly still on the panadapter (the radio keeps
   * spots ~10 min) — repainting a long-gone station would re-plant it. */
  if (!m || so_now(s) - m->at > 10 * 60 * (gint64)G_USEC_PER_SEC)
    return;
  /* Resend the STORED reported value — re-quantising from the raw memo
   * here would undo the emit-side hysteresis on every repaint. */
  const double out_hz = m->out_hz;
  char text[48];
  g_snprintf(text, sizeof(text), "%.0f dB", m->snr_db);
  /* A refresh of an existing label, not a new spot: no dedup-memo touch, no
   * token — the regular re-announce schedule stays as it was. */
  skim_tci_client_spot(s->tci, call, m->mode, out_hz,
                       skim_spot_argb_for_dup(verdict), text);
}

void skim_spot_out_delete(SkimSpotOut *s, const char *call) {
  if (!call || !call[0])
    return;
  g_hash_table_remove(s->memo, call);
  if (s->tci) { skim_tci_client_spot_delete(s->tci, call); }
}

guint64 skim_spot_out_count(const SkimSpotOut *s) {
  return s->emitted;
}
