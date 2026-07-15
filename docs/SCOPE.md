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
- **M2 — polyphase channelizer. IMPLEMENTED (offline-verified 2026-07-15).**
  Wideband IQ → N narrow **complex** channels via a polyphase filter bank
  (WDSP FFT / resampler). Gate: `skimmer-chan-test` —
  synthetic multi-tone input, verify per-channel isolation + alias rejection,
  measure CPU (target: whole CW segment well under one core).
  Done as `src/engine/channelizer.c`: 2×-oversampled PFB — M = rate/spacing
  channels, K·M-tap prototype (WDSP `fir_bandpass`, BH4, Σh-normalised), hop
  M/2, backward fftw3f FFT (channel c ⇔ +c·spacing in true orientation),
  (−1)^c fix on odd hops, per-channel 8 s output rings with a dropped counter.
  WDSP vendored as a **subset** (fir/resample/impulse_cache + all headers +
  header-only rnnoise/specbleach stubs — decided with Richard 2026-07-15, the
  full mirror would be ~95 MB of NN weights a skimmer never runs;
  `vendor/wdsp/VENDOR.md` has provenance + the extend/re-sync procedure),
  smoke-gated by `skimmer-wdsp-smoke`. Measured at 48 k/125 Hz (M = 384):
  adjacent channels −109 dBc, mirror −302 dBc, ±30 Hz in-channel offsets
  recovered to 0.01 Hz (phase preserved for RTTY/PSK), channel-edge tone −6 dB
  in both straddlers; at the real 192 k/125 Hz geometry (M = 1536) the whole
  segment channelizes in **0.9 % of one core**.
- **M3 — CW decode backend. IMPLEMENTED (synthetic gate 2026-07-15; the
  off-air A/B vs fldigi/CW Skimmer awaits a recorded capture).** Per-channel
  envelope → adaptive threshold → dot/dash timing → adaptive WPM → Morse;
  HMM/Bayes for a ragged fist (planned refinement — v1 is classical).
  Implements `decode.h`. Gate: `skimmer-cw-test` on synthetic CW.
  Done as `decode_cw.c`: |IQ| envelope (3-tap MA) → dual-rate trackers (peak
  attack/0.8 s release; floor = EMA of the below-midpoint samples, i.e. the
  quiet-state MEAN — a min-follower reads Rayleigh noise as signal) → Schmitt
  keying (on 0.55/off 0.30 of the span) → pending-run classifier with blip
  folding (a sub-glitch dropout resumes the interrupted run, discarded noise
  pings re-bridge the space they split) → adaptive dit (EMA; clustering
  bootstrap) → live char emission at 2.2 dits / word space at 5.5 → Morse LUT.
  Squelch is layered: peak>4×floor with hysteresis (close at 2.6× — a single
  threshold flaps during word gaps and eats the following char) AND a
  keying-likeness test (fraction of samples near the peak: CW ≈ its duty
  cycle, noise ≈ 4 % — peak ratio alone cannot tell a weak signal from noise).
  Estimates per event: WPM, SNR, confidence, and the tone offset inside the
  channel from the marks' phase slope (M5 refines spot frequencies with it).
  Gate results: exact copy 15–35 WPM; 12 dB SNR, ±15 % jitter and 10 dB QSB
  copy with ≤2 errors; 18→28 WPM re-locks; 20 s of noise emits nothing; and
  end-to-end through the real channelizer the right channel copies while a
  noise-only channel stays mute. (Adjacent-channel ghosts of very strong
  stations are real signals — the M5 station tracker dedups them.)
- **M4 — callsign extraction + validation. IMPLEMENTED (offline gate
  2026-07-15).** Prefix/suffix regex + known-call dictionary + plausibility
  scoring; suppress garbage (RBN-grade). Gate: `skimmer-call-test` on a
  labelled decode corpus (precision/recall).
  Done as `callsign.c`: a structural parser over the four shapes real calls
  take (single-letter series, two-letter prefix, letter+digit country prefix,
  digit-first prefix) with the ITU allocation encoded where it discriminates —
  the letter+digit table is what kills decode garbage like "T1BR" (T1 is not
  allocated) while passing T77XX, E73ABC, C6AGU, 3DA0RS. Q* is rejected
  outright (Q-codes). Portable designators parse from either side (OK1BR/P,
  F/OK1BR). Extraction is a stateful per-channel tokenizer with CW context:
  scores 0.55 structural + 0.25 DE marker (survives ≤2 garbled tokens) +
  0.10 CQ window + 0.20 repetition (+0.05 at ≥3) + 0.15 known-call dictionary
  (MASTER.SCP format, `skim_callsign_dict_load`), spot threshold 0.70 — a lone
  structurally-valid token is never spotted. Gate: 26 real calls accepted,
  18 garbage shapes rejected, labelled corpus at precision 1.0 / recall 1.0,
  token continuity across fragmented feeds, dictionary boost, and 2×4000-token
  fuzz (E/T noise babble; random alnum single mentions) with zero spots.
- **M5 — spot feeder + light UI. IMPLEMENTED (offline pipeline gate
  2026-07-15; the live panadapter check awaits Richard at the radio).** Valid
  call on a frequency → `SPOT:…` back over TCI (renders on the `sdr-for-linux`
  panadapter, click tunes) + the station-list window + decode log. Gate: live —
  spots appear on the radio panadapter and a click tunes correctly.
  Done in three layers. `station.c`: tracker keyed by call with the ghost rule
  — the same call within 300 Hz merges and the STRONGER report positions the
  station (adjacent-channel splatter of a big signal folds back into one spot).
  `spot_out.c`: per-call dedup (re-spot after 180 s or a >150 Hz QSY), global
  token-bucket rate limit, sinks = TCI client + callback (gates now, RBN M6).
  `pipeline.c`: the engine assembled — the TCI client's LWS thread queues IQ
  blocks (bounded, drops counted), the engine thread channelizes, walks every
  channel through decoder + extractor, folds into the tracker and offers to
  the spot feeder; the bank (and per-channel state) rebuilds if the device IQ
  rate changes mid-run. The GTK app is the light UI: host + connect toggle,
  frequency-sorted station list (call/kHz/WPM/SNR/heard), tailing decode log,
  1 Hz status line; engine events marshalled via g_idle_add. Offline gate
  `skimmer-spot-test` (20 checks): tracker + policy units, then the WHOLE
  chain over a real WebSocket — a mock TCI server streams a synthesized
  two-station 48 kHz band, the pipeline spots BACK, and the mock asserts both
  calls at ±30 Hz absolute (measured: exact to the Hz), zero bogus calls
  (RBN precision end to end), zero dropped blocks.
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
