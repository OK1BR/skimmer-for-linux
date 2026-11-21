/* station.c — per-frequency station tracker (M5).
 *
 * Storage: call → GSList of records. ONE record per call per band — a
 * transmitter is only ever in one place: a report within
 * SKIM_STATION_SAMECALL_HZ merges (the stronger report's frequency and SNR
 * win — that folds adjacent-channel ghosts of a strong station back into
 * one spot on the peak of the most readable decode; live-tuned 2026-07-15:
 * 300 Hz was too narrow, one call showed up 5× across the splatter), and a
 * confident report beyond it is a QSY that MOVES the record (an S&P station
 * hops around the band — its marker follows; live-caught 2026-07-15:
 * EA2BTN listed on 14044 and 14050 at once).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "station.h"

#include "callsign.h"

#include <math.h>
#include <string.h>

struct _SkimStationTable {
  GHashTable *by_call;        /* call (owned str) → GSList of SkimStation*   */
  guint       count;
  SkimStationGoneFunc gone_cb;
  gpointer            gone_user;
  GHashTable *left;           /* call (owned str) → Left*: records that
                               * went silent, kept for the twin rule         */
};

/* A record that left the table — pruned, or its frequency taken over. */
typedef struct {
  double hz;                  /* where it sat                                */
  guint  hearings;            /* the readings it had                         */
  gint64 at;                  /* when it was last heard                      */
} Left;

SkimStationTable *skim_station_table_new(void) {
  SkimStationTable *t = g_new0(SkimStationTable, 1);
  t->by_call = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  t->left = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  return t;
}

static void free_list(gpointer list) {
  g_slist_free_full(list, g_free);
}

void skim_station_table_free(SkimStationTable *t) {
  if (!t)
    return;
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) { free_list(val); }
  g_hash_table_destroy(t->by_call);
  g_hash_table_destroy(t->left);
  g_free(t);
}

void skim_station_table_set_gone_cb(SkimStationTable *t, SkimStationGoneFunc cb,
                                    gpointer user_data) {
  t->gone_cb   = cb;
  t->gone_user = user_data;
}

/* SKIM_ST_DEBUG=1 traces evictions/folds/QSY — the table mutations that a
 * final-state dump cannot explain (a phantom takeover chain leaves no trace). */
static gboolean st_debug(void) {
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    g_once_init_leave(&once, g_getenv("SKIM_ST_DEBUG") ? 2 : 1);
  }
  return once == 2;
}

/* TRUE when `shorter` reads as a clipped decode of `longer` — a leading or
 * trailing fragment ("M0K" of "M0KKB", "31RM" of "Z31RM"). */
static gboolean call_is_clip(const char *shorter, const char *longer) {
  gsize ls = strlen(shorter), ll = strlen(longer);
  if (ls < 3 || ls >= ll)
    return FALSE;
  return strncmp(longer, shorter, ls) == 0 ||
         strcmp(longer + (ll - ls), shorter) == 0;
}

