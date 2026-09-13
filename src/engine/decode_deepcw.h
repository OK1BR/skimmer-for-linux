/* decode_deepcw.h — DeepCW neural CW backend (Conformer + CTC over a
 * 65-bin × 15 ms log-magnitude spectrogram; model by e04, AGPL-3.0-only,
 * https://github.com/e04/deepcw-engine).
 *
 * Implements decode.h on the channelizer's complex baseband: a 20-point
 * Hann DFT per 16 ms gives the model's 12.5 Hz bins straight from the
 * 250 Hz channel (no audio path), the channel's ±5 bins are seated at the
 * tile centre, the rest is zero (offline-verified equal to full audio),
 * each window is peak-normalised (the model is scale-invariant only over a
 * range real IQ falls below). Inference runs on the ENGINE thread, on the
 * channel's own frame clock (replays stay deterministic), only on channels
 * whose window shows a keyed line above the floor, and text is committed
 * as soon as a character sits ≥ tail seconds before the window end; the
 * whole ring is re-read every tick (full left context) and a frame cursor
 * keeps anything from being emitted twice.
 *
 * ONNX Runtime is dlopen-ed (ort_shim.c) and the model file resolved at run
 * time (SKIM_DEEPCW_MODEL, else the user data dir); when either is missing
 * skim_decode_deepcw_available() says why and the pipeline stays on v2.
 *
 * Inference runs on SKIM_DEEPCW_WORKERS (2) threads by default — the engine
 * thread only snapshots a window and later commits the result — or INLINE
 * with SKIM_DEEPCW_SYNC=1, which skimmer-replay sets so that a replay is
 * deterministic (results land in stream order, never a wall-clock race).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_DECODE_DEEPCW_H
#define SKIMMER_DECODE_DEEPCW_H

#include "decode.h"

G_BEGIN_DECLS

/* The backend singleton (static vtable). */
const SkimDecodeBackend *skim_decode_deepcw(void);

/* Runtime + model resolvable and loaded (cached; the first call loads).
 * FALSE + error names what is missing — the app shows it on the engine
 * row, the pipeline logs it and falls back. */
gboolean skim_decode_deepcw_available(GError **error);

/* Resolved model path (SKIM_DEEPCW_MODEL or the default under the user
 * data dir) — informational, not necessarily existing. Caller frees. */
char *skim_decode_deepcw_model_path(void);

/* "1.30.0 via libonnxruntime.so.1, CUDA:0" once loaded, NULL before. */
const char *skim_decode_deepcw_runtime_info(void);

/* Inference device for the NEXT session: "cpu" (default) or "cuda"
 * (SKIM_DEEPCW_DEVICE is the env spelling for replays). A device that is
 * not usable falls back to the CPU — runtime_info says so. */
void skim_decode_deepcw_set_device(const char *device);
/* Drop the loaded session so the next use reloads it (device change,
 * model change). Safe while the pipeline is stopped; workers still
 * finishing a window keep the old session alive until they are done. */
void skim_decode_deepcw_reset(void);

/* --- test hooks (skimmer-deepcw-test) ------------------------------------ */
typedef struct {
  gboolean gate;             /* last tick's gate verdict                  */
  double   ratio_db;         /* line over floor, last gated window        */
  double   duty;             /* keyed duty, last window                   */
  double   wpm;              /* dit estimate → WPM (0 = unknown)          */
  guint64  frames_abs;       /* frames seen                               */
  guint64  committed;        /* commit cursor (abs frame)                 */
  guint    ticks;            /* inference runs so far                     */
  gboolean dead;             /* unusable rate / no runtime                */
  gboolean inflight;         /* async: a window is at the workers         */
  guint    draft_len;        /* chars of tail-guard draft from the last tick */
  gboolean pane_open;        /* an over region is open in the pane        */
} SkimDeepcwDebug;
void skim_decode_deepcw_debug(gpointer state, SkimDeepcwDebug *dbg);

/* The commit rule on a raw CTC log-prob matrix (logp[T][42], window =
 * abs frames [w0, w0+T)): greedy collapse; every spike that sits ≥ tail
 * frames before the window end and beyond the cursor (+ margin frames for
 * characters — a re-placed copy of the last committed character lands
 * within ±2 frames) is appended to out; *cursor moves to the last
 * committed spike, *last_space squeezes consecutive spaces. Returns the
 * number of committed characters; *conf = their mean posterior. Pure —
 * no state, no model. */
guint skim_deepcw_commit(const float *logp, guint T, guint64 w0,
                         guint64 *cursor, guint tail, guint margin,
                         gboolean *last_space, GString *out, double *conf);

/* The same rule, plus the DRAFT: every spike INSIDE the tail guard (beyond
 * the cursor/margin, spaces squeezed on from the final text) is appended
 * to `draft` — the model's current reading of the not-yet-final tail. It
 * is display only (the pane shows it dim) and is re-read at the next
 * tick; the cursor and last_space move only for final characters. NULL
 * for draft behaves exactly like skim_deepcw_commit. */
guint skim_deepcw_commit_ex(const float *logp, guint T, guint64 w0,
                            guint64 *cursor, guint tail, guint margin,
                            gboolean *last_space, GString *out, double *conf,
                            GString *draft);

G_END_DECLS

#endif /* SKIMMER_DECODE_DEEPCW_H */
