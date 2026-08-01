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
 TCI WS client ──► IQ block (192/384k float32, true orientation as received)
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
- **M1 — TCI client + IQ ingest. IMPLEMENTED; orientation LIVE-VERIFIED
  2026-07-15 the hard way:** the first live run decoded real stations mirrored
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
  Offline gate: `skimmer-tci-test` — mock TCI server, 16 checks incl. the
  orientation correlation (+12 kHz stays +12 kHz, image < −40 dB) and the spot
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
- **M6 — telnet spot feed. IMPLEMENTED (gate 2026-07-15); LOCAL-ONLY by
  decision.** The RBN does not take spots from a skimmer directly — the
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
  is never fed), re-spot 600 s / QSY 100 Hz / 5 per s. Gate
  `skimmer-rbn-test` (26 checks): handshake + line format + multi-client
  broadcast against a local telnet sink, then the OFFLINE pipeline over a
  synthesized three-station band — the two CQ callers arrive at the exact
  kHz ONCE each (dedup across two band passes), the S&P answerer is tracked
  locally but NEVER hits the wire, zero unvalidated lines.
- **CW decoder v2 — soft-decision semi-Markov Viterbi. IMPLEMENTED
  (offline 2026-07-15; opt-in `SKIM_CW_V2=1` until live A/B flips the
  default).** v1's plumbing (envelope, trackers, squelch, tone offset)
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
- **Tone splitter — two stations in ONE channel decode separately.
  IMPLEMENTED (offline 2026-07-16; opt-in `SKIM_TONE_SPLIT=1` until a live
  session confirms it).** Motivation: the 14036 slot (live 2026-07-15) —
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
  bit-identical; unarmed, the splitter is not even built). Gate
  `skimmer-split-test` (46 checks, BOTH CW backends): sideband immunity,
  Δf 50/30 Hz both texts copy, Δf 15 Hz contested + never split, slot TTL
  collapse/re-engage (90 s — a slot survives the other side's over), and
  the whole offline pipeline with two stations in one channel — both calls
  tracked to the Hz, zero mutations.
- **Clickable callsigns in decoded text → logbook prefill (requested by
  Richard 2026-08-01; IMPLEMENTED offline the same day, live check
  pending).** The decode view already highlights callsigns; make them
  clickable. A click behaves exactly like a panadapter spot click in
  `sdr-for-linux`: tune the radio to the station (the row-activation
  `vfo:0,0,<hz>` path already exists) and announce the click over TCI
  (`rx_clicked_on_spot`/`clicked_on_spot` with call + exact frequency in Hz)
  so `log-for-linux` reacts instantly — it prefills its Call entry, pulls its
  window forward and focuses the field with the call selected. That logbook
  side is implemented and live as of 2026-08-01 and needs the spot's exact
  frequency (`hz > 0`) in the message for its QSY-away staleness check
  (prefill is dropped when the VFO wanders > 200 Hz off the spot).
  The open point resolved by reading the server: it did NOT relay a
  client-sent click (its parser knew only `spot`/`spot_delete`/`spot_clear`;
  the click broadcast was wired to its own panadapter alone), so the relay
  was added to `sdr-for-linux`'s `tci_server.c` — both forms accepted from
  any client, rebroadcast to every client as `rx_clicked_on_spot` + legacy
  (its `sdrfl-tci-test` covers the relay). Skimmer side: a left click on
  any whitespace-delimited pane token that validates as a callsign (ends
  trimmed of punctuation/over marks; drag-select does not fire) issues
  `skim_pipeline_tune` + the new `skim_pipeline_spot_clicked` →
  `clicked_on_spot:call,hz;` with the pane's pinned slot frequency (exact
  carrier, not the 100 Hz-stepped VFO); a hand cursor over clickable calls
  is the affordance. Gate: `skimmer-tci-test` checks the wire format. Live
  (skimmer click → radio tunes + logbook prefills) still unverified — needs
  the rebuilt `sdr-for-linux` server running.
- **Dup-aware spot & decode coloring via the logbook (requested by Richard
  2026-08-01; logbook side DONE, skimmer side IMPLEMENTED the same day —
  gate `skimmer-dup-test`, live look pending).** Goal: the
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
  Implementation (offline-proven 2026-08-01): `src/engine/dup_query.c` —
  connected non-blocking UDP client, 60 s answer TTL, 2 s re-ask
  suppression for unanswered calls; every failure mode collapses to
  UNKNOWN = default colour (live-probed: answers carry a trailing
  newline; malformed requests get silence). The pipeline owns one
  instance (pipeline-lifetime — the cache rides out reconnects);
  `spot_out` emit asks with a 5 ms budget and colours the SPOT ARGB via
  the shared `skim_spot_argb_for_dup` rule (`SKIM_SPOT_ARGB` bright /
  `SKIM_SPOT_ARGB_DUP` gray, spot_out.h); the pane highlight asks with
  a 0 ms budget (`skim_pipeline_dup_verdict`, GTK thread never blocks)
  and tints via a second gray underline tag — verdict sharpening between
  overlapping scans swaps the tag. Gate `skimmer-dup-test` (10 checks:
  verdict round-trips incl. the newline, request wire format, silence →
  UNKNOWN, re-ask suppression, TTL survives the logbook closing, no
  listener → UNKNOWN, the colour rule). Live look 2026-08-01: colours
  work, but a just-logged QSO grayed the panadapter label only on the
  next re-announce (≤180 s) — too slow for contest flow (Richard).
  **PUSH extension (skimmer side IMPLEMENTED; logbook side REQUESTED —
  agent of log-for-linux, please implement the send):** the moment a
  QSO is logged (or deleted/edited so a call's verdict changes), the
  logbook sends the STANDARD answer datagram (`DUP <call>` / `B4 <call>`
  / `NEW <call>`, same format, trailing newline fine) UNSOLICITED from
  the :2238 socket to the source address of every `DUP?` request seen
  recently (last ~10 min is plenty; just the last requester works too).
  No new protocol: the skimmer parses it exactly like an answer — the
  cache flips, and a colour-changing flip (green↔gray) repaints the
  live panadapter label AT ONCE (resend of the last emission with only
  the ARGB changed; dedup/re-announce schedule untouched; only labels
  fresher than the radio's 10 min spot TTL) and re-tints the decode
  pane within 2 s (periodic tail rescan). Do NOT write TCI `spot:`
  from the logbook — two writers of one label race (the skimmer's
  re-announce would repaint green until its cache expires). Gate
  `skimmer-dup-test` covers the push path (unsolicited datagram →
  change queue → cache flip; 15 checks).
- **Later — RTTY backend** (FSK 45.45 bd, Baudot/ITA2), **PSK backend**
  (BPSK31 + BPSK63, Costas loop, varicode), and an optional **own-panorama
  waterfall** (port `waterfall.c`).

## Safety / etiquette

Read-only against the radio, with one deliberate exception: the skimmer
*consumes* IQ, *sends spots*, and — only when the user activates a station row —
*tunes* (`vfo:0,0,<hz>`; added 2026-07-15 at Richard's request). It never keys
and never changes radio state on its own (no TRX/TUNE/CW from here). The RBN
feed must never emit unvalidated callsigns — M4 gates M6. Richard's global rule
applies: consent before any major/irreversible step.
