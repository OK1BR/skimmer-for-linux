# Skimmer for Linux — scope & plan

Goal: a native Linux **multi-channel skimmer** that decodes *every* signal in a
band segment in parallel — the free-software counterpart of **SDC** (UT4LW) and
**CW Skimmer** (VE3NEA), but native GTK4/C on Linux. It is a **TCI client**: it
pulls a wideband IQ stream from our own **[`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux)**
TCI server, decodes it, and feeds spots back to the radio panadapter and to the
Reverse Beacon Network.

Author: Richard Fakenberg, **OK1BR**. Licence: GPL-3.0-or-later.

This file says what the skimmer **is** and **why** it is built the way it is —
decisions with their rationale, the measured numbers behind them, rejected
approaches, and the contracts with the sibling apps. What is open lives in
[GitHub Issues](https://github.com/OK1BR/skimmer-for-linux/issues); how each
piece was built is the git history (the last diary-style version of this file:
commit 825b170). The neural CW engine has its own record: `docs/DEEPCW.md`.

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
  `type=0` (IQ), float32, 2 ch, `length = frames×2`. **The wire carries TRUE
  spectrum orientation** — the server conjugates its RF-inverted raw HPSDR DDC
  feed on send (the ExpertSDR convention; "a +12 kHz DDC tone appears at −12 kHz
  on the wire" is relative to the raw DDC feed, *not* to RF). Clients must NOT
  conjugate on ingest — that mirrors the band around the DDC centre (live-caught
  2026-07-15). `iq_samplerate` is device-global radio state announced in the
  init block.
- **Spots** (F6d-2e): `SPOT:call,mode,freq,ARGB,text;` / `SPOT_DELETE:call` /
  `SPOT_CLEAR` render callsign labels on the panadapter; a click issues
  `rx_clicked_on_spot:0,0,call,hz` and tunes the radio. 192-entry store, dedup by
  callsign, 10-min TTL, re-announce refreshes.

**We build here (the skimmer):** TCI *client* → wideband channelizer → pluggable
decode backends → callsign extraction/validation → spot output (TCI + RBN) + a
light native UI.

## Architecture

```
 TCI WS client ──► IQ block (192/384k float32, true orientation as received;
    │                        the block's centre stamped by the SDR — iq_stamp)
    │
    ├─► spectrum tap (spectrum.c) ──► waterfall rows (M8) — independent of the channelizer
    │
    ├─► polyphase channelizer ──► N narrow COMPLEX baseband channels
    │        (complex, phase-preserving — RTTY/PSK need phase, not just magnitude)
    │
    ├─► tone splitter (opt-in) — two carriers in ONE channel → a slot per carrier
    │
    ├─► pluggable decode backend per active channel / slot
    │        decode.h:  channel(complex baseband) → { text, confidence, freq, wpm/baud }
    │        · decode_cw_v2   the CW default: soft-decision semi-Markov Viterbi
    │        · decode_cw      the classical v1 (SKIM_CW_ENGINE=v1)
    │        · decode_deepcw  neural, ONNX Runtime through ort_shim — docs/DEEPCW.md
    │        · decode_rtty    45.45 Bd / 170 Hz Baudot (M7)
    │        · decode_psk     planned — issue #16
    │
    ├─► callsign extraction + validation  (structure + ITU allocation + CW context + MASTER.SCP)
    │
    ├─► station tracker  (freq / callsign / SNR / WPM / first-last-heard; ghost merge)
    │
    ├─► dup query  (UDP to log-for-linux :2238 — NEW / B4 / DUP / INV → spot and pane colours)
    │
    └─► output:  SPOT back to the sdr-for-linux panadapter · local telnet feed (:7300)
                 · own window: waterfall + callsign column + decode pane
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
- **`waterfall.c`** — its palette table, interpolation and percentile
  noise-floor auto-range are copied into the M8 waterfall, so the two apps
  colour a band alike.
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
  the existing panadapter and a click tunes.
- Its own window is light: the M8 waterfall with a callsign column, and the
  decode pane below it. (The original station list was removed on 2026-09-05,
  Richard's call, once the column carried everything it showed.)
- A telnet spot feed in the CW Skimmer dialect — LOCAL clients only, by
  decision (M6) — which still drives a *robust* callsign validator: we must
  not spot garbage.

## Milestones

Every milestone ships an offline/headless gate binary (`skimmer-*-test`) plus,
where relevant, a live check against a running `sdr-for-linux`. The labels
M0…M8 are cited from source comments — keep them.

- **M0 — scaffold.** `meson` project, GPLv3, engine skeleton (GLib-only,
  headless) + a minimal GTK4/libadwaita window.
- **M1 — TCI client + IQ ingest. Orientation live-verified 2026-07-15 the
  hard way:** the first live run decoded real stations mirrored
  around the DDC centre (out-of-band CW spots) — the client was conjugating a
  wire that already carries true orientation (see the TCI facts above). Fixed:
  ingest is pass-through. WebSocket client (libwebsockets),
  handshake (`protocol:ExpertSDR3,…` → `ready;` → `start;`), `iq_samplerate` +
  `iq_start:0`, reassemble binary Stream `type=0` blocks, **verify the wire
  orientation** (codified: a station +12 kHz above centre arrives at +12 kHz on
  the wire; no client-side conjugate), print IQ stats / a raw spectrum.
  Reuse: libwebsockets, piHPSDR `tci.c` reference. Done as
  `src/engine/tci_client.c` (own LWS service thread, text split on `;`,
  byte-stream Stream reassembly so WS fragmentation is invisible, IQ passed
  through as received, dds tracked live, outgoing text queue that M5's spot()
  already rides).
  Since 2026-09-05 the client also sends `iq_stamp:1;` after `iq_start`, takes
  the stamped centre over the `dds` label and splits a block where the centre
  changes inside it (M8), and parses `trx`/`tune` into a TX-state callback
  (TX hold). Gates: `skimmer-tci-test` (a mock TCI server, incl. the
  orientation correlation — +12 kHz stays +12 kHz, image < −40 dB — and the
  spot wire format); live probe `skimmer-tci-probe [host] [port] [rate] [secs]`.
- **M2 — polyphase channelizer.**
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
- **M3 — CW decode backend (the classical v1).** Per-channel
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
  Gate `skimmer-cw-test`: exact copy 15–35 WPM; 12 dB SNR, ±15 % jitter and
  10 dB QSB copy with ≤ 2 errors; 20 s of noise emits nothing.
- **M4 — callsign extraction + validation.** Prefix/suffix regex + known-call
  dictionary + plausibility
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
  structurally-valid token is never spotted.
  Gate `skimmer-call-test` (labelled corpus + fuzz: E/T noise babble and
  random alnum single mentions spot nothing).
- **M5 — spot feeder.** Valid call on a frequency → `SPOT:…` back over TCI
  (renders on the `sdr-for-linux` panadapter, click tunes). Three layers.
  `station.c`: tracker keyed by call with the ghost rule
  — the same call within 300 Hz merges and the STRONGER report positions the
  station (adjacent-channel splatter of a big signal folds back into one spot).
  `spot_out.c`: per-call dedup (re-spot after 180 s or a >150 Hz QSY), global
  token-bucket rate limit, sinks = TCI client + callback (gates now, RBN M6).
  `pipeline.c`: the engine assembled — the TCI client's LWS thread queues IQ
  blocks (bounded, drops counted), the engine thread channelizes, walks every
  channel through decoder + extractor, folds into the tracker and offers to
  the spot feeder; the bank (and per-channel state) rebuilds if the device IQ
  rate changes mid-run.
  Gate `skimmer-spot-test`: tracker + policy units, then the WHOLE chain over
  a real WebSocket — a mock TCI server streams a synthesized two-station band,
  the pipeline spots BACK, and the mock asserts both calls to the Hz, zero
  bogus calls, zero dropped blocks.
- **M6 — telnet spot feed. LOCAL-ONLY by decision.** The RBN does not take
  spots from a skimmer directly — the
  Aggregator (closed, Windows-only .NET, undocumented uplink protocol)
  connects TO the skimmer's telnet server (the CW Skimmer convention,
  default port 7300) and relays. Richard decided 2026-07-15 NOT to feed the
  RBN network (no Aggregator under Wine); the server instead serves LOCAL
  cluster clients — loggers like BRlog — while staying
  Aggregator-compatible in dialect. `rbn_feed.c` is that server: a GLib/GIO GSocketService on its
  own GMainContext thread (login handshake, any number of clients,
  non-blocking writes — a stalled client is dropped, the Aggregator
  reconnects), broadcasting classic cluster lines
  `DX de OK1BR-#: 7032.0 DL1ABC CW 25 dB 22 WPM CQ 1234Z`. The feed is
  app-owned so aggregator sessions ride out TCI reconnects; Preferences
  gained an RBN group (enable / operator callsign / port, persisted under
  `[rbn]`), the status line shows the port + client count. Policy: a second
  `spot_out` instance — the RBN is ALWAYS CQ-only (independent of the local
  panadapter switch) and gated at callsign score ≥0.85 (vs 0.70 locally:
  repetition, dictionary or DE+CQ context required, a single unmarked copy
  is never fed), re-spot 600 s / QSY 100 Hz / 5 per s.
- **CW decoder v2 — soft-decision semi-Markov Viterbi (the PIPELINE
  DEFAULT since 2026-08-04 — Richard's
  call after the 2026-08-01 contest session ran it live all day. The
  classical v1 stays in the tree behind `SKIM_CW_V1=1`; the flip is
  measured on the 600 s YOTA-contest replay — v2 tables 19 stations to
  v1's 12, v1 misses the segment's loudest signal outright (LZ5R, 39 dB,
  673 v2 reports), mutates SN1T→IN1T and mints the phantom TM00TFR;
  v2 costs ~50 % more CPU, 52× realtime vs 79×).** v1's plumbing (envelope, trackers, squelch, tone offset)
  carries a new decision layer: per-sample mark/space log-likelihoods (a
  span discriminator that FOLLOWS QSB + a noise-anchored Rayleigh term
  that tells a −18 dB in-dash dropout from a real space) feed a Viterbi
  over {dit, dah} × {element/char/word space} segments with log-normal
  duration priors tied to the adaptive dit; lag-committed traceback emits
  chars live. Solid channels ride out envelope-gate dips (the µ_m/µ_s
  ratio knows a fade from silence — v1's gate tears "9A170NT" apart at
  exactly that point). Gate: the M3 suite runs for BOTH backends, plus two
  v2-only cases — element-eating QSB (16 dB @ 0.31 Hz: v1 dist 14, v2
  dist 1) and sub-dit flutter notches (v1 dist 5, v2 dist 0). Replay A/B
  on the recorded corpus: "oper" — v1 tables the mutilated "9A1G" (21
  reports), v2 the true **9A170NT** (121 reports, 0.90, CQ); contest A/B —
  same core stations, lone-E/T noise 15.6 → 11.6 %, 3 extra weak-signal
  calls each, ~60× realtime (v1 ~90×).
- **Tone splitter — two stations in ONE channel decode separately
  (2026-07-16; opt-in `SKIM_TONE_SPLIT=1` until a live session confirms it).**
  Motivation: the 14036 slot (live 2026-07-15) —
  two carriers < 60 Hz apart share a channel, their envelopes beat and the
  decoder mutates BOTH calls. `tone_split.c` watches each channel's
  Welch-averaged spectrum (64-pt FFT, 2 s EMA); when it resolves ≥2
  carriers ≥ 20 Hz apart it opens a SLOT per carrier (phase-continuous NCO
  to ~0 Hz + 31-tap windowed-sinc lowpass, cutoff riding the spacing:
  clamp(0.55·Δf, 10, 32) Hz) and the pipeline runs a separate
  decoder + extractor + frequency lock per slot (slot-major arrays;
  arbitration works on the effective in-channel offset, so all M5 ghost
  rules carry over). Keying sidebands look like carriers (hard 50 % keying:
  first pair ~4 dB down) — a peak whose mirror about a stronger carrier
  holds comparable power is dropped, so a lone loud station never splits
  against itself. Two lines closer than 20 Hz overlap in keying bandwidth —
  linear filters cannot part them (that would take joint demod / SIC, a
  possible later stage): the slot goes CONTESTED, its text still shows but
  breeds no callsign candidates — the beat mutations stop reaching spots.
  Single-carrier channels ride a sample-exact passthrough (legacy path
  bit-identical; unarmed, the splitter is not even built).
  Slot TTL 90 s — a slot survives the other side's over. Gate
  `skimmer-split-test` runs for BOTH CW backends.
- **Clickable callsigns → logbook prefill (Richard, 2026-08-01).** A left
  click on any whitespace-delimited pane token that validates as a callsign
  (ends trimmed of punctuation/over marks; drag-select does not fire) behaves
  exactly like a panadapter spot click: `skim_pipeline_tune` +
  `clicked_on_spot:call,hz;` over TCI with the pane's pinned slot frequency
  (exact carrier, not the 100 Hz-stepped VFO). `log-for-linux` prefills its
  Call entry from it and needs the exact frequency (`hz > 0`) for its
  QSY-away staleness check (the prefill is dropped when the VFO wanders
  > 200 Hz off the spot); it fills only an EMPTY Call entry, by design. The
  server side had to learn the relay: sdr-for-linux's `tci_server.c` accepts
  `clicked_on_spot` / `rx_clicked_on_spot` from any client and rebroadcasts
  both forms to every client.
- **Dup-aware spot & decode colouring via the logbook (Richard, 2026-08-01).**
  The operator must see at a glance which spotted calls are already worked,
  so he does not click duplicates.
  operator must see at a glance which spotted calls are already worked, so
  he does not click duplicates. `log-for-linux` now runs a read-only UDP
  lookup service on `127.0.0.1:2238` (always on while the logbook runs;
  implemented + live-verified 2026-08-01):
  request `DUP? <call> <freq_hz> <mode>` (single datagram, UTF-8, e.g.
  `DUP? 9A8A 14025000 CW`) → reply `NEW <call>` / `B4 <call>` /
  `DUP <call>` sent back to the requester. `DUP` = call+band+mode already
  logged in the logbook's ACTIVE CONTEST (band derived from `freq_hz` on
  the log side); `B4` = worked before at any time; `NEW` = not in the log.
  Malformed requests get NO reply — treat a ~1 s timeout as "unknown" and
  keep the default color (logbook not running must never break spotting).
  Skimmer work: query when a validated callsign becomes a station/spot
  (small TTL cache, say 60 s, so the decode pane does not re-ask per
  frame), then (a) choose the `SPOT:` ARGB by verdict — worked/dup dimmed
  (e.g. gray), NEW stays the current bright color — the panadapter needs
  no changes, dedup-by-callsign recolors the label and the existing 180 s
  re-announce keeps it fresh; and (b) tint the decode-pane callsign
  highlight the same way. Invalidate the cache entry for a call after the
  operator logs it (simplest: short TTL is enough — a just-logged call
  flips to DUP on the next re-announce/query). Do NOT read the logbook's
  SQLite directly — the dup rule and active-contest context live in the
  logbook, the UDP answer is the contract.
  As built (`src/engine/dup_query.c`): a connected non-blocking UDP client,
  60 s answer TTL, 2 s re-ask suppression for unanswered calls; every failure
  mode collapses to UNKNOWN = default colour (answers carry a trailing
  newline; malformed requests get silence). The pipeline owns one instance
  (the cache rides out reconnects); `spot_out` asks with a 5 ms budget and
  colours the SPOT ARGB via the shared `skim_spot_argb_for_dup` rule, the
  pane highlight asks with a 0 ms budget (the GTK thread never blocks).
  **Push:** the moment a QSO is logged (or deleted/edited so a call's verdict
  changes) the logbook sends the STANDARD answer datagram UNSOLICITED from the
  :2238 socket to every peer with a valid `DUP?` in the last 10 min (max 8).
  No new protocol: the skimmer parses it exactly like an answer — the cache
  flips, and a colour-changing flip repaints the live panadapter label AT
  ONCE (a resend of the last emission with only the ARGB changed; the
  dedup/re-announce schedule untouched; only labels fresher than the radio's
  10 min spot TTL) and re-tints the decode pane within 2 s. Do NOT write TCI
  `spot:` from the logbook — two writers of one label race (the skimmer's
  re-announce would repaint green until its cache expires). Gate
  `skimmer-dup-test`.
- **INV verdict — contest-invalid stations gray out like dups (Richard's
  priority 2026-08-08, mid-WAE).** The logbook's :2238 dup service has a
  FOURTH verdict (log-for-linux `8093437`):
  `8093437`): `INV <call>` = under the ACTIVE CONTEST's rules no valid
  QSO with this station is possible at all — e.g. WAE scores only
  EU↔non-EU, so for OK1BR every EU station answers INV; EUHFC is the
  inverse (non-EU → INV). The logbook resolves country/continent from
  cty.dat and knows each contest's verified rule; the skimmer must NOT
  re-derive any of that (the UDP answer stays the whole contract — same
  reasoning as "do not read the logbook's SQLite"). Skimmer work is
  deliberately small: treat `INV` exactly like the existing worked/dup
  path everywhere a verdict lands — parser (query answers AND
  unsolicited pushes may both carry it), TTL cache, `SPOT:` ARGB via
  `skim_spot_argb_for_dup` (gray, same as DUP — the operator meaning is
  identical: do not call), decode-pane underline tint. An unknown
  verdict string from a NEWER logbook must keep collapsing to UNKNOWN =
  default color, never crash the parser (that tolerance is why INV can
  ship on the logbook side first). Extend gate `skimmer-dup-test`:
  The "gray" decision has ONE truth, `skim_dup_verdict_gray()` in
  `dup_query.h` — the parser's flip rule and `skim_spot_argb_for_dup` both
  call it, so a future verdict is added in exactly two lines.