/* The other reading of the same pair — gh#21. "Longer is the real call" is
 * wrong when the longer one is the call with the text that FOLLOWS it glued
 * on: a station sending "5NN 78 OL1B I C BK" was tabled as OL1BIC, and the
 * clip rules fed every right OL1B report into that record (80 m contest
 * fixture: OL1B 71 reports, OL1BI 57, OL1BIC 50 — the table showed OL1BIC
 * with 128). The signal cannot tell a torn call from a call plus a stray
 * letter; the dictionary can: it knows the short call and not the long one.
 * Then the short one is the station and the long one the garble.
 *
 * Where the dictionary knows neither, the READINGS decide — gh#34, after
 * OK1HRA's 808ac63. His taps had "CQ CQ DESM7FBJE SM7FBJ": seven readings
 * of SM7FBJ fed an SM7FBJE record read once, the feed gate (gh#24) held
 * that record, and the station never went out — 93 such pairs in 57 h, 17
 * of them spotted as the glued bust (RV6BTCQ, LA8IGLA, G0GYYK). On the same
 * taps the readings alone also handed real calls to their torn readings,
 * so they rule only where a torn reading is the rarer event:
 *  - the shorter call was read at least twice, and at least twice as often
 *    as the longer one: a one-off glue against a call read again and again
 *    (how often each was REPORTED says nothing — the best candidate is
 *    re-reported after every token);
 *  - the glued text FOLLOWS the call: a reading that lacks the head is the
 *    decoder losing a leading letter (K1FBH outnumbered OK1FBH), not glue;
 *  - the shorter call ends in two letters or more: F6H, I4D, LA9Q are torn
 *    real calls far more often than calls of their own;
 *  - the glued text is not a lone T, the easiest final letter to lose
 *    (DF1MA outnumbered DF1MAT).
 * A glued CQ is never part of a call, whatever the counts. On the 80 m
 * evening of 2026-11-01 this is DL1HR read 11× against DL1HRFB read 2×,
 * DL9MGE 7× against DL9MGED 1×, DK8EO 5× against DK8EODS 1× — each real
 * call lost its record to the glued reading under the clip rule alone. */

/* The call proper of a portable form — the longer side of the slash
 * ("F/OK1BR", "OK1BR/P"); the whole string when there is none. */
static const char *call_core(const char *call, gsize *len) {
  const char *slash = strchr(call, '/');
  if (!slash) {
    *len = strlen(call);
    return call;
  }
  const gsize left = (gsize)(slash - call), right = strlen(slash + 1);
  if (right > left) {
    *len = right;
    return slash + 1;
  }
  *len = left;
  return call;
}

/* TRUE when the call ends in two letters or more right after a digit —
 * "SM7FBJ", "DL1HR"; not "F6H", "LA9Q", whose single final letter is the
 * shape of a torn reading. */
static gboolean call_suffix_2(const char *call) {
  gsize n;
  const char *a = call_core(call, &n);
  const char *z = a + n;
  guint letters = 0;
  while (z > a && z[-1] >= 'A' && z[-1] <= 'Z') { z--; letters++; }
  return letters >= 2 && z > a && z[-1] >= '0' && z[-1] <= '9';
}

/* TRUE when `longer` reads as `shorter` with text glued on — the shorter
 * call is the station. read_short / read_long are the readings each call
 * had (SkimStation.hearings). */
static gboolean call_is_glue(const char *shorter, guint read_short,
                             const char *longer, guint read_long) {
  if (!call_is_clip(shorter, longer) || skim_callsign_dict_has(longer))
    return FALSE;
  if (skim_callsign_dict_has(shorter))
    return TRUE;                            /* gh#21: the dictionary knows  */
  const gsize ls = strlen(shorter);
  if (strncmp(longer, shorter, ls) != 0)
    return FALSE;                           /* the head is missing, not glue */
  const char *glued = longer + ls;
  if (strcmp(glued, "CQ") == 0)
    return TRUE;
  if (strcmp(glued, "T") == 0 || !call_suffix_2(shorter))
    return FALSE;
  return read_short >= 2 && read_short >= 2 * MAX(read_long, 1u);
}

