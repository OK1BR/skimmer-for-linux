/* callsign.c — callsign extraction + validation (M4, docs/SCOPE.md).
 *
 * Validation = a structural parse against the shapes real callsigns take,
 * with the ITU allocation encoded where it discriminates:
 *   V1  single-letter series (B F G I K M N R W) + area digit(s) + suffix
 *   V2  two-letter prefix (any but Q*) + area digit(s) + suffix
 *   V3  letter+digit country prefix (per-letter table: T2..T8 yes, T1 NO —
 *       exactly the class of CW decode garbage this must kill) + optional
 *       area digit(s) + suffix; the no-area form needs a ≥2-letter suffix
 *       (C6AGU yes, "5NN" no)
 *   V4  digit-first prefix 2..9 + letter (+letter: 3DA) + area + suffix
 * Suffix: 1–4 chars ending in a letter. Portable designators are split on
 * '/' and either side may be the call (OK1BR/P, F/OK1BR).
 *
 * Extraction: tokenised stream with CW context — a token after DE gets the
 * strongest marker, tokens shortly after CQ get a weaker one, repetitions
 * accumulate, and an optional known-call dictionary (MASTER.SCP) boosts.
 * Scores: 0.55 structural+allocation, +0.25 DE, +0.10 CQ, +0.20 repeated
 * (+0.05 at ≥3), +0.15 dictionary — capped at 1.0. The spot threshold 0.70
 * means a lone structurally-valid token is never spotted (the RBN rule).
 *
 * Engine-thread only (no locking), like the rest of the pipeline.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "callsign.h"

#include <string.h>

/* --- ITU allocation ---------------------------------------------------------
 * Single-letter series and the letter+digit country prefixes. The two-letter
 * space is allocated for every first letter except Q (reserved for Q-codes).
 * Maintainable data — extend here when a rare prefix shows up missing. */
static const char SINGLE_LETTER[] = "BFGIKMNRW";

static const char *ld_digits(char letter) {
  switch (letter) {
  case 'A': return "2456789";   /* A2 Botswana … A9 Bahrain                  */
  case 'C': return "2345689";   /* C2 Nauru … C9 Mozambique (C7 unassigned)  */
  case 'D': return "23456789";  /* D2 Angola … D7-9 Korea                    */
  case 'E': return "234567";    /* E2 Thailand … E7 Bosnia                   */
  case 'H': return "2346789";   /* H2 Cyprus … H8-9 Panama                   */
  case 'J': return "2345678";   /* J2 Djibouti … J8 St Vincent               */
  case 'L': return "23456789";  /* L2-L9 Argentina                           */
  case 'P': return "2345";      /* P2 PNG, P3 Cyprus, P4 Aruba, P5 DPRK      */
  case 'S': return "2579";      /* S2 Bangladesh, S5 Slovenia, S7, S9        */
  case 'T': return "235678";    /* T2 Tuvalu … T8 Palau — T1 does NOT exist  */
  case 'V': return "2345678";   /* V2 Antigua … V8 Brunei                    */
  case 'Y': return "23456789";  /* Y2-Y9 Germany                             */
  case 'Z': return "2368";      /* Z2 Zimbabwe, Z3 Macedonia, Z6, Z8         */
  default:  return "";
  }
}

static gboolean is_letter(char c) { return c >= 'A' && c <= 'Z'; }
static gboolean is_digit(char c)  { return c >= '0' && c <= '9'; }

/* area digit(s) [1-2] + suffix [1-4, ends in a letter], the common tail. */
static gboolean tail_ok(const char *s) {
  if (!is_digit(*s))
    return FALSE;
  s++;
  if (is_digit(*s)) { s++; }
  gsize n = strlen(s);
  if (n < 1 || n > 4 || !is_letter(s[n - 1]))
    return FALSE;
  for (gsize i = 0; i < n; i++) {
    if (!is_letter(s[i]) && !is_digit(s[i]))
      return FALSE;
  }
  return TRUE;
}

