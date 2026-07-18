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
babble-first eviction.
**Phase A — DONE (offline-proven 2026-07-18).** The reader STREAMS:
per-layer bounded lookahead (LOOK sums to 22 runs ≈ 2–4 s commit lag,
left field 230) + causal windowed mark median (31, `MED_WIN` — C and
train_ctc.py must match). Blob **CWRD v3** carries per-layer `look`
(v2 loads as look=dil, batch-only). `skim_cw_reader_stream_*` = same
float ops through per-layer rings — stream output BIT-EQUAL to read()
(gated, toy + trained). decode_cw_v2 arms the stream mid-over on the
solid thresholds, feeds the backlog, commits WHOLE WORDS to aux;
v2 blob → old batch path. EOF loses only the last word now (EA3BP
ragchew streamed where batch showed nothing). run3 (causal) synthetic
CER 0.039/0.050 vs run2 0.038/0.047; real A/B: EA3BP 0.086 vs run2
0.109 (calls exact both), EA1EYL 20 exact calls vs 17. Weights
/var/tmp/skimmer-cw-ml/run3/cw-reader-run3.bin. Found+fixed on the
way: **aux-only hits were dragging the freq lock** (no tone measure →
pin walked 20 Hz once streaming multiplied aux hits; locks now update
only on decoded-text hits) and **the app UI froze under contest load**
(one g_idle_add per event → unbounded pending-source list; five
callbacks now share ONE queue + a single drain idle, batch collapses
per-station and pane text — live force-quit 2026-07-18, fix soaking).
**Phase C data opened + run4 SHIPPED (2026-07-18 evening).**
`ml/harvest_real.py` mines consensus-labeled pairs from run dumps (label
= trivial decode on machine keying, the reader model only CHECKS at
CER ≤ 0.15 — disagreement drops the chunk); train_ctc.py mixes real
pairs (re-sped + jittered) into synthetic batches (`--real*`, `--init`).
run4 = run3 + 12k fine-tune steps at 12 % real (contest blocks A–D, 274
pairs): synthetic 0.029/0.039, held-out blocks F+G CER 0.046 vs run3
0.059 (labels biased TOWARD run3), EA1EYL 21 exact calls, EA3BP body
exact. **Blob run4 is IN-REPO: data/cw-reader.bin** (provenance
data/cw-reader.md); the cw-reader gate runs torch-parity +
stream==batch on it every `meson test`. App: **Preferences → CW reader,
default OFF** (Richard 2026-07-18; plain launch never arms — ear rule),
switch resolves blob + sets SKIM_CW_READER for the next reconnect.
**Engine clock fix:** offline replay runs TTLs/dedup on STREAM time
(pipe_now_us; spot_out injectable clock; replay now also prunes) — A/B
tables were speed-contaminated (44× vs 5× lost a third of the table to
wall-clock flock expiry); verified line-identical on contest block F.
**Coalescing soak: 74 min contest load, responsive, clean exit** (the
old build froze at ~75 min — watch one more long live session).
IQ corpus: 10 recordings + 20 m contest blocks A–H (600 s each,
2026-07-18) in ~/.local/share/skimmer-for-linux/iq/.
**Phase B — hybrid pane, draft → final (offline-proven 2026-07-18 late).**
Richard picked the UX: v2 writes the pane live per char; reader word
commits rewrite the over IN PLACE (continuous text — the streamed aux
"\n…\n" lines had shredded the pane, live-caught). Engine composes:
`decode.h` pane ops (full-state OPEN/SET/CLOSE + APPEND, self-healing
on a dropped op), `pane_log.c` = the ONE applier (app + gate share it),
v2 tracks per-char draft end-times and seats the model/draft seam on
run end-times (stream API `*_pos` reports the CTC frame per committed
char). OPEN takes back the shown draft; SET = final prefix + live
draft tail (app dims the tail via GtkTextTag; reload re-dims); CLOSE
seals with the reader's text + "· " separator. Batch re-reads (v2
blob / short solid over) go through the same ops — cw2 emits NO aux
text at all now. d.text/extractor/spot/flock paths untouched
(pane_own suppresses only the pane append; v1 has no ops hook —
uninit-field guard gate-caught). Weak channels never arm = pure v2
(non-negotiable rule holds). Gate `skimmer-pane-test` (34 checks:
pane_log units, positions, offline pipeline — pane == sealed over
exactly once, dlog increments == over, deterministic, station table
bit-identical reader on/off) — 10 gates total. Real replay (binec):
classical decode lines line-identical on/off, station table identical,
EA1EYL reads clean.
**Per-word confidence gate (live-driven, same evening).** Richard's
first live look: contest band, the pane full of model drivel — run4
mangles clean 25 WPM machine keying that v2 copies perfectly (ED1R:
"TEST EL1R D1R"; E/T babble committed as text) — the model was built
for hand-fist chaos and the takeover handed it everything solid.
Channel-stat gates FAILED on measurement (elem_err: EA1EYL's marks are
CLEAN ~0.05, it's his gaps that tear; gap-chaos EMA: QSK chop + callers
inflate contest channels above hand fists — neither separates). What
works: the net's OWN confidence — per-char logit margins (winner minus
runner-up, `*_pos` API now returns them) measured on binec + a live
20 m capture separate drivel (mean ~4, worst char ~0) from real words
(TEST 13+, exact calls 6-10). Gate: a word replaces draft only at
mean ≥ 6 AND min ≥ 3 (RR_MARG_* in decode_cw_v2.c, run4-calibrated —
a new blob recalibrates); a rejected word's span keeps v2's draft in
the over composition, and rejected babble whose span v2 kept silent
vanishes. Live capture: 16 % of words accepted (EB7KA, OY1CT, EA8QP,
TEST, 5NN…), EA1EYL keeps its exact calls. Known limit: a CONFIDENT
mutation ("EA1EKL" min 4.0) still passes — that is run5's job (phase C
on blocks E-H with the harvest consensus filter fixed: it currently
DROPS the chunks the model misreads, so its systematic errors — word
gap fusion on machine keying — never get trained away). Gate
skimmer-pane-test runs a jittered HAND fist (σ0.20 — machine keying
reads confident enough to pass, but the fist exercises both accept and
reject paths); dlog assertion is containment now, not equality.
IQ: iq-20260718-phaseB-live-192k.cf32 (600 s live 20 m contest,
centre 14034.156 kHz) joined the corpus.
**Live VERDICT on the gated pane (2026-07-18 late, Richard):** the
batch hole fix helped (whole-over garbage rewrites 39 → 3 on the
capture; the gate also lost its one-shot bypass — reader reaches the
pane ONLY via streamed, word-gated overs now; a hybrid pane needs a
v3 blob), but the field result stands: **CORRECT gray v2 draft gets
broken by the model's "confident" white commits — run4's confidence
is not calibrated, and confident mutations outnumber real corrections
on a live band. run4 is NOT usable as a pane rewriter.** Both causes
confirmed: (1) dirty context — the raw Schmitt run stream reaches the
model without v2's layered defenses; (2) the model itself — trained
on synthetic hand fists, and the harvest consensus filter DROPPED
exactly the chunks it misreads, so fusion errors never trained away.
The phase B machinery (ops, seam, firming, margin gate) is done,
gate-proven, and stays; the reader switch stays default OFF and
should stay off until run5. **run5 plan (next session, phase C):**
fix harvest to keep disagreement-with-confident-machine-decode chunks
(the teaching examples); train on contest blocks E-H + the phaseB
capture (machine keying + chop — clean gaps are facts); calibrate
RR_MARG_* with a labeled sweep, not a hand fit; A/B the context
hypothesis (feed the reader only outside v2 squelch vs raw).
**run5 — the reader learns the truth (offline-proven 2026-07-18 late 2).**
The harvest bias died by REDESIGN, not a patch: labels now come from **v2
itself** — `emit()` writes every decoded char + end-of-audio time +
elem_err/solid into `<SKIM_CW_DUMP_RUNS>.chars` (file order = text order,
do NOT re-sort: a lag-committed char and its word space share a tau), and
`harvest_real.py` slices those chars onto run chunks by time. The reader
model is OUT of the label path entirely (only flags "teach" chunks it
misreads — 3× oversampled by train_ctc `--teach-boost`). New label-side
filter: **run inflation** (chunk runs > 1.5× the label's morse element
count = a splatter shadow where v2 rightly squelched a strong neighbour —
its fragment label would teach DROPPING real text; measured on F: <1.3 →
0 % garbage, >2.0 → 94 %). Yield 5143 pairs (A–G + phaseB, 86 % teach)
vs run4's 274. run5 = run3 + 20k steps at 25 % real: **held-out H CER
0.25 vs run4 0.428; EA3BP 0.008 vs 0.016; noise transcription trained
away** (run5 blanks what v2's squelch blanks — "PA4O TU 5NN 2" where
run4 wrote "EEEAAOEEEIEX 5NN 2M"). **Blob data/cw-reader.bin = run5.**
Margin gate recalibrated by measurement (`ml/margin_sweep.py`, 3-class:
match/conflict/orphan vs v2 words from .chars): 6/3 stands (higher bars
halve recall, conflict share barely moves); phaseB 6/3: 219 match / 93
conflict vs run4's 228/214, and accepted orphans are REAL weak-signal
words (TEST 198×, CQ, calls in the station tables), not babble. Two new
defenses: **gated feed is default** (reader eats no dead-air runs — the
hoisted v2 pause predicate; raw via SKIM_CW_READER_RAW=1; station tables
bit-identical, 2.8× faster, half the candidates) and the **same-shape
guard** (a word rewriting its whole draft span 1:1 with 1–2 chars
changed is a confident wrong call — EA2KC over EA2CC, H7KA over EB7KA —
fist corrections change length; draft stays, RRWORD tags MUT). binec:
3 exact EA1EYL accepted (=run4), "EA1EYLL" rejected at min 0.51;
9A170NT table identical reader on/off. 10 gates green. Known limit:
conflicts where reader and v2 disagree at equal confidence remain
(~50/600 s accepted, half are reader out-reading v2 fragments) — the
phase-D witness/consensus design is the answer, not a bigger threshold.
Reader stays **default OFF until Richard's live look** at the run5 pane.
**Clock re-lock — fast WPM adaptation (live-driven 2026-07-18 night,
Richard: "adapt the WPM faster instead of inferring meaning with the
model").** Both backends: ring of recent raw mark durations re-clustered
boot-style on every commit (`clock_push`) — a BIMODAL ring whose low
cluster leaves the ±(20/25) % band is a new speed: JUMP the dit, don't
glide (the per-mark EMA is pulled the WRONG way past the 2-dit class
boundary — self-consistently, elem_err stays low). Unimodal rings
(dits-or-dahs ambiguous) resolve via the SPACE ring: the smallest space
class with ≥3 members within 1.5× = the element gap; marks 1:1 with it
are dits, ≥2.2:1 are dahs (est/3). NOT the min (torn-dah glitch pairs
sit under it) and NOT the median (dah-heavy "MM DE UB7M" flips it to
char gaps — jumped a clean 24 WPM clock 3× up, replay-caught).
clk_est sheds one outlier (a torn-dah fragment); looser clusters are a
FIST, not a speed (σ0.2 jitter false-jumped the pane gate). Rings clear
at over breaks/pauses (the next over re-locks on ITS marks alone) —
and the CLEAR MUST RESET THE HEADS: n=0 with a rotated head made evals
read stale slots (the whole family of "one char late" mysteries).
v2 adds the mirror clock-lost watchdog (24 dits, no dah — a wrong
up-jump reads everything as dits and self-confirms). Gate: cw-test
"QSO turnaround" (18→30 + 26→14 across a 3 s gap, whole-over
last_over_dist ≤3 incl. the "VVV" warmup gen_env really keys). Real
A/B: 20 m QSO pair CT3KN/CT3D same-freq — 53 → 365 reports; binec
NU3EQ→RU3EQ (first char fixed); oper/UB7M/R1AL/9A170NT tables intact
+ OH2BO newly tracked; EA1EYL/EA3BP untouched; 10 gates green.
Still pending live: **M3 off-air A/B** (fldigi/CW Skimmer comparison),
**v2 live session**, **tone splitter live session** (run the app with
`SKIM_CW_V2=1 SKIM_TONE_SPLIT=1`). MASTER.SCP can go to
`~/.config/skimmer-for-linux/master.scp` (the app loads it if present).
**Release: Richard decided 2026-07-18 — tag v0.1.0 only AFTER his live
validation session** (v2 + splitter + one more soak), then default
flips + release. Next: phase B (model owns pane text on solid channels,
v2 keeps weak); RTTY/PSK backends.

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