/* The twin — gh#21. A reading that differs from a station's call by a
 * character or two is the same over keyed or copied wrong: IZ2FMI and S2FME
 * beside IZ2FME, S9ABM and IL9ABM beside DL9ABM, DM8EO and MK8EON beside
 * DK8EO. With DE in front it scores 0.80 on that single reading, gets a
 * record of its own and — after a CQ — a spot on the panadapter. The clip
 * rules above never see the pair (neither call is the head or the tail of
 * the other), and the station it belongs to has mostly left the table by
 * then: DL9ABM gave its call every four to eight minutes that evening, the
 * record lives two.
 *
 * On the 80 m evening of 2026-11-01 (cw-v2) the table held 252 calls in
 * five and a half hours; 57 of them were a look-alike of a call RBN's
 * skimmers spotted within 1 kHz at that time, and all but a handful of the
 * edit twins among them had been read ONCE. So:
 *  - a call MASTER.SCP does not know, read once — by the extractor that
 *    reports it, and never read more often while the table knew it —
 *  - that is within SKIM_STATION_TWIN_EDITS edits of a call with a record
 *    on that frequency, or with one that left it within
 *    SKIM_STATION_TWIN_MEMO_US, the two not being a clip pair,
 *  - where that call is the better attested one: MASTER.SCP knows it, or
 *    it was read twice or more —
 * is a reading of that station, and the report is dropped: no record, no
 * spot. When the better attested call arrives second, the twin's record
 * goes at once. A call read twice stands, whatever sits beside it: OK1ABC
 * and OK1ABD do work each other, and the second reading is what the RBN
 * feed asks of a call the dictionary does not know anyway (gh#24).
 *
 * A call the dictionary knows is never a twin. In a contest the short known
 * calls sit two edits from one another all over the band: in the first ten
 * minutes of 20 m on 2026-11-21, 194 of the 214 calls tabled were in
 * MASTER.SCP, and the rule without this clause dropped EW2A beside EA2N,
 * OG3B beside OG7F, LZ3RM beside LZ3SM — eleven calls, every one of them in
 * the dictionary; with it, none. So the misread that lands on a known call
 * (OK1C of OK1CZ, the 80 m fixture of 2026-09-12) stays with the table:
 * nothing in the signal or the dictionary tells it from a caller.
 *
 * The evening of 2026-11-01 with the rule: 237 calls, 43 look-alikes, the
 * 105 calls RBN confirms all still there; none of the 22 calls it dropped
 * is in RBN's file of that day on any band, and the RBN feed is the same
 * byte for byte. What is left are clip pairs outside their 30 s window
 * (SN31RO of SN31ROT), calls whose station was never read whole (OK2B and
 * K2BVX of OK2BVX), and the misread that comes before the first right
 * reading. SKIM_ST_TWIN=0 turns the rule off for an A/B (docs/BENCH.md). */
static gboolean twin_rule_on(void) {
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    const char *e = g_getenv("SKIM_ST_TWIN");
    g_once_init_leave(&once, e && e[0] == '0' ? 1 : 2);
  }
  return once == 2;
}

/* TRUE when the two calls are look-alikes the clip rules do not own. */
static gboolean call_is_twin(const char *a, const char *b) {
  if (strcmp(a, b) == 0 || call_is_clip(a, b) || call_is_clip(b, a))
    return FALSE;
  return skim_callsign_edits(a, b) <= SKIM_STATION_TWIN_EDITS;
}

/* TRUE when the call, read `reads` times, is one a twin yields to. */
static gboolean twin_rival(const char *call, guint reads) {
  return reads >= 2 || skim_callsign_dict_has(call);
}

/* TRUE when the call, read `reads` times, may be a twin at all. */
static gboolean twin_weak(const char *call, guint reads) {
  return twin_rule_on() && reads <= 1 && !skim_callsign_dict_has(call);
}

/* The readings a call stands on: those the report (or record) carries, and
 * the most the table knew of that call — its record, or the one that left. */
static guint call_reads(const SkimStationTable *t, const char *call,
                        guint hearings) {
  for (GSList *l = g_hash_table_lookup(t->by_call, call); l; l = l->next) {
    hearings = MAX(hearings, ((const SkimStation *)l->data)->hearings);
  }
  const Left *g = g_hash_table_lookup(t->left, call);
  return g ? MAX(hearings, g->hearings) : hearings;
}