/* Structural + allocation check of a bare call (no '/' designators). */
static gboolean core_valid(const char *s) {
  gsize n = strlen(s);
  if (n < 3 || n > 8)
    return FALSE;
  for (gsize i = 0; i < n; i++) {
    if (!is_letter(s[i]) && !is_digit(s[i]))
      return FALSE;
  }
  if (s[0] == 'Q')
    return FALSE;                              /* Q-codes, never callsigns   */

  if (is_letter(s[0]) && is_letter(s[1])) {    /* V2: two-letter prefix      */
    return tail_ok(s + 2);
  }
  if (is_letter(s[0]) && is_digit(s[1])) {
    /* V1: single-letter series — F5IN, K1A, B1HQ.                          */
    if (strchr(SINGLE_LETTER, s[0]) && tail_ok(s + 1))
      return TRUE;
    /* V3: letter+digit country prefix — E73ABC, T77XX, C6AGU.              */
    if (strchr(ld_digits(s[0]), s[1])) {
      if (tail_ok(s + 2))
        return TRUE;                           /* with an area digit         */
      gsize m = strlen(s + 2);                 /* no area: ≥2 letters only   */
      if (m >= 2 && m <= 4) {
        for (gsize i = 2; i < n; i++) {
          if (!is_letter(s[i]))
            return FALSE;
        }
        return TRUE;
      }
    }
    return FALSE;
  }
  if (is_digit(s[0]) && s[0] >= '2' && is_letter(s[1])) {
    /* V4: digit-first — 9A1AA, 2E0ABC, 3DA0RS (optional second letter).    */
    if (tail_ok(s + 2))
      return TRUE;
    if (is_letter(s[2]) && tail_ok(s + 3))
      return TRUE;
    return FALSE;
  }
  return FALSE;
}

gboolean skim_callsign_is_valid(const char *s) {
  if (!s || !s[0])
    return FALSE;
  const char *slash = strchr(s, '/');
  if (!slash)
    return core_valid(s);
  if (strchr(slash + 1, '/'))
    return FALSE;                              /* at most one designator     */
  /* Either side may be the call: OK1BR/P, OK1BR/4, F/OK1BR. The other side
   * must be short (a designator or a bare DXCC prefix). */
  gsize left = (gsize)(slash - s), right = strlen(slash + 1);
  char core[16];
  if (left < sizeof(core) && right >= 1 && right <= 3) {
    memcpy(core, s, left);
    core[left] = '\0';
    if (core_valid(core))
      return TRUE;
  }
  if (right < sizeof(core) && left >= 1 && left <= 3) {
    if (core_valid(slash + 1))
      return TRUE;
  }
  return FALSE;
}

/* --- known-call dictionary ---------------------------------------------------- */

static GHashTable *s_dict;                     /* call → itself (owned)      */

gboolean skim_callsign_dict_load(const char *path, GError **error) {
  char *data = NULL;
  if (!g_file_get_contents(path, &data, NULL, error))
    return FALSE;
  if (s_dict) { g_hash_table_destroy(s_dict); }
  s_dict = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  char **lines = g_strsplit(data, "\n", -1);
  for (char **l = lines; *l; l++) {
    char *call = g_strstrip(*l);
    if (!call[0] || call[0] == '#')
      continue;
    char *up = g_ascii_strup(call, -1);
    g_hash_table_add(s_dict, up);
  }
  g_strfreev(lines);
  g_free(data);
  return TRUE;
}

guint skim_callsign_dict_size(void) {
  return s_dict ? g_hash_table_size(s_dict) : 0;
}

static gboolean dict_has(const char *call) {
  return s_dict && g_hash_table_contains(s_dict, call);
}

/* --- stateful extractor -------------------------------------------------------- */

/* Enough slots that a rare clean copy of a QSB-mangled call SURVIVES the
 * flood of its own mutations until the next clean copy arrives — with 12,
 * 9A170NT's single good decode kept getting evicted before its second
 * hearing could lift it over the spot threshold (live, 2026-07-15). */
#define MAX_CAND    24
#define CALL_MAX    16
#define CQ_WINDOW   3                          /* tokens after CQ that count */

/* A candidate goes STALE this many processed tokens after its last hit —
 * the frequency changed hands and the new occupant must win immediately;
 * an ever-growing count kept the OLD call on top for the rest of the run
 * (live-caught 2026-07-15). The clock ticks on channel traffic, not wall
 * time, so a station pausing between overs never ages out. Short on
 * purpose: while a stale best keeps being re-reported, its station record
 * keeps a fresh last_heard and the takeover eviction never fires — the
 * tuned label flickered between the old and the new occupant (EA2BTN vs
 * IT9IQN, live-caught 2026-07-15). A runner repeats their call every CQ
 * cycle (~5-10 tokens), so 12 keeps real stations alive. */
#define CAND_STALE_TOKENS 12

typedef struct {
  char     call[CALL_MAX];
  guint    count;
  guint    last_tok;                           /* x->tok_n at the last hit   */
  gboolean de_marked;
  gboolean cq_context;
} Cand;

struct _SkimCallsignExtractor {
  GString *tok;                                /* partial token across feeds */
  gint     de_pending;                         /* DE marker lives ≤ 2 tokens */
  gint     cq_recent;                          /* tokens since CQ (≤ window) */
  guint    tok_n;                              /* tokens processed (age clock)*/
  char     prev_tok[CALL_MAX];                 /* previous token (join hyp.) */
  gboolean prev_valid;                         /* it was a valid call itself */
  gboolean prev_de;                            /* DE applied to it           */
  gboolean prev_cq;                            /* CQ window applied to it    */
  Cand     cand[MAX_CAND];
  guint    ncand;
};