- **TX hold — decoding freezes during the operator's own transmission
  (2026-08-15).** Reported by Richard live, first RTTY QSO attempts: "when I
  answer a call, after my over it hangs and doesn't decode for a while."
  Mechanism (verified from the radio side): while the SDR transmits, its RX
  is deliberately deafened (T/R relay + both step attenuators at 31 dB — TX
  protection), so the whole band disappears from the IQ stream for the length
  of the over. Every acquired RTTY channel then rides its release logic (~2 s
  under the bar), and when the answering station comes back right after
  unkey, acquisition must re-converge first → the first seconds of the reply
  are lost; CW trackers suffer the same physics. Radio side (sdr-for-linux
  cc470af): `trx:0,true/false;` reports the REAL keyed state (any RF: MOX,
  TUNE, CW and RTTY text keying — before, only the MOX button), broadcast by
  the 500 ms reporter; worst-case ~500 ms key-on latency sits comfortably
  inside the ~2 s release bar. As built: the pipeline swallows blocks while
  held plus a 0.3 s post-TX settle grace (`HOLD_GRACE_S`), capped at 30 s
  (`HOLD_CAP_S` — a stuck trx must not freeze the skimmer forever); an
  optional `resync` backend hook resets framers at resume (RTTY and DeepCW
  implement it); `skim_pipeline_set_tx_hold` is public for the offline
  harness; the spectrum tap is fed BEFORE the hold check — the waterfall
  follows the data, not the flag (M8 below: a muted stream pauses it).
  Measured on the wire 2026-09-19 (SAC CW, eight overs recorded): the IQ is
  EXACT zeros for the length of an over, not an attenuated band; the hold
  engaged 0.04–0.43 s after the zeros began and released 0.30–0.79 s after
  they ended (the 500 ms poll, plus the grace) — so the decoders still see up
  to ~0.4 s of dead stream before every freeze and lose up to ~0.8 s of the
  answer after it. Measured in the RTTY gate: WITH the hold the reply decodes
  complete from its first character and nothing decodes from the held band;
  WITHOUT it 77 garbage decodes leak during the own-TX silence and the
  reply's head is lost. Live: the 2026-08-23 contest day logged 54 clean
  hold/release pairs, none unpaired. **The hold has a BEGIN hook too (gh#18,
  2026-09-19):** a backend that commits behind the live edge — DeepCW's 1 s
  tail guard — used to lose that tail at every hold (the swallowed blocks
  never bring its right context, `resync` then abandons it), which in search
  and pounce is the call of the very station being answered. The optional
  `hold_begin` backend hook fires when the hold engages, and while it lasts
  the pipeline pumps such a backend with zero-frame `process()` calls every
  4th swallowed block, so flushed text reaches the pane, the extractor and
  the station table DURING the transmission. The wire's mute is itself an
  event in the data: a band cut to exact zeros between two samples is a
  broadband click in every channel (a dit, to a decoder). Rules and numbers:
  `docs/DEEPCW.md`, "TX hold".
