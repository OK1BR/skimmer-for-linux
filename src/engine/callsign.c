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

#define MAX_CAND    12
#define CALL_MAX    16
#define CQ_WINDOW   3                          /* tokens after CQ that count */

typedef struct {
  char     call[CALL_MAX];
  guint    count;
  gboolean de_marked;
  gboolean cq_context;
} Cand;

struct _SkimCallsignExtractor {
  GString *tok;                                /* partial token across feeds */
  gint     de_pending;                         /* DE marker lives ≤ 2 tokens */
  gint     cq_recent;                          /* tokens since CQ (≤ window) */
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
  x->ncand      = 0;
}

static double cand_score(const Cand *c) {
  double s = 0.55;                             /* structural + allocation    */
  if (c->de_marked)  { s += 0.25; }
  if (c->cq_context) { s += 0.10; }
  if (c->count >= 2) { s += 0.20; }
  if (c->count >= 3) { s += 0.05; }
  if (dict_has(c->call)) { s += 0.15; }
  return MIN(s, 1.0);
}

static void take_token(SkimCallsignExtractor *x, const char *tok) {
  if (strcmp(tok, "DE") == 0) {
    x->de_pending = 2;
    return;
  }
  if (strcmp(tok, "CQ") == 0) {
    x->cq_recent = 0;
    return;
  }
  if (x->cq_recent <= CQ_WINDOW) { x->cq_recent++; }

  if (!skim_callsign_is_valid(tok) || strlen(tok) >= CALL_MAX) {
    /* A garbled token between DE and the call must not eat the marker
     * ("DE R OK1BR") — but the marker does not live forever either. */
    if (x->de_pending > 0) { x->de_pending--; }
    return;
  }

  Cand *c = NULL;
  for (guint i = 0; i < x->ncand; i++) {
    if (strcmp(x->cand[i].call, tok) == 0) { c = &x->cand[i]; break; }
  }
  if (!c) {
    if (x->ncand < MAX_CAND) {
      c = &x->cand[x->ncand++];
    } else {
      c = &x->cand[0];                         /* evict the weakest          */
      for (guint i = 1; i < MAX_CAND; i++) {
        if (cand_score(&x->cand[i]) < cand_score(c)) { c = &x->cand[i]; }
      }
    }
    memset(c, 0, sizeof(*c));
    g_strlcpy(c->call, tok, sizeof(c->call));
  }
  c->count++;
  if (x->de_pending > 0) { c->de_marked = TRUE; }
  if (x->cq_recent <= CQ_WINDOW) { c->cq_context = TRUE; }
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

double skim_callsign_extractor_best(SkimCallsignExtractor *x,
                                    char *out, gsize out_size) {
  double best = 0.0;
  const Cand *bc = NULL;
  for (guint i = 0; i < x->ncand; i++) {
    double s = cand_score(&x->cand[i]);
    if (s > best) { best = s; bc = &x->cand[i]; }
  }
  if (!bc || best < SKIM_CALLSIGN_SPOT_THRESHOLD) {
    if (out && out_size) { out[0] = '\0'; }
    return 0.0;
  }
  if (out && out_size) { g_strlcpy(out, bc->call, out_size); }
  return best;
}

double skim_callsign_extract(const char *text, char *out, gsize out_size) {
  SkimCallsignExtractor *x = skim_callsign_extractor_new();
  skim_callsign_extractor_feed(x, text);
  skim_callsign_extractor_feed(x, " ");        /* flush the last token       */
  double s = skim_callsign_extractor_best(x, out, out_size);
  skim_callsign_extractor_free(x);
  return s;
}
