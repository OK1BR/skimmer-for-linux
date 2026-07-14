# Skimmer for Linux — scope & plan

Goal: a native Linux **multi-channel skimmer** that decodes *every* signal in a
band segment in parallel — the free-software counterpart of **SDC** (UT4LW) and
**CW Skimmer** (VE3NEA), but native GTK4/C on Linux. It is a **TCI client**: it
pulls a wideband IQ stream from our own **[`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux)**
TCI server, decodes it, and feeds spots back to the radio panadapter and to the
Reverse Beacon Network.

Author: Richard Fakenberg, **OK1BR**. Licence: GPL-3.0-or-later.

## Why this exists

CW Skimmer (Windows, closed source) is the reference tool a whole segment of the
hobby is built on — RBN, contest skimming, propagation research. On Linux there
is no native equivalent. `sdr-for-linux` already exposes a proven, ExpertSDR-
compatible **TCI server with a wideband IQ stream** (verified live 2026-07-10
against both SDC and CW Skimmer). That makes the hard half — getting clean,
correctly-oriented wideband IQ out of the radio — *already done*. This project is
the other half: the decoder and the spot pipeline.

## The two halves — what already exists vs. what we build

**Already done, in `sdr-for-linux` (do not rebuild):**
- TCI server on `ws://<host>:40001`, `PROTOCOL:ExpertSDR3,1.9`
  (`src/tci_server.c`, milestone F6d-2).
- **IQ stream** (F6d-2d, LIVE-VERIFIED with SDC + CW Skimmer): `iq_samplerate`
  `{48,96,192,384}k`, `iq_start:0` / `iq_stop:0`. Binary Stream frames, header
  `type=0` (IQ), float32, 2 ch, `length = frames×2`. **Orientation is the
  ExpertSDR convention = complex CONJUGATE of the HPSDR DDC feed** (a +12 kHz DDC
  tone appears at −12 kHz on the wire). `iq_samplerate` is device-global radio
  state announced in the init block.
- **Spots** (F6d-2e): `SPOT:call,mode,freq,ARGB,text;` / `SPOT_DELETE:call` /
  `SPOT_CLEAR` render callsign labels on the panadapter; a click issues
  `rx_clicked_on_spot:0,0,call,hz` and tunes the radio. 192-entry store, dedup by
  callsign, 10-min TTL, re-announce refreshes.

**We build here (the skimmer):** TCI *client* → wideband channelizer → pluggable
decode backends → callsign extraction/validation → spot output (TCI + RBN) + a
light native UI.

## Architecture

```
 TCI WS client ──► IQ block (192/384k float32, complex conjugate)
    │
    ├─► conjugate/normalise to true spectrum orientation
    │
    ├─► polyphase channelizer ──► N narrow COMPLEX baseband channels (~50–500 Hz)
    │        (complex, phase-preserving — RTTY/PSK need phase, not just magnitude)
    │
    ├─► pluggable decode backend per active channel
    │        decode.h:  channel(complex baseband) → { text, confidence, freq, wpm/baud }
    │        · decode_cw   (phase 1)
    │        · decode_rtty (phase 2)
    │        · decode_psk  (phase 3: BPSK31, BPSK63)
    │
    ├─► station tracker  (freq / callsign / SNR / WPM / first-last-heard)
    │
    ├─► callsign extraction + validation  (prefix regex + known-call dictionary + plausibility)
    │
    └─► output:  SPOT back to sdr-for-linux panadapter   ·   RBN telnet feed   ·   local list + log
```

### Key design decision: the channelizer is mode-agnostic and complex

The first mode is CW (on/off keying — an envelope in an FFT bin would suffice).
But RTTY (mark/space FSK) and especially PSK (BPSK, needs a Costas/PLL on the
carrier) require **phase**. So the channelizer emits a **decimated complex I/Q
stream per channel** from day one, not an FFT-magnitude waterfall. Building it
CW-only would force a rewrite at PSK. CW-first is the *first backend on shared
infrastructure*, not a dead end.

### Reuse from `sdr-for-linux` (same in-tree vendoring policy, GPLv3)

- **WDSP + fftw3f** — the FFT and the `create_resample` resampler (the very call
  the TCI server uses to decimate IQ per client). In-tree copy under
  `vendor/wdsp` (a copy, not a submodule — matches `sdr-for-linux`). Decision:
  vendor the whole WDSP block first (bezbolestné), prune later if worth it.
- **`waterfall.c` / `panadapter.c`** — the pure-Cairo renderer, when we add a
  full skimmer panorama (a later phase; the feeder model doesn't need it early).
- **libwebsockets** — TCI client transport.

### Reference code (studied, not linked)

- **piHPSDR `tci.c`** — the original is a TCI *client*; direct reference for our
  client side (RX audio / IQ / control). `sdr-for-linux` adapted it into a
  server, so we walk it the other direction.
- **fldigi** (GPL) — single-channel CW / RTTY / PSK31 decoders to study before
  writing the multi-channel versions.
- **SDC (UT4LW)** and **CW Skimmer (VE3NEA)** — decode-quality benchmarks.

## Scope of "the whole band"

The TCI ceiling is 384 kHz of IQ — a protocol limit, not the radio's. That is
ample for a mode segment: the CW subband (e.g. 7000–7040) is ~40 kHz, so 192k
covers it with margin. "All traffic on the band" means the whole CW (later RTTY /
PSK) segment decoded at once — not the entire 3.5/7/14 MHz allocation.

## Output model: feeder + light UI (decided 2026-07-15)

The skimmer is a **feeder**, not a second full SDR window:
- Decodes and pushes `SPOT:…` back into `sdr-for-linux`, where labels render on
  the existing panadapter and a click tunes (F6d-2e is done — we get it for free).
- Its own window is a **light station list** (callsign / freq / mode / WPM|baud /
  SNR / first-heard / last-heard) plus a decode log.
- **RBN telnet feed is a goal** (native Linux RBN nodes are scarce) — this drives
  a *robust* callsign validator; we must not spot garbage.
- A full own-panorama waterfall (à la CW Skimmer) is a later, optional phase once
  the decoder is good; `waterfall.c` can seed it.

## Milestones (each independently testable, in the `sdr-for-linux` house style)

Every milestone ships an offline/headless gate binary (`skimmer-*-test`) plus,
where relevant, a live check against a running `sdr-for-linux`.

- **M0 — scaffold.** `meson` project, GPLv3, docs, engine skeleton (GLib-only,
  headless) + a minimal GTK4/libadwaita window. Gate: `meson compile` is clean;
  the empty app launches.
- **M1 — TCI client + IQ ingest. IMPLEMENTED (offline-verified 2026-07-15;
  live probe run the same evening against a real SDR-for-Linux on 80 m:
  handshake + 192 kHz stream ok, effective rate −0.02 %; the eyeball
  orientation check against the panadapter awaits Richard's verdict).** WebSocket client (libwebsockets),
  handshake (`protocol:ExpertSDR3,…` → `ready;` → `start;`), `iq_samplerate` +
  `iq_start:0`, reassemble binary Stream `type=0` blocks, **verify the conjugate
  orientation** (codify the wire convention: a +12 kHz DDC tone must land at
  −12 kHz; conjugate to recover true spectrum), print IQ stats / a raw spectrum.
  Reuse: libwebsockets, piHPSDR `tci.c` reference. Done as
  `src/engine/tci_client.c` (own LWS service thread, text split on `;`,
  byte-stream Stream reassembly so WS fragmentation is invisible, Q negated on
  ingest, dds tracked live, outgoing text queue that M5's spot() already rides).
  Offline gate: `skimmer-tci-test` — mock TCI server, 16 checks incl. the
  orientation correlation (+12 kHz recovered, image < −40 dB) and the spot
  format. Live gate: `skimmer-tci-probe [host] [port] [rate] [secs]` — prints
  handshake, IQ stats (effective vs. nominal rate), top spectrum peaks + an
  ASCII panorama in true orientation; the eyeball check against the panadapter
  (station above centre ⇒ positive offset) is the orientation verdict, since
  the skimmer is read-only and cannot key a reference tone.
- **M2 — polyphase channelizer.** Wideband IQ → N narrow **complex** channels via
  a polyphase filter bank (WDSP FFT / resampler). Gate: `skimmer-chan-test` —
  synthetic multi-tone input, verify per-channel isolation + alias rejection,
  measure CPU (target: whole CW segment well under one core).
- **M3 — CW decode backend.** Per-channel envelope → adaptive threshold → dot/dash
  timing → adaptive WPM → Morse; HMM/Bayes for a ragged fist. Implements
  `decode.h`. Gate: `skimmer-cw-test` on recorded CW; A/B against fldigi and
  CW Skimmer on the same off-air capture.
- **M4 — callsign extraction + validation.** Prefix/suffix regex + known-call
  dictionary + plausibility scoring; suppress garbage (RBN-grade). Gate:
  `skimmer-call-test` on a labelled decode corpus (precision/recall).
- **M5 — spot feeder + light UI.** Valid call on a frequency → `SPOT:…` back over
  TCI (renders on the `sdr-for-linux` panadapter, click tunes) + the station-list
  window + decode log. Gate: live — spots appear on the radio panadapter and a
  click tunes correctly.
- **M6 — RBN telnet feed.** Emit validated spots to the Reverse Beacon Network
  (telnet spot format), rate-limited and de-duplicated. Gate: local telnet sink
  captures well-formed, deduped spots; then a supervised live RBN feed.
- **Later — RTTY backend** (FSK 45.45 bd, Baudot/ITA2), **PSK backend**
  (BPSK31 + BPSK63, Costas loop, varicode), and an optional **own-panorama
  waterfall** (port `waterfall.c`).

## Safety / etiquette

Read-only against the radio: the skimmer only *consumes* IQ and *sends spots*; it
never keys or changes radio state (no TRX/TUNE/CW/DDS from here). The RBN feed
must never emit unvalidated callsigns — M4 gates M6. Richard's global rule
applies: consent before any major/irreversible step.