/* The call this report is a twin of, or NULL when it stands. */
static const char *twin_of(SkimStationTable *t, const SkimStation *st) {
  if (!twin_weak(st->call, call_reads(t, st->call, st->hearings)))
    return NULL;
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    for (GSList *l = val; l; l = l->next) {
      const SkimStation *e = l->data;
      if (fabs(e->freq_hz - st->freq_hz) <= SKIM_STATION_MERGE_HZ &&
          call_is_twin(st->call, key) &&
          twin_rival(key, call_reads(t, key, e->hearings))) {
        if (st_debug()) {
          g_printerr("station: TWIN %s dropped beside %s @ %.0f Hz (read %u, "
                     "heard %.0f s ago)\n", st->call, e->call, e->freq_hz,
                     call_reads(t, key, e->hearings),
                     (st->last_heard - e->last_heard) / 1e6);
        }
        return key;
      }
    }
  }
  g_hash_table_iter_init(&it, t->left);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const Left *g = val;
    if (st->last_heard - g->at <= SKIM_STATION_TWIN_MEMO_US &&
        fabs(g->hz - st->freq_hz) <= SKIM_STATION_MERGE_HZ &&
        !g_hash_table_lookup(t->by_call, key) &&
        call_is_twin(st->call, key) && twin_rival(key, g->hearings)) {
      if (st_debug()) {
        g_printerr("station: TWIN %s dropped beside %s @ %.0f Hz (read %u, "
                   "left, heard %.0f s ago)\n", st->call, (const char *)key,
                   g->hz, g->hearings, (st->last_heard - g->at) / 1e6);
      }
      return key;
    }
  }
  return NULL;
}

/* A record goes silent: remember it for the twins that come after. */
static void left_note(SkimStationTable *t, const SkimStation *e) {
  Left *g = g_hash_table_lookup(t->left, e->call);
  if (!g) {
    g = g_new0(Left, 1);
    g_hash_table_insert(t->left, g_strdup(e->call), g);
  }
  g->hz       = e->freq_hz;
  g->hearings = MAX(g->hearings, e->hearings);
  g->at       = e->last_heard;
}

/* A confident report of a DIFFERENT call on the same frequency evicts records
 * that have been silent past the takeover age — someone else settled there;
 * without this the old call stayed listed (and kept being re-reported by its
 * channel's extractor) for the whole 10-minute TTL (Richard, 2026-07-15).
 * A record whose call is a CLIP of the incoming one goes immediately — it
 * was never a station, just a torn decode of this very signal; and so does
 * a record that is the incoming call with text GLUED on, and one that is a
 * TWIN of it read once (gh#21). */
static void takeover_sweep(SkimStationTable *t, const SkimStation *st) {
  const guint st_reads = call_reads(t, st->call, st->hearings);
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    if (strcmp(key, st->call) == 0)
      continue;
    GSList *list = val, *l = list;
    gboolean changed = FALSE;
    while (l) {
      SkimStation *e = l->data;
      GSList *next = l->next;
      const gboolean clip =
          call_is_clip(e->call, st->call) &&
          !call_is_glue(e->call, e->hearings, st->call, st->hearings);
      const gboolean glue =
          call_is_glue(st->call, st->hearings, e->call, e->hearings);
      const gboolean twin =
          twin_weak(e->call, call_reads(t, e->call, e->hearings)) &&
          call_is_twin(e->call, st->call) && twin_rival(st->call, st_reads);
      if (fabs(e->freq_hz - st->freq_hz) <= SKIM_STATION_MERGE_HZ &&
          (st->last_heard - e->last_heard > SKIM_STATION_TAKEOVER_US ||
           clip || glue || twin)) {
        if (st_debug()) {
          g_printerr("station: EVICT %s @ %.0f Hz by %s @ %.0f Hz (%s, read "
                     "%u vs %u)\n", e->call, e->freq_hz, st->call, st->freq_hz,
                     clip ? "clip" : glue ? "glue" : twin ? "twin" : "silent",
                     e->hearings,
                     st->hearings);
        }
        if (t->gone_cb) { t->gone_cb(e, t->gone_user); }
        if (!clip && !glue && !twin) { left_note(t, e); }   /* it was a station */
        list = g_slist_remove(list, e);
        g_free(e);
        t->count--;
        changed = TRUE;
      }
      l = next;
    }
    if (changed) {
      if (!list) {
        g_hash_table_iter_remove(&it);
      } else {
        g_hash_table_iter_replace(&it, list);
      }
    }
  }
}