- **A GNOME-correct About dialog — the family contract (written down
  2026-08-04 at Richard's request, across every app of the family; built
  2026-08-08).** Every app must open
  the same kind of About from its primary menu, and its strings must agree
  with what the `.desktop` entry and the AppStream metainfo already say —
  one truth about the app, not three. The contract, in `AdwAboutDialog`
  terms (`adw_about_dialog_new`, NOT the deprecated `AdwAboutWindow`;
  shown with `adw_dialog_present`): `application_icon` = the GApplication
  id, which is also the installed icon's file name — get that wrong and
  the dialog shows a generic gear; `application_name`; `version` from the
  meson project version (one source of truth); `developer_name`
  "Richard Fakenberg, OK1BR"; `copyright`; `license_type`
  `GTK_LICENSE_GPL_3_0`; `comments` — the same one-liner the metainfo
  carries; `website` + `issue_url`; `debug_info` with versions and paths,
  so a bug report can be pasted straight from its Copy button; and an
  acknowledgement section wherever third-party code is vendored (here:
  WDSP). The menu item is the LAST one in the primary menu, "About
  Skimmer for Linux", per the GNOME HIG.
  **The version must be findable FROM THE UI** (Richard, 2026-08-04): a
  `--version` flag on the command line does NOT satisfy this. Someone who
  launched the app from the app grid must be able to see which version he
  is running without leaving it — the About dialog is that place. A CLI
  flag is welcome on top, never instead.
  As built: the header bar's primary menu (hamburger) holds Preferences, then
  "About Skimmer for Linux" LAST; `debug_info` carries GTK + libadwaita
  runtime versions, TCI host, telnet-feed state, the CW engine, the
  settings / MASTER.SCP / decode-log paths. On top (never instead):
  `--version` / `-v`, answered in `handle-local-options` so it prints from the
  LOCAL process and exits — it can never activate (raise) a running instance.
