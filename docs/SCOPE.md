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
  the decoder is good; `waterfall.c` can seed it. *(Pulled forward 2026-09-05 as
  M8 on LB0EI's request, Richard's call — see the M8 entry.)*

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
  (offline 2026-07-15; the PIPELINE DEFAULT since 2026-08-04 — Richard's
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
  is the affordance. Gate: `skimmer-tci-test` checks the wire format.
  Live status (contest evening 2026-08-01): the skimmer SENDS on click and
  the server RELAYS both forms — proven on the wire (an observer client
  captured ~30 real clicks, YL3FT/YT6X/UX0LL/SD7X/…; SKIM_PANE_DEBUG
  traces press→release→sent). The remaining open end is the LOGBOOK
  prefill: log-for-linux fills only an EMPTY Call entry (typed text is
  never overwritten, by design) and its build was being restarted during
  the click tests — one click with an empty Call field against the current
  logbook build still awaits Richard's confirmation.
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
  **PUSH extension (skimmer side IMPLEMENTED; logbook side IMPLEMENTED
  2026-08-01 — `logfl_dup_srv_notify()`, sent from the :2238 socket to all
  peers with a valid `DUP?` in the last 10 min, max 8 peers; fires on QSO
  logged (manual + WSJT-X), delete, and cell edit incl. the OLD identity
  when call/band/mode moved; gate `log-dupq-test` covers the push, live
  query verified, first real logged QSO is the live push check —
  **LIVE-VERIFIED 2026-08-01: a logged QSO grays the panadapter label
  instantly, Richard confirmed**):** the
  moment a
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
  change queue → cache flip; 25 checks since the INV verdict landed).
