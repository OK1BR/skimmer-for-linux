/* ort_shim.h — ONNX Runtime through its C API, loaded at RUN time.
 *
 * The skimmer never links libonnxruntime: this shim dlopen()s it
 * (SKIM_ORT_LIB, else libonnxruntime.so.1, else libonnxruntime.so), asks
 * OrtGetApiBase()->GetApi(ORT_API_VERSION) for the API table the vendored
 * header describes (vendor/onnxruntime, v1.21) and exposes the four things
 * a decode backend needs: open the runtime, load a model, run one float
 * tensor through it, close. Missing library or model = a GError, never an
 * abort — the DeepCW engine then reports "not available".
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_ORT_SHIM_H
#define SKIMMER_ORT_SHIM_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct _SkimOrt SkimOrt;
typedef struct _SkimOrtSession SkimOrtSession;

/* dlopen the runtime + create the environment. lib_override may be NULL
 * (then SKIM_ORT_LIB, then the sonames). Returns NULL + error when the
 * library is missing or answers the wrong API version. */
SkimOrt *skim_ort_open(const char *lib_override, GError **error);
void     skim_ort_close(SkimOrt *ort);
/* Runtime version string ("1.30.0") and the path the loader used. */
const char *skim_ort_version(const SkimOrt *ort);
const char *skim_ort_library(const SkimOrt *ort);

/* Load a model. intra_threads = ONNX Runtime intra-op threads (0 = its
 * default). Input/output names are read from the model (index 0 each). */
SkimOrtSession *skim_ort_session_new(SkimOrt *ort, const char *model_path,
                                     int intra_threads, GError **error);
void            skim_ort_session_free(SkimOrtSession *s);
const char     *skim_ort_session_input_name(const SkimOrtSession *s);
const char     *skim_ort_session_output_name(const SkimOrtSession *s);

/* Run one float32 tensor (in, dims[nd]) through the model; the output
 * tensor (index 0) is copied into *out (g_malloc'ed, caller frees) with
 * its shape in out_dims[0..*out_nd) (out_dims must hold ≥ 8). */
gboolean skim_ort_run(SkimOrtSession *s, const float *in,
                      const int64_t *dims, int nd,
                      float **out, int64_t *out_dims, int *out_nd,
                      GError **error);

#define SKIM_ORT_ERROR (skim_ort_error_quark())
GQuark skim_ort_error_quark(void);

G_END_DECLS

#endif /* SKIMMER_ORT_SHIM_H */
