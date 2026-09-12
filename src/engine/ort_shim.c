/* ort_shim.c — ONNX Runtime C API behind dlopen (see ort_shim.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "ort_shim.h"

#include <dlfcn.h>
#include <string.h>

#include "onnxruntime_c_api.h"

G_DEFINE_QUARK(skim-ort-error, skim_ort_error)

struct _SkimOrt {
  void         *dl;
  const OrtApi *api;
  OrtEnv       *env;
  char         *version;
  char         *library;
};

struct _SkimOrtSession {
  SkimOrt        *ort;
  OrtSession     *session;
  OrtMemoryInfo  *mem;
  char           *in_name;
  char           *out_name;
  char           *device;       /* what actually runs (see header)         */
};

/* Append the CUDA execution provider (device 0). FALSE + *why on failure —
 * typically the provider library is not installed (onnxruntime-cpu) or
 * the driver is missing; the caller then stays on the CPU. */
static gboolean append_cuda(const OrtApi *api, OrtSessionOptions *opt,
                            char **why) {
  /* Arch's onnxruntime-cuda 1.29 provider library references cuDNN
   * symbols but does not list libcudnn.so.9 as NEEDED (only cudart and
   * cublas), so its load fails with "undefined symbol:
   * cudnnGetConvolutionBackwardDataAlgorithm_v7" although the installed
   * cuDNN 9.26 exports it (checked with nm). Bringing cuDNN into the
   * process first, with global symbol visibility, lets the provider
   * resolve; harmless where the provider is linked correctly, and a
   * missing cuDNN is reported by the provider load itself below. */
  static gsize cudnn_once;
  if (g_once_init_enter(&cudnn_once)) {
    void *h = dlopen("libcudnn.so.9", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { g_message("ort: libcudnn.so.9 not preloadable (%s)", dlerror()); }
    g_once_init_leave(&cudnn_once, 1);
  }
  OrtCUDAProviderOptionsV2 *co = NULL;
  OrtStatus *st = api->CreateCUDAProviderOptions(&co);
  if (st) {
    *why = g_strdup(api->GetErrorMessage(st));
    api->ReleaseStatus(st);
    return FALSE;
  }
  const char *keys[] = { "device_id" };
  const char *vals[] = { "0" };
  st = api->UpdateCUDAProviderOptions(co, keys, vals, 1);
  if (st) {
    *why = g_strdup(api->GetErrorMessage(st));
    api->ReleaseStatus(st);
    api->ReleaseCUDAProviderOptions(co);
    return FALSE;
  }
  st = api->SessionOptionsAppendExecutionProvider_CUDA_V2(opt, co);
  api->ReleaseCUDAProviderOptions(co);
  if (st) {
    *why = g_strdup(api->GetErrorMessage(st));
    api->ReleaseStatus(st);
    return FALSE;
  }
  return TRUE;
}

/* Turn an OrtStatus into a GError (and release it). TRUE = there was one. */
static gboolean take_status(const OrtApi *api, OrtStatus *st, GError **error,
                            const char *what) {
  if (!st) { return FALSE; }
  g_set_error(error, SKIM_ORT_ERROR, 1, "%s: %s", what,
              api->GetErrorMessage(st));
  api->ReleaseStatus(st);
  return TRUE;
}

SkimOrt *skim_ort_open(const char *lib_override, GError **error) {
  const char *cands[4];
  guint n = 0;
  if (lib_override && lib_override[0]) { cands[n++] = lib_override; }
  const char *env = g_getenv("SKIM_ORT_LIB");
  if (env && env[0]) { cands[n++] = env; }
  cands[n++] = "libonnxruntime.so.1";
  cands[n++] = "libonnxruntime.so";

  void *dl = NULL;
  const char *used = NULL;
  GString *tried = g_string_new(NULL);
  for (guint i = 0; i < n && !dl; i++) {
    dl = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);
    if (dl) { used = cands[i]; }
    else { g_string_append_printf(tried, "%s%s (%s)", i ? ", " : "",
                                  cands[i], dlerror()); }
  }
  if (!dl) {
    g_set_error(error, SKIM_ORT_ERROR, 2,
                "ONNX Runtime library not found — tried %s", tried->str);
    g_string_free(tried, TRUE);
    return NULL;
  }
  g_string_free(tried, TRUE);

  const OrtApiBase *(*get_base)(void) =
      (const OrtApiBase *(*)(void))dlsym(dl, "OrtGetApiBase");
  if (!get_base) {
    g_set_error(error, SKIM_ORT_ERROR, 3, "%s has no OrtGetApiBase", used);
    dlclose(dl);
    return NULL;
  }
  const OrtApiBase *base = get_base();
  const OrtApi *api = base->GetApi(ORT_API_VERSION);
  if (!api) {
    g_set_error(error, SKIM_ORT_ERROR, 4,
                "%s (version %s) does not provide ONNX Runtime API %d",
                used, base->GetVersionString(), ORT_API_VERSION);
    dlclose(dl);
    return NULL;
  }
  OrtEnv *oenv = NULL;
  OrtStatus *st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "skimmer", &oenv);
  if (take_status(api, st, error, "CreateEnv")) {
    dlclose(dl);
    return NULL;
  }
  SkimOrt *o = g_new0(SkimOrt, 1);
  o->dl = dl;
  o->api = api;
  o->env = oenv;
  o->version = g_strdup(base->GetVersionString());
  o->library = g_strdup(used);
  return o;
}

