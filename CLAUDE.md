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
- **Every commit and tag is GPG-signed** (`commit.gpgsign=true`, key
  35AE58A8…B33931; gpg signs without a prompt). Never override with
  `-c commit.gpgsign=false` / `--no-gpg-sign`; verify `git log --format=%G? -1`
  = `G` after each commit. Richard, 2026-09-13, after one unsigned commit
  (60fe31f) slipped in — that one stays as is, on his word.

## Work queue — GitHub Issues (since 2026-09-18)

Bugs, ideas, debt and anything still waiting for a live check are **GitHub
Issues** (`gh issue list -R OK1BR/skimmer-for-linux`), not a file in `docs/`.
`docs/BACKLOG.md` (SKM-1…SKM-20) left the tree on 2026-09-18: eight items were
done, the rest became #3–#16 — every "BACKLOG" / "SKM-N" mention below refers
to its last version, commit 96606bf. SKM-3's measured record (DeepCW) moved to
`docs/DEEPCW.md`. Richard's reason: finished and long-verified items kept
sitting there as "open", and nobody could see what was really left.

- Labels: type `bug` / `enhancement` / `debt`; `severity: high` = wrong data
  or something that leaves the machine wrong, `medium` = gets in the
  operator's way, `low` = cosmetic or log noise; `needs-live-check` = done in
  code, gates green, but the issue **stays open until the behaviour was seen
  live**; `at-the-radio` = the check needs the rig; `deferred` = parked on
  purpose.
- A commit closes its issue with `Fixes #N` only when nothing is left to
  verify live — otherwise `Refs #N`, and the issue is closed by hand once the
  check passed. Before filing a "needs live check", look for evidence that
  real operation already proved it (logs, contest data).
- Issue text is public and goes out under Richard's name: English, never
  hard-wrapped, shown to him (with a Czech translation) before it is posted.
- `docs/` keeps only what can have value in the future (Richard, 2026-09-18):
  binding rules, decisions with their rationale, hard-won protocol/DSP facts,
  measured numbers that justify a choice, rejected approaches, plans for work
  still ahead. Build diaries, gate counts, "state at the end of the day"
  blocks and old contest notes do not belong there — git keeps them. Notes
  from live operation may start in `docs/CONTEST-NOTES-<date>.md`, are triaged
  into issues, and then the file leaves the tree.

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

