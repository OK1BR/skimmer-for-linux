/* callsign.c — callsign extraction + validation (M4).
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
 * '/' and either side may be the call (OK1BR/P, F/OK1BR); the short side is
 * letters and digits only, like the call itself.
 *
 * Extraction: tokenised stream with CW context — a token after DE gets the
 * strongest marker, tokens shortly after CQ get a weaker one, repetitions
 * accumulate, and an optional known-call dictionary (MASTER.SCP) boosts.
 * A degenerate fist can close the gaps AROUND the markers ("CQCQCQ
 * DEEA1EYL") or every gap of the over ("CQCQCQDEEA5JQFEA5JQFK"), or stretch
 * the gaps INSIDE the call to a word gap's length ("D E G 0WD T G 0WD T");
 * strictly-shaped fallbacks recover those tokens, and all of them fire only
 * where the normal path fails — cleanly keyed streams never enter them.
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

/* The short side of a '/' call: a designator (P, MM, QRP, 4) or a bare DXCC
 * prefix (F, EA8) — 1–3 bytes, letters and digits only. */
static gboolean side_ok(const char *s, gsize n) {
  if (n < 1 || n > 3)
    return FALSE;
  for (gsize i = 0; i < n; i++) {
    if (!is_letter(s[i]) && !is_digit(s[i]))
      return FALSE;
  }
  return TRUE;
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
   * must be short (a designator or a bare DXCC prefix) and, like the call,
   * letters and digits only: the over-break mark the CW decoders write
   * (U+00B7, two bytes) glues onto a designator as readily as onto any
   * token, and a length check alone let PA3BUL/D· out as a valid call. */
  gsize left = (gsize)(slash - s), right = strlen(slash + 1);
  char core[16];
  if (left < sizeof(core) && side_ok(slash + 1, right)) {
    memcpy(core, s, left);
    core[left] = '\0';
    if (core_valid(core))
      return TRUE;
  }
  if (right < sizeof(core) && side_ok(s, left)) {
    if (core_valid(slash + 1))
      return TRUE;
  }
  return FALSE;
}

/* --- known-call dictionary ---------------------------------------------------- */

static GRWLock     s_dict_lock;                /* guards the two below       */
static GHashTable *s_dict;                     /* call → itself (owned)      */
static char        s_dict_release[32];         /* "2026.09.18"; "" = none    */

/* One stripped dictionary line → TRUE when it carries a call. Blank lines,
 * '#' comments and "!!" directives (Super Check Partial opens its files with
 * "!!Order,1,1") carry none, and neither does anything holding a character
 * no callsign has — whatever header the format grows next stays out too.
 * Deliberately NOT skim_callsign_is_valid(): the list is the authority on
 * who is on the air, the validator only knows the prefixes it was taught. */
static gboolean dict_line_is_call(const char *s) {
  if (!s[0] || s[0] == '#' || (s[0] == '!' && s[1] == '!'))
    return FALSE;
  gsize n = 0;
  gboolean digit = FALSE, alpha = FALSE;
  for (const char *p = s; *p; p++, n++) {
    if (g_ascii_isdigit(*p)) { digit = TRUE; }
    else if (g_ascii_isalpha(*p)) { alpha = TRUE; }
    else if (*p != '/') { return FALSE; }
  }
  return digit && alpha && n >= 3 && n <= 15;
}

/* The one parser: the loader and skim_callsign_dict_inspect() walk a file
 * the same way, so what a check counted is what a load would take. Runs on
 * a length, not on a terminator — the updater hands it bytes off the network,
 * and those may be anything (an HTML error page, a NUL-ridden binary). A line
 * longer than the stack copy, or one hiding a NUL, is junk by definition. */
static void dict_parse(const char *data, gsize len, GHashTable *into,
                       SkimCallsignDictInfo *info) {
  SkimCallsignDictInfo z = { 0 };
  const char *p = data, *end = data + len;
  while (p < end) {
    const char *nl = memchr(p, '\n', (gsize)(end - p));
    gsize n = nl ? (gsize)(nl - p) : (gsize)(end - p);
    char line[64];
    gboolean fits = n < sizeof(line) && !memchr(p, '\0', n);
    if (fits) {
      memcpy(line, p, n);
      line[n] = '\0';
      g_strstrip(line);
    }
    p += n + (nl ? 1 : 0);
    if (!fits) { z.junk++; continue; }
    if (!dict_line_is_call(line)) {
      if (line[0] == '#') {                    /* "# Release 2026.09.18"     */
        const char *r = line + 1;
        while (*r == ' ') { r++; }
        if (!z.release[0] && g_ascii_strncasecmp(r, "Release ", 8) == 0) {
          g_strlcpy(z.release, r + 8, sizeof(z.release));
          g_strstrip(z.release);
        }
      } else if (line[0] && !(line[0] == '!' && line[1] == '!')) {
        z.junk++;
      }
      continue;
    }
    for (char *c = line; *c; c++) { *c = g_ascii_toupper(*c); }
    z.calls++;
    if (skim_callsign_is_valid(line)) { z.valid++; }
    if (into) { g_hash_table_add(into, g_strdup(line)); }
  }
  if (info) { *info = z; }
}

void skim_callsign_dict_inspect(const char *data, gsize len,
                                SkimCallsignDictInfo *info) {
  dict_parse(data, len, NULL, info);
}

/* A reload may come at any time (the app swaps a freshly downloaded file in
 * while the engine thread scores tokens and the GTK thread underlines them):
 * the new table is built with no lock held, the swap is two pointer moves
 * under the writer lock, and the old table dies outside it. Readers pay one
 * uncontended reader lock per lookup. */