- **Hysteresis on the reported spot frequency (2026-08-08).**
  `skim_spot_out_emit()` quantised with no memory:
  `out_hz = round(freq_hz / rh) * rh` (`src/engine/spot_out.c`). A station
  whose frequency estimate wanders across a grid boundary therefore gets a
  DIFFERENT reported frequency on every re-announce (180 s) although it never
  moved — the panadapter label jumps a whole grid step and the telnet feed
  carries a spot that disagrees with the one before it. `qsy_hz` (30 Hz) does
  not cover this: it gates whether a NEW emission happens at all, not which
  value a scheduled re-announce carries. **Only with the grid switched on** —
  "Frequency step" defaults to `Exact` (0), where the reported value follows
  the estimate and that is exactly what we want (the label converges onto the
  true carrier — see the `qsy_hz` comment); the grid choices are 10/20/50/100 Hz.
  Fix: keep the last emitted `out_hz` in `SpotMemo` and re-quantise only when
  the raw estimate leaves the reported cell by more than half a step plus a
  small margin — otherwise re-send the value already on the label. The raw
  `freq_hz` stays untouched; it is the QSY policy's input.
  `skim_spot_out_recolour()` must then resend the STORED reported value
  instead of re-quantising — today it recomputes from the memo's raw value and
  happens to agree, but after this change recomputing would undo the
  hysteresis on every repaint.
  Gate: extend `skimmer-spot-test` with a station parked on a grid boundary —
  estimate jittering a few Hz across it, several re-announces — and assert the
  reported frequency is emitted once and never alternates.
  Origin: the idea (not the code) comes from e04's DeepCW and s53zo's SO2R
  fork of it, whose pileup tracker keeps an internal EMA frequency and a
  separate *reported* one that is updated only past half a bin (widths past
  0.75 bin). **Principle only — neither repository carries any licence, so
  none of their code may be copied into this GPLv3 tree.**
  As built: `SpotMemo` carries `out_hz` + `out_rh` (the grid it was quantised
  to); emit re-sends the stored value unless the raw estimate sits > ¾ step
  from it OR the grid setting changed; `Exact` is untouched (follows the
  estimate, no memory); recolour resends the stored `out_hz`. The gate's
  boundary-parked station FAILS on the pre-fix code — the alternation is what
  it catches.