/* The reverse clip direction: a report of "M0K" while a fresh "M0KKB" sits on
 * the frequency is a torn decode of that station — fold it into the longer
 * record instead of creating a phantom. The same for glue: "OL1BIC" while a
 * fresh "OL1B" the dictionary knows sits there feeds OL1B. Returns the record
 * it fed, or NULL. */
static SkimStation *clip_fold(SkimStationTable *t, const SkimStation *st) {
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    /* either the report is a clip of the record, or the report is the
     * record's call with text glued on — the record is the station; which
     * of the two it is may turn on the record's readings (gh#34) */
    if (!call_is_clip(st->call, key) && !call_is_clip(key, st->call))
      continue;
    for (GSList *l = val; l; l = l->next) {
      SkimStation *e = l->data;
      const gboolean clip =
          call_is_clip(st->call, key) &&
          !call_is_glue(st->call, st->hearings, key, e->hearings);
      if (!clip && !call_is_glue(key, e->hearings, st->call, st->hearings))
        continue;
      if (fabs(e->freq_hz - st->freq_hz) <= SKIM_STATION_MERGE_HZ &&
          st->last_heard - e->last_heard <= SKIM_STATION_TAKEOVER_US) {
        if (st_debug()) {
          g_printerr("station: FOLD %s into %s @ %.0f Hz (%s, read %u vs %u)\n",
                     st->call, e->call, e->freq_hz, clip ? "clip" : "glue",
                     st->hearings, e->hearings);
        }
        e->last_heard = st->last_heard;
        e->reports++;
        return e;
      }
    }
  }
  return NULL;
}

