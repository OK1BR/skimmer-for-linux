/* callsign.h — callsign extraction + validation (RBN-grade).
 *
 * Pulls candidate callsigns out of decoded text and scores their plausibility:
 * a structural parser with the ITU allocation tables (letter+digit country
 * prefixes are where decode garbage like "T1BR" dies), CW context markers
 * (DE / CQ), repetition counting and an optional known-call dictionary
 * (MASTER.SCP format: one call per line). The RBN feed must never emit
 * unvalidated calls, so this gates spotting (M4 gates M6).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_CALLSIGN_H
#define SKIMMER_CALLSIGN_H

#include <glib.h>

G_BEGIN_DECLS

/* Score a candidate must reach before it may be spotted: structural validity
 * alone (0.55) is NOT enough — it takes a DE marker, repetition, CQ context
 * or a dictionary hit on top. */
#define SKIM_CALLSIGN_SPOT_THRESHOLD 0.70

/* TRUE if s is a structurally valid amateur callsign with an allocated
 * prefix. Portable designators are understood (OK1BR/P, F/OK1BR, OK1BR/4). */
gboolean skim_callsign_is_valid(const char *s);

/* Optional known-call dictionary (MASTER.SCP style: one call per line, '#'
 * comments, "!!" directives and lines that cannot be a call are skipped).
 * Replaces any previously loaded dictionary, and may do so from any thread
 * at any time: lookups on other threads see the old table or the new one,
 * never a half-built one. A file that cannot be read leaves the loaded
 * dictionary as it was. */
gboolean skim_callsign_dict_load(const char *path, GError **error);
guint    skim_callsign_dict_size(void);

/* The loaded file's "# Release 2026.09.18" comment as "2026.09.18", or NULL
 * when it carried none / nothing is loaded. Caller frees. */
char    *skim_callsign_dict_release(void);

/* What a load of these bytes WOULD take, without touching the loaded
 * dictionary — the updater's sanity check on a download. Safe on arbitrary
 * bytes (no terminator needed, NULs and endless lines are junk). */
typedef struct {
  guint calls;                 /* lines carrying a call                      */
  guint valid;                 /* of those, skim_callsign_is_valid()         */
  guint junk;                  /* neither call, blank, '#' nor "!!" line     */
  char  release[32];           /* "2026.09.18", "" when the file names none  */
} SkimCallsignDictInfo;
void skim_callsign_dict_inspect(const char *data, gsize len,
                                SkimCallsignDictInfo *info);

/* TRUE when the loaded dictionary knows this exact call (case-insensitive).
 * The scorer uses it for its +0.15 boost; the app highlights dictionary
 * hits in the monitor pane. */
gboolean skim_callsign_dict_has(const char *call);

/* Stateful per-channel extractor: feed decoded text incrementally (token
 * fragments survive across calls), poll best() for the leading candidate. */
typedef struct _SkimCallsignExtractor SkimCallsignExtractor;

SkimCallsignExtractor *skim_callsign_extractor_new(void);
void   skim_callsign_extractor_free(SkimCallsignExtractor *x);
void   skim_callsign_extractor_reset(SkimCallsignExtractor *x);
void   skim_callsign_extractor_feed(SkimCallsignExtractor *x, const char *text);

/* Best current candidate: fills out (out_size cap) and returns its score,
 * or 0.0 when no candidate reaches SKIM_CALLSIGN_SPOT_THRESHOLD. */
double skim_callsign_extractor_best(SkimCallsignExtractor *x,
                                    char *out, gsize out_size);

/* As best(), and reports whether the candidate was heard CALLING — in the
 * context of a CQ/TEST/QRZ marker (leading or trailing). The CQ-only spot
 * policy keys off this: S&P answers do not own the frequency. */
double skim_callsign_extractor_best_ex(SkimCallsignExtractor *x,
                                       char *out, gsize out_size,
                                       gboolean *cq_context);

/* How many copies of `call` this extractor has counted: the times the call
 * was keyed and READ on this channel — not how often the pipeline reported
 * it (the best candidate is re-reported after every token). 0 when the
 * call is no candidate here (never read, or evicted). The RBN feed asks
 * for two (gh#24): one copy with DE and CQ around it already scores 0.90,
 * and a garble keyed once reads exactly like that. */
guint  skim_callsign_extractor_hearings(const SkimCallsignExtractor *x,
                                        const char *call);

/* The caller's clock, stamped on every copy read from now on — set it
 * before feed(). Candidates age by TOKENS (traffic on the channel), not by
 * time: through an hour of silence a call stays the best candidate with
 * all its hearings, and the first noise token after it reports the call
 * again (gh#26). last_read() tells that report from a fresh one — the
 * clock at the last copy actually READ (0 while no clock was ever set),
 * or -1 when the call is no candidate here. */
void   skim_callsign_extractor_set_now(SkimCallsignExtractor *x, gint64 now_us);
gint64 skim_callsign_extractor_last_read(const SkimCallsignExtractor *x,
                                         const char *call);

/* One-shot convenience over a complete text buffer (same scoring). */
double skim_callsign_extract(const char *text, char *out, gsize out_size);

/* How far apart two calls are: the Levenshtein distance (an insertion, a
 * deletion or a substitution each count one). A misread of a call is a
 * step or two away from it (TR2C of IR2C, N4QWW of UR4QWW); a call longer
 * than SkimStation's 15 characters is treated as far from everything. */
guint skim_callsign_edits(const char *a, const char *b);

/* TRUE when one call is the other's head or tail (both at least three
 * characters, not equal): a boundary gone wrong — the call cut (RK3D of
 * RK3DJW) or the next letter glued on (F6FXXK of F6FXX). */
gboolean skim_callsign_is_edge(const char *a, const char *b);

G_END_DECLS

#endif /* SKIMMER_CALLSIGN_H */