- **M7 — RTTY backend (2026-08-15, mid-contest; live-verified the same
  morning — real contest spots on the telnet feed, 11 % CPU at
  192 k / 768 channels).** Known leaks, tunable on the recorded fixture: a
  non-45.45 digimode passes the squelch as sustained garbage; FT8-band
  single-char leaks.
  `decode_rtty.c` implements `decode.h` for
  45.45 Bd / 170 Hz-shift Baudot: a Hann-periodogram pair finder (the WEAKER
  tone scores, so a lone carrier can never acquire; sub-bin centre from
  floor-subtracted tone centroids) → two NCOs riding the tracked centre
  ±85 Hz into one-bit moving-sum matched filters with per-tone peak
  normalisation (ATC — a mark faded 10 dB below the space still slices) →
  TWO start-bit-anchored UARTs on y and −y (every character re-syncs on its
  own start edge; whichever polarity sustains valid framing is elected, so
  reversed signals copy) → ITA2 letters + US-TTY figures, unshift-on-space.
  Layered squelch, each layer gate-measured: pair above the floor AND
  mark/space envelope ANTI-correlation (true FSK −0.8…−1.0, two unrelated
  carriers 170 Hz apart hover at 0; open < −0.35, close > −0.15, moments
  primed neutral at acquire) AND valid-frame EMA (0.55/0.35) AND a fast
  per-char presence gate (τ 60 ms) covering the ~2 s between a transmitter
  stopping and the slow periodogram release, plus a 0.7 s post-acquisition
  settle embargo. Chars framed while the squelch proves are buffered and
  flushed on open — over heads are not eaten. The channelizer grew
  `skim_channelizer_new_ex(rate, spacing, passband, taps)`: RTTY needs a
  passband ≥ 85 + spacing/2 so BOTH tones of a station anywhere between
  channel centres land in one channel — the RTTY bank is 250 Hz spacing,
  ±225 Hz cutoff, K = 16 (the critical +415 Hz alias onto the −85 Hz space
  tone measures −124 dBc). Pipeline: `SkimPipelineConfig.mode` picks
  backend, bank geometry and the mode string on stations/spots/dup queries;
  the tone splitter/focus env vars are CW-only (an FSK pair IS two carriers
  to the splitter). App: Preferences → Decoding → Mode (CW/RTTY, persisted
  `[decode] mode`, change reconnects the engine), speed column and tuned
  header show Bd, About debug_info carries the mode.
  Gates: `skimmer-rtty-test` (hardcoded ITA2 bit vectors as the independent
  table witness, offsets, figures/UOS, reversed polarity, AWGN, QSB, selective
  fade, squelch on noise / keyed CW / two carriers, the worst-case straddler
  through the real wide bank, the WHOLE offline pipeline in RTTY mode) and
  `skimmer-chan-test`.
  **Open: the first ~2 s of every over are lost** — four measured causes, the
  approved fix is a pre-roll replay: issue #6.
