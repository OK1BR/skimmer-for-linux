# vendor/onnxruntime — ONNX Runtime C API header (header only)

- Upstream: https://github.com/microsoft/onnxruntime
- File: `include/onnxruntime/core/session/onnxruntime_c_api.h` at tag
  **v1.21.0** (`ORT_API_VERSION 21`), copied verbatim 2026-09-12.
- Licence: MIT (`LICENSE` beside it, Microsoft Corporation).

Why a copy: the skimmer never LINKS ONNX Runtime. `src/engine/ort_shim.c`
`dlopen`s `libonnxruntime.so.1` at run time (or `SKIM_ORT_LIB`), asks
`OrtGetApiBase()->GetApi(21)` and uses only that API table, so the binary
builds and runs on a machine without the runtime — the DeepCW engine then
reports "not available" instead of failing the build or the launch. The
header is the only build-time need, and API version 21 is the oldest one a
distribution we care about ships (Debian trixie: `libonnxruntime1.21`;
Fedora 1.26; Arch 1.29; the newer libraries answer `GetApi(21)` — verified
against 1.30.0). Do not bump the version casually: `GetApi(N)` returns NULL
on a library older than N and the engine silently becomes unavailable.