const SkimStation *skim_station_table_report(SkimStationTable *t,
                                             const SkimStation *st) {
  SkimStation *folded = clip_fold(t, st);
  if (folded) { return folded; }
  if (twin_of(t, st))
    return NULL;                               /* a misread, not a station  */
  takeover_sweep(t, st);
  GSList *list = g_hash_table_lookup(t->by_call, st->call);
  for (GSList *l = list; l; l = l->next) {
    SkimStation *e = l->data;
    if (fabs(e->freq_hz - st->freq_hz) <= SKIM_STATION_SAMECALL_HZ) {
      /* Merge: the stronger report positions the station — as an EMA, not a
       * replacement: single estimates scatter ±10-20 Hz around the carrier,
       * averaging converges onto it (stations sit on round frequencies;
       * 14048.99 for a 14049.00 station bugged Richard, 2026-07-15). */
      if (st->snr_db >= e->snr_peak) {
        e->freq_hz += 0.3 * (st->freq_hz - e->freq_hz);
        e->snr_peak = st->snr_db;
        e->speed    = st->speed;
      }
      /* The dB SHOWN follows the reports as an EMA of their POWER, not as
       * their maximum (gh#27): the maximum of a minute of estimates is the
       * upper edge of their scatter and, on real air, the peak of the
       * station's QSB — CW Skimmer's figure is the signal strength over
       * ~45 s of history "assuming the Rayleigh fading model", whose
       * estimate is the mean power (a mean of dB values would sit 2.5 dB
       * under it on a fully fading signal). The positioning rule above
       * keeps comparing against the peak, so the record's frequency moves
       * exactly as before. */
      {
        const double ep = pow(10.0, e->snr_db / 10.0);
        const double rp = pow(10.0, st->snr_db / 10.0);
        e->snr_db = 10.0 * log10(MAX(ep + SKIM_STATION_SNR_GAIN * (rp - ep), 1e-6));
      }
      e->score      = MAX(e->score, st->score);
      e->hearings   = MAX(e->hearings, st->hearings);
      e->last_read  = MAX(e->last_read, st->last_read);
      if (st->addressed) { e->addressed = TRUE; }   /* answered — gh#31     */
      if (st->cq) { e->cq = TRUE; }            /* calling here — owns it     */
      if (e->addressed) { e->cq = FALSE; }     /* …unless it was answered    */
      e->last_heard = st->last_heard;
      e->reports++;
      return e;
    }
  }
  if (list) {
    /* Same call far away — the transmitter MOVED (S&P hop / QSY). A
     * confident report relocates the record; a one-off garble does not. */
    SkimStation *e = list->data;
    if (st->score >= SKIM_STATION_QSY_SCORE) {
      if (st_debug()) {
        g_printerr("station: QSY %s %.0f -> %.0f Hz\n", e->call, e->freq_hz,
                   st->freq_hz);
      }
      e->freq_hz    = st->freq_hz;
      e->snr_db     = st->snr_db;
      e->snr_peak   = st->snr_db;
      e->speed      = st->speed;
      e->score      = MAX(e->score, st->score);
      e->hearings   = MAX(e->hearings, st->hearings);
      e->last_read  = MAX(e->last_read, st->last_read);
      e->cq         = st->cq;                  /* new place, new context     */
      e->addressed  = st->addressed;
      e->last_heard = st->last_heard;
      e->reports++;
    }
    return e;
  }
  SkimStation *e = g_memdup2(st, sizeof(*st));
  e->reports  = 1;
  e->snr_peak = st->snr_db;
  if (list) {
    list = g_slist_append(list, e);           /* head unchanged, no reinsert */
  } else {
    g_hash_table_insert(t->by_call, g_strdup(st->call),
                        g_slist_append(NULL, e));
  }
  t->count++;
  return e;
}

guint skim_station_table_size(const SkimStationTable *t) {
  return t ? t->count : 0;
}

const SkimStation *skim_station_table_lookup(const SkimStationTable *t,
                                             const char *call) {
  GSList *list = g_hash_table_lookup(t->by_call, call);
  return list ? list->data : NULL;
}

void skim_station_table_foreach(SkimStationTable *t, SkimStationFunc f,
                                gpointer user_data) {
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    for (GSList *l = val; l; l = l->next) { f(l->data, user_data); }
  }
}

guint skim_station_table_prune(SkimStationTable *t, gint64 now_us,
                               gint64 max_age_us) {
  const gint64 cutoff = now_us - max_age_us;
  guint removed = 0;
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, t->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    GSList *list = val, *l = list;
    while (l) {
      SkimStation *e = l->data;
      GSList *next = l->next;
      if (e->last_heard < cutoff) {
        if (st_debug()) {
          g_printerr("station: PRUNE %s @ %.0f Hz (silent %.0f s, t=%.0f s)\n",
                     e->call, e->freq_hz, (now_us - e->last_heard) / 1e6,
                     now_us / 1e6);
        }
        if (t->gone_cb) { t->gone_cb(e, t->gone_user); }
        left_note(t, e);
        list = g_slist_remove(list, e);
        g_free(e);
        removed++;
        t->count--;
      }
      l = next;
    }
    if (!list) {
      g_hash_table_iter_remove(&it);
    } else {
      g_hash_table_iter_replace(&it, list);
    }
  }
  g_hash_table_iter_init(&it, t->left);        /* the memo ages out too      */
  while (g_hash_table_iter_next(&it, &key, &val)) {
    if (now_us - ((const Left *)val)->at > SKIM_STATION_TWIN_MEMO_US) {
      g_hash_table_iter_remove(&it);
    }
  }
  return removed;
}