SkimCallsignExtractor *skim_callsign_extractor_new(void) {
  SkimCallsignExtractor *x = g_new0(SkimCallsignExtractor, 1);
  x->tok = g_string_new(NULL);
  x->cq_recent = CQ_WINDOW + 1;
  return x;
}

void skim_callsign_extractor_free(SkimCallsignExtractor *x) {
  if (!x)
    return;
  g_string_free(x->tok, TRUE);
  g_free(x);
}

void skim_callsign_extractor_reset(SkimCallsignExtractor *x) {
  g_string_set_size(x->tok, 0);
  x->de_pending = 0;
  x->cq_recent  = CQ_WINDOW + 1;
  x->tok_n      = 0;
  x->ncand      = 0;
  x->prev_tok[0] = '\0';
  x->prev_valid = x->prev_de = x->prev_cq = FALSE;
}

static double cand_score(const SkimCallsignExtractor *x, const Cand *c) {
  if (x->tok_n - c->last_tok > CAND_STALE_TOKENS)
    return 0.0;                                /* not heard lately — dead    */
  double s = 0.55;                             /* structural + allocation    */
  if (c->de_marked)  { s += 0.25; }
  if (c->cq_context) { s += 0.10; }
  if (c->count >= 2) { s += 0.20; }
  if (c->count >= 3) { s += 0.05; }
  if (dict_has(c->call)) { s += 0.15; }
  return MIN(s, 1.0);
}

static void cand_add(SkimCallsignExtractor *x, const char *call,
                     gboolean de_marked, gboolean cq_context) {
  Cand *c = NULL;
  for (guint i = 0; i < x->ncand; i++) {
    if (strcmp(x->cand[i].call, call) == 0) { c = &x->cand[i]; break; }
  }
  if (!c) {
    if (x->ncand < MAX_CAND) {
      c = &x->cand[x->ncand++];
    } else {
      /* Evict the weakest; among equally weak (stale ones all score 0),
       * the LEAST-REPEATED goes first — a stale twice-heard call is worth
       * keeping over a stale one-off mutation. */
      c = &x->cand[0];
      for (guint i = 1; i < MAX_CAND; i++) {
        double si = cand_score(x, &x->cand[i]), sc = cand_score(x, c);
        if (si < sc || (si == sc && x->cand[i].count < c->count)) {
          c = &x->cand[i];
        }
      }
    }
    memset(c, 0, sizeof(*c));
    g_strlcpy(c->call, call, sizeof(c->call));
  }
  c->count++;
  c->last_tok = x->tok_n;
  if (de_marked)  { c->de_marked  = TRUE; }
  if (cq_context) { c->cq_context = TRUE; }
}

/* Tokens that never take part in a join: prosigns and stock CW abbreviations
 * glue onto a neighbouring call into a VALID-looking phantom ("OK1BR K" →
 * "OK1BRK", "R EA3I" → "REA3I" — R is a legal prefix). */
static gboolean join_stop_word(const char *s) {
  static const char *STOP[] = {
    "K", "KN", "BK", "SK", "AR", "AS", "TU", "R", "E", "T", "EE",
    "PSE", "QRZ", "QRL", "TEST", "NR", "UR", "5NN", "599", "73", "88",
    /* stock QSO vocabulary — "UB7M TNX" glued into the valid-looking
     * phantom UB7MTNX (live-caught 2026-07-15) */
    "TNX", "FER", "RPRT", "QSO", "QTH", "AGN", "HW", "GA", "GE", "GM",
    "DR", "OM", "ES", "VY", "ABT", "HPE", "SRI", "RIG", "ANT", "WX", "OP",
  };
  for (guint i = 0; i < G_N_ELEMENTS(STOP); i++) {
    if (strcmp(s, STOP[i]) == 0) { return TRUE; }
  }
  return FALSE;
}

