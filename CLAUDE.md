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
**CW v2 — semi-Markov Viterbi decoder (offline-proven 2026-07-15).**
`decode_cw_v2.c`: v1 plumbing + soft per-sample LLR (span discriminator
follows QSB; noise-anchored Rayleigh term tells dropouts from spaces) →
segment Viterbi with duration priors on the adaptive dit → lag-committed
traceback. Solid channels ride out envelope-gate dips (µ_m/µ_s ratio).
Replay A/B: "oper" corpus — v1 reads mutilated "9A1G", v2 the true
9A170NT (121 reports); contest precision held, E/T noise down. **v1 stays
the pipeline default — `SKIM_CW_V2=1` arms v2** (app + replay); flip the
default after Richard's live session. Gate `skimmer-cw-test` runs BOTH
backends + QSB/flutter cases (35 checks).
**Tone splitter (offline-proven 2026-07-16, opt-in `SKIM_TONE_SPLIT=1`).**
`tone_split.c`: per-channel Welch periodogram + keying-sideband mirror
filter → ≥2 carriers ≥20 Hz apart get one SLOT each (NCO + adaptive-cutoff
FIR) with its own decoder/extractor/freq-lock in the pipeline (slot-major
arrays, arbitration on the effective in-channel offset); <20 Hz →
CONTESTED (text shows, candidates blocked — the 14036 beat mutations stop
at the tracker). Passthrough is sample-exact when idle; unarmed = old code
path. Gate `skimmer-split-test` (46 checks, both backends + full offline
pipeline) — 8 gates total in `meson test`. Live validation pending; flip
the default together with v2.
**Extractor variant C — glued markers (offline-proven 2026-07-16, always on).**
A degenerate fist collapses the gaps around the markers ("CQCQCQ DEEA1EYL",
live 14014.4 — the fist model rightly refuses such spacing, so the fix is
lexical): a token of nothing but "CQ" repeated ≥2× counts as a CQ marker,
and a DE-prefixed token that is NO call itself but whose remainder validates
yields the call with the full DE marker (DEEA1EYL→EA1EYL; a real DE1ABC
stays whole). The TORN twin (same operator, same evening, live): stretched gaps read
as word gaps → "C Q C Q C Q DE EA1EYL" — ≥2 adjacent single-letter
"C","Q" pairs in strict alternation also make a CQ marker (a machine
always keys CQ into ONE token). All fallbacks fire only where the normal
path fails — machine keying untouched (oper corpus station table +
decode log bit-identical pre/post). Binec replay: EA1EYL 0.70/no-CQ →
1.00/CQ; 2sq replay (14009.45): CQ flag from the torn form too.
Gate `skimmer-call-test` 23 checks.
**Neural CW reader — prototype (offline-proven 2026-07-16 late, opt-in).**
`cw_reader.c` + `ml/`: a ~310k-param dilated-TCN+CTC net over SYMBOLIC
run durations re-reads a finished over with bidirectional context — for
STRONG hand-keyed stations whose timing chaos (torn/fused gaps, speed
changes, bug fists) defeats per-element decoding (EA3BP live case).
Trained offline on synthetic fists (ml/fist_synth.py, torch in
/var/tmp/skimmer-cw-ml); C inference is dependency-free, blob carries a
torch test vector the gate verifies against (plus an independent hand
reference). DISPLAY ONLY — re-reads leave via the take_aux_text() hook
("aux" decode-log lines); the extractor NEVER sees them (a babble
re-read minted a phantom EI55ISI station before the separation; flip
side: it also lifted the real, torn UA6AX — hence the phase-D "model
as witness" question). Armed ONLY by explicit SKIM_CW_READER=<blob>
(offline analyses; final weights /var/tmp/skimmer-cw-ml/run2/) — a
plain app launch never arms it: the operator checks the pane by EAR
and injected lines break that flow (Richard). Real A/B, final weights:
EA3BP CER 0.133→0.031 (calls exact), EA1EYL torn CQ reads clean.
Gate `skimmer-reader-test` — 9 gates total. Caveat: an over ending at
capture EOF never flushes (replay artifact).
**DECISION 2026-07-16: the model becomes the MAIN decode path via a
HYBRID.** Roadmap: (A) streaming inference (causal features, ~2–4 s
commit lag, retrain) → (B) model owns the pane text on solid channels;
v2 keeps weak signals (non-negotiable) → (C) data: real ham vocabulary
(LOTW…), EA1EYL tear class, independent per-element-class sigmas
(R3BDL swing: dits σ0.36 / dahs σ0.10 — the inverse of bug mode),
regression fixtures with CER bars in the gate, blob versioned in-repo
→ (D) only then any model input to the spot path ("witness" design).
Spots stay classical until D. Also from the 40 m collection: carrier
rule 0.8 s floor + v2 clock-lost watchdog (a ~3× mid-stream speed drop
no longer mutes either backend; gated); pane routes ±60 Hz unfixed /
±25 fixed (ear tuning sits off zero-beat); freqlog 1024 slots with
babble-first eviction. IQ corpus: 10 recordings in
~/.local/share/skimmer-for-linux/iq/.
Still pending live: **M3 off-air A/B** (fldigi/CW Skimmer comparison),
**v2 live session**, **tone splitter live session** (run the app with
`SKIM_CW_V2=1 SKIM_TONE_SPLIT=1`). MASTER.SCP can go to
`~/.config/skimmer-for-linux/master.scp` (the app loads it if present).
Next: RTTY/PSK backends; v2 + splitter default flips.

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