void skim_ort_close(SkimOrt *o) {
  if (!o) { return; }
  if (o->env) { o->api->ReleaseEnv(o->env); }
  /* The runtime spins worker threads; unloading the object while they
   * wind down has bitten other users of the C API — keep it mapped for
   * the process lifetime (the app opens it once). */
  g_free(o->version);
  g_free(o->library);
  g_free(o);
}

const char *skim_ort_version(const SkimOrt *o) { return o ? o->version : NULL; }
const char *skim_ort_library(const SkimOrt *o) { return o ? o->library : NULL; }

SkimOrtSession *skim_ort_session_new(SkimOrt *o, const char *model_path,
                                     int intra_threads, const char *device,
                                     GError **error) {
  g_return_val_if_fail(o != NULL && model_path != NULL, NULL);
  const OrtApi *api = o->api;
  OrtSessionOptions *opt = NULL;
  OrtStatus *st = api->CreateSessionOptions(&opt);
  if (take_status(api, st, error, "CreateSessionOptions")) { return NULL; }
  char *dev_label = NULL;
  if (device && g_ascii_strcasecmp(device, "cuda") == 0) {
    char *why = NULL;
    if (append_cuda(api, opt, &why)) {
      dev_label = g_strdup("CUDA:0");
    } else {
      dev_label = g_strdup_printf("CPU (cuda unavailable: %s)", why);
      g_free(why);
    }
  } else {
    dev_label = g_strdup("CPU");
  }
  if (intra_threads > 0) {
    st = api->SetIntraOpNumThreads(opt, intra_threads);
    if (take_status(api, st, error, "SetIntraOpNumThreads")) {
      api->ReleaseSessionOptions(opt);
      g_free(dev_label);
      return NULL;
    }
  }
  st = api->SetInterOpNumThreads(opt, 1);
  if (take_status(api, st, error, "SetInterOpNumThreads")) {
    api->ReleaseSessionOptions(opt);
    g_free(dev_label);
    return NULL;
  }
  st = api->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL);
  if (take_status(api, st, error, "SetSessionGraphOptimizationLevel")) {
    api->ReleaseSessionOptions(opt);
    g_free(dev_label);
    return NULL;
  }
  st = api->SetSessionLogSeverityLevel(opt, 3);      /* errors only        */
  if (st) { api->ReleaseStatus(st); }               /* cosmetic — ignore  */

  OrtSession *sess = NULL;
  st = api->CreateSession(o->env, model_path, opt, &sess);
  api->ReleaseSessionOptions(opt);
  if (st && g_str_has_prefix(dev_label, "CUDA")) {
    /* The provider appended but the session refused (no usable GPU, a
     * driver/library mismatch): say so and build the CPU session. */
    char *why = g_strdup(api->GetErrorMessage(st));
    api->ReleaseStatus(st);
    g_free(dev_label);
    dev_label = g_strdup_printf("CPU (cuda unavailable: %s)", why);
    g_free(why);
    st = api->CreateSessionOptions(&opt);
    if (take_status(api, st, error, "CreateSessionOptions")) { g_free(dev_label); return NULL; }
    if (intra_threads > 0) { OrtStatus *s2 = api->SetIntraOpNumThreads(opt, intra_threads); if (s2) api->ReleaseStatus(s2); }
    { OrtStatus *s2 = api->SetInterOpNumThreads(opt, 1); if (s2) api->ReleaseStatus(s2); }
    { OrtStatus *s2 = api->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL); if (s2) api->ReleaseStatus(s2); }
    { OrtStatus *s2 = api->SetSessionLogSeverityLevel(opt, 3); if (s2) api->ReleaseStatus(s2); }
    st = api->CreateSession(o->env, model_path, opt, &sess);
    api->ReleaseSessionOptions(opt);
  }
  if (take_status(api, st, error, "CreateSession")) { g_free(dev_label); return NULL; }

  OrtAllocator *alloc = NULL;
  st = api->GetAllocatorWithDefaultOptions(&alloc);
  if (take_status(api, st, error, "GetAllocatorWithDefaultOptions")) {
    api->ReleaseSession(sess);
    g_free(dev_label);
    return NULL;
  }
  char *in_name = NULL, *out_name = NULL;
  st = api->SessionGetInputName(sess, 0, alloc, &in_name);
  if (take_status(api, st, error, "SessionGetInputName")) {
    api->ReleaseSession(sess);
    g_free(dev_label);
    return NULL;
  }
  st = api->SessionGetOutputName(sess, 0, alloc, &out_name);
  if (take_status(api, st, error, "SessionGetOutputName")) {
    alloc->Free(alloc, in_name);
    api->ReleaseSession(sess);
    g_free(dev_label);
    return NULL;
  }
  OrtMemoryInfo *mem = NULL;
  st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem);
  if (take_status(api, st, error, "CreateCpuMemoryInfo")) {
    alloc->Free(alloc, in_name);
    alloc->Free(alloc, out_name);
    api->ReleaseSession(sess);
    g_free(dev_label);
    return NULL;
  }
  SkimOrtSession *s = g_new0(SkimOrtSession, 1);
  s->ort = o;
  s->session = sess;
  s->mem = mem;
  s->in_name = g_strdup(in_name);
  s->out_name = g_strdup(out_name);
  s->device = dev_label;
  alloc->Free(alloc, in_name);
  alloc->Free(alloc, out_name);
  return s;
}

