# Vendored libspecbleach — HEADER ONLY

**Only `include/specbleach_adenoiser.h` (+ license) is vendored.** WDSP's
`comm.h` unconditionally includes `sbnr.h`, which includes
`<specbleach_adenoiser.h>` — so the header must exist to compile *any* WDSP
translation unit. The skimmer does **not** build `sbnr.c` and does **not**
link the libspecbleach library (see `vendor/wdsp/VENDOR.md`).

| | |
|---|---|
| Upstream | https://github.com/lucianodato/libspecbleach (Luciano Dato) |
| Vendored via | `sdr-for-linux` `vendor/libspecbleach/include/` ← piHPSDR `974acba` |
| Vendored on | 2026-07-15 |
| License | LGPL-2.1 (see license file) — GPL-compatible |

If spectral NR is ever wanted, mirror the full `sdr-for-linux/vendor/
libspecbleach` instead and build `sbnr.c` back into WDSP.