static void take_token(SkimCallsignExtractor *x, const char *tok) {
  if (strcmp(tok, "\xC2\xB7") == 0) {
    /* The decoder's over-break mark ("·") is metadata, not received text —
     * fully transparent here: it must not eat a DE marker and must not
     * break prev_tok, or a QSB dip inside a call kills the join hypothesis
     * ("LZ67 · PP" never reassembled into LZ67PP; live-caught 2026-07-15). */
    return;
  }
  x->tok_n++;
  if (strcmp(tok, "DE") == 0) {
    x->de_pending = 2;
    x->prev_tok[0] = '\0';                     /* a call never straddles DE  */
    return;
  }
  if (strcmp(tok, "CQ") == 0 || strcmp(tok, "TEST") == 0 ||
      strcmp(tok, "QRZ") == 0 || strcmp(tok, "CWT") == 0 ||
      strcmp(tok, "TU") == 0) {
    /* A calling marker: CQ/QRZ, TEST and CWT (CWops) — both contest-style
     * leading ("TEST SD1A") and trailing ("SD1A TEST" / "F5IN CWT") —
     * bless the call that was JUST sent too, then open the window for the
     * one that follows. TU is LEADING-only: "TU M0NGN" is the runner
     * closing a QSO and re-announcing (Richard, 2026-07-15), but in
     * "<call> TU" the thanks may go to the OTHER station. */
    x->cq_recent = 0;
    if (strcmp(tok, "TU") != 0 && x->prev_tok[0]) {
      for (guint i = 0; i < x->ncand; i++) {
        if (strcmp(x->cand[i].call, x->prev_tok) == 0) {
          x->cand[i].cq_context = TRUE;
          break;
        }
      }
    }
    x->prev_tok[0] = '\0';
    return;
  }
  if (x->cq_recent <= CQ_WINDOW) { x->cq_recent++; }

  const gboolean valid = skim_callsign_is_valid(tok) && strlen(tok) < CALL_MAX;
  const gboolean de_now = x->de_pending > 0;
  const gboolean cq_now = x->cq_recent <= CQ_WINDOW;

  /* Join hypothesis — the sloppy-fist fix: an operator who stretches an
   * inter-letter gap splits their call across two tokens ("EA3I XQ" for
   * EA3IXQ, live-caught 2026-07-15). Try gluing the previous token on:
   * accept when the JOIN is a structurally valid call and it explains
   * something the parts do not (a dictionary hit, or a fragment that is no
   * call by itself). Repetition then outscores the torn variants, and the
   * station table's clip fold retires them. */
  if (x->prev_tok[0] && !join_stop_word(x->prev_tok) && !join_stop_word(tok)) {
    char join[CALL_MAX];
    if (strlen(x->prev_tok) + strlen(tok) < CALL_MAX) {
      g_snprintf(join, sizeof(join), "%s%s", x->prev_tok, tok);
      if (skim_callsign_is_valid(join) &&
          (dict_has(join) || !valid || !x->prev_valid)) {
        cand_add(x, join, x->prev_de || de_now, x->prev_cq || cq_now);
      }
    }
  }

  g_strlcpy(x->prev_tok, tok, sizeof(x->prev_tok));
  x->prev_valid = valid;
  x->prev_de    = de_now;
  x->prev_cq    = cq_now;

  if (!valid) {
    /* A garbled token between DE and the call must not eat the marker
     * ("DE R OK1BR") — but the marker does not live forever either. */
    if (x->de_pending > 0) { x->de_pending--; }
    return;
  }

  cand_add(x, tok, de_now, cq_now);
  x->de_pending = 0;
}

void skim_callsign_extractor_feed(SkimCallsignExtractor *x, const char *text) {
  for (const char *p = text; *p; p++) {
    const char c = *p;
    if (c == ' ' || c == '\n' || c == '\t') {
      if (x->tok->len) {
        if (x->tok->len < CALL_MAX) { take_token(x, x->tok->str); }
        g_string_set_size(x->tok, 0);
      }
      continue;
    }
    g_string_append_c(x->tok, g_ascii_toupper(c));
    if (x->tok->len > CALL_MAX) { g_string_set_size(x->tok, 0); }
  }
}

double skim_callsign_extractor_best_ex(SkimCallsignExtractor *x,
                                       char *out, gsize out_size,
                                       gboolean *cq_context) {
  double best = 0.0;
  const Cand *bc = NULL;
  for (guint i = 0; i < x->ncand; i++) {
    double s = cand_score(x, &x->cand[i]);
    /* Tie goes to the LONGER call: a torn fragment ("EA3I") and its join
     * ("EA3IXQ") both max the score once repeated — the join is the call. */
    if (s > best ||
        (s == best && bc && strlen(x->cand[i].call) > strlen(bc->call))) {
      best = s;
      bc = &x->cand[i];
    }
  }
  if (cq_context) { *cq_context = bc ? bc->cq_context : FALSE; }
  if (!bc || best < SKIM_CALLSIGN_SPOT_THRESHOLD) {
    if (out && out_size) { out[0] = '\0'; }
    return 0.0;
  }
  if (out && out_size) { g_strlcpy(out, bc->call, out_size); }
  return best;
}

double skim_callsign_extractor_best(SkimCallsignExtractor *x,
                                    char *out, gsize out_size) {
  return skim_callsign_extractor_best_ex(x, out, out_size, NULL);
}

double skim_callsign_extract(const char *text, char *out, gsize out_size) {
  SkimCallsignExtractor *x = skim_callsign_extractor_new();
  skim_callsign_extractor_feed(x, text);
  skim_callsign_extractor_feed(x, " ");        /* flush the last token       */
  double s = skim_callsign_extractor_best(x, out, out_size);
  skim_callsign_extractor_free(x);
  return s;
}
