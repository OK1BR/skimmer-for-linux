# Vendored RNNoise — HEADER ONLY

**Only `include/rnnoise.h` (+ `COPYING`) is vendored.** WDSP's `comm.h`
unconditionally includes `rnnr.h`, which includes `rnnoise.h` — so the header
must exist to compile *any* WDSP translation unit. The skimmer does **not**
build `rnnr.c` and does **not** link the RNNoise library (the trained model is
~75 MB of NN weights a skimmer never runs — see `vendor/wdsp/VENDOR.md`).

| | |
|---|---|
| Upstream | https://github.com/xiph/rnnoise (Jean-Marc Valin / Xiph) |
| Vendored via | `sdr-for-linux` `vendor/rnnoise/include/rnnoise.h` ← piHPSDR `974acba` |
| Vendored on | 2026-07-15 |
| License | BSD (see `COPYING`) — GPL-compatible |

If the skimmer ever needs the neural NR (it should not), mirror the full
`sdr-for-linux/vendor/rnnoise` instead and build `rnnr.c` back into WDSP.
