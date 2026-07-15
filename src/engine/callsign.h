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
 * comments). Replaces any previously loaded dictionary. Engine-thread only. */
gboolean skim_callsign_dict_load(const char *path, GError **error);
guint    skim_callsign_dict_size(void);

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

/* One-shot convenience over a complete text buffer (same scoring). */
double skim_callsign_extract(const char *text, char *out, gsize out_size);

G_END_DECLS

#endif /* SKIMMER_CALLSIGN_H */