- **PSK backend** (BPSK31 + BPSK63, Costas loop, varicode) — planned, issue #16.
- **M8 — waterfall view + callsign column (2026-09-05, shipped in v0.4.0).**
  Roy Andre Løntjern, LB0EI, keeps Windows for CW Skimmer's pileup display: a
  waterfall with frequency VERTICAL and time flowing sideways, a kHz scale,
  and a column of callsigns to the right of it, each on its own frequency,
  click to tune. Richard adopted exactly that layout (CW Skimmer's
  `ContestShot.gif` as the reference). **The TX half of Roy's workflow is NOT
  in scope here:** in a split pileup the click has to move the TX frequency,
  and sdr-for-linux has no split — its TCI `vfo:rx,ch,f` handler ignores the
  channel index, `split_enable`/`rit_*`/`xit_*` are echo-only (a split was
  built there on 2026-09-06 and removed the same day on Richard's call).
  **Engine (`spectrum.c`, GLib + fftw3f):** an FFT tap on the raw IQ band,
  independent of the channelizer — a picture must show keying, so the window
  must be shorter than a dit, which the 125 Hz channels never are. Two named
  constants define it: the target bin `SKIM_SPECTRUM_BIN_HZ` = 23.4375 Hz (N
  derived from the rate — 2048/4096/8192/16384 at 48/96/192/384 k, so the bin
  is the same whatever `iq_samplerate` the radio announces) and
  `SKIM_SPECTRUM_HOP_DIV` = 4 (a row every N/4 frames = 10.7 ms, 93.75 rows/s
  at every rate; 42.7 ms Hann window at 192 k). Both are first guesses for
  Richard's live look. Forward FFT, fftshifted rows, byte = dBFS + 200
  (sdr-for-linux's waterfall convention shifted to full-scale; a −6 dBFS tone
  reads 194, a −60 dBFS noise floor ~95). Pipeline: `skim_pipeline_set_spectrum_cb`
  + `skim_pipeline_set_spectrum_enabled` (atomic, default OFF — no FFT for a
  hidden view); the tap is fed at the top of `process_block`, BEFORE the TX
  hold check; the object is built lazily on the engine thread at the block's
  rate (fftw's planner is not thread-safe — same thread as the channelizer's
  plan).
  **The picture pauses on a muted stream (gh#17, 2026-09-19).** Live in SAC
  CW the waterfall went white on every own over and needed ~4 s to settle,
  the over itself standing in it as a black band. Measured on a recording of
  eight overs: the wire carries exact zeros during TX, such a row reads
  −200 dBFS, the view's floor tracker (20th percentile, EMA 0.01/row) fell
  from −127 to −191…−198 dBFS within one over, and every 1 dB of that drift
  recoloured the WHOLE history against the sunken floor. The pause therefore
  hangs off the DATA, inside the tap: a window of nothing but exact zeros
  yields no row, and a row straddling a mute edge is computed from its live
  part alone through the retune cut path (fresh Hann, floor renormalised;
  zero runs under N/32 stay in the full window, under a hop of live signal
  no row). Same recording after: tracked floor −127.0…−125.9 dBFS over all
  300 s, no row below −128, decode output identical. Rejected by
  measurement: pausing on the TX hold (the operator's first idea) — the
  `trx` flag trails the zeros by 0.04–0.43 s, ~40 dead rows per over would
  still drag the floor ~24 dB (computed: 1 − 0.99⁴⁰ of the 73 dB gap), and at
  release it would cost 0.3–0.8 s of live picture; rows without the edge cut — a chopped window smeared the
  strongest line across the row only 15 dB down and moved the floor 2.8 dB.
  A server that keeps sending a deafened band through TX (none seen) would
  need the flag after all. Live-verified the same evening in the contest
  (Richard): the picture pauses for the over and continues; the gap carries
  no marker, and he did not ask for one.
  **The view (same day, headless-verified).** `src/app/wf_compose.c` is the
  GLib-only history + composer (gate-tested): full-resolution rows in a ring
  (`SKIM_WF_HISTORY_ROWS` 2048 ≈ 22 s, 16 MB at 192 k), a pannable/zoomable
  window composed into 0xAARRGGBB pixels — frequency VERTICAL with the highest
  at the top, time to the RIGHT, and MAX-pooling in both axes (several bins per
  pixel row when zoomed out, `SKIM_WF_ROWS_PER_PX` = 2 history rows per column
  = 21 ms/px), so a 50 Hz CW line never averages away; bins are centred on
  their frequency (the + 0.5 the gate caught: without it every line sat half a
  bin high). Palette table, interpolation and the percentile noise-floor
  auto-range are copied from sdr-for-linux's `waterfall.c` (`SKIM_WF_SPAN_DB`
  40 above the 20th-percentile floor, EMA τ ≈ 1 s) so the two apps colour a
  band alike. `src/app/wf_view.c` is the GTK4 widget: pixels go up as a
  `GdkMemoryTexture` (NEAREST, sdr's snapshot pattern), new rows shift the
  picture left and compose only the new columns (17 µs for two), a pan, zoom,
  resize or a > 1 dB floor drift recomposes everything (5.4 ms for the whole
  192 k band on 700×600); Cairo draws the kHz scale (CW Skimmer style — the
  kHz's last three digits, tick ladder 100 Hz … 50 kHz picked for ≥ 26 px),
  the VFO as a spot-green line + tab, and the (still empty) callsign column.
  Wheel pans the frequency window, Ctrl+wheel zooms (2 kHz … the band), and
  the scale strip can be grabbed and dragged (grab cursor; the label under
  the pointer travels with it). **A retune moves ONLY the green marker**
  (Richard at the first live look: the window must never jump under his
  eyes); when the VFO leaves the window an arrow at the top or bottom of the
  scale says which way. **The history lives in ABSOLUTE frequency** (second
  live-look fix, same day: "you reset my waterfall"): sdr-for-linux has no
  CTUN, so every retune moves the IQ centre — the first cut cleared the
  history and recentred on every tune. Now each row carries its own bin
  shift against the grid anchor (the first centre seen), the composer
  subtracts it per row, old rows stay exactly where they were, new rows land
  shifted, and the window stands; a tone the retune pushed out of the band
  shows in the old columns and floor in the new ones (gated, 56 checks). The
  window recentres on the VFO only when the new band no longer overlaps it
  at all (a band change), and only a rate change (new bin width) clears the
  history.
  **Retunes — the waterfall flows through them.** Every retune moves the IQ
  centre, and the centre label (`dds:`) rides the TCI control channel while
  the IQ rides the data channel — a row placed on a stale label is drawn a
  whole tuning step off. The final design, after four rejected ones: **the
  SDR stamps every IQ block with its centre** (`iq_stamp:1;`, a family TCI
  extension — h[8] = centre of the block's first frame, h[9] = frame offset
  of a change inside the block, h[10] = the centre from there on; opt-in,
  because whether SDC / CW Skimmer tolerate non-zero reserved header words is
  unverified), the boundary placed by a capture clock on the SDR side
  (sdr-for-linux 98c57de); `tci_client` splits a block at h[9]; the spectrum
  tap labels every row with the centre at its window MIDDLE, and a row whose
  window holds a centre boundary is computed from its LARGEST single-centre
  segment only (a fresh Hann over that segment, the rest zeroed, the floor
  renormalised) — a discrete step shows a 2-column widening of a line instead
  of a bar. Servers without stamps degrade to label-at-block-arrival (±1
  block of jitter, inherent). Rejected on the way, each after a live look or
  a measurement: clearing and recentring the history on every tune ("you
  reset my waterfall"); a retune guard that dropped rows until the centre had
  stood still for 0.7 s (the picture paused while the knob turned); a
  constant label delay line (`SKIM_WF_LABEL_LAG` — no constant removes ±1
  block of jitter; the default is 0 now, `SKIM_WF_LAG_ROWS` stays for
  experiments); an explicit guard band around the boundary (measured
  unnecessary — the fresh Hann tapers to zero there, 58 dB down). Live
  verdict (Richard, 2026-09-05): on a real band the waterfall flows through a
  retune with no teeth, no bars, no pause; dragging at 50–100 kHz/s a line is
  inherently ~4× wider while the knob turns — the physics of a 10.7 ms hop.
  **The callsign column.**
  CW Skimmer's layout to the letter of the brief: a rail down the column's
  left edge, a yellow dot on it at every tracked station's frequency, the
  callsign beside it with a "CQ " prefix for callers, and a connector from
  the dot to the text — flat while the label sits on its frequency, slanted
  once a crowd pushed it off. The seating is a pure function in
  `wf_compose.c` so the spectrum gate covers it: labels keep frequency
  order (a higher station's label never drops below a lower's); a crowd is
  seated by isotonic regression on anchor − k·pitch (pool adjacent
  violators), which is the least-squares answer to "consecutive centres
  ≥ pitch apart" and makes a cluster spread SYMMETRICALLY about its mean
  anchor instead of piling downward; runs at the edges slide inside; and
  when more labels want seats than the column has, the lowest-priority ones
  hide (fixed station > CQ > SNR) while their dots stay. Colours are the
  spot colours through `skim_dup_verdict_gray` — pane, panadapter and column
  can never disagree; the pane's fixed station is bold; hover gives a faint
  backdrop and the pointer cursor. The widget gets a snapshot of the station
  table (CQ and S&P alike) at the end of any drain that touched the table or
  the fixation, on the view switch, and à 2 s for verdict recolours. A click
  on a label is the panadapter-spot gesture: tune to the station's TRACKED
  frequency (its carrier, not the pixel's Hz), `clicked_on_spot` over TCI so
  the logbook prefills (the pane-click path), the pane fixed on the station;
  a press that travelled more than 4 px is a drag, not a click.
  The label's tooltip carries kHz / speed / dB / heard / age (a dB printed
  after the call was tried and taken out again on Richard's look). With the
  column carrying all of it, **the station list was deleted** (Richard,
  2026-09-05). **App plumbing:** rows travel the ONE event queue (at most
  `SPEC_PENDING_MAX` 48 pending — the oldest row is dropped, a stalled UI must
  not hoard 1.5 MB/s), one `queue_draw` per drain; the engine computes
  spectrum rows ONLY while the waterfall shows. Preferences → Display → Colour
  scheme lists the same six schemes as sdr-for-linux, applies live and
  persists as `[ui] palette`. **`SKIM_IQ_FILE=<cf32>`** replays a recording
  into the UI through the offline pipeline at real-time pace (its `.meta`
  sidecar or `SKIM_IQ_RATE`/`SKIM_IQ_CENTER`), looping — no radio needed for a
  look or a demo.
  **Not built, on purpose:** a polling-server fallback (settle when labels
  arrive in ≥ 400 ms steps) — one click followed by stillness is
  indistinguishable from a polling label, so any cadence detector would bring
  the pause back on his most common gesture; smoothing of a per-label SNR — an
  EMA is an unmeasured lever. **Shelved as polish, not to be offered again
  without a visible symptom (Richard, 2026-09-06):** coherent re-centring of
  the window (works only if the radio's DDC NCO is phase-continuous across a
  retune — measure the phase jump on a strong stable carrier first, never
  assume) and the sub-row `SDRFL_DDC_LAT_MS` latency. Waiting for a contest
  look: bin / hop / span and the crowded fan-out (issues #9, #10). One lesson
  from the live evening: never import a script with radio side effects — a
  peek script imported the TCI test client, whose `main()` ran at import and
  sent `vfo:0,0,4;` to the live radio; the client has a main guard now.

## Safety / etiquette

Read-only against the radio, with one deliberate exception: the skimmer
*consumes* IQ, *sends spots*, and — only when the user clicks a callsign (the
waterfall column or the decode pane) —
*tunes* (`vfo:0,0,<hz>`; added 2026-07-15 at Richard's request). It never keys
and never changes radio state on its own (no TRX/TUNE/CW from here). The RBN
feed must never emit unvalidated callsigns — M4 gates M6. Richard's global rule
applies: consent before any major/irreversible step.