void skim_ort_session_free(SkimOrtSession *s) {
  if (!s) { return; }
  const OrtApi *api = s->ort->api;
  if (s->session) { api->ReleaseSession(s->session); }
  if (s->mem) { api->ReleaseMemoryInfo(s->mem); }
  g_free(s->in_name);
  g_free(s->out_name);
  g_free(s->device);
  g_free(s);
}

const char *skim_ort_session_device(const SkimOrtSession *s) {
  return s ? s->device : NULL;
}

const char *skim_ort_session_input_name(const SkimOrtSession *s) {
  return s ? s->in_name : NULL;
}
const char *skim_ort_session_output_name(const SkimOrtSession *s) {
  return s ? s->out_name : NULL;
}

gboolean skim_ort_run(SkimOrtSession *s, const float *in,
                      const int64_t *dims, int nd,
                      float **out, int64_t *out_dims, int *out_nd,
                      GError **error) {
  g_return_val_if_fail(s != NULL && in != NULL && out != NULL, FALSE);
  const OrtApi *api = s->ort->api;
  size_t count = 1;
  for (int i = 0; i < nd; i++) { count *= (size_t)dims[i]; }

  OrtValue *iv = NULL;
  OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
      s->mem, (void *)in, count * sizeof(float), dims, (size_t)nd,
      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv);
  if (take_status(api, st, error, "CreateTensorWithDataAsOrtValue")) {
    return FALSE;
  }
  const char *in_names[1] = { s->in_name };
  const char *out_names[1] = { s->out_name };
  OrtValue *ov = NULL;
  st = api->Run(s->session, NULL, in_names, (const OrtValue *const *)&iv, 1,
                out_names, 1, &ov);
  api->ReleaseValue(iv);
  if (take_status(api, st, error, "Run")) { return FALSE; }

  OrtTensorTypeAndShapeInfo *info = NULL;
  st = api->GetTensorTypeAndShape(ov, &info);
  if (take_status(api, st, error, "GetTensorTypeAndShape")) {
    api->ReleaseValue(ov);
    return FALSE;
  }
  size_t ond = 0;
  st = api->GetDimensionsCount(info, &ond);
  if (take_status(api, st, error, "GetDimensionsCount")) {
    api->ReleaseTensorTypeAndShapeInfo(info);
    api->ReleaseValue(ov);
    return FALSE;
  }
  if (ond > 8) { ond = 8; }
  st = api->GetDimensions(info, out_dims, ond);
  api->ReleaseTensorTypeAndShapeInfo(info);
  if (take_status(api, st, error, "GetDimensions")) {
    api->ReleaseValue(ov);
    return FALSE;
  }
  size_t ocount = 1;
  for (size_t i = 0; i < ond; i++) { ocount *= (size_t)out_dims[i]; }
  float *data = NULL;
  st = api->GetTensorMutableData(ov, (void **)&data);
  if (take_status(api, st, error, "GetTensorMutableData")) {
    api->ReleaseValue(ov);
    return FALSE;
  }
  *out = g_malloc(ocount * sizeof(float));
  memcpy(*out, data, ocount * sizeof(float));
  *out_nd = (int)ond;
  api->ReleaseValue(ov);
  return TRUE;
}