> Read this section as a CHRONOLOGICAL log: entries are appended, not
> rewritten, so where two of them disagree the LATER one is the truth
> (gate counts, defaults, what is in the tree). The last entry carries
> the current state and the open list.

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
9A170NT (121 reports); contest precision held, E/T noise down. **v2 is the
pipeline DEFAULT since 2026-08-04** (was opt-in `SKIM_CW_V2=1`; that name
is still accepted, `SKIM_CW_V1=1` now selects the classical v1 — see the
2026-08-04 entry at the end). Gate `skimmer-cw-test` runs BOTH
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
stream==batch on it every `meson test`. [SUPERSEDED — the blob, `ml/`
and that gate were deleted 2026-07-19, …ba12f77.] App: **Preferences → CW reader,
default OFF** (Richard 2026-07-18; plain launch never arms — ear rule),
switch resolves blob + sets SKIM_CW_READER for the next reconnect.
**Engine clock fix:** offline replay runs TTLs/dedup on STREAM time
(pipe_now_us; spot_out injectable clock; replay now also prunes) — A/B
tables were speed-contaminated (44× vs 5× lost a third of the table to
wall-clock flock expiry); verified line-identical on contest block F.
**Coalescing soak: 74 min contest load, responsive, clean exit** (the
old build froze at ~75 min — watch one more long live session).
IQ corpus: DELETED 2026-07-19 at Richard's explicit, informed request
(12 GB; the warning that off-air captures are unrepeatable and carry
the hand-truthed validation cases was given and he chose full
deletion). Decoder improvements are validated LIVE or on freshly
recorded captures (`skimmer-tci-probe <host> <port> <rate> <secs>
<dump.cf32>` records a new one in minutes).
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
[SUPERSEDED — deleted 2026-07-19, …ba12f77; no blob ships today.]
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
**LIVE VERDIKT run5 + ROZHODNUTÍ (Richard, 2026-07-19 ~00:30): the AI
comes OUT of decoding.** run5's white commits still degrade text v2 got
right the first time — two trained generations, calibrated margins,
gated context and the same-shape guard all failed to make the model a
net positive on a live band. The app no longer HAS a reader: the
Preferences switch, settings [reader], blob resolver and env arming are
all removed from src/app/main.c — a launch can never invoke the model.
The pane is pure v2 (draft = final, no dim/rewrite in practice; the
engine's SKIM_CW_READER env path + pane-op machinery still exist for
OFFLINE replay analyses only, and their gates keep them honest while
they remain). Phase D (model as spot witness) is dead. The classical
path (v2 + clock re-lock + tone splitter) is the decoder. **Removal
DONE in part (…ba12f77, 2026-07-19): `ml/`, the in-repo blob
`data/cw-reader.bin` and the `skimmer-reader-test` gate are GONE from
the tree** — any older paragraph above that calls the blob "in-repo"
is history, not current state. What still sits in `src/engine/`:
`cw_reader.c`/`.h` (dormant, reachable only via the `SKIM_CW_READER`
env path, and with no blob shipped it cannot arm on its own) and
`pane_log.c`/`.h` (NOT dead — the app's decode pane is built on it).
Excising the cw_reader remnant awaits Richard's explicit call.
**Tone FOCUS — narrow slot on a lone carrier (offline-proven 2026-07-19,
opt-in `SKIM_TONE_FOCUS=1`).** Lever #1 of the classical-decoding plan:
the envelope used to integrate the whole 125 Hz channel while a CW signal
is ~50 Hz wide — ~4 dB of noise paid for nothing. Focus = the splitter's
slot machinery with ONE slot: a channel showing exactly one stable carrier
for 2 evals tightens slot 0 onto it (NCO + FIR; cutoff = env value ≥ 5 Hz,
plain `=1` → 25 Hz). The slot tracks the carrier (claim radius + EMA), the
TTL releases back to passthrough, a genuine second carrier spawns a real
split with the focus slot riding through UNRESET; contested/pending
channels never focus, and in focus mode a pending-unverifiable second line
counts as contested evidence (the narrow filter would pass a beat the wide
path at least flagged). `SKIM_TONE_FOCUS` implies the splitter machinery;
fixed on the way: all slots dying in one eval used to leave the channel
dark forever (no passthrough revival). Gate `skimmer-split-test` now 90
checks × both backends incl. the weak-lone-carrier A/B — same IQ, wide
envelope decodes NOTHING (squelch), focus copies ≤ 1 err; end-to-end the
tracker gets OK1BR on the right absolute Hz only with focus armed. Also
new: **A/B spot harness `tools/ab_spots.py`** (plan lever #2) — collects
our telnet feed and SDC's side by side (JSONL), reports who-first/freq
deltas/mutation suspects (edit distance ≤ 2, MASTER.SCP adjudicates) and
per-side-only station events; mock-verified, awaiting a live SDC run.
Still pending live: **M3 off-air A/B** (fldigi/CW Skimmer comparison) and
the **tone splitter + FOCUS live session** (run the app with
`SKIM_TONE_FOCUS=1` — focus arms the splitter too; splitter alone briefly
live 2026-07-20 post-reground, band died before a verdict; armed again
during the 2026-08-01 contest — 37 054 `tone-split` lines in
`/var/tmp/skimmer-app-20260801-live8.log` — but the day went to the UI
congestion hunt, so still NO verdict). The **v2 live session is DONE**
(2026-08-01 contest day) and v2 is the default since 2026-08-04.
MASTER.SCP can go to
`~/.config/skimmer-for-linux/master.scp` (the app loads it if present).
**Squelch attack fixed — pre-roll replay (offline-proven 2026-07-19 late,
always on, v2 only).** Úkol #4, the ONE weakness the SDC A/B measured:
the head of an over after dead air. Two consumers: the keying-duty EMA
(~0.3 s before a cold channel counts as keying) and the dit BOOTSTRAP
(first 4-8 marks spent learning the clock after the 8 s forget —
"YOTA"→"OTA" is exactly Y's 4 marks; 32 % of R3OM's overs in the live
log). Fix in decode_cw_v2: every sample lands in a ~2 s pre-roll ring
(dead air included); when the channel wakes with a usable clock (pause
reopen / bootstrap learn) the ring replays through the SAME soft path
(`soft_step` — the per-sample lattice code, factored verbatim). Guards,
each fixture-measured: replay starts only at an onset preceded by
≥2 dits of quiet (a mid-char element gap can never qualify — no partial
chars minted); `pre_from` fence at pause entry + clock-lost watchdog
(already-emitted audio never replays — no duplicate text); an embedded
≥14-dit quiet in the span aborts (that is QRM + a break, not one over);
and a head-SNR bar 5.6× measured ON THE RING (onset-run mean vs quiet
mean) — the 12 dB RV6HV head re-read as torn "AEV6HV" where the old
code stayed quiet, and a mutation near the extractor is dearer than a
hole. NOTE: the env-ratio at wake time is useless for that bar — the
gate opens on the rising edge, so it reads 4-5× by construction at any
station strength. A keying-duty FAST ATTACK was built, measured and
REMOVED: the replay covers its entire win, and the early wake tore
marginal heads (fixture) + fed the bootstrap bank-warmup fragments
(gate). v1 untouched. YOTA fixture A/B: strict textual superset —
LZ5R 35→37, DF8V 13→14, EA6NB/UX8IX +2, YOTA 33→34, OH2B (the 14100
NCDXF beacon — pure head-after-pause traffic) newly tracked, NO count
lower anywhere; station table keeps all 17 + OH2B + EN0IMT. Gate:
cw-test "squelch attack" section (10 s pause × 33/20 WPM exact, 4 s
reopen exact, cold start reads from the FIRST char) — 47 checks. Live
metric still pending: R3OM-class "OTA TEST" rate next live session.
**Splitter REGROUND (offline-proven 2026-07-20, …77cf12a).** The live
RG5A class (úkol #3) reproduced on the YOTA fixture — the old splitter
run lost LZ5R (39 dB, 673 reports) OUTRIGHT, cut IT9JYI to 11/248 and
minted 4 phantoms — and dissected into FOUR stacked defects, each
found and fixed by measurement: (1) find_carriers' 8 dB peak bar sat
over the FULL-BAND median, which the channelizer stopband drags to ~0
— 3-4 dB noise wiggles engaged 743 splits/600 s (fix: passband lower
quartile — the exact trap the focus bar had documented); (2) a
bin-edge carrier scallops BELOW its own keying sidebands, so the
symmetric pair had no accepted anchor and split ±13 Hz of a 44 dB
runner — mirrors now test about ANY candidate line ≥ 0.25×; (3) WIDE
lane — passthrough decoder/extractor/flock moved to a persistent
per-channel lane (SL(c, SPLIT_MAX)); engage PARKS it, collapse RESUMES
it warm. Before, one wrong split mid-pileup meant a fresh decoder that
never re-acquired, LZ5R's own pileup mutations takeover-evicted the
record (clip rule) and TTL-prune erased it — the vanished-station
mechanism; (4) utility gate — no engage/focus while the wide lane
copies SOLIDLY (owner hints per decode at confidence ≥ 0.85 ≈
elem_err 0.1, 5 s self-decay). Measured: beat garble reports CONFIDENT
mutations at 0.6-0.7 (why beat text always validated), so the bar sits
at clean copy and the gate's beat pairs still split. Contested now
blocks only the extractor FEED — the already-proven candidate keeps
reporting through pending episodes (181 blocked feeds on one channel
were starving YO3LW). Fixture after: 3389/3513 baseline reports
(−3.5 %), ZERO phantoms, IT9JYI/EA6NB/F5UTN/DF8V/SN1T/UX8IX exact,
RV6HV +34, G3BOY +21; residue OY1CT −74, DL0G −43, LZ5R −35, YO3LW
−31 (contested episodes). A co-keying (colit) engage gate was built,
measured (1 of 213 engages differed, table identical) and REMOVED per
the no-benefit rule. Dead ends (do not retry without new data): power
delta can't separate (the psd averages duty — the killer engage read
Δ3.2 dB), colit can't (128 ms windows blur keying into over-level
overlap — legit pairs read 0.78-1.00 like sidebands), weak-only
engage bar kills the core comparable-pair case (gate-caught). New
env-gated debug: SKIM_ST_DEBUG traces the station table (EVICT/FOLD/
QSY/PRUNE + per-hit cand/report lines); SKIM_TS_DEBUG adds
contested-drop lines, ENGAGE delta dB and the ts pointer for
channel↔splitter mapping. 9 gates green; splitter replay 20 % faster
(a per-hop qsort left with colit). First live launch same night
(40 m): engages Δ1-6 dB comparable pairs, band went quiet before a
verdict — splitter live validation STILL OPEN (A/B dataset live5
reserved for it; live4 = post-squelch-fix, no splitter — don't mix).
**Callsign click-through → logbook (offline-proven 2026-08-01).** A left
click on any decode-pane token that validates as a call acts like a
panadapter spot click: tune to the pane's pinned slot Hz (exact carrier,
not the stepped VFO) + NEW `clicked_on_spot:call,hz;` over TCI
(`skim_tci_client_spot_clicked` / `skim_pipeline_spot_clicked`); the
sdr-for-linux server now RELAYS client-sent clicks to every client
(both forms; its sdrfl-tci-test covers it) and log-for-linux prefills
its Call entry (that side was already live). Hand cursor over clickable
calls; drag-select never fires; token ends trimmed of punctuation/over
marks. Gate: skimmer-tci-test wire check (17 checks). The LIVE chain
(pane click → radio tunes + logbook prefills) is unverified — it needs
the sdr-for-linux server REBUILT with the relay and restarted.
**Dup-aware colours from the logbook (offline-proven 2026-08-01).**
log-for-linux runs a read-only UDP dup service (127.0.0.1:2238,
`DUP? call hz mode` → `NEW/B4/DUP call`, silence on malformed;
live-probed — answers end in \n). `dup_query.c`: non-blocking client,
60 s TTL cache, 2 s re-ask suppression, every failure → UNKNOWN
(logbook closed must never break spotting). Pipeline owns it; spot_out
colours SPOT ARGB by verdict (shared rule skim_spot_argb_for_dup:
NEW/UNKNOWN bright SKIM_SPOT_ARGB, DUP/B4 gray SKIM_SPOT_ARGB_DUP —
the 180 s re-announce recolours), pane scp highlight tints the same
way via a second gray tag (verdict-sharpened rescans swap tags; app
asks with 0 wait — GTK never blocks). Pane + panadapter colours come
from the SAME constants since today (scp_tag was #44cc44, now
#30C060 = spot green; the sdr panadapter renders client ARGB verbatim,
alpha 0.95). PUSH: the logbook sends the standard answer datagram
unsolicited on QSO log/delete/edit (its side: logfl_dup_srv_notify,
peers from the last 10 min); dup_query queues green↔gray flips (answers
AND pushes), the engine thread drains per block and resends the last
emission with only the ARGB changed (labels < 10 min only), the pane
re-tints its tail à 2 s. Gate skimmer-dup-test (15 checks) — 10 gates
total. **LIVE-VERIFIED 2026-08-01: a logged QSO grays the label
instantly (Richard).** Lesson: meson test does NOT relink the app —
build `ninja skimmer-for-linux` explicitly or the old binary keeps
running (cost one "why is everything green" round).
**UI-thread congestion killed (live-measured + live-verified
2026-08-01).** Richard: the app chokes under contest load, worse over
time. Profilers unavailable (ptrace_scope=1, no perf) → built
SKIM_LAG_DEBUG: 250 ms main-loop heartbeat (logs stalls with length) +
5 s counters (ev/append/retag/reload/worst-drain). It convicted
apply_station: a per-event LINEAR scan of the station GListStore plus
remove+append churn — cost grew with the table (drain 3→22 ms over
6 min at constant ~200 ev/s, main thread 10→99 %). Fix: call→row hash,
in-place update + single-row items_changed (rows carry their unsorted
position; apply_gone maintains it). Same-age comparison after: main
thread 0 %, drain flat 0.6-6 ms at full load. Also: dup verdicts now
SERVE STALE past the TTL (background revalidate; the old
expiry→UNKNOWN flap re-tagged the pane every 60 s per gray call), and
the earlier stutter round removed blind re-tagging, per-motion cursor
allocation, and the poll-under-lock in dup lookups. UDP itself was
exonerated by measurement (0 drops, 0 kernel time). Two follow-up
levers, both telemetry-driven: a TTL-prune wave no longer runs the
pane resolver per eviction, and the resolver runs ONCE per drain (a
VFO pileup ran it per report — 35-57 ms drains). FINAL soak (1+ h,
band change included): main 0-5 %, drains 2-12 ms typical at up to
320 ev/s, zero stalls. Residual 20-35 ms one-offs = prune waves;
next lever IF ever needed: batched removals (g_list_store_splice) —
not deployed, no measured benefit today (the iron rule).
**Release: Richard decided 2026-07-18 — tag v0.1.0 only AFTER his live
validation session** (v2 + relock + splitter + one more soak), then
default flips + release. **Direction (Richard, 2026-07-19): better
SIGNAL decoding, classically — "AI je mrtvá cesta."** Candidate levers,
best first: (1) tone-coherent second-stage filtering — the channel is
125 Hz but a CW signal is ~50 Hz and we already track its exact offset
(freq locks + phase-slope foff): a narrow filter riding the lock buys
real dB exactly where stations are lost today; (2) M3 off-air A/B vs
CW Skimmer/fldigi to MEASURE where we lose; (3) torn-dah merge
hypotheses in the v2 lattice (QSK chop: 32+8+26 splits seen all over
the live captures); (4) RTTY/PSK backends; (5) sub-20 Hz joint demod
for contested slots (far).
**v2 IS THE DEFAULT + docs regrounded (2026-08-04, Richard's call after
the 2026-08-01 contest day ran v2 live).** `pipeline.c` flipped:
`SKIM_CW_V1=1` selects the classical v1, everything else gets v2
(`SKIM_CW_V2` still parsed so old command lines keep meaning what they
said). Measured on the 600 s YOTA-contest replay, default vs
`SKIM_CW_V1=1`: **19 stations vs 12**; v1 loses the segment's LOUDEST
signal outright (LZ5R, 39 dB, 673 reports for v2), mutates SN1T→IN1T,
mints the phantom TM00TFR (103 reports) and drops IT9JYI/DF8V/DL6KVA;
v2 costs ~50 % more CPU (52× realtime vs 79×). 10 gates green with the
new default — the spot-pipeline gate now exercises v2. Also fixed: the
**"CQ only" preference subtitle never rendered** — a raw `&` in "S&P"
made AdwActionRow's Pango markup reject the whole string (the GTK
warning sat in EVERY live log since M5; verified by parsing both forms
through `Pango.parse_markup`). Docs regrounded to the tree: gate count
8→10, the v2-opt-in lines, the deleted ML blob, the stale
`SKIM_CW_V2=1 SKIM_TONE_SPLIT=1` launch line.
**v0.1.0 release plumbing built the same day**, copied from
log-for-linux's v0.1.0 (2026-08-02) and sdr-for-linux: version
0.0.0 → 0.1.0, `data/` (desktop entry with the bindir baked in, the
scalable icon, AppStream metainfo) installed via `subdir('data')` +
`gnome.post_install`, `packaging/` (nfpm deb/rpm + AUR PKGBUILD) and
`.github/workflows/build.yml` (gates on PRs/tags; AppImage + deb + rpm
built and container install-tested at tag time only — no CI per
commit, Richard 2026-07-12). **The GApplication id changed
`cz.ok1br.SkimmerForLinux` → `cz.ok1br.skimmer_for_linux`** — it has
to equal the icon/desktop file name or Wayland shows a generic icon,
and snake_case is the family form. Verified: staged install lands the
four paths nfpm expects, `desktop-file-validate` and `appstreamcli
validate` pass, 10 gates green on the release build. **Open before the
tag:** (1) tone splitter + FOCUS live validation still has no verdict
(they stay opt-in, so this does not block); (2) the click-through's
last link — logbook prefill into an EMPTY Call entry — is still
unconfirmed live; (3) `packaging/PKGBUILD` carries `sha256sums=SKIP`
until the tag tarball exists.
**INV verdict — contest-invalid grays like DUP (offline-proven + live wire
check 2026-08-08, mid-WAE).** The logbook's :2238 service grew `INV <call>`
(log-for-linux `8093437`): the active contest scores no QSO with that
station at all (WAE: every EU call for an EU op). `SKIM_DUP_INV` in the
enum; the gray decision has ONE truth now — `skim_dup_verdict_gray()`
(dup_query.h), called by both the parser's flip rule and
`skim_spot_argb_for_dup` — so spot ARGB, pane tint and push recolours all
follow from two lines (parse + gray set). Unknown verdicts from a newer
logbook keep collapsing to UNKNOWN before the cache (now gate-proven).
Gate `skimmer-dup-test` 15 → 25 checks (INV round-trip/push/TTL-survival,
unknown-verdict answer + push tolerance, colour). Live wire check against
the running logbook mid-WAE: DL1AA→INV, K1AA→NEW, OK1BR→INV. The visual
chain (gray label on the live panadapter) rides the exact path DUP
live-verified 2026-08-01; a live INV look during a contest is welcome but
nothing new executes there.
**Reported-frequency hysteresis (offline-proven 2026-08-08).** With a
frequency grid on (Preferences → Frequency step 10/20/50/100; default
Exact untouched), `spot_out` re-quantised from the raw estimate on every
re-announce — a station parked on a cell edge alternated a whole grid step
on the label and in the telnet feed. Now `SpotMemo` keeps the reported
`out_hz` + its grid `out_rh`; a re-announce re-sends the stored value
unless the raw sits > ¾ step away (s53zo/DeepCW principle, no code —
their trees carry no licence) or the grid setting changed; recolour
resends the stored value instead of recomputing. Gate `skimmer-spot-test`
21 → 26 checks (boundary jitter over four re-announces = ONE value, QSY
re-quantises, grid change re-quantises, Exact follows raw) — and the new
check was run against the pre-fix code to confirm it goes red.
**About dialog + primary menu + --version (built 2026-08-08).** The gear
button became the family hamburger menu (Preferences, then "About Skimmer
for Linux" last, GNOME HIG); AdwAboutDialog field set per sdr-for-linux,
debug_info per log-for-linux (GTK/adwaita versions, TCI host, feed state,
settings/MASTER.SCP/decode-log paths); comments = metainfo summary =
desktop Comment. CLI `--version`/-v answers in handle-local-options —
prints locally and exits, can never raise a running instance (verified
live: printed 0.1.0 while Richard's instance ran mid-WAE, untouched).
10 gates green. NOT yet seen on screen — both family apps were live in
the contest; visual look + a menu click pending after WAE (his running
binary is the installed pre-About build anyway).
**AUR: the package is published (2026-08-12).** The git/SSH backend at
`ssh://aur@aur.archlinux.org` came back from the maintenance that had
blocked the v0.2.0 publication on 2026-08-09 (web + RPC had stayed up
throughout — only git/SSH was down). A fresh `makepkg` from the
published tag tarball went first: the recorded sha256 matches, `check()`
= 10 gates green, `skimmer-for-linux-0.2.0-1-x86_64.pkg.tar.zst` built,
`namcap PKGBUILD` clean (the package's three warnings are informative —
implicit glibc + hicolor-icon-theme, plus an unused `libfftw3.so.3`: the
binary links both fftw flavours and only calls the single-precision one,
and the `fftw` package provides both, so the dependency is right). Then
`PKGBUILD` + `.SRCINFO` (`makepkg --printsrcinfo`) went out as an
"Initial import" commit on master of
`ssh://aur@aur.archlinux.org/skimmer-for-linux.git` (account ok1br, key
`~/.ssh/aur_ed25519` — the flow sdr-for-linux took). Verified after the
push: the package page answers 200 with "skimmer-for-linux 0.2.0-1",
cgit shows the import commit, `list-repos` lists it beside
sdr-for-linux; the RPC info/search index lags the push by minutes, so an
empty RPC answer right after a push means nothing. **Next release:** bump
`pkgver`/`_pkgtag`, tag FIRST and take the tarball's sha256 from the
published tag (the two-step flow), regenerate `.SRCINFO`, commit + push
to the AUR remote — `packaging/PKGBUILD` in this repo stays the single
source, the AUR clone is a copy of it. README's Install section now
names the AUR package (`paru -S skimmer-for-linux`) instead of the old
"AUR package to follow".
**M7 — RTTY mode (offline-proven 2026-08-15, built mid-RTTY-contest; live
validation pending).** `decode_rtty.c` implements decode.h: periodogram
pair finder (WEAKER tone scores — a lone carrier can never acquire;
sub-bin centre via floor-subtracted centroids) → NCOs on the tracked
centre ±85 Hz → one-bit moving-sum matched filters + per-tone peak
normalisation (ATC; mark −10 dB under space still copies exact) → dual
start-bit-anchored UARTs at 45.45 Bd on y AND −y (elected by sustained
framing validity — reversed/wrong-sideband signals copy) → ITA2 +
US-TTY figures, UOS. Layered squelch, every layer gate-measured: pair
over floor + envelope ANTI-correlation (FSK −0.8…−1.0, two unrelated
carriers ~0 → open −0.35/close −0.15, moments primed neutral at
acquire) + frame-valid EMA 0.55/0.35 + fast presence gate (τ60 ms —
kills the 2 s of noise-framing between TX stop and the slow release)
+ 0.7 s settle embargo (half-converged slicer minted a phantom V).
Pend-buffer flushes on squelch open — over heads not eaten. Channelizer
grew `skim_channelizer_new_ex(rate, spacing, passband, K)`: RTTY bank
= 250 Hz spacing, ±225 Hz passband (both tones of ANY straddler in one
channel needs ≥ 85+spacing/2), K=16 (critical +415 Hz alias onto the
space tone: −124 dBc). Pipeline `SkimPipelineConfig.mode` (CW default)
picks backend + bank + mode string (station/spot/dup/decode-log);
splitter/focus env vars are CW-only (FSK pair = two carriers to the
splitter — RTTY mód je ignoruje). App: Preferences → Decoding → Mode
(persisted `[decode] mode`, change reconnects), speed column + tuned
header show Bd, About debug_info carries the mode. Gates:
`skimmer-rtty-test` 25 checks (hardcoded ITA2 bit vectors as the
independent table witness; offsets; figures/UOS; reversed; AWGN; QSB;
selective fade; squelch noise/keyed-CW/two-carrier; worst-case
straddler through the real wide bank exact in BOTH channels; whole
offline pipeline in RTTY mode — 2 CQ stations tabled, RTTY, ±4 Hz,
zero phantoms), chan-test 18→25 (wide-bank geometry + alias).
**12 gates green.** Open: live session on the contest band (mode
switch, real pileups, spot colours vs logbook RTTY dup service),
reported-frequency convention (we spot the pair CENTRE; RBN convention
is the MARK tone — decide with Richard before the telnet feed carries
RTTY), per-channel CPU at M=768 RTTY channels unmeasured live.
**M7 LIVE — the RTTY mode works on a real contest band (Richard's
verdict 2026-08-15 morning, mid-contest, 20 m).** Session: dds
14 086 kHz, 192 k IQ, the new build launched with `[decode] mode=rtty`
against the running sdr-for-linux. The WHOLE chain proven live: real
spots on the telnet feed (`DX de OK1BR-#: 14093.2 LA1TV RTTY 20 dB
45 BPS CQ 0839Z`, then IZ0FVD 14091.5) and clean strong-station text
in the decode log ("CQ CQ DE SV1JDZ … PSE K" at 20-23 dB, LA1TV
repeats exact at 10-16 dB). Whole-app CPU 11 % of one core at 192 k /
768 channels, no stall, no crash over the session. Richard: "skimmer
vypadá, že funguje." OPEN, fixture-tunable (the live band found what
synthetic cases could not): (1) squelch LEAK classes — 14121.5 held a
SUSTAINED garbage stream (a non-45.45 digimode passes the pair test +
framing bar; wrong-baud discrimination is the missing layer) and the
CW segment (14008) leaked sporadic weak fragments; neither ever
validated into a callsign (extractor held — zero bogus spots), but
they dirty the decode log/panes; (2) SV1JD(Z) decoded strongly for
minutes yet was not seen in the feed watcher window — possibly spotted
in a 30 s observation gap, possibly a call-tail flap (SV1JD ↔ SV1JDZ)
keeping the extractor under the 0.85 feed bar — the fixture replay
with SKIM_ST_DEBUG answers this exactly; (3) FT8 band (14074-076)
acquires often, emits only single chars — harmless but wasted demod
CPU; a "known non-RTTY subband" damper is a possible later lever.
**Fresh IQ fixture recorded for all of it:**
`/var/tmp/skimmer-iq/iq-20260815-rtty-live1-192k.cf32` (297 s, 457 MB,
192 k, centre 14 086 960 Hz, .meta sidecar hand-written — the probe
only writes it on its own clean exit path; capture carries LA1TV +
SV1JD(Z) + IZ0FVD + the 14121.5 garbage source + the FT8 band).
Threshold work happens OFFLINE against this fixture (SKIM_MODE=rtty
replay), the established house method.
**SKM-1 fixed (2026-09-05) — the first pass over `docs/BACKLOG.md`, the
single work queue since 2026-08-25.** The YO DX HF teardown criticals
(two in one millisecond at close) are a coincidence, not a constant:
`gtk_window_close` destroys the tree synchronously inside the close-request
dispatch and `g_application_run` finishes the SAME iteration's dispatch
list before it sees the released window, so a tick due together with the
close event runs on finalized widgets. Reproduced deterministically under
gdb (headless Broadway, isolated config on a `.invalid` host — the live
sdr-for-linux binds 0.0.0.0:40001, never use loopback for a test client),
fixed with `app_teardown()` on `close-request` + `shutdown` backstop
(source ids cleared, pipeline stopped = engine thread joined, feed freed)
and ONE sentinel `app->closing` for every late callback; a
`GTK_IS_LABEL()` probe on the freed label would be UB, not a guard.
Same reproduction after: zero criticals, close→exit 4 ms. **Connected
close LIVE-VERIFIED the same afternoon** (Richard closed the builddir app
mid-session against his running sdr-for-linux: exit 0, one teardown line,
zero criticals). **11 gates green — the "12" in the M7
entry above was a miscount; meson lists 11.** **SKM-2 closed the same day
as upstream GTK/Pango, no code change:** the day-1 numbers were 16
warnings (8 images × 2, INT_MIN baseline, 16/16), reproduced byte-identical
on our binary under an empty fontset (first `pango_context_get_metrics`
returns 0/0 where fonts give 14550/3623; control run 0 lines); the source
reading lives in sdr-for-linux's SDR-3 write-up. Re-open recipe if the
warnings ever matter: `G_DEBUG=fatal-warnings gdb -batch -ex run -ex bt --args
builddir/skimmer-for-linux` catches the first one with a backtrace; if the
trace runs through `gtk_layout_manager_measure` and the Pango metrics are
zero it is this again — check the font cache first (`fc-cache -rv`,
`~/.cache/fontconfig`), not our code. **SKM-6 fixed the same
day (Richard's "ano"):** a second launch used to re-run `on_activate` in the
primary instance (second window, second App, second feed bind — reproduced
headless: the primary's stderr showed its own feed failing to bind);
`on_activate` now presents `gtk_application_get_active_window()` and
returns when it exists. Harness after: second and third launch each exit 0
at once, the primary logs `app: second launch — presenting the existing
window` twice, the feed binds once, zero warnings. The backlog's bug
section is empty.
**M8 — waterfall view, engine half (offline-proven 2026-09-05; Richard's
"ano" to SKM-4 half 1).** CW Skimmer's layout is the brief (frequency
vertical, time sideways, callsign column to the right, click tunes the
single VFO); the TX-split half is NOT possible against sdr-for-linux today
(`vfo:rx,ch,f` ignores the channel, split/RIT/XIT are echo-only, no VFO B
anywhere — filed as SDR-12 there). `spectrum.c` = FFT tap on the raw band,
bin 23.4375 Hz at every rate (N from the rate), a row per 10.7 ms, fftshifted
bytes = dBFS + 200, fed BEFORE the TX hold so the picture never freezes,
default OFF (`skim_pipeline_set_spectrum_enabled`). Gate
`skimmer-spectrum-test` 39 checks — the +12 kHz-ABOVE-centre orientation at
48/96/192/384 k is the first assertion; real-air check on the RTTY fixture
puts the strongest bins in the CW segment and the FT8 band, where they
belong. `SKIM_SPECTRUM_DUMP=1` on `skimmer-replay` prints the peak once a
second. **12 gates.** **The view landed the same day (headless-verified on
the RTTY fixture via Broadway + headless Chrome):** `wf_compose.c` (GLib-only,
gate-tested: full-res ring, pan/zoom window, max-pooling both axes, bins
centred — the + 0.5 the gate caught; palette + percentile floor copied from
sdr's waterfall.c) and `wf_view.c` (GdkMemoryTexture NEAREST, incremental
columns 17 µs, full band recompose 5.4 ms, kHz scale, VFO marker, wheel =
pan, Ctrl+wheel = zoom, drag the scale strip = pan; **a retune moves only
the marker, never the window** — Richard's first live-look rule; off-window
VFO = arrow on the scale; history in ABSOLUTE frequency with a per-row bin
shift, because the SDR has no CTUN and every retune moves the IQ centre —
his second live-look catch, "you reset my waterfall"; third catch "it tears
while tuning" → the SDR reported GUI tuning only every 500 ms — fixed there
(`tci_server_freq_changed`) + a retune guard here: 3-row delay, and after a
centre change EVERY row is dropped until the centre has stood still ~0.7 s —
the picture pauses while the knob turns instead of growing teeth, no matter
how slowly a server reports — and his FOURTH remark: that pause is wrong
too, the waterfall must FLOW through a retune; so the sdr build with the
immediate broadcast is now LIVE and `SKIM_WF_DEBUG=1` logs label-vs-data
moves per row to measure the lag; NEXT SESSION: discrete-step measurement →
label delay line instead of dropping rows). Rows ride the ONE
event queue as EV_SPECTRUM (cap 48 pending, oldest dropped), one queue_draw
per drain; header toggles list | waterfall over a GtkStack, `[ui] view`
persisted (old `station_list` migrates); spectrum computed only while shown.
**`SKIM_IQ_FILE=<cf32>` replays a recording into the UI** (offline pipeline,
real-time pace, looping) — the way to look at the waterfall without a
radio. Lag telemetry under the replay: 94 rows/s, worst drain 0.2 ms.
Gate 51 checks. **Palette picker** (Richard's first live-look request):
Preferences → Display → Colour scheme, the SDR's six schemes, live apply,
`[ui] palette`. Next: callsign column + click.
**M8 — callsign column + click-to-tune (headless-verified 2026-09-05
evening).** The column right of the scale is CW Skimmer's: a rail down its
left edge, a yellow dot on it at every tracked station's frequency, the
callsign beside it ("CQ " prefix for callers), and a connector dot→text —
flat when the label sits on its frequency, slanted when it had to move.
Layout is GLib-only in `wf_compose.c` (`skim_wf_layout_labels`): frequency
order kept, crowding labels seated by isotonic regression on
anchor − k·pitch (pool-adjacent-violators — a cluster spreads
SYMMETRICALLY about its mean anchor instead of piling downward), edge
clamps, and over capacity the lowest-priority labels hide (fixed station
> CQ > SNR; the dots stay). Label colours are the spot colours
(`SKIM_SPOT_ARGB`/`_DUP` through `skim_dup_verdict_gray` — pane,
panadapter and column agree), the pane's fixed station bold, hover
backdrop + pointer cursor. `skim_wf_view_set_stations` takes a snapshot of
the station table (CQ and S&P alike — the column is the list in another
shape), rebuilt at the end of a drain that touched the table or the
fixation, on the view switch and à 2 s (verdict recolours). Click = the
panadapter-spot gesture: `skim_pipeline_tune` to the station's TRACKED Hz
(not the pixel's) + `clicked_on_spot` (logbook prefill — the same path as
a pane click) + pane fixation; a press that travelled > 4 px is a drag,
not a click. Gate `skimmer-spectrum-test` 61 → 75 checks (layout + hit
test). Headless (Broadway + a CDP click, RTTY fixture, private D-Bus
session so Richard's live instance stays untouched): SV1JDZ labelled at
14 086.96, the click logged `wf click: SV1JDZ @ 14086964 Hz — tune +
clicked_on_spot`, pane fixed on it, label bold. NOT verified: the live
tune (offline `skim_pipeline_tune` is a no-op) and the logbook prefill —
Richard's live look. 12 gates green. README regrounded: twelve gates, as
meson lists them.
**Waterfall is the DEFAULT view (Richard's first live look at the column,
2026-09-05: "the station list probably isn't needed, the waterfall view is
much clearer").** `settings_load_view` falls back to `VIEW_WF` when no
`[ui] view` (nor the pre-M8 `station_list`) key is saved; a saved choice
always wins. The list and its toggle STAY for now — it is still the only
place showing SNR, WPM/Bd, heard and age; once the column carries those
(tooltip or dB after the call, CW Skimmer style) the list goes entirely
(Richard's two-step call). Same look: a hairline `GtkSeparator` under the
header bar, the faint line the decode pane already had above it — the
waterfall used to meet the header with no edge; shown ONLY while the
waterfall is the top view (Richard's second remark), hidden for the list
and the pane-only layout.
**Preferences in tabs (Richard, same session: "there is getting to be a
lot in there").** Four `AdwPreferencesPage`s in the family look (title +
symbolic icon, as sdr-for-linux's Radio/CW/TCI/Audio): Radio (TCI
server), Decoding (mode), Spots (panadapter policy + telnet feed),
Display (pane font, waterfall palette). Rows, data keys and
`prefs_closed` untouched — only the grouping changed.
**End of 2026-09-05:** live = sdr-for-linux `build/` ee7d08b + skimmer
`builddir` 8c5d458 (`SKIM_WF_DEBUG=1`, log live12). Open, in order: (1)
the callsign click LIVE (tune + logbook prefill) — unreported; (2) the
column takes over SNR/WPM/heard/age, THEN the station list is deleted
(Richard's two-step call, step one done); (3) waterfall flows through a
retune — SDR `p2_set_frequency` kick → re-measure → label delay line;
(4) bin/hop/span, crowded fan-out and column drain cost by his look /
`SKIM_LAG_DEBUG`. Full list: SCOPE M8 "STATE AT THE END OF 2026-09-05,
EVENING".
**The waterfall flows through a retune (offline-proven 2026-09-05 evening;
live measurement pending).** Open item (3) built on both sides. sdr-for-linux:
`p2_set_frequency` kicks the keepalive timer for ONE High-Priority packet on
a frequency CHANGE (piHPSDR parity: `rx_frequency_changed` →
`schedule_high_priority` only, read from dl1ycf master), without advancing
the cadence; gate `sdrfl-txiq-ring-test` [7] 46 → 50 checks (ten retunes on
the wire within 30 ms — 0.0 ms measured, old code red at 4/10; RX/TX-specific
≤ cadence; unchanged frequency does not kick). Before it the live probe
measured IQ 3–11 rows (32–117 ms) behind the dds label — the 100 ms quantum
over a ~3-row floor. Skimmer: the retune guard (3-row delay + 0.7 s settle
DROP = the pause) is gone; `wf_compose.c` has a **label delay line** — each
row placed on the label current `SKIM_WF_LABEL_LAG` (3) rows earlier, nothing
delayed or dropped, `SKIM_WF_LAG_ROWS=<n>` overrides for measurement; the
view no longer full-recomposes on a mere centre change (would be per row
during a sweep). `SKIM_WF_DEBUG=1` probe rebuilt for any step size (best lag
L 0..16 per row + flip row + per-episode summary). Gate spectrum 75 → 76;
headless replay draws as before. **Live-measured the same evening** (both
apps restarted on the new builds at Richard's "ok"): the raw-row probe was
blind (passband roll-off + DC spur correlate at zero shift; fixed by
detrend + masks + positive excursions only); then his knob sweeps voted
L=2: 557 / L=1: 305 / L=3: 103 rows and four discrete TCI steps flipped
the data 3, 2, 3, 2 rows after the label → **`SKIM_WF_LABEL_LAG` = 2**
(his instance: `SKIM_WF_LAG_ROWS=2`, log live15). **His look: "a bit
better, smooth, but the teeth are still there"** — spikes = whole rows one
tuning STEP off both ways (~10 % of sweep rows ≥ 4 bins, tail to 357),
because the `dds` label attaches to whichever IQ block is arriving: ±1
block of jitter no constant lag removes. **Fix built + gated the same
evening, live look pending: IQ centre STAMPS.** sdr-for-linux: `iq_stamp:1;`
(opt-in, echoed) fills h[8] = centre of the block's first frame, h[9] =
frame offset of a retune inside the block, h[10] = the new centre — the
DDC-ring sample index at the change + `SDRFL_DDC_LAT_MS` (2.5) through the
resampler ratio; unstamped clients byte-identical (gate sdrfl-tci-test
43 → 50). Skimmer: client asks, takes h[8] over the label, splits the block
at h[9] (tci gate 17 → 20); `spectrum.c` takes the centre per push and
labels each row with the centre at its window MIDDLE (gate 76 → 92); the
app delay line defaults to 0 (`SKIM_WF_LAG_ROWS` kept for experiments).
Acceptance: sweep rows ≥ 4 bins off 9.7 % → ≈ 0. **Live 20:42–20:54:** stamps
confirmed on the wire, every sweep row on its own label (worst 11 bins vs
132). His second recording: spikes gone, vertical BARS at retunes remain =
the 4-hop window straddle (a row holding the step carries a line at both
positions). Fixed in `spectrum.c`: a straddling row is computed from its
largest single-centre segment only (fresh Hann, floor renormalised),
labelled with that segment's centre — a step shows a 2-column widening, a
sweep ~4× wider lines while turning; gate spectrum 92 → 100 (tone stays on
its absolute frequency across a +5 kHz step, ghost 66 dB down). Skimmer
relaunched on it (live18). His look: bars STILL there — measured cause: the
stamp boundary sat 10–20 ms early with a spread (probe flips 1–2 rows late)
because the SDR's TCI push trails the capture clock by the listener's
socket backlog; fixed in sdr-for-linux 98c57de with a capture clock from the
pushes (boundary by TIME; gate ±0.5 ms). An n/16 guard band in the skimmer
cut was built, measured to add nothing (the segment Hann's taper already
leaves a late label no weight: 58 dB down) and removed; gate 100 → 108.
SDR restarted 21:27 on the clock: flip metric 0 rows on 14/16 retunes, 0 of
116 sweep rows ≥ 4 bins off. **LIVE VERDICT (Richard, ~21:35): on 80 m "už
to nedělá" — the waterfall flows through a retune, no teeth, no bars, no
pause; open item (3) CLOSED.** His first look had been at 1 Hz on the DC
line at 50–100 kHz/s, where a line is inherently ~4× wider while turning.
Left, each a separate decision: coherent re-centring of the window (only if
the DDC NCO is phase-continuous — measure first), `SDRFL_DDC_LAT_MS` 1.0 ms
unmeasured below a row, the SDR coming up at 1 Hz after a restart. **END OF
2026-09-05 (night): live = sdr `build/` 98c57de + skimmer `builddir` 2ec8fd1;
open in order: callsign click live → column takes over the list's columns,
list deleted → SDR-13 → coherent re-centring only after a phase measurement
→ DDC latency → bin/hop/span + fan-out + drain cost.** ⚠ 20:44 a peek script
imported the TCI client (main ran at import) and tuned his radio to 4 Hz for
~25 s; restored, main-guarded. Not built by design: a polling-server settle
fallback (a click + stillness looks the same).

**Callsign click LIVE-VERIFIED (Richard, 2026-09-05 22:05) — open item (1)
CLOSED.** Logbook from its `builddir` (76349f8) beside sdr 98c57de + skimmer
2ec8fd1 on the one TCI server, Call entry empty; a click on a waterfall
column label: "jo, to vypadá, že funguje" — radio tuned, logbook took the
call. Log corroboration for the tune half: two discrete ±855-bin retunes
at 22:05:03/22:05:46 in the `SKIM_WF_DEBUG` probe (single label step, rows
after at L=0 — a click, not a knob). The `wf click:` line is gated on
`SKIM_PANE_DEBUG` (unset live) and neither relay nor logbook logs a
received click, so the prefill half rests on his eyes. The pre-tag open
item (2) above (prefill into an EMPTY Call entry) is closed by the same
test. Open, in order: column takes over the list's columns → list deleted
→ SDR-13 → coherent re-centring after a phase measurement → DDC latency →
bin/hop/span + fan-out + drain cost.

**Column takes over the list's columns (offline-proven 2026-09-05 night;
Richard's live look pending) — open item (2), step one.** The waterfall
label reads "CQ DL1ABC 23 dB" (dB dimmer, never bold) and its tooltip
carries the rest, "DL1ABC · 14025.30 kHz" / "25 WPM · 23 dB · heard 12× ·
age 8s" ("Bd" in RTTY) — the list's cell formats, pinned in GLib-only
`wf_compose.c` formatters so the gate reads them and the two views cannot
drift; `SkimWfStation` grew mode/speed/reports/last_heard; `query-tooltip`
uses the click's hit test and sets the tip area to the label's seat band.
Widths Pango-measured (118 of 154 px worst case, `COLUMN_W` unchanged).
Gate spectrum 108 → 117; 12 gates green. Headless (Broadway, RTTY
fixture, CDP hover): label and tooltip render; the decimal comma is the
locale (the list shows the same) and the 831-minute age is the replay
clock (the list's Age column too; live both clocks are monotonic). Not
built: dB smoothing — the suffix may flicker under load, his look
decides. Step two (delete the list, its toggle, sorter, fmt_*) waits for
that look.

**Live look on the column (Richard, 2026-09-05 ~23:25): the dB after the
call is OUT — "stačí to, co je v tooltipu, tam to vypadá líp".** Label is
"CQ SV1JDZ" again; the tooltip alone carries kHz / speed / dB / heard /
age. `skim_wf_label_text` + `_snr_text` and their four checks removed
(spectrum 117 → 113), tooltip machinery unchanged. Step two (delete the
list) next, on his word.

**The station list is GONE (Richard's "ano, vyhoď ten seznam", 2026-09-05
~23:30) — step two done.** `main.c` −190 lines: `GtkColumnView` + columns +
formatters, sort model + `freq_cmp`, `on_row_activated`, list toggle,
`GtkStack`, `VIEW_LIST`. The `GListStore` of `SkimRow` stays as the station
table's mirror (column + pane resolver read it). Header keeps ONE toggle,
waterfall on / off; `[ui] view` = "waterfall" | "none", a saved "list" (and
pre-M8 `station_list=true`) falls to the waterfall. Headless-verified
(Broadway, RTTY fixture, CDP toggling; settings written as expected, zero
warnings); 12 gates green. **Open, in order: SDR-13 → coherent re-centring
only after the NCO phase measurement → DDC latency → bin/hop/span, fan-out,
column drain cost under contest load.**

**END OF 2026-09-05 (Richard ~23:45, "jo, je to dobrý" on the list-less
window): live = sdr `build/` 98c57de + skimmer `builddir` c1556f5 (live21)
+ logbook `builddir` 76349f8; next session starts at SDR-13 in
sdr-for-linux.**

**2026-09-06 — open list re-cut (Richard's call).** SDR-13 read, not
reproduced: persistence path clean, 1 Hz = the GUI tuning clamp, last
frequency before the SIGTERM unrecoverable — stays open in sdr-for-linux's
BACKLOG (1f288d9) with a recipe for the next planned restart. Items (4)
coherent re-centring and (5) DDC latency are SHELVED as invisible polish
(do not offer again without a visible symptom). **Next session, in order:
(a) prepare and publish v0.4.0** — 40 commits since v0.3.0 (M8 waterfall +
column + click, SKM-1/SKM-6, list gone), flow as v0.3.0: tag first, sha256
from the published tarball, `.SRCINFO`, AUR push; **(b) RTTY over-head
pre-roll fix** (OPEN item under M7 in SCOPE, fixture in
`/var/tmp/skimmer-iq/`).

**v0.4.0 RELEASED 2026-09-06 (Richard's "ano" to the shown bump diff + the
notes, en + cs).** Content: M8 waterfall + callsign column + click-to-tune,
the retune flow (IQ centre stamps), the station list gone, Preferences in
four tabs, SKM-1/SKM-6. Bump a9dc73f (meson, metainfo release entry + a
waterfall paragraph in the description, desktop Keywords, PKGBUILD SKIP,
README regrounded: M7/M8 in Status, a waterfall paragraph, no "station
row"), signed tag v0.4.0 = eb73cbe, checksum commit 194c42a (tarball
sha256 bbd35f4d…cb1cfc — one `curl -f`, `tar t` before trust; codeload
answered 200 first try). Flow refinement that WORKED: `gh release create
--verify-tag --notes-file` run right after the tag push (release 10:38:31,
CI run 34028010063 queued 10:38:56) — the CI attach step found the curated
release, no empty body to edit afterwards. CI green (gates + artifacts),
AppImage/deb/rpm attached. AUR 0.4.0-1 (fc5a832): the downloaded tarball
was placed in the clone so `makepkg -f` did not download; sha matched,
check() 12 gates green; namcap adds two informative "implicitly satisfied"
warnings (cairo, pango — the waterfall links them directly, gtk4 pulls
both) to the known three. CORRECTION to the entry above: sdr-for-linux's
last tag is **v0.5.0** (3943ee4, 2026-08-25), not v0.4.2 — the local clone
had no fetched tags; the four retune-flow commits (ee7d08b, 51f981a,
e2fb076, 98c57de) all sit after v0.5.0, so the notes say "v0.5.0 works,
the smooth retune wants main ≥ 98c57de" (the client falls back to the
label when h[8] is 0, the server ignores unknown commands — both
verified). sdr itself was not released (not asked). Not done: local
install to ~/.local, a waterfall screenshot for README/metainfo (Richard's
live shot). **Next: (b) the RTTY over-head pre-roll fix.**

**gh#2 — SunSDR/ExpertSDR3 asked for; TCI hardening SKM-7..11 DONE
(offline-proven 2026-09-11; first foreign-server report the same day).**
Tomas SM0ONR (gh#2, 2026-09-10) asks whether a SunSDR runs the skimmer.
Audit of `tci_client.c` against the TCI 2.0 spec + Thetis' TCIServer.cs
found five assumptions that hold only for sdr-for-linux, all fixed: ONE
WebSocket message = ONE Stream block (`lws_is_final_fragment &&
lws_remaining_packet_payload == 0`; trailing bytes ignored + logged once,
short message dropped + warned — SKM-9); blocks with h[0] ≠ 0 dropped
(Thetis AlwaysStreamIQ pushes every receiver — SKM-7); the centre stamps
h[8..10] count only after the server echoed `iq_stamp:1` (SKM-8); the IQ
request goes out as three text frames (SKM-10); `[tci] port` is a setting
(Preferences → Radio → Port, 1–65535, default 40001; probe, pipeline and
About read it — SKM-11). Gate `skimmer-tci-test` 20 → 32 checks incl. a
second session against a mock that never echoes `iq_stamp` and a third
against one that answers `iq_samplerate:96000` and never starts IQ (both
diagnostic lines read back through a log-writer tap); every fixture
is followed by a MARKER block and the wait is "all queued markers back"
(the first version read three checks green on the pre-fix code because the
fixture was still queued — timing trap, fixed); against the pre-fix client
five checks go red. Three diagnostic log lines for a remote tester: first
accepted block (receiver/rate/format/channels/frames), `iq_samplerate`
echo ≠ request, and a 3 s no-IQ warning quoting what the server announced
— armed via `lws_sul_schedule` when `iq_start:0` is written, because
`lws_service()` (lws ≥ 3.2) ignores its timeout and a polled check came
~5 s late on a silent link. 12 gates green; SKM-11 headless-verified (isolated config on port 40123 →
probe + WS connects land there). **His report the same afternoon: at his
default settings "connects, no output"; with the device bandwidth at
156 or 312 kHz the waterfall shows, decoding works and spots appear on the
ExpertSDR3 panorama — the first confirmed run against a foreign TCI
server.** Hypothesis, unverified: a device bandwidth below our requested
192 k makes ExpertSDR3 refuse or silently not start IQ; his log with the
new lines decides. Open: how Tomas gets a build (main vs a v0.4.1 tag),
and the reply in gh#2 (Richard's approval, en + cs).

**v0.4.1 RELEASED 2026-09-11 (Richard's "dobře, pokračuj" to the shown bump
diff + notes en/cs; his rule: NO epithet — title "Skimmer for Linux 0.4.1",
tag message "release: v0.4.1").** Content: SKM-7..11 (TCI hardening + port
setting + diagnostics), README regrounded (any TCI server "in principle",
host + port in Preferences, not yet a tested target, gh#2 linked), metainfo
release entry, PKGBUILD two-step. Flow as v0.4.0: bump commit → signed tag →
push 19:33:07 → `gh release create --verify-tag --notes-file` 19:33:09 (CI
run 34628325674 started 17:33:09Z, attached to the curated release) →
tarball sha256 4f7c705b…ea378 (171 entries, `curl -f` + `tar t`) → checksum
commit 5e48489 → AUR clone in `/var/tmp/skimmer-v041/aur`, `makepkg` with
check() = 12 gates green, namcap only the known five informative warnings,
`.SRCINFO`, commit ff119a4 "Update to 0.4.1" pushed 19:34:54 (page showed
0.4.1-1 within a minute) → `meson install -C build-release` → `~/.local`
launcher = 0.4.1, `builddir` rebuilt to 0.4.1 too. CI green: gates +
artifacts, AppImage/deb/rpm attached (not draft, not prerelease). CI note:
GitHub warns actions/checkout@v4, upload-artifact@v4 and
softprops/action-gh-release@v2 target Node 20 and are forced onto Node 24 —
bump the action majors before the next tag. About shows the version through
`SKIMMER_VERSION` = meson `project_version` (Richard asked to watch it). The
release build's engine is the code that ran live from 18:49 (spots on the
feed and the panadapter verified). Next: the gh#2 reply (draft en + cs in
`/var/tmp/skimmer-v041/reply-tomas-*.md`, waiting for Richard's "ano"),
then SKM-12.

**DeepCW neural CW engine — evaluated + BUILT offline (2026-09-12, Richard's
"ano" after the evaluation).** The published DeepCW model (e04, AGPL-3.0-only:
a 3.6 M-parameter Conformer + CTC over a 65-bin × 15 ms log1p spectrogram)
read two real IQ fixtures at least as well as v2 and found EA6AOY, which v2
logged as EAAOY for 180 s; GPLv3 §13 ↔ AGPL §13 permit the combination
(both texts read). Built: `vendor/onnxruntime/` (MIT C API header v1.21),
`ort_shim.c` (dlopen — the app never links the runtime; missing runtime or
model = "not available", pipeline falls back to v2), `decode_deepcw.c` behind
the decode.h vtable (20-point DFT tiles from the 250 Hz channel, 10 s ring,
1.6 s ticks, energy gate, word-gap commit, one word per process()),
`SkimCwEngine` in the pipeline config + `SKIM_CW_ENGINE=v1|v2|deepcw`, gate
`skimmer-deepcw-test` (27 checks, model part SKIPs without
`SKIM_ORT_LIB`/`SKIM_DEEPCW_MODEL`) — **13 gates.** Offline A/B on the 20 m
and the fresh 80 m contest fixture: full account in
`docs/DEEPCW.md` (wins EA6AOY, OK5O, OK1CZ, DL3GAK; costs: fewer hits →
station-table takeovers lose some S&P callers, OK1DOL parked on a spur by the
same-call rule, mutation twins). Replays run inference INLINE (1.3–1.6×
realtime, `SKIM_DEEPCW_SYNC=1` set by skimmer-replay); the app runs it on
`SKIM_DEEPCW_WORKERS` (2) threads with a per-channel mailbox, the commit rule
staying on the engine thread. **The switch is in: Preferences → Decoding →
"CW engine" (Classical (v2) / DeepCW (neural)), `[decode] engine`, subtitle
= availability on this machine, About carries the resolved engine, the app
logs `app: pipeline engine <name>`.** Headless-verified (Broadway, isolated
config, IQ replay into the UI): DeepCW decodes through the async path; a
missing model → warning + v2. Model at
`~/.local/share/skimmer-for-linux/models/deepcw/` (not in git).
**GPU (same night, Richard's ok):** `onnxruntime-cuda` 1.29 installed from
extra (system-wide `libonnxruntime.so.1` — no `SKIM_ORT_LIB` needed), the
shim appends the CUDA provider with a CPU fallback + reason (Arch quirk: the
provider lacks a NEEDED on libcudnn — the shim preloads it RTLD_GLOBAL),
workers batch all queued windows into one run, Preferences → Decoding →
**Device** (CPU / GPU (CUDA), shown only for DeepCW, `[decode] device`),
gate 35 checks. 80 m replay: 103 s on CUDA:0 vs 190 s CPU, same table ±1
marginal station. NPU needs an OpenVINO runtime path (no Arch ORT package
has that provider). **Latency (2026-09-13): the commit rule is a SLIDING
window** — the whole 10 s ring re-read every tick (0.5 s on CUDA, 1.0 s
CPU), a character final once ≥ 1.0 s before the window end, a frame cursor
against double emission; the old drop-committed-audio rule tore words when
sped up (34 → 27 stations), this one keeps 33 with v2-like report depth and
puts OK1DOL/OK1MDK on their real frequencies. Latency ≈ 1.5 s on the GPU.
Classical v2 stays the default; Richard runs DeepCW on CUDA live (live27);
next: gray draft text in the pane via the phase-B ops, the station-table
QSY rule, OL1B/OK1C-class tears.
**DeepCW LIVE first look + gray DRAFT built (2026-09-13).** Richard on the
engine live (20 m, quiet Sunday band): "zatím to překládá dobře", the delay
is noticeable, its verdict waits for a CW contest. Then the draft, on his
word with a recorded return point (main 45435e9): `skim_deepcw_commit_ex`
returns the tail guard's reading as a draft; the backend composes an over
region (pending final words plain + draft dim) through the decode.h pane
ops at the end of `process()`, OPEN/SET/CLOSE, no `fresh`, `pane_own`
FALSE — extractor, station table, spots and decode log untouched by
construction; `SKIM_DEEPCW_DRAFT=0` switches it off. Pipeline fix on the
way: ops-only hits route by the backend's tone offset (`hit_placeholder`),
or a draft on a tone > 25 Hz off centre opened a never-closed region in
another pane slot. Gate deepcw 35 → 50 (draft units, pane invariant with
the model, whole-pipeline routing at +40 Hz — red on the pre-fix code).
Headless-verified on the 80 m fixture: the gray tail firms in place, no
doubled word, no hole; ~56 ops/s band-wide, worst drain 0.2 ms. Richard's
live look on the draft pending (his instance still runs the pre-draft
build). Full account in `docs/DEEPCW.md`.
**Draft made readable + DeepCW word gaps (2026-09-13 afternoon).** His look
at the raw draft: "není nic čitelné" — measured: the last 384 ms of the
window read 11–44 % right, so the draft is now the tail's RELIABLE PREFIX
(≥ 0.384 s from the end, posterior ≥ 0.9: 98 % right, lead ≈ 0.6 s;
`SKIM_DEEPCW_DRAFT_MIN/_P`). His second look: "lepší, ale nedělá mezery
mezi slovy" — not the draft (finals identical, widget == FreqLog): DeepCW
finals lose word gaps. Fixed two of three causes (weak-gap glue only for a
torn call tail and never before a known short word; a gap placed on the
cursor passes unless a re-read of the committed gap): +5 % spaces, 34 vs 33
stations, none lost. The third — the model emits no space after a long
pause / at an over boundary — resisted an envelope-based gap twice (5 dits
at the WPM: 13 stations lost; fixed 0.6 s: 6 lost + mutations) and was
REMOVED; a real keyed detector would be the prerequisite. Gate deepcw 58
checks, 13 gates green. Details in `docs/DEEPCW.md`.
**END OF 2026-09-13 (Richard ~16:40: "budu dál testovat, pro tuto session
je to prozatím vše").** Live = skimmer `builddir` 848cb66 + this docs/debug
commit (live32, DeepCW on CUDA, draft on) against the installed
sdr-for-linux 0.5.1. His "slyším CW, ale nic nepřekládá" was a QSO's weak
side (CT1BMW) under G4BPJ, not the channel — and it surfaced a KEYED copy
of strong stations at the DDS centre in the IQ stream (~20 dB down), to be
filed in sdr-for-linux. `SKIM_FLOCK_DEBUG` added. The unsigned commit
60fe31f stays (Richard: do not rewrite it; every commit from now on is
signed — ground rule above). Open, in order: (1) file the DC keyed-copy
finding in sdr-for-linux's BACKLOG and measure it there; (2) his verdict on
the draft + word gaps after more testing; (3) the model's omitted gaps
after long pauses (needs a real keyed detector); (4) latency verdict waits
for a CW contest.

**gh#17 — the waterfall pauses on a muted stream (offline-proven 2026-09-19,
mid-SAC-CW; Richard's live look pending).** Live: every own over turned the
waterfall white, ~4 s to settle, the over a black band in it. Measured on
`iq-20260919-sac-20m-192k.cf32` (eight overs): the wire carries EXACT zeros
during TX (eight zero runs, to the sample), such a row reads −200 dBFS, the
view's floor tracker fell −127 → −191…−198 dBFS within one over and
`wf_view.c` recoloured the whole history on every 1 dB of drift. Fix in
`spectrum.c` `emit_row`: a kept segment of nothing but exact zeros → no row;
a zero run ≥ N/32 at either end → the row comes from the live part alone via
the retune cut path (fresh Hann, floor renormalised); under a hop of live
signal → no row. The pause hangs off the DATA, not the TX hold — sdr-for-linux
reports `trx` from its 500 ms reporter poll (tci_server.c, read), measured
0.04–0.43 s behind the zeros at key-on and 0.30–0.79 s at release (poll +
grace): a flag pause would still leak ~40 dead rows per over and cut 0.3–0.8 s
of live picture after it — not built. The edge cut was measured too: without
it a chopped window smears the strongest line across the row 15 dB down and
moves the floor 2.8 dB. Same recording after: tracked floor −127.0…−125.9 dBFS
over 300 s, no row under −128, station table + counters identical. Gate
`skimmer-spectrum-test` 113 → 139 (mute section × 4 rates + pipeline; 25 red
on the pre-fix code; the reset/label checks feed noise now — an exact-zero
window draws no row by design). `SKIM_SPECTRUM_DUMP` prints the tracked floor
per second. 13 gates green. Where the zeros originate (radio DDC vs
sdr-for-linux) is NOT verified — the wire fact suffices. **Finding for
separate tickets (not acted on):** the same late flag means the DECODERS see
up to ~0.4 s of dead stream before every hold and lose up to ~0.8 s of the
answer after it — an immediate `trx` broadcast in sdr-for-linux (parity with
the `p2_set_frequency` kick) and/or a data-driven hold here; plausibly
related to #18.

**gh#17 LIVE-VERIFIED (Richard, 2026-09-19 ~18:10, SAC CW, 20 m).** His
instance restarted on 1b5a9c4 at 18:06 (live34, DeepCW on CUDA; IQ back in
0.8 s) and his own overs from 18:06:30 on: "vypadá to dobře, jen se pausne
a vyčká, dokud TX neskončí" — no white-out, no black band, the picture waits
and continues. The TX gap carries no marker and he did not ask for one.
Still open from this work, as separate tickets on his word after the
contest: the late `trx` flag on the DECODER side (immediate broadcast in
sdr-for-linux; a data-driven hold here).

**gh#18 — DeepCW: a TX hold commits the tail instead of dropping it
(offline-proven 2026-09-19 evening, mid-SAC-CW; Richard's live look
pending).** Live: the gray draft vanished when he keyed — in S&P the call of
the station being answered, lost to the pane AND the spot path (the tail
guard never gets its right context through a hold, `resync` abandons it);
replayed on the SAC recording with its eight own overs as holds:
`TEST SE5E TEST SE5TEST`. Built: decode.h **`hold_begin`** (optional hook,
fired when the hold engages) + the pipeline **pumps** such a backend with
zero-frame `process()` every 4th swallowed block (pass 2 of `process_block`
moved verbatim into `dispatch_hits`), so the flushed text reaches pane,
extractor and station table DURING the transmission; DeepCW runs one more
inference over the window with 0.5 s of dead air appended and commits the
tail. Four measured rules (full account in `docs/DEEPCW.md`, "TX hold"):
(1) the window ends at the LIVE end — the wire's mute (exact zeros, flag
0.04–0.43 s late) is a broadband CLICK in every channel and the model read
it as "E" at p 1.00; (2) the last word goes out only when its own line was
silent 6 dits at the live end (noise-relative bar — a half-peak bar called
KC1XX silent in a QSB dip and "KC1" went out), else the flush stops at the
last word gap: a cut call must not validate; (3) one spike under p 0.5 in
the tail zone drops the last word whole; (4) the word is closed with a gap
only when whole (fixes `OH2BBMOK1BR`; a cut head stays open and fuses, as
before). Measured on 37 synthetic overs laid into the recording
(`SKIM_REPLAY_HOLDS` / `_MUTE` / `_FROM` / `_TO` in skimmer-replay,
`SKIM_DEEPCW_ONLY=<lo-hi kHz>` — the full contest band runs 0.1× realtime on
the CPU and the GPU stayed with the live instance): 241 of 258 flushed
characters agree with the no-hold reading, ALL 213 at p ≥ 0.9; 64 whole last
words exact, 4 shorter than the reference's word (two after 0.7 s of silence,
one the exchange "5NN 18", one possibly real: SM6) — NONE of the four
validates as a callsign (`skim_callsign_is_valid`; OK1B/SM6X/OH2X do), which
is the criterion the issue set. Direction B (promote the draft at resync)
REJECTED on the same dumps: a word HEAD on 201 of 239 cut words. Station
table identical but for one stale-candidate artifact (SM2EKM, traced).
Known limit: keying inside ~0.35 s of the over's end still loses the last
word (DFT/prototype smear in the silence test) — smear compensation is the
next lever if live asks for it. Gate `skimmer-deepcw-test` 58 → 69 (sync
65), red without the live-end scan; 13 gates green. `SKIM_DEEPCW_FLUSH=0` =
the old path for A/B.

**gh#18 — first live look (Richard, 2026-09-19 ~20:20, SAC CW, 20 m).** His
instance restarted on 544f756 at 20:08:37 (live35, DeepCW on CUDA; a restart,
settings untouched; IQ back in 0.1 s) and after a first round of testing:
"uvidíme při závodech, ale co teď testuji, tak se to chová mnohem lépe" — an
interim verdict, not the final one (the contest was still on, but the band
"už není tak plné" by then): the issue STAYS OPEN (`needs-live-check`) until
a full contest band has shown it over many overs. What to watch there:
the known ~0.35 s limit (an answer keyed right on the end of the over) and
whether it asks for the smear compensation.

**gh#15 — MASTER.SCP keeps itself current (offline-proven + one real-site
run 2026-09-19; Richard's live look pending).** New dependency **libcurl**
(Richard's "ano"; all C). Why not libsoup-3: `linuxdeploy-plugin-gtk` does
not bundle GIO modules (read in its script), so an AppImage on a non-Ubuntu
host would have no TLS; libcurl rides along with its TLS libs (neither
libcurl nor libgnutls is on the AppImage excludelist — read 2026-09-19; only
libgmp is, the host's is used) and the CA bundle is looked up in code (`skim_scp_env_ca_path`, only under
`$APPIMAGE`/`$APPDIR` or `$SSL_CERT_FILE` — AppImage-on-Fedora itself is
NOT verified, no such build exists before a tag). Site facts, checked by
hand: `/api/v1/files` names files (no URLs), the file is at `/<name>`, 304
to If-None-Match, and **the ETag is the sha256 of the body** (undocumented —
enforced only where an ETag is 64 hex). Three commits: (1) `Fixes #7`, the
loader skips "!!Order,1,1" and anything lexically not a call — NOT
`skim_callsign_is_valid`, which rejects 72 of the release's 50 003 calls
(C4W, D4C, C5A … — a ticket of its own); (2) `callsign.c` reloads under
readers (new table built unlocked, pointer swap under a `GRWLock`, old table
freed outside; the old in-place reload SEGFAULTs under the new gate check),
one length-bounded parser for load and `skim_callsign_dict_inspect()`, the
"# Release" line kept for About; (3) `src/app/scp_update.c` (GLib + libcurl,
GTK-free): file list → skip when the list's etag equals the local sha256 →
conditional GET → in-memory checks (sha256 vs ETag, ≥ 10 000 calls, ≥ 98 %
validating, ≤ 1 % junk lines, ≥ 50 % of the file replaced — measured on the
real file: 99.86 % validate, 0 junk) → `g_file_set_contents_full` (atomic)
→ live `dict_load` on the WORKER thread. **Nothing waits on the network:**
curl multi interface + `curl_multi_wakeup` from the cancelling thread
(free() mid-transfer measured 0.1 ms; a bounded 0.4 s grace only for a stuck
resolver), connect 10 s / total 60 s / low-speed aborts, SIGPIPE ignored
(CURLOPT_NOSIGNAL drops libcurl's own shield), env read on the main thread
only. **Rate limits persisted in `master.scp.state`** (Richard's request):
1 completed check / 24 h, 1 h after a failure, 4 attempts / 24 h, 3 full
downloads / 7 days, the attempt recorded BEFORE the request, no request at
all when the state file cannot be written, future stamps clamped to now.
Preferences → Decoding → "Keep MASTER.SCP current" (default ON, `[scp]
auto_update`), About carries release + last check; `SKIM_SCP_URL` points
the app at a mock. Gates: call-test 23 → 32, new `skimmer-scp-test` 45
checks against an in-process mock HTTP server on its own GMainContext thread
(21 failure classes each leave file + loaded dictionary untouched; main-loop
beat gap 10 ms through a 1.5 s-slow server) — **14 gates**, clean under
ASan + UBSan. Headless app run (private D-Bus session, isolated XDG,
`.invalid` TCI host, Python mock): updated 15 s after launch, second launch
sent nothing. Real site once, on a scratch copy of the 2026.07.15 file:
UPDATED to 2026.09.18 in 0.60 s, second run NOT_DUE. NOT verified: the
Preferences group and About lines on screen (dialogs open warning-free,
nobody looked), AppImage TLS. **Live check = Richard restarts after SAC: the
log says `scp: … updated — release …` ~15 s in and About shows the new
release; no radio needed.** Build lesson: his live instance runs from
`builddir`, so this work was built in `/var/tmp/skimmer-issue15/build`.

**gh#15 LIVE-VERIFIED and CLOSED (Richard, 2026-09-20 ~11:27, SAC CW, 20 m).**
His instance relaunched on 7cb1fbe against the running sdr: the check went out
15 s after launch, against the real site, and replaced the hand-copied July
file — `scp: 2026-09-20 11:27 updated — release 2026.09.18, 50003 calls (was
50014)`. The reload-under-readers path ran while the extractor and the pane
were reading the table, at full contest load and through his own overs: no
warning, no crash, the engine kept decoding. About: `(50003 calls, release
2026.09.18)` + `auto-update: on, last completed check 2026-09-20 11:27` — and
that line is itself the evidence the swap reached the engine, because
`skim_callsign_dict_size()`/`_release()` read the live table under the reader
lock, the one the extractor queries through `dict_has()`. Preferences →
Decoding → "Callsign dictionary" (the switch + a read-only "Loaded list —
Release 2026.09.18 · 50003 calls" row) renders as intended. His July
`master.scp` was overwritten; a copy was taken first
(`~/.config/skimmer-for-linux/master.scp.2026-07-15.bak`). Left open, NOT part
of the issue: TLS from inside an AppImage (CA bundle under
`$APPIMAGE`/`$APPDIR`) — a release check, to be made when the next tag builds
one; and a ticket of its own for `skim_callsign_is_valid` rejecting 72 of the
list's calls (C4W, D4C, C5A, D2A …), not filed yet.

## Layout

```
src/engine/   headless, GLib-only:
  tci_client   WS client, IQ ingest (true orientation), outgoing SPOT
  channelizer  polyphase filter bank → complex baseband per channel
  decode.h     backend interface: channel → { text, confidence, freq, wpm/baud }
  decode_cw    CW backend (phase 1: v1 classical + v2 Viterbi)
  decode_rtty  RTTY backend (M7: Baudot FSK 45.45 Bd); later decode_psk
  decode_deepcw  DeepCW neural CW backend (Conformer + CTC via ort_shim/dlopen)
  station      per-frequency station tracker
  callsign     extraction + validation (RBN-grade)
  spot_out     TCI SPOT feed + RBN telnet feed
src/app/      GTK4/libadwaita: main.c (window: waterfall + callsign column over the
              decode pane), wf_view.c (widget), wf_compose.c (GLib-only pixels + layout),
              scp_update.c (GLib + libcurl, GTK-free: MASTER.SCP background updater)
vendor/wdsp/  in-tree WDSP copy (FFT + resampler)
vendor/onnxruntime/  ONNX Runtime C API header only (MIT) — dlopen at run time
docs/SCOPE.md the plan
```
