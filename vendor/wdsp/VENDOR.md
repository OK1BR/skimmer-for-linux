# Vendored WDSP — SUBSET

WDSP is the DSP toolbox we build & link — **we do not reimplement DSP**, and we
**do not modify** these sources (warnings are silenced in `meson.build`, never
patched). Unlike `sdr-for-linux` (which mirrors the whole engine), the skimmer
vendors a **subset**: the FIR designer, the rational resampler and their
closure. The full engine would drag in ~95 MB (rnnoise NN weights + EMNR data)
that a skimmer never runs — decided with Richard 2026-07-15.

## Source of truth

| | |
|---|---|
| Author | WDSP by Warren Pratt (NR0V); Linux/Android port by John Melton (g0orx/n6lyt) |
| Vendored via | `sdr-for-linux` `vendor/wdsp/` (verbatim mirror) ← https://github.com/dl1ycf/pihpsdr subdir `wdsp/` |
| piHPSDR commit | `974acbac07fe7dd3e24f28f3956a9ffb3a1ebaf1` (`974acba`, 2026-07-03) |
| Vendored on | 2026-07-15 |
| License | GPL (see `COPYING`) — compatible with this project's GPL-3.0-or-later |

Each vendored file is byte-identical to the sdr-for-linux mirror at that
commit. The upstream `Makefile` is kept as the reference for the full source
list; `README.md` is upstream's.

## What we build (the subset)

- **All `.h`** — `comm.h` includes every WDSP header unconditionally, so the
  full header set must be present to compile anything.
- **`fir.c`** — `fir_bandpass` designs our channelizer prototype filter
  (Blackman-Harris windowed sinc) and the FIRs of later milestones.
- **`resample.c`** — `create_resample` / `xresample`, the same call the
  sdr-for-linux TCI server uses to decimate IQ per client.
- **`impulse_cache.c`** — fir.c's impulse cache (harmless when uninitialised:
  `get`/`add` are no-ops before `init_impulse_cache`).

Deliberately **not** compiled:

- `utilities.c` / `linux_port.c` — fir/resample need exactly two surfaces from
  them (`malloc0`, the CriticalSection pthread wrappers), but the TUs drag in
  the `txa`/`ch` engine globals and the whole WDSP thread runtime. The two
  surfaces are reimplemented 1:1 in `src/engine/wdsp_port.c` (signatures from
  WDSP's own headers).
- `rnnr.c` / `sbnr.c` and the rest of the engine — the NR chains need the
  vendored rnnoise (75 MB of NN weights) and libspecbleach *libraries*;
  `comm.h` only needs their **headers**, vendored header-only under
  `vendor/rnnoise/` and `vendor/libspecbleach/` (see their VENDOR.md).
- `calculus` / `zetaHat.bin` runtime data — only EMNR reads them; not vendored.

## Extending the subset

Need another WDSP module? Copy the `.c` from the sdr-for-linux mirror (or
piHPSDR `974acba`), add it to `meson.build`, then `meson compile` — undefined
symbols at link time name the next `.c` of the closure. Record additions here.

## Check for upstream updates / re-sync

1. Re-sync `sdr-for-linux/vendor/wdsp` first (its VENDOR.md has the procedure).
2. `diff -r` our subset files against the refreshed mirror; re-copy changed
   ones (headers **all**, `.c` only the vendored three).
3. Update the commit/date above, `meson compile`, run
   `./build/skimmer-wdsp-smoke` and `./build/skimmer-chan-test`.
