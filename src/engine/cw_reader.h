/* cw_reader.h — the neural CW reader (prototype): a small dilated-TCN + CTC
 * network over SYMBOLIC mark/space run durations, trained offline on
 * synthetic "fists" (ml/train_ctc.py). It re-reads a whole over at once with
 * bidirectional context — timing chaos a strong hand-keyed operator produces
 * (torn/fused gaps, mid-over speed changes, bug weighting) that the causal
 * per-element decoders cannot resolve.
 *
 * Text from this reader feeds the DECODE PANE only. It never feeds the spot
 * path: a lexically-primed model can hallucinate plausible callsigns from
 * garbage, and the RBN rule (never spot unvalidated calls) stays with the
 * classical extractor+validator.
 *
 * Weights: flat blob written by ml/export_c.py, loaded once (thread-safe
 * lazily via the pipeline). Forward pass is plain C loops — no dependencies.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_CW_READER_H
#define SKIMMER_CW_READER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimCwReader SkimCwReader;

/* Load a weights blob (ml/export_c.py format). NULL + error on a bad file. */
SkimCwReader *skim_cw_reader_load(const char *path, GError **error);
void          skim_cw_reader_free(SkimCwReader *r);

/* Re-read one over: runs = alternating key/duration pairs (dur_ms[i] with
 * key_mark[i] TRUE for marks), n of them, in stream order. Returns the CTC
 * greedy text (owned by caller, g_free) — "" when the over is too short. */
char *skim_cw_reader_read(const SkimCwReader *r, const gboolean *key_mark,
                          const double *dur_ms, guint n);

/* Self-test against the blob's baked-in test vector (max |logit diff|).
 * The offline gate asserts this stays < 1e-3. */
double skim_cw_reader_selftest(const SkimCwReader *r);

G_END_DECLS

#endif /* SKIMMER_CW_READER_H */