gboolean skim_callsign_dict_load(const char *path, GError **error) {
  char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &data, &len, error))
    return FALSE;
  GHashTable *fresh = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            NULL);
  SkimCallsignDictInfo info;
  dict_parse(data, len, fresh, &info);
  g_free(data);
  g_rw_lock_writer_lock(&s_dict_lock);
  GHashTable *old = s_dict;
  s_dict = fresh;
  g_strlcpy(s_dict_release, info.release, sizeof(s_dict_release));
  g_rw_lock_writer_unlock(&s_dict_lock);
  if (old) { g_hash_table_destroy(old); }
  return TRUE;
}

guint skim_callsign_dict_size(void) {
  g_rw_lock_reader_lock(&s_dict_lock);
  guint n = s_dict ? g_hash_table_size(s_dict) : 0;
  g_rw_lock_reader_unlock(&s_dict_lock);
  return n;
}

char *skim_callsign_dict_release(void) {
  g_rw_lock_reader_lock(&s_dict_lock);
  char *r = s_dict_release[0] ? g_strdup(s_dict_release) : NULL;
  g_rw_lock_reader_unlock(&s_dict_lock);
  return r;
}

static gboolean dict_has(const char *call) {
  g_rw_lock_reader_lock(&s_dict_lock);
  gboolean has = s_dict && g_hash_table_contains(s_dict, call);
  g_rw_lock_reader_unlock(&s_dict_lock);
  return has;
}

gboolean skim_callsign_dict_has(const char *call) {
  if (!call || !call[0] || strlen(call) >= 24)
    return FALSE;
  char up[24];
  g_strlcpy(up, call, sizeof(up));
  for (char *p = up; *p; p++) { *p = g_ascii_toupper(*p); }
  return dict_has(up);
}

/* --- stateful extractor -------------------------------------------------------- */

/* Enough slots that a rare clean copy of a QSB-mangled call SURVIVES the
 * flood of its own mutations until the next clean copy arrives — with 12,
 * 9A170NT's single good decode kept getting evicted before its second
 * hearing could lift it over the spot threshold (live, 2026-07-15). */
#define MAX_CAND    24
#define CALL_MAX    16
#define OVER_MAX    48                         /* a whole over in one token  */
#define CQ_WINDOW   3                          /* tokens after CQ that count */
#define RUN_MAX     12                         /* fragments between two edges */
#define RUN_PARTS   6                          /* a torn call: ≤ 6 of them   */

/* A candidate goes STALE this many processed tokens after its last hit —
 * the frequency changed hands and the new occupant must win immediately;
 * an ever-growing count kept the OLD call on top for the rest of the run
 * (live-caught 2026-07-15). The clock ticks on channel traffic, not wall
 * time, so a station pausing between overs never ages out. Short on
 * purpose: while a stale best keeps being re-reported, its station record
 * keeps a fresh last_heard and the takeover eviction never fires — the
 * tuned label flickered between the old and the new occupant (EA2BTN vs
 * IT9IQN, live-caught 2026-07-15). A runner repeats their call every CQ
 * cycle (~5-10 tokens), so 12 keeps real stations alive. The one thing
 * this clock cannot see — the candidate a quiet channel's noise brings
 * back an hour after the station left, hearings and all — is left to the
 * caller's clock stamped on every copy (last_us, gh#26): the RBN feed
 * refuses a report whose last copy is older than its window. */
#define CAND_STALE_TOKENS 12

typedef struct {
  char     call[CALL_MAX];
  guint    count;
  guint    last_tok;                           /* x->tok_n at the last hit   */
  gint64   last_us;                            /* x->now_us at the last hit  */
  guint    parts;                              /* fewest tokens it came in   */
  gboolean de_marked;
  gboolean cq_context;
  guint    ds_seq;                             /* the despaced stretch (gh#33) */
  guint    ds_cnt;                             /* copies counted in it       */
} Cand;

struct _SkimCallsignExtractor {
  GString *tok;                                /* partial token across feeds */
  gint     de_pending;                         /* DE marker lives ≤ 2 tokens */
  gint     cq_recent;                          /* tokens since CQ (≤ window) */
  guint    cq_pairs;                           /* adjacent "C","Q" pairs seen */
  gboolean cq_half;                            /* last token was a lone "C"  */
  guint    tok_n;                              /* tokens processed (age clock)*/
  gint64   now_us;                             /* caller's clock (set_now)   */
  char     prev_tok[CALL_MAX];                 /* previous token (join hyp.) */
  gboolean prev_valid;                         /* it was a valid call itself */
  gboolean prev_de;                            /* DE applied to it           */
  gboolean prev_cq;                            /* CQ window applied to it    */
  gboolean prev_gap;                           /* an over break came after it */
  GString *run;                                /* fragments since the last   */
  guint    run_n;                              /* edge, glued (torn call)    */
  gboolean run_de, run_cq;
  char     run_hit[CALL_MAX];                  /* its join counted on arrival */
  char     run_lead;                           /* lone E/T/K/R right before  */
  gboolean run_spelled;                        /* every piece one character  */
  GString *ds_text;                            /* the stretch so far, spaces */
  GString *ds_bound;                           /* dropped (gh#33); '1' where */
  guint    ds_seq;                             /* a token began; its number  */
  gboolean ds_frag;                            /* it holds an unread fragment */
  gint64   ds_last_us;                         /* clock of its last token    */
  Cand     cand[MAX_CAND];
  guint    ncand;
};