- **INV verdict — contest-invalid stations gray out like dups.
  (Richard's priority 2026-08-08, mid-WAE; DONE the same day.)** The logbook's :2238
  dup service grew a FOURTH verdict on 2026-08-08 (log-for-linux commit
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
  INV round-trip, INV via push, unknown-verdict tolerance. Without this,
  an EU spot during WAE stays bright green and invites a QSO the
  rules score at zero — the logbook's entry row already warns in red,
  but the operator hunts from the panadapter, hence the priority.
  Implementation (2026-08-08): `SKIM_DUP_INV` in the verdict enum; the
  "gray" decision now has ONE truth, `skim_dup_verdict_gray()` in
  dup_query.h — the parser's flip rule and `skim_spot_argb_for_dup`
  both call it, so a future verdict is added in exactly two lines
  (parse + gray-set membership) and spot ARGB, pane tint, recolour
  pushes all follow. Unknown verdict strings were already dropped
  before the cache (gate-proven now, not just by reading). Gate grew
  15 → 25 checks (INV round-trip/push/cache-survival, unknown-verdict
  answer AND push tolerance, INV colour). Live wire check the same
  afternoon against the running logbook mid-WAE: `DL1AA → INV`,
  `K1AA → NEW`, `OK1BR → INV`, trailing newline as documented. The
  full visual chain (gray label on the live panadapter) is the same
  code path DUP took through its 2026-08-01 live verification; INV
  changes only the string→enum map and the gray set.
  Richard's request, across every app of the family).** Every app must open
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
  **BUILT 2026-08-08.** The header bar's standalone gear button became the
  family primary menu (hamburger, `open-menu-symbolic`, packed where the
  gear sat): Preferences, then "About Skimmer for Linux" LAST — the same
  form as sdr-for-linux, whose About (`src/gui.c`) supplied the field set;
  `debug_info` follows log-for-linux's (GTK + libadwaita runtime versions,
  TCI host, telnet-feed state, settings/MASTER.SCP/decode-log paths —
  pasteable via the dialog's Copy button). Comments line = the metainfo
  `<summary>` = the `.desktop` Comment, verbatim; acknowledgement section
  credits vendored WDSP. On top (never instead): `--version` /`-v` on the
  CLI, answered in `handle-local-options` so it prints from the LOCAL
  process and exits — it can never activate (raise) a running instance.
  Verified: build + all 10 gates green, `--version` prints 0.1.0 while
  the live instance ran undisturbed. The dialog itself is code-true to
  the family reference but NOT yet seen on screen — both family apps were
  live mid-WAE, and a second GApplication instance would either forward
  to or visually poke the operator's session. Look pending after the
  contest (the running binary is the installed pre-About one anyway).
- **The Website field in the repo header. (Written down 2026-08-04 at
  Richard's request, across every one of his projects; DONE for THIS repo
  2026-08-08.)** Every OK1BR repo had that field empty while its README
  already points at [rifak.cz](https://rifak.cz) — so the GitHub sidebar,
  the first place a visitor looks, linked nowhere. Set via
  `gh repo edit OK1BR/skimmer-for-linux --homepage https://rifak.cz` and
  verified by reading it back (`gh repo view --json homepageUrl`). The
  sibling repos keep their own copy of this note — each gets set when
  someone works that project.
- **Hysteresis on the reported spot frequency. (Written down 2026-08-07;
  DONE 2026-08-08.)** `skim_spot_out_emit()` quantised with no memory:
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
  Implementation (2026-08-08): `SpotMemo` carries `out_hz` + `out_rh` (the
  grid it was quantised to); emit re-sends the stored value unless the raw
  estimate sits > ¾ step from it (the "half a step plus a small margin",
  matching the fork's 0.75-bin figure) OR the grid setting changed
  (`out_rh` differs — a preference flip re-quantises on the next
  re-announce instead of leaving the label on the old grid forever);
  `Exact` is untouched (follows the estimate, no memory). Recolour resends
  the stored `out_hz`. Gate `skimmer-spot-test` +5 checks (26 total):
  boundary-parked station with ±few Hz jitter over four 181 s
  re-announces keeps ONE reported value, a real QSY re-quantises at once,
  a grid change re-quantises, Exact follows raw; verified the new check
  FAILS on the pre-fix code (the alternation is what it catches).
- **M7 — RTTY backend. IMPLEMENTED (offline gates 2026-08-15, mid-contest);
  LIVE-VERIFIED the same morning** — real contest spots (LA1TV 14093.2,
  IZ0FVD 14091.5 — `RTTY … 45 BPS CQ` on the telnet feed), clean
  strong-station copy, 11 % CPU at 192 k/768 channels; open fixture-tunable
  leaks are logged in CLAUDE.md (a non-45.45 digimode passes the squelch as
  sustained garbage; FT8-band single-char leaks; a 297 s live capture in
  `/var/tmp/skimmer-iq/` carries all the cases). `decode_rtty.c` implements `decode.h` for
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
  header show Bd, About debug_info carries the mode. Gates:
  `skimmer-rtty-test` (25 checks: hardcoded ITA2 bit vectors as the
  independent table witness, offsets, figures/UOS, reversed polarity, AWGN,
  QSB, selective fade, squelch on noise/keyed-CW/two-carrier, worst-case
  straddler through the real wide bank, and the WHOLE offline pipeline in
  RTTY mode — two CQing stations tabled exactly, mode RTTY, ±4 Hz, no
  phantoms); `skimmer-chan-test` 18 → 25. 11 gates total.
- **OPEN — RTTY over-head startup is confused (measured 2026-08-15 on the
  live capture; a fix is still owed).** Live symptom (Richard, parked on a
  fixed frequency): the first characters of every over are wrong and text
  appears only after a delay. Measured offline on
  `iq-20260815-rtty-live1-192k.cf32` (engine replay + a literal Python port
  of `decode_rtty.c` on the SV1JDZ channel, text-identical to the engine):
  every over head decodes `CQ DE SV1JDZ…` — the first `CQ ` is eaten,
  deterministically. Four stacked causes, by share: (1) the 0.7 s
  **settle embargo** runs before the UARTs are armed, so nothing in that
  window is framed OR buffered — the "over heads are not eaten" pend
  guarantee above only holds from arm onward; with the usual ~0.7 s diddle
  preamble the acquisition hides in the preamble and settle lands exactly
  on the first text chars; (2) **UART sync-in**: the framers arm
  mid-character, a false start bit can frame garbage that passes the stop
  check and flushes from pend as a garbled head (live: `L DEV1JDZ…`);
  (3) **ok_ema reaches re-acquire at ~0** — between TX end and the ~3 s
  periodogram release the framers grind noise and decay it, so the ×0.5
  "keep half" at release is moot and the squelch re-proves 5 valid chars
  every over → OPEN sits at ACQ + 1.53 s (0.7 settle + 5 × 0.165 s) on
  every single over; (4) **acquisition** itself: PSD EMA τ 0.8 s against
  the 8× bar = 0.5 s at 24 dB but seconds near threshold, and everything
  sent before it is unrecoverable. Net: first text ~2.0 s after key-on for
  a strong station. Derived, unmeasured: a FIGS falling into the settle
  window prints the following number group as letters. Candidate fix,
  Richard-approved direction pending: a **pre-roll replay** in the RTTY
  backend after the CW squelch-attack pattern (~2 s baseband ring replayed
  through the converged demod/UART after acquisition + settle) — covers
  the settle window, pre-acquisition text and the sync-in garble at once.
  Separate class spotted on the way (NOT startup): a reproducible mid-over
  loss of ` S` after `DE` (`DEV1JDZ`, `VQJDZ` — engine and sim agree), to
  be dissected on the same fixture.
- **Later — PSK backend** (BPSK31 + BPSK63, Costas loop, varicode). *(The
  own-panorama waterfall that used to sit here is M8 below, since 2026-09-05.)*
- **M8 — waterfall view (BACKLOG SKM-4, half 1). Engine tap IMPLEMENTED
  (offline gate 2026-09-05); the view is in progress.** Roy Andre Løntjern,
  LB0EI, keeps Windows for CW Skimmer's pileup display: a waterfall with
  frequency VERTICAL and time flowing sideways, a kHz scale, and a column of
  callsigns to the right of it, each on its own frequency, click to tune.
  Richard adopted exactly that layout (2026-09-05, CW Skimmer's
  `ContestShot.gif` as the reference): it takes the station list's slot in the
  top half of the window via a header toggle, the decode pane stays below,
  a click on a callsign tunes like a station row. **The TX half of Roy's
  workflow is NOT in scope here:** in a split pileup the click has to move the
  TX frequency, and sdr-for-linux has no VFO B or split — its TCI `vfo:rx,ch,f`
  handler ignores the channel index and sets the single frequency, and
  `split_enable`/`rit_*`/`xit_*` are accepted, stored and echoed without a
  backend (read in `tci_server.c`, 2026-09-05). That is sdr-for-linux work
  first (its BACKLOG SDR-12); the skimmer will use whatever TCI offers once it
  exists.
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
  hold check, so the picture keeps flowing while the decoders freeze; the
  object is built lazily on the engine thread at the block's rate (fftw's
  planner is not thread-safe — same thread as the channelizer's plan).
  **Gate `skimmer-spectrum-test` (39 checks):** at all four rates a +12 kHz
  tone lands ABOVE the centre within ±bin/2 and a tone below the centre
  below it (the 2026-07-15 mirror trap, gated), bin width constant, 90 rows
  per second of IQ, tone−floor ≥ 55 dB in bytes, reset semantics; through the
  offline pipeline: no rows while disabled, rows carry the stream centre,
  peak on the right absolute Hz, rows keep coming during TX hold, silence
  after disable. **Real-air orientation check:** `SKIM_SPECTRUM_DUMP=1`
  replay of the 2026-08-15 RTTY fixture (centre 14 086 960) puts the band's
  strongest bin at 14 017/14 027 kHz (CW segment) and 14 074–14 076 kHz (the
  FT8 band) — a mirrored spectrum would have put them at 14 098–14 157 kHz.
  12 gates total.
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
  history. **Retune tearing (third live-look catch, same day: "it breaks up
  and jitters while tuning"):** the centre label rides the TCI control
  channel while the IQ rides the data channel, and sdr-for-linux reported
  GUI-side tuning only through its 500 ms reporter — so while the knob turned
  the skimmer stamped up to half a second of rows with a stale centre and drew
  them shifted (the sloping traces). Two-sided fix: sdr-for-linux now
  broadcasts dds/vfo IMMEDIATELY from its frequency setter
  (`tci_server_freq_changed()` in `engine_set_frequency`, every tuning path;
  takes effect once the SDR runs that build), and the skimmer's
  `SkimWfGuard` delays every row by `SKIM_WF_RETUNE_GUARD` = 3 rows, drops
  the rows waiting when the centre changes, and then drops EVERY row until
  the centre has stood still for `SKIM_WF_RETUNE_SETTLE` = 66 rows (≈ 0.7 s,
  longer than any TCI server's polling cadence) — so a label that only
  updates every 500 ms can never place a row on a stale centre and no server
  version can grow teeth. Richard's rule, stated at the third look: when the
  centre moves the picture only gets filled in at the edges, nothing restarts,
  nothing recentres; while the knob turns the waterfall pauses and resumes
  ~0.7 s after it stops, the marker keeps moving. Gate: 50 rows across a
  change → 34 committed in order, 13 dropped, 3 waiting; a knob turning
  (centre changes every 6 rows) commits nothing mid-turn; a steady centre
  drops nothing (61 checks).
  **STATE AT THE END OF 2026-09-05 (next session continues here).** Richard's
  fourth remark: the settle pause itself is wrong — "while retuning the
  spectrum sort of stops"; he wants the waterfall to FLOW through a retune,
  the picture only filling in at the edges. That needs the true centre per
  row, i.e. the label-to-data latency, which is now measurable: the
  sdr-for-linux build with the immediate dds/vfo broadcast (ee7d08b) is
  RUNNING live since 15:0x (restarted from `build/sdr-for-linux` at
  Richard's "ano, zkus to"; the skimmer reconnected by itself), and the
  skimmer carries an env-gated probe, `SKIM_WF_DEBUG=1`: for every row it
  logs the band move the LABELS imply over the last four rows against the
  move the DATA shows (cross-correlation with the row four back — disjoint
  windows; consecutive rows share 75 % of their samples and their noise
  correlates at lag 0, which blinded the first probe) and the difference.
  A first live pass (panadapter drag: 241 label changes of 4–170 Hz within
  4 s) was recorded by the blind probe; the fixed probe has so far seen only
  ±1-bin noise — **the measurement with a few DISCRETE ≥ 1 kHz steps (click
  on a station on the panadapter, pause, next) is still owed**. Plan once
  the lag L (rows between label change and data shift) is known: replace the
  settle DROP with a **label delay line** — stamp each row with the centre
  that was current L rows earlier — so no row is dropped at all and the
  waterfall keeps flowing while the knob turns; keep a small guard (±1–2
  rows) for jitter; the 0.7 s settle stays only as a fallback for servers
  that report by polling (detectable: labels arriving in ≥ 400 ms steps).
  Then the callsign column + click-to-tune (still empty), then bin/hop/span
  by his look. Live today: the ±3-row guard + settle 0.7 s build is what he
  ran; palettes and drag-pan verified by him; the SKM-2 GtkImage warnings
  recurred on the desktop at every launch (BACKLOG update).
  **MEASURED at the very end (probe log `/var/tmp/skimmer-app-20260905-m8-live8.log`,
  rows 769–802, one ~350 ms tuning sweep down at ~12–25 kHz/s):** the LABELS
  move every row (−2 … −48 bins per 4 rows), the DATA moves in JUMPS of +44,
  +70, +56 bins every ~9–10 rows (≈ 100 ms), each jump equal to the label
  movement accumulated since the previous jump (sign: a fixed station rises
  in bin index when the band tunes down — consistent). Root cause read in
  sdr-for-linux `src/engine/protocol2.c`: `p2_set_frequency()` only STORES the
  frequency; the keepalive timer thread pushes it in the next High-Priority
  packet "≤ 100 ms, rapid tuning coalesces into ~10 retunes/s", whereas the
  TCI label now leaves on every GUI step (ee7d08b). So the residual mismatch
  is not transport latency but the radio getting the frequency 100 ms late
  and in quanta. **First step next session, SDR side:** make
  `p2_set_frequency` KICK the keepalive timer exactly as `p2_set_tx_state`
  does (`kick_cond` — piHPSDR parity, schedule_high_priority on every freq
  change), so the DDC follows each step within a millisecond or two; then
  re-measure with the probe (expect data to follow the label within 1–2
  rows), and only then size the skimmer guard / label delay line (likely
  ±1–2 rows, no settle) so the waterfall flows through tuning with the
  picture filling in at the edges. If some latency remains, the label delay
  line absorbs it. The P1 path (`p1_set_frequency`) needs the same look.
  **App:** rows travel the ONE event queue (EV_SPECTRUM, blob + centre + bin;
  at most `SPEC_PENDING_MAX` 48 pending — the oldest row is dropped, a
  stalled UI must not hoard 1.5 MB/s), one `queue_draw` per drain; the top
  area is a `GtkStack` {station list | waterfall} driven by two linked header
  toggles (both off = decode pane only, the old behaviour), persisted as
  `[ui] view` (the pre-M8 `station_list` key migrates); the engine computes
  spectrum rows ONLY while the waterfall shows. **`SKIM_IQ_FILE=<cf32>`**
  replays a recording into the UI through the offline pipeline at real-time
  pace (its `.meta` sidecar or `SKIM_IQ_RATE`/`SKIM_IQ_CENTER`), looping — no
  radio needed for a look or a demo. Verified headless (Broadway + a separate
  headless Chrome): the 2026-08-15 RTTY fixture shows the FSK pairs at 14 082
  and 14 087 kHz and the carriers near 14 090 in a 20 kHz window, scale
  078–096; `SKIM_LAG_DEBUG` under the replay: 94 rows/s through the queue,
  worst drain 0.2 ms, no stall. Gate `skimmer-spectrum-test` 39 → 51 checks
  (composer: orientation, round-trip, zoomed-in rows centred on the tone,
  zoomed-out max-pooling, time direction, beyond-history and out-of-band
  floor, history reset on a centre change). **Palette picker (Richard,
  2026-09-05, at his first live look):** Preferences → Display → Colour
  scheme lists the same six schemes as sdr-for-linux (Classic, Mono white,
  Mono green, Mono amber, Inferno, Turbo), applies live — the whole history
  recolours at once — and persists as `[ui] palette` (an index, like the
  SDR's `[display] palette`); default Classic, the SDR's default too.
  **Next: the callsign column + click-to-tune; the live look decides
  bin/hop/span.**
  **The callsign column landed the same evening (headless-verified).**
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
  a press that travelled more than 4 px is a drag, not a click. Gate
  `skimmer-spectrum-test` 61 → 75 checks (far-apart unmoved, pair spread
  ±pitch/2 about the mean, five-cluster centred, neighbour nudged, top and
  bottom clamps, outside hidden, over-capacity count + priority + order,
  hit test incl. a hidden label). Headless with the RTTY fixture (Broadway,
  a CDP-driven click, a private D-Bus session so the live instance stayed
  untouched): SV1JDZ labelled at 14 086.96, the click logged `wf click:
  SV1JDZ @ 14086964 Hz — tune + clicked_on_spot` and fixed the pane on it.
  **Not verified:** the radio actually tuning and the logbook prefilling —
  the offline replay has no TCI, `skim_pipeline_tune` is a no-op there;
  Richard's live look decides. Still open from the morning: the SDR-side
  `p2_set_frequency` kick + the label delay line (Richard reprioritised the
  column over it).
  **Richard's first live look at the column (2026-09-05, ~17:00):** "the
  station list probably isn't needed, the waterfall view is much clearer"
  → the WATERFALL is now the default top view for a fresh install
  (`settings_load_view` → `VIEW_WF` with no saved key; a saved choice wins).
  Two-step decision: the list and its toggle stay until the column shows
  what only the list shows today (SNR, speed, heard, age); then the list —
  widget, sorter, columns, toggle — goes entirely.
  Same look, same minute: a hairline separator between the header bar and
  the top view, matching the one above the decode pane — visible only while
  the waterfall is the top view (his second remark), hidden otherwise.
  Preferences split into four tabs the same session (Richard: "there is
  getting to be a lot in there"): Radio · Decoding · Spots (panadapter
  policy + telnet feed) · Display — `AdwPreferencesPage`s with the family's
  title + symbolic icon; nothing behind the rows changed.
  **STATE AT THE END OF 2026-09-05, EVENING (next session continues
  HERE — this supersedes the morning block above).** Live on Richard's
  desk: sdr-for-linux from `build/` (ee7d08b, immediate dds/vfo broadcast)
  and the skimmer from `builddir` at 8c5d458 (column + default waterfall +
  waterfall-only hairline + tabbed Preferences), launched with
  `SKIM_WF_DEBUG=1`, log `/var/tmp/skimmer-app-20260905-m8-live12.log`.
  Richard's verdict on the column: the waterfall view is "much clearer" —
  it is the working view now. **Open, in order:**
  (1) **The click on a callsign LIVE** — tune + `clicked_on_spot` → logbook
  prefill — is still unreported: the offline replay proved the app-side
  path only (`wf click: … tune + clicked_on_spot` in the log, pane fixed);
  ask him first thing.
  (2) **The column takes over what only the list shows** — SNR, WPM/Bd,
  heard, age (tooltip on the label, or dB after the call as CW Skimmer
  prints it); **then the station list goes entirely** — widget, sorter,
  `fmt_*` columns, `freq_cmp`, its toggle and `[ui] view=list` (map to the
  waterfall on load). Richard's two-step decision; step one is done
  (waterfall default, list kept).
  (3) **The waterfall must FLOW through a retune** (his fourth remark this
  morning): SDR side first — `p2_set_frequency` kicks the keepalive timer
  like `p2_set_tx_state` (`kick_cond`; same look at `p1_set_frequency`),
  re-measure with `SKIM_WF_DEBUG=1` on discrete ≥ 1 kHz steps, then replace
  the 0.7 s settle DROP with a label delay line sized by the measured lag.
  (4) bin/hop/span by his look; the crowded fan-out (slanted connectors,
  hidden low-priority labels) has never been seen live — the RTTY fixture
  showed one station; drain cost with the column shown under contest load
  is unmeasured (`SKIM_LAG_DEBUG`).
  Housekeeping: the SKM-2 GtkImage baseline warnings now also fire when
  Preferences opens (the four switcher icons — same upstream class, no code
  change); scratch `/var/tmp/skimmer-wf-calls` holds the separate build
  dir, the headless screenshots and `cdp.mjs` (30-line CDP driver over
  node's WebSocket: click + screenshot on a Broadway page) — offered for
  the bin; the `dbus-run-session` trick is what lets a headless test
  instance coexist with Richard's live one (GApplication uniqueness is per
  session bus).
  **The waterfall FLOWS through a retune — both halves built and gated
  (offline-proven 2026-09-05 evening; live measurement + Richard's look
  pending).** Open item (3) above. **SDR side** (sdr-for-linux):
  `p2_set_frequency` now KICKS the keepalive timer for ONE High-Priority
  packet when the frequency CHANGES — piHPSDR parity read from dl1ycf master
  the same evening (`rx_frequency_changed` → `schedule_high_priority` and
  nothing else) — and the kick does NOT advance the cadence: the timer keeps
  waiting for the same 100 ms tick, so a knob at 250 steps/s sends 250 HP
  packets and nothing more (RX/TX-specific keep their 200 ms rhythm); an
  unchanged frequency does not kick. Gate `sdrfl-txiq-ring-test` section [7]
  (46 → 50 checks): ten retunes 15 ms apart each on the wire within 30 ms
  (measured 0.0 ms worst; the OLD code goes red at 4 of 10, 31 ms worst),
  RX/TX-specific ≤ cadence, the same frequency ×10 → 3 HP in 300 ms. The
  live probe had measured, before the fix, the IQ arriving 3–11 rows
  (32–117 ms) behind the dds label on eight discrete 4–18 kHz steps — the
  100 ms quantum over a ~3-row floor (log live12, rows 2390…63420).
  **Skimmer side:** the retune guard (3-row delay + 0.7 s settle DROP — the
  pause Richard saw) is GONE; `wf_compose.c` carries a **label delay line**
  instead: every row is placed on the centre label that was current
  `SKIM_WF_LABEL_LAG` (3) rows EARLIER — nothing delayed, nothing dropped;
  `SKIM_WF_LAG_ROWS=<n>` overrides it for a measurement session; a rate
  change restarts the line on the new label. The view no longer forces a
  full recompose on a mere centre change (only when the window had to
  recentre): stored rows keep their own shift and the window stands still,
  so nothing drawn changes — and without the settle that recompose would have
  run per row while the knob turns (94 × 5 ms a second on the main thread).
  The `SKIM_WF_DEBUG=1` probe was rebuilt for any step size (the old
  ±128-bin cross-correlation clipped on every real step: 192…785 bins): a
  retune episode keeps the last pre-change row as the reference and for every
  later row reports the best lag L in 0..16 — correlation at the shift each
  candidate label implies — plus the plain flip (the first row that matches
  the reference better at the full step than at zero: the data's arrival),
  and one summary line per episode ("data flipped N rows after the first
  label change"). Gate `skimmer-spectrum-test` 75 → 76 (delay line: discrete
  step, turning knob row-exact, steady, lag 0, rate restart, cap). Headless
  replay of the RTTY fixture: draws as before, drain ≤ 0.3 ms. **LIVE
  MEASUREMENT DONE the same evening (Richard's "ok, zkusíme to"; both apps
  restarted on the new builds, SDR first):** the first probe build was
  blind — a Pearson correlation of raw rows read 0.87 at ZERO shift across
  an 8 kHz retune, because the IQ passband roll-off and the DC spur sit on
  the same bins whatever the tuning; the probe now detrends each row
  (running mean ±32 bins), masks the outer 5 % and ±8 bins around DC, keeps
  only the positive excursions (the lines) and correlates those. With that:
  Richard's own knob sweeps gave 972 voting rows — **L=2: 557, L=1: 305,
  L=3: 103** — and four discrete steps sent over TCI (`vfo:0,0,<hz>;` from
  a stdlib WebSocket client, `/var/tmp/skimmer-wf-calls/tci_step.py`,
  the knob still) flipped the data **3, 2, 3, 2 rows** after the label
  (a first round was contaminated by his concurrent tuning — the echo
  showed a VFO value nobody sent). **`SKIM_WF_LABEL_LAG` = 2** (≈ 21 ms);
  his instance runs it via `SKIM_WF_LAG_ROWS=2` (log live15). Probe rows
  in the steady state after a step no longer vote (every candidate lag
  implies the same shift there — the tie fell on L=0 and swamped the
  histogram). His look at a knob sweep — slope, no tear, never a pause —
  is the open verdict. Not built, on purpose: a polling-server fallback (settle
  when labels arrive in ≥ 400 ms steps) — one click followed by stillness is
  indistinguishable from a polling label, so any cadence detector would bring
  the pause back on his most common gesture; no such server is measured.
  Also not moved: the delay line stays in the waterfall; a time-stamped
  version in `tci_client` would also fix the pipeline's centre mid-retune,
  but the pipeline flushes its decoders on a centre change anyway — go there
  only if spot frequencies during tuning ever matter.

## Safety / etiquette

Read-only against the radio, with one deliberate exception: the skimmer
*consumes* IQ, *sends spots*, and — only when the user activates a station row —
*tunes* (`vfo:0,0,<hz>`; added 2026-07-15 at Richard's request). It never keys
and never changes radio state on its own (no TRX/TUNE/CW from here). The RBN
feed must never emit unvalidated callsigns — M4 gates M6. Richard's global rule
applies: consent before any major/irreversible step.
