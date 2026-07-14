# Skimmer for Linux — project context

Instructions and context for Claude Code working in this repo. (Richard's global
`~/.claude/CLAUDE.md` rules also apply — consent before major/irreversible
changes, work in Czech with Richard, etc.)

## What this is

A native **GTK4 multi-channel skimmer** for Linux — CW first, then RTTY and PSK
(BPSK31/63). It is a **TCI client**: it pulls a wideband IQ stream from the
**TCI server in [`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux)**,
channelizes it, decodes every signal in the segment in parallel, and feeds spots
back to the radio panadapter and to the RBN. The full plan is in
[`docs/SCOPE.md`](docs/SCOPE.md) — read it first.

## Ground rules

- **Language: C** (GTK4/libadwaita front-end, GLib-only headless engine), built
  with **meson**. All of OK1BR's Linux apps are C so DSP/render code is shared
  across projects. Do not introduce Rust/Go/Python as a primary language.
- **Don't reimplement DSP — reuse WDSP** (in-tree `vendor/wdsp`, same policy as
  `sdr-for-linux`: a copy, not a submodule; `VENDOR.md` records upstream +
  pinned commit). Use its FFT + `create_resample`.
- **Engine is headless and GLib-only** (`src/engine/`) — no GTK in the engine, so
  every milestone has an offline/headless gate binary. GTK4/libadwaita lives only
  in `src/app/`.
- **The channelizer is complex/phase-preserving and mode-agnostic** from the
  start (RTTY/PSK need phase). Decode backends implement the `decode.h` interface.
- **Read-only to the radio.** The skimmer consumes IQ and sends spots only — it
  never keys or changes radio state.
- **RBN feed must never emit unvalidated callsigns** — M4 (validation) gates M6
  (RBN).

## TCI facts that matter (from sdr-for-linux `docs/TCI-SCOPE.md`)

- Server: `ws://<host>:40001`, `PROTOCOL:ExpertSDR3,1.9`.
- IQ: `iq_samplerate:{48,96,192,384};` + `iq_start:0;`. Binary Stream header
  `type=0`, float32, 2 ch, `length = frames×2`.
- **Orientation = complex CONJUGATE of the DDC feed** (a +12 kHz DDC tone lands
  at −12 kHz on the wire) — conjugate on ingest.
- `iq_samplerate` is device-global radio state, announced in the init block.
- Spots back: `SPOT:call,mode,freq,ARGB,text;` / `SPOT_DELETE:call` /
  `SPOT_CLEAR`. A click on the radio issues `rx_clicked_on_spot:0,0,call,hz`.

## Status

**M2 — polyphase channelizer, offline-verified 2026-07-15.** `channelizer.c`
is a 2×-oversampled PFB (WDSP `fir_bandpass` prototype, fftw3f, hop M/2,
per-channel rings); measured −109 dBc adjacent isolation, phase preserved,
0.9 % of one core for the full 192 k segment. `vendor/wdsp` is a **SUBSET**
(fir/resample/impulse_cache + headers; rnnoise/specbleach header-only stubs) —
see `vendor/wdsp/VENDOR.md` before touching it. Gates: `skimmer-wdsp-smoke`,
`skimmer-chan-test`, all in `meson test`.
M1 (TCI client) is live-verified against a real radio except the **panadapter
eyeball orientation check — still awaiting Richard's verdict** ("station above
centre ⇒ positive offset in `skimmer-tci-probe`").
Next: **M3** — CW decode backend (`decode_cw.c`, `skimmer-cw-test` on recorded
CW, A/B against fldigi/CW Skimmer).

## Layout

```
src/engine/   headless, GLib-only:
  tci_client   WS client, IQ ingest, conjugate, outgoing SPOT
  channelizer  polyphase filter bank → complex baseband per channel
  decode.h     backend interface: channel → { text, confidence, freq, wpm/baud }
  decode_cw    CW backend (phase 1); later decode_rtty, decode_psk
  station      per-frequency station tracker
  callsign     extraction + validation (RBN-grade)
  spot_out     TCI SPOT feed + RBN telnet feed
src/app/      GTK4/libadwaita: main.c, window (station list + log), later waterfall
vendor/wdsp/  in-tree WDSP copy (FFT + resampler)
docs/SCOPE.md the plan
```
