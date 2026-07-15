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
- **Read-only to the radio — except an explicit user tune.** The skimmer
  consumes IQ and sends spots; it never keys and never changes radio state on
  its own. The single deliberate write is `vfo:0,0,<hz>` when the USER
  activates a station row (added 2026-07-15 at Richard's request).
- **RBN feed must never emit unvalidated callsigns** — M4 (validation) gates M6
  (RBN).

## TCI facts that matter (from sdr-for-linux `docs/TCI-SCOPE.md`)

- Server: `ws://<host>:40001`, `PROTOCOL:ExpertSDR3,1.9`.
- IQ: `iq_samplerate:{48,96,192,384};` + `iq_start:0;`. Binary Stream header
  `type=0`, float32, 2 ch, `length = frames×2`.
- **The wire carries TRUE spectrum orientation — do NOT conjugate on ingest.**
  The server conjugates its RF-inverted raw HPSDR DDC feed on send (that is the
  ExpertSDR convention SDC/CW Skimmer consume as-is). The TCI-SCOPE line
  "a +12 kHz DDC tone lands at −12 kHz on the wire" is relative to the raw DDC
  feed, *not* to RF. A client-side conjugate mirrors every frequency around the
  DDC centre — live-caught 2026-07-15 (spots landed out of band).
- `iq_samplerate` is device-global radio state, announced in the init block.
- Spots back: `SPOT:call,mode,freq,ARGB,text;` / `SPOT_DELETE:call` /
  `SPOT_CLEAR`. A click on the radio issues `rx_clicked_on_spot:0,0,call,hz`.

## Status

**M3 — CW decoder, synthetic gate green 2026-07-15.** `decode_cw.c` v1
(classical, HMM later): envelope → mean-floor tracker → Schmitt keying →
blip-folding run classifier → adaptive dit → Morse LUT; layered squelch
(peak/floor hysteresis + keying-duty test) keeps noise channels MUTE. Copies
15–35 WPM exact; 12 dB SNR / 15 % jitter / 10 dB QSB ≤2 errors. Gates:
`skimmer-cw-test` (+ `skimmer-chan-test`, `skimmer-wdsp-smoke`, tci) all in
`meson test`. `vendor/wdsp` is a **SUBSET** — read `vendor/wdsp/VENDOR.md`
before touching it.
**M5 — the skimmer skims (offline-verified 2026-07-15).** `pipeline.c`
assembles the engine (TCI → bank → per-channel decoders/extractors → tracker
→ spots) on its own thread; `station.c` merges same-call reports within
300 Hz (stronger SNR positions — ghost dedup); `spot_out.c` dedups (180 s /
150 Hz QSY) + token-bucket rate limit. GTK app = station list + decode log
+ connect toggle (engine events marshalled by g_idle_add; engine stays
GTK-free). Gate `skimmer-spot-test`: full chain over a real WebSocket vs a
mock TCI server — spot frequencies exact to the Hz, zero bogus calls. Gates
now 6 in `meson test` (tci, wdsp, chan, cw, call, spot-pipeline).
**M6 — telnet spot feed (gate green 2026-07-15; LOCAL-ONLY by decision).**
`rbn_feed.c` = a CW-Skimmer-dialect telnet SERVER (default port 7300) on its
own GMainContext thread; app-owned, so client sessions survive TCI
reconnects. Feed policy is a second `spot_out`: ALWAYS CQ-only + score
≥0.85 (stricter than the 0.70 panadapter gate), re-spot 600 s / QSY 100 Hz.
Preferences → Telnet spot feed (enable, operator call, port;
`settings.ini [rbn]`). Gate `skimmer-rbn-test` (26 checks) — 7 gates total
in `meson test`. **Richard decided 2026-07-15: NO feed to the RBN network**
— the only sanctioned uplink is the closed Windows-only Aggregator (no
Wine here) and its protocol is undocumented. The server is a LOCAL cluster
source for loggers (BRlog); the dialect stays Aggregator-compatible should
that ever change.
Still pending live: **M3 off-air A/B**. MASTER.SCP can go to
`~/.config/skimmer-for-linux/master.scp` (the app loads it if present).
Next: **HMM/Viterbi CW decoder v2** (QSB mutations — the recorded "oper"
sample is the corpus), RTTY/PSK backends.

## Layout

```
src/engine/   headless, GLib-only:
  tci_client   WS client, IQ ingest (true orientation), outgoing SPOT
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
