/* cw_reader.h — the neural CW reader: a small dilated-TCN + CTC network over
 * SYMBOLIC mark/space run durations, trained offline on synthetic "fists"
 * (ml/train_ctc.py). It reads through the timing chaos a strong hand-keyed
 * operator produces (torn/fused gaps, mid-over speed changes, bug weighting)
 * that the causal per-element decoders cannot resolve.
 *
 * Two ways in:
 *   - read(): one-shot over a finished over (any blob version);
 *   - stream (phase A): CWRD v3 blobs are trained with a BOUNDED right
 *     receptive field (per-layer `look`, ~22 runs total) and causal
 *     windowed-median features, so the same net runs incrementally — push
 *     runs as they complete, committed text comes back ~2-4 s behind the
 *     key. The concatenated stream output is bit-identical to read() over
 *     the same runs.
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

/* --- streaming (CWRD v3 blobs only) ---------------------------------------- */

typedef struct _SkimCwReaderStream SkimCwReaderStream;

/* Per-channel incremental state (~200 kB). NULL for a v2 blob — those nets
 * are symmetric and cannot stream; fall back to read() at the over break. */
SkimCwReaderStream *skim_cw_reader_stream_new(const SkimCwReader *r);
void skim_cw_reader_stream_free(SkimCwReaderStream *s);

/* Push one completed run. Returns text newly committed by the bounded
 * lookahead (g_free), or NULL when nothing became final yet. */
char *skim_cw_reader_stream_push(SkimCwReaderStream *s, gboolean key_mark,
                                 double dur_ms);

/* Over break: commit everything still in flight and reset the stream for
 * the next over. Returns the tail text (g_free) or NULL. push+flush output
 * concatenated is bit-identical to read() of the same runs. */
char *skim_cw_reader_stream_flush(SkimCwReaderStream *s);

/* push/flush that also report WHERE and HOW SURELY each committed char was
 * read: on a non-NULL return, *pos (g_free) holds strlen(text) entries —
 * pos[i] is the 0-based index, among the runs pushed since the last flush,
 * of the run at whose CTC output frame text[i] fired (monotone
 * non-decreasing); *marg (g_free) holds the logit margin (winner minus
 * runner-up) at that frame — the net's own confidence, sharp on clean
 * reads and near zero where classes fight. The pane composition
 * (decode_cw_v2 phase B) seats the model/draft seam on pos and gates each
 * word on marg. Either out may be NULL. */
char *skim_cw_reader_stream_push_pos(SkimCwReaderStream *s, gboolean key_mark,
                                     double dur_ms, guint **pos,
                                     float **marg);
char *skim_cw_reader_stream_flush_pos(SkimCwReaderStream *s, guint **pos,
                                      float **marg);

G_END_DECLS

#endif /* SKIMMER_CW_READER_H */