SkimCallsignExtractor *skim_callsign_extractor_new(void) {
  SkimCallsignExtractor *x = g_new0(SkimCallsignExtractor, 1);
  x->tok = g_string_new(NULL);
  x->run = g_string_new(NULL);
  x->ds_text  = g_string_new(NULL);
  x->ds_bound = g_string_new(NULL);
  x->cq_recent = CQ_WINDOW + 1;
  return x;
}

void skim_callsign_extractor_free(SkimCallsignExtractor *x) {
  if (!x)
    return;
  g_string_free(x->tok, TRUE);
  g_string_free(x->run, TRUE);
  g_string_free(x->ds_text, TRUE);
  g_string_free(x->ds_bound, TRUE);
  g_free(x);
}

static void run_clear(SkimCallsignExtractor *x) {
  g_string_set_size(x->run, 0);
  x->run_n  = 0;
  x->run_de = x->run_cq = FALSE;
  x->run_hit[0] = '\0';
  x->run_lead = '\0';
  x->run_spelled = TRUE;
}

void skim_callsign_extractor_reset(SkimCallsignExtractor *x) {
  g_string_set_size(x->tok, 0);
  x->de_pending = 0;
  x->cq_recent  = CQ_WINDOW + 1;
  x->cq_pairs   = 0;
  x->cq_half    = FALSE;
  x->tok_n      = 0;
  x->ncand      = 0;
  x->prev_tok[0] = '\0';
  x->prev_valid = x->prev_de = x->prev_cq = x->prev_gap = FALSE;
  run_clear(x);
  g_string_set_size(x->ds_text, 0);
  g_string_set_size(x->ds_bound, 0);
  x->ds_seq++;
  x->ds_frag = FALSE;
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

static void cand_add(SkimCallsignExtractor *x, const char *call, guint parts,
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
    c->parts = parts;
  }
  c->parts = MIN(c->parts, parts);
  c->count++;
  c->last_tok = x->tok_n;
  c->last_us  = x->now_us;
  if (c->ds_seq != x->ds_seq) {                /* a copy in the stretch of   */
    c->ds_seq = x->ds_seq;                     /* text despace_scan() reads  */
    c->ds_cnt = 0;                             /* (gh#33)                    */
  }
  c->ds_cnt++;
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

/* "CQCQCQ" — a fist whose inter-word gaps collapse sends the whole CQ chain
 * as ONE token (EA1EYL, live-caught 2026-07-16: word ≈ letter gaps at ~5-6
 * dits, the fist model rightly refuses such a fit — so the fix belongs
 * here, in the lexicon). Strict shape: nothing but "CQ" repeated, ≥2 times;
 * a cleanly keyed stream cannot produce it, so the normal CQ path is
 * untouched. */
static gboolean cq_run_token(const char *s) {
  gsize n = strlen(s);
  if (n < 4 || (n % 2) != 0)
    return FALSE;
  for (gsize i = 0; i < n; i += 2) {
    if (s[i] != 'C' || s[i + 1] != 'Q')
      return FALSE;
  }
  return TRUE;
}

/* The torn call — gh#3. An operator who leaves a word gap after EVERY group
 * sends "UA 6 H NU" (live 2026-09-11, 14039: four tokens in 16 of 28 overs,
 * three in the rest, six when it got worse). Gluing neighbours token by
 * token cannot work there: "UA6 H" validates one token before "UA6 H NU"
 * does, the pipeline reports the best candidate after every token, and the
 * head went to the panadapter as the call. So the fragments between two
 * EDGES — DE, a calling marker, a stop word, a token that is a call by
 * itself, the decoder's over break — are glued as a WHOLE: all of the run
 * or nothing, or the same call keyed two or three times over. No head, no
 * tail, no call plus the first letter of its own repeat ("SP6OS SP"). */

/* A serial or a report ("79", "001") and a lone "/" are never a piece of a
 * call — a single digit may be ("UA 6 H NU"). Without this the exchange in
 * front of a torn call joined its run and killed it ("5NN 7 79 OL7 ABS",
 * 80 m contest fixture). */
static gboolean run_edge_token(const char *tok) {
  if (strcmp(tok, "/") == 0)
    return TRUE;
  gsize n = 0;
  for (; tok[n]; n++) {
    if (tok[n] < '0' || tok[n] > '9')
      return FALSE;
  }
  return n >= 2;
}

static void run_take(SkimCallsignExtractor *x, const char *tok,
                     gboolean de, gboolean cq) {
  if (x->run_n > RUN_MAX)
    return;                                    /* babble — wait for an edge  */
  x->run_n++;
  g_string_append(x->run, tok);
  x->ds_frag = TRUE;                           /* unread — gh#33 may look    */
  if (tok[1] != '\0') { x->run_spelled = FALSE; }
  if (de) { x->run_de = TRUE; }
  if (cq) { x->run_cq = TRUE; }
}

/* E, T, K and R are stop words because noise and procedure are full of
 * them — and they are letters of calls. Next to a run, with no break in
 * between, a lone one may be either: "E A2 D DC" is EA2DDC keyed letter by
 * letter, not A2DDC after a stray dit; "F 4 I K C" is not F4I and a K. When
 * the call reads with that letter glued on as well, the run is ambiguous
 * and counts as nothing — what the extractor made of such text before.
 * After the run this applies to spelled-out runs only: "UA 6 H NU K" ends
 * in a real K, and UA6HNUK would validate too. */
static gboolean run_ambiguous(const SkimCallsignExtractor *x, const char *call,
                              char trail) {
  char with[CALL_MAX + 2];
  if (x->run_lead) {
    g_snprintf(with, sizeof(with), "%c%s", x->run_lead, call);
    if (skim_callsign_is_valid(with)) { return TRUE; }
  }
  if (trail && x->run_spelled) {
    g_snprintf(with, sizeof(with), "%s%c", call, trail);
    if (skim_callsign_is_valid(with)) { return TRUE; }
  }
  return FALSE;
}

static char lone_stop_letter(const char *tok) {
  return (tok[1] == '\0' && strchr("ETKR", tok[0])) ? tok[0] : '\0';
}

/* TRUE when the run read as a call (and counted). `trail` = the lone stop
 * letter that ended it, or 0. */
static gboolean run_close(SkimCallsignExtractor *x, char trail) {
  const gsize len = x->run->len;
  gboolean hit = FALSE;
  if (x->run_n >= 2 && x->run_n <= RUN_MAX) {
    for (guint r = 3; r >= 1 && !hit; r--) {
      if (len % r != 0 || x->run_n < 2 * r || x->run_n > RUN_PARTS * r)
        continue;
      const gsize l = len / r;
      if (l >= CALL_MAX)
        continue;
      char call[CALL_MAX];
      memcpy(call, x->run->str, l);
      call[l] = '\0';
      gboolean same = TRUE;
      for (guint i = 1; i < r && same; i++) {
        same = memcmp(x->run->str + i * l, call, l) == 0;
      }
      if (!same || !skim_callsign_is_valid(call))
        continue;
      if (run_ambiguous(x, call, trail))
        break;
      hit = TRUE;
      /* the two-token join the dictionary knows was counted on arrival */
      if (r == 1 && strcmp(call, x->run_hit) == 0)
        break;
      for (guint i = 0; i < r; i++) {
        cand_add(x, call, (x->run_n + r - 1) / r, x->run_de, x->run_cq);
      }
    }
  }
  return hit;
}

static void despace_note(SkimCallsignExtractor *x, const char *tok, gsize n);
static void despace_edge(SkimCallsignExtractor *x);

static void take_token(SkimCallsignExtractor *x, const char *tok) {
  if (strcmp(tok, "\xC2\xB7") == 0) {
    /* The decoder's over-break mark ("·") is metadata, not received text —
     * transparent for the two-token join: it must not eat a DE marker and
     * must not break prev_tok, or a QSB dip inside a call kills the join
     * hypothesis ("LZ67 · PP" never reassembled into LZ67PP; live-caught
     * 2026-07-15). A RUN of fragments ends here: pieces from two overs are
     * not one call ("M7· O ·" read M7O, which the dictionary knows — a new
     * station from one hearing of two scraps, SAC fixture). */
    run_close(x, 0);
    run_clear(x);
    despace_edge(x);
    x->prev_gap = x->prev_tok[0] != '\0';
    return;
  }
  x->tok_n++;
  {                                            /* gh#33: the stretch sees   */
    const gsize bl = strlen(tok);              /* every token's bare text   */
    const gboolean m = bl > 2 && strcmp(tok + bl - 2, "\xC2\xB7") == 0;
    despace_note(x, tok, m ? bl - 2 : bl);
  }
  /* Letter-spaced CQ chain — the TORN twin of the fused "CQCQ" run: the
   * same degenerate fist that glues gaps shut also stretches them, and the
   * CQ chain arrives as single-letter tokens "C Q C Q" (EA1EYL again,
   * live-caught 2026-07-16 — one operator, both shapes in one evening).
   * Two adjacent pairs in strict alternation make a CQ marker; a machine
   * always keys CQ into ONE token, so clean traffic never comes here. */
  if (tok[0] == 'C' && tok[1] == '\0') {
    if (x->cq_half) { x->cq_pairs = 0; }       /* "C C" broke the chain      */
    x->cq_half = TRUE;
  } else if (x->cq_half && tok[0] == 'Q' && tok[1] == '\0') {
    x->cq_half = FALSE;
    if (++x->cq_pairs >= 2) { x->cq_recent = 0; }
  } else {
    x->cq_half  = FALSE;
    x->cq_pairs = 0;
  }
  if (strcmp(tok, "DE") == 0) {
    run_close(x, 0);
    run_clear(x);
    despace_edge(x);
    x->de_pending = 2;
    x->prev_tok[0] = '\0';                     /* a call never straddles DE  */
    return;
  }
  if (strcmp(tok, "CQ") == 0 || strcmp(tok, "TEST") == 0 ||
      strcmp(tok, "QRZ") == 0 || strcmp(tok, "CWT") == 0 ||
      strcmp(tok, "TU") == 0 || cq_run_token(tok)) {
    /* A calling marker: CQ/QRZ, TEST and CWT (CWops) — both contest-style
     * leading ("TEST SD1A") and trailing ("SD1A TEST" / "F5IN CWT") —
     * bless the call that was JUST sent too, then open the window for the
     * one that follows. TU is LEADING-only: "TU M0NGN" is the runner
     * closing a QSO and re-announcing (Richard, 2026-07-15), but in
     * "<call> TU" the thanks may go to the OTHER station. */
    run_close(x, 0);
    run_clear(x);
    despace_edge(x);
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
   * station table's clip fold retires them. Two FRAGMENTS are a different
   * matter — they may be the head of a longer torn call, so that join
   * belongs to the run (above), unless the dictionary knows it or an over
   * break sits between the two (no run crosses one). */
  const gboolean stop = join_stop_word(tok);
  if (x->prev_tok[0] && !join_stop_word(x->prev_tok) && !stop) {
    char join[CALL_MAX];
    if (strlen(x->prev_tok) + strlen(tok) < CALL_MAX) {
      g_snprintf(join, sizeof(join), "%s%s", x->prev_tok, tok);
      const gboolean known = dict_has(join);
      if (skim_callsign_is_valid(join) &&
          (known || valid != x->prev_valid || x->prev_gap)) {
        cand_add(x, join, 2, x->prev_de || de_now, x->prev_cq || cq_now);
        if (!valid && !x->prev_valid) {
          g_strlcpy(x->run_hit, join, sizeof(x->run_hit));
        }
      }
    }
  }
  /* The over-break mark arrives GLUED to a character the decoder committed
   * together with it ("NU·", "R·"): for the run that is the bare token,
   * then the mark. */
  const gboolean de_glued = !valid && strlen(tok) >= 5 && tok[0] == 'D' &&
                            tok[1] == 'E' && skim_callsign_is_valid(tok + 2);
  char  bare[CALL_MAX];
  gsize bl = strlen(tok);
  const gboolean marked = bl > 2 && strcmp(tok + bl - 2, "\xC2\xB7") == 0;
  g_strlcpy(bare, tok, sizeof(bare));
  if (marked) { bare[bl - 2] = '\0'; }
  if (stop || valid || de_glued || run_edge_token(bare) ||
      (marked && (join_stop_word(bare) || skim_callsign_is_valid(bare)))) {
    run_close(x, marked ? '\0' : lone_stop_letter(tok));   /* an edge    */
    run_clear(x);
    despace_edge(x);
    if (!marked) { x->run_lead = lone_stop_letter(tok); }
  } else {
    run_take(x, bare, de_now, cq_now);
    if (marked) {
      run_close(x, 0);
      run_clear(x);
      despace_edge(x);
    }
  }

  /* DE-strip — the other half of the degenerate-fist fix: the gap between
   * DE and the call collapses too and the marker arrives GLUED on
   * ("DEEA1EYL", live-caught 2026-07-16). Strictly a fallback: fires only
   * when the whole token is NOT a valid call itself (a real DE1ABC stays
   * whole, and so does every cleanly keyed token) and the remainder
   * validates; the call inherits the full DE marker, exactly as if the gap
   * had been keyed. */
  if (de_glued) {
    cand_add(x, tok + 2, 1, TRUE, cq_now);
    g_strlcpy(x->prev_tok, tok + 2, sizeof(x->prev_tok));
    x->prev_valid = TRUE;
    x->prev_de    = TRUE;
    x->prev_cq    = cq_now;
    x->prev_gap   = FALSE;
    x->de_pending = 0;
    return;
  }

  g_strlcpy(x->prev_tok, tok, sizeof(x->prev_tok));
  x->prev_valid = valid;
  x->prev_de    = de_now;
  x->prev_cq    = cq_now;
  x->prev_gap   = FALSE;

  if (!valid) {
    /* A garbled token between DE and the call must not eat the marker
     * ("DE R OK1BR") — but the marker does not live forever either. */
    if (x->de_pending > 0) { x->de_pending--; }
    return;
  }

  cand_add(x, tok, 1, de_now, cq_now);
  x->de_pending = 0;
}

/* A whole over keyed as ONE token — gh#4: the degenerate fist with EVERY gap
 * closed (EA5JQF, IQ fixture of 2026-09-11, 14040.00: the pane read
 * "CQCQCQDEEA5JQFEA5JQFK" twice in 180 s and the station never reached the
 * table). Strict shape, the whole token or nothing: an optional CQ chain,
 * DE, ONE valid call keyed at least twice back to back, then nothing or a
 * closing word. The repeat is what makes it safe — "DEEA5JQFK" reads as
 * EA5JQF and a K just as well as EA5JQFK, a doubled call reads one way
 * only — and the shortest call that fits is the call ("DEK1AK1A" is K1A
 * twice, not the structurally valid K1AK1A). The words then go through
 * take_token() one by one: the call scores exactly as if the gaps had been
 * keyed. */
static gboolean take_fused_over(SkimCallsignExtractor *x, const char *tok) {
  static const char *CLOSE[] = { "K", "KN", "AR", "ARK", "+", "+K", "PSEK" };
  char  buf[OVER_MAX + 1];
  gsize n = g_strlcpy(buf, tok, sizeof(buf));
  /* the over-break mark glued to the last character, as in take_token() */
  const gboolean marked = n > 2 && strcmp(buf + n - 2, "\xC2\xB7") == 0;
  if (marked) { buf[n - 2] = '\0'; }
  if (skim_callsign_is_valid(buf))
    return FALSE;                              /* a real call stays whole    */
  const char *p = buf;
  gboolean cq = FALSE;
  for (; p[0] == 'C' && p[1] == 'Q'; p += 2) { cq = TRUE; }
  if (p[0] != 'D' || p[1] != 'E')
    return FALSE;
  p += 2;
  const gsize len = strlen(p);
  for (gsize l = 3; l < CALL_MAX && 2 * l <= len; l++) {
    char call[CALL_MAX];
    memcpy(call, p, l);
    call[l] = '\0';
    guint r = 1;
    while (strncmp(p + r * l, call, l) == 0) { r++; }
    if (r < 2 || !skim_callsign_is_valid(call))
      continue;
    const char *close = p + r * l;
    gboolean closed = close[0] == '\0';
    for (guint i = 0; i < G_N_ELEMENTS(CLOSE) && !closed; i++) {
      closed = strcmp(close, CLOSE[i]) == 0;
    }
    if (!closed)
      continue;
    if (cq) { take_token(x, "CQ"); }
    take_token(x, "DE");
    for (guint i = 0; i < r; i++) { take_token(x, call); }
    if (close[0]) { x->prev_tok[0] = '\0'; }   /* nothing joins across it    */
    if (marked) { take_token(x, "\xC2\xB7"); }
    return TRUE;
  }
  return FALSE;
}

/* A token too long to be a call, and no over either, is babble (a carrier
 * chopped by QSB, two stations on one tone): nothing in it counts, and no
 * join or run reaches across it. It is dropped WHOLE — cut at CALL_MAX, its
 * last few characters used to arrive as a token of their own, and a tail
 * that happened to read as a call was a candidate. */
static void take_babble(SkimCallsignExtractor *x) {
  x->tok_n++;
  x->cq_half  = FALSE;
  x->cq_pairs = 0;
  run_close(x, 0);
  run_clear(x);
  x->prev_tok[0] = '\0';
  if (x->cq_recent <= CQ_WINDOW) { x->cq_recent++; }
  if (x->de_pending > 0) { x->de_pending--; }
}

/* Stretched character gaps — gh#33, after OK1HRA (ok1hra/skimmer-for-linux
 * 292439e, written anew). A fist whose character gaps stretch to a word
 * gap's length comes out with spaces INSIDE the call: "CQ CQ CQ D E G 0WD
 * T G 0WD T G 0WD T P SE K", "Y O4 BE X Y O4 BE X", "DE O K2B VX · O K2B
 * VX E" (3552 kHz, 2026-11-01). On two recordings of 2026-10-15/16 that
 * was 15–16 % of the stations CW Skimmer Server spotted and OK1HRA's
 * skimmer missed. Token by token it cannot be read: T and E are stop words,
 * K2B is a call of its own, and the run (gh#3) ends at every one of them —
 * all it yields are the two-token torsos OK2B and K2BVX, and those went on
 * the air as stations. So the text is kept once more with its spaces
 * dropped, and after a calling or sender marker in it a call keyed back to
 * back — twice if MASTER.SCP knows it, three times otherwise, never a copy
 * that runs on into a longer known call — is read as that many copies.
 *
 * Strictly a fallback: only text the plain path left a FRAGMENT in is
 * looked at (a cleanly keyed over has none — every token a call, a marker,
 * a stop word or an exchange — and never enters here), and copies any path
 * already counted in the same stretch of text are not counted again, so a
 * clean copy beside torn ones is one copy, whichever path read it. The
 * stretch survives the decoder's over-break mark: this fist's gap between
 * two copies (3.7 s) is longer than the quiet that writes the mark (14
 * dits, ~1 s at 18 WPM, cw-v2), so the mark lands BETWEEN the copies and
 * an over holds one — and DeepCW writes no mark at all. So the stretch is
 * read at every EDGE of the run (gh#3) — a stop word, a whole call, DE, a
 * marker, the mark — right after the run was read and counted there (240
 * bytes at most; the repeat has to be complete to count, so reading early
 * changes nothing but the moment) and dropped after DS_GAP_US of silence
 * (a copy a minute later is another transmission), at DS_MAX bytes, at a
 * babble token (nothing reaches across one) and at a reset. A pass that
 * finds nothing new touches no candidate: a stale call is not kept alive
 * by the text around it.
 * SKIM_CALL_DESPACE=0 turns the path off — the bench's A/B (docs/BENCH.md). */
#define DS_MAX     240                         /* bytes of a stretch kept    */
#define DS_GAP_US  (20 * G_USEC_PER_SEC)       /* silence that ends one      */

static gboolean despace_on(void) {
  static gint on = -1;
  if (G_UNLIKELY(on < 0)) {
    const char *e = g_getenv("SKIM_CALL_DESPACE");
    on = !(e && strcmp(e, "0") == 0);
  }
  return on;
}

static void despace_clear(SkimCallsignExtractor *x) {
  g_string_set_size(x->ds_text, 0);
  g_string_set_size(x->ds_bound, 0);
  x->ds_seq++;
  x->ds_frag = FALSE;
}

/* The markers at s — CQ/TEST/QRZ chains, "DX" inside one, DE — and how
 * many bytes they take; 0 when s opens with none. */
static gsize despace_markers(const char *s, gboolean *cq, gboolean *de) {
  gsize i = 0;
  for (;;) {
    if (strncmp(s + i, "CQ", 2) == 0)              { i += 2; *cq = TRUE; }
    else if (strncmp(s + i, "TEST", 4) == 0)       { i += 4; *cq = TRUE; }
    else if (strncmp(s + i, "QRZ", 3) == 0)        { i += 3; *cq = TRUE; }
    else if (i && strncmp(s + i, "DX", 2) == 0)    { i += 2; }
    else if (strncmp(s + i, "DE", 2) == 0)         { i += 2; *de = TRUE; }
    else return i;
  }
}

/* The LAST copy of a repeat, at p in the stretch (l bytes): cut short when
 * it runs on into a longer call — then it is that call with its tail
 * lost, not a copy of its own. Two readings of "longer call":
 *  - the dictionary knows it ("OK1OOK1OA" is OK1OA twice, not OK1O);
 *  - the decoder committed it as ONE token, the copy its head: "DEOK1JR·
 *    OK1JRU" (3552.5 kHz, 2026-11-01 20:51) despaces to OK1JR twice,
 *    MASTER.SCP knows OK1JR and not OK1JRU, and the torso took the
 *    station's place in the table. The token boundaries tell: the copy
 *    opens the token and the token, a valid call, outlives it. A token
 *    that begins INSIDE the copy ("VXE" of "O K2B VXE") is no such
 *    evidence — E and T stick to anything — and neither is a token that
 *    is the copy with a word of the QSO glued on ("SO5CWK", "HA8ARCQ",
 *    "G6PZDX", "DJ5AZCQ"): a valid shape, but the word says the call
 *    ended before it. A longer call this extractor merely holds as a
 *    candidate is not consulted: those glued shapes are candidates too,
 *    and one of them would veto every later copy of the real call. */
static gboolean despace_cut(const SkimCallsignExtractor *x, gsize p, gsize l) {
  const char *s = x->ds_text->str, *b = x->ds_bound->str;
  const gsize n = x->ds_text->len;
  char ext[CALL_MAX];
  for (gsize e = l + 1; e <= l + 4 && p + e <= n && e < CALL_MAX; e++) {
    memcpy(ext, s + p, e);
    ext[e] = '\0';
    if (dict_has(ext))
      return TRUE;
  }
  gsize te = p + l;                            /* one past the copy          */
  if (te >= n || b[te] == '1' || b[p] != '1')
    return FALSE;                              /* the token ends with it, or */
  for (gsize i = p + 1; i < te; i++) {         /* began before it            */
    if (b[i] == '1')
      return FALSE;                            /* a token opened inside it   */
  }
  while (te < n && b[te] != '1') { te++; }
  if (te - p >= CALL_MAX)
    return FALSE;
  memcpy(ext, s + p, te - p);
  ext[te - p] = '\0';
  if (!skim_callsign_is_valid(ext))
    return FALSE;
  const char *word = ext + l;                  /* what the token adds        */
  gboolean cq = FALSE, de = FALSE;
  if (join_stop_word(word) || strcmp(word, "DX") == 0 ||
      despace_markers(word, &cq, &de) == strlen(word))
    return FALSE;                              /* a word of the QSO glued on */
  return TRUE;
}

/* The call keyed back to back at the head of s (n bytes): its length, the
 * call itself and the copies in *reps; 0 when none is. Shortest first, as
 * in take_fused_over(): "K1AK1AK1A" is K1A three times, never the
 * structurally valid K1AK1A. */
static gsize despace_repeat(const char *s, gsize n, char *call, guint *reps) {
  for (gsize l = 3; l < CALL_MAX && 2 * l <= n; l++) {
    memcpy(call, s, l);
    call[l] = '\0';
    if (!skim_callsign_is_valid(call))
      continue;
    guint k = 1;
    while ((k + 1) * l <= n && memcmp(s + k * l, call, l) == 0) { k++; }
    if (k < 2 || (k < 3 && !dict_has(call)))
      continue;
    *reps = k;
    return l;
  }
  return 0;
}

/* The scan has found `found` copies of `call` in the stretch so far: count
 * those not yet counted there by any path. The stretch is read again at
 * every edge, so a pass that finds nothing new adds nothing — except the
 * markers: a TEST keyed after copies already counted blesses them now. */
static void despace_credit(SkimCallsignExtractor *x, const char *call,
                           guint found, gboolean de, gboolean cq) {
  guint have = 0;
  for (guint i = 0; i < x->ncand; i++) {
    Cand *c = &x->cand[i];
    if (strcmp(c->call, call) == 0) {
      have = c->ds_seq == x->ds_seq ? c->ds_cnt : 0;
      if (have) {
        if (de) { c->de_marked  = TRUE; }
        if (cq) { c->cq_context = TRUE; }
      }
      break;
    }
  }
  if (have >= found)
    return;
  if (G_UNLIKELY(g_getenv("SKIM_DESPACE_DEBUG"))) {
    g_printerr("despace: %s x%u +%u%s%s |%s|\n", call, found, found - have,
               de ? " de" : "", cq ? " cq" : "", x->ds_text->str);
  }
  for (guint k = have; k < found; k++) { cand_add(x, call, 2, de, cq); }
}

#define DS_CALLS 16                            /* distinct calls one scan tallies */

static void despace_scan(SkimCallsignExtractor *x) {
  const char *s = x->ds_text->str;
  const gsize n = x->ds_text->len;
  struct { char call[CALL_MAX]; guint found; } seen[DS_CALLS];
  guint nseen = 0;
  for (gsize i = 0; i + 8 <= n;) {             /* DE + a 3-char call twice   */
    gboolean cq = FALSE, de = FALSE;
    const gsize h = despace_markers(s + i, &cq, &de);
    if (!h) { i++; continue; }
    char call[CALL_MAX];
    guint reps = 0;
    const gsize l = despace_repeat(s + i + h, n - i - h, call, &reps);
    if (!l) { i += h; continue; }
    const gsize p = i + h;                     /* the copies start here      */
    i = p + l * reps;
    /* the cut test looks at the LAST copy only: an inner copy always runs
     * on into the next one, and SP7AJ repeated would be refused whenever
     * MASTER.SCP knows an SP7AJS */
    if (despace_cut(x, p + (reps - 1) * l, l)) {
      if (G_UNLIKELY(g_getenv("SKIM_DESPACE_DEBUG"))) {
        g_printerr("despace: %s x%u cut — runs on into a longer call |%s|\n",
                   call, reps, s);
      }
      continue;
    }
    /* a calling marker right after the copies blesses them too ("SD1A
     * SD1A TEST"), as a trailing TEST does in take_token() */
    if (!cq) {
      gboolean c2 = FALSE, d2 = FALSE;
      despace_markers(s + i, &c2, &d2);
      cq = c2;
    }
    /* the same call keyed in two runs of the stretch ("CQ DE X X K CQ DE
     * X X K") is counted over both */
    guint j = 0;
    while (j < nseen && strcmp(seen[j].call, call) != 0) { j++; }
    if (j == nseen) {
      if (nseen == DS_CALLS)
        continue;                              /* babble — nothing more here */
      g_strlcpy(seen[nseen].call, call, sizeof(seen[nseen].call));
      seen[nseen++].found = 0;
    }
    seen[j].found += reps;
    despace_credit(x, call, seen[j].found, de, cq);
  }
}

/* A token take_token() takes — its bare text (n bytes, the mark cut off)
 * joins the stretch before the token is read. */
static void despace_note(SkimCallsignExtractor *x, const char *tok, gsize n) {
  if (!despace_on() || !n)
    return;
  if (x->ds_text->len && x->now_us - x->ds_last_us > DS_GAP_US) {
    despace_clear(x);
  }
  g_string_append_len(x->ds_text, tok, n);
  g_string_append_c(x->ds_bound, '1');
  for (gsize i = 1; i < n; i++) { g_string_append_c(x->ds_bound, '0'); }
  x->ds_last_us = x->now_us;
}

/* An EDGE of the run (gh#3) — DE, a marker, a stop word, a whole call, the
 * over-break mark, the end of the text: the run has just been read and
 * counted, so the stretch is read now and never counts a copy twice. The
 * cap is checked here too: a stretch of DS_MAX has been read whole. */
static void despace_edge(SkimCallsignExtractor *x) {
  if (!despace_on())
    return;
  if (x->ds_frag && x->ds_text->len) { despace_scan(x); }
  if (x->ds_text->len > DS_MAX) { despace_clear(x); }
}

void skim_callsign_extractor_feed(SkimCallsignExtractor *x, const char *text) {
  for (const char *p = text; *p; p++) {
    const char c = *p;
    if (c == ' ' || c == '\n' || c == '\t') {
      if (x->tok->len) {
        const char *tok = x->tok->str;
        const gsize n   = x->tok->len;
        if (n <= OVER_MAX && take_fused_over(x, tok)) {
          /* its words went through take_token() one by one */
        } else if (n < CALL_MAX) {
          take_token(x, tok);
        } else {
          take_babble(x);
          despace_clear(x);                    /* nothing reaches across it  */
        }
        g_string_set_size(x->tok, 0);
      }
      continue;
    }
    /* one character past the cap says "too long"; the rest is not kept */
    if (x->tok->len <= OVER_MAX) {
      g_string_append_c(x->tok, g_ascii_toupper(c));
    }
  }
}

double skim_callsign_extractor_best_ex(SkimCallsignExtractor *x,
                                       char *out, gsize out_size,
                                       gboolean *cq_context) {
  double best = 0.0;
  const Cand *bc = NULL;
  for (guint i = 0; i < x->ncand; i++) {
    /* A call glued from three or more fragments has to REPEAT before it is
     * believed: a torn fist tears every time, a garble does not — and the
     * run carries its DE/CQ markers, so a one-off would go straight out.
     * Two-token joins keep their old standing. */
    if (x->cand[i].parts > 2 && x->cand[i].count < 2)
      continue;
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

guint skim_callsign_extractor_hearings(const SkimCallsignExtractor *x,
                                       const char *call) {
  if (!x || !call || !call[0])
    return 0;
  for (guint i = 0; i < x->ncand; i++) {
    if (strcmp(x->cand[i].call, call) == 0) { return x->cand[i].count; }
  }
  return 0;
}

void skim_callsign_extractor_set_now(SkimCallsignExtractor *x, gint64 now_us) {
  if (x) { x->now_us = now_us; }
}

gint64 skim_callsign_extractor_last_read(const SkimCallsignExtractor *x,
                                         const char *call) {
  if (!x || !call || !call[0])
    return -1;
  for (guint i = 0; i < x->ncand; i++) {
    if (strcmp(x->cand[i].call, call) == 0) { return x->cand[i].last_us; }
  }
  return -1;
}

guint skim_callsign_edits(const char *a, const char *b) {
  const gsize la = strlen(a), lb = strlen(b);
  if (la > 15 || lb > 15)
    return 99;
  guint prev[16], cur[16];
  for (gsize j = 0; j <= lb; j++) { prev[j] = (guint)j; }
  for (gsize i = 1; i <= la; i++) {
    cur[0] = (guint)i;
    for (gsize j = 1; j <= lb; j++) {
      const guint sub = prev[j - 1] + (a[i - 1] != b[j - 1]);
      cur[j] = MIN(MIN(prev[j] + 1, cur[j - 1] + 1), sub);
    }
    memcpy(prev, cur, (lb + 1) * sizeof(guint));
  }
  return prev[lb];
}

gboolean skim_callsign_is_edge(const char *a, const char *b) {
  const gsize la = strlen(a), lb = strlen(b);
  if (MIN(la, lb) < 3 || strcmp(a, b) == 0)
    return FALSE;
  return g_str_has_prefix(a, b) || g_str_has_prefix(b, a) ||
         g_str_has_suffix(a, b) || g_str_has_suffix(b, a);
}

double skim_callsign_extract(const char *text, char *out, gsize out_size) {
  SkimCallsignExtractor *x = skim_callsign_extractor_new();
  skim_callsign_extractor_feed(x, text);
  skim_callsign_extractor_feed(x, " ");        /* flush the last token       */
  run_close(x, 0);                             /* the end of the text: an edge */
  despace_edge(x);
  double s = skim_callsign_extractor_best(x, out, out_size);
  skim_callsign_extractor_free(x);
  return s;
}
