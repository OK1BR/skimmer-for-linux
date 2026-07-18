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

#define SPOT_ARGB 0xFF30C060u                  /* green-ish label            */

typedef struct {
  double freq_hz;
  gint64 at;
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
  const double out_hz =
      rh > 1 ? round(freq_hz / rh) * rh : freq_hz;
  m->at      = now;

  if (s->tci) {
    char text[48];
    g_snprintf(text, sizeof(text), "%.0f dB", snr_db);
    skim_tci_client_spot(s->tci, call, mode, out_hz, SPOT_ARGB, text);
  }
  if (s->sink) { s->sink(call, mode, out_hz, snr_db, speed, s->sink_user); }
  s->emitted++;
  return TRUE;
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
