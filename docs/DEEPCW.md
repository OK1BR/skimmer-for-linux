# DeepCW — the neural CW engine: what was measured, decided and rejected

The second CW decode backend behind `decode.h` (`src/engine/decode_deepcw.c`,
ONNX Runtime through `src/engine/ort_shim.c`), evaluated 2026-09-12 and built
2026-09-12/13. The classical v2 decoder stays the default. **What is still open
lives in issue #5**; this file is the measured record — moved here, minus the
gate-count and headless-check narration, from the work-queue write-up when the
queue went to GitHub Issues (BACKLOG SKM-3; last full version: commit 96606bf).
Upstream: <https://github.com/e04/deepcw-engine> (model + minimal Python/Node
example), demo front-end <https://github.com/e04/web-deep-cw-decoder>.

## Why a second backend

`decode.h` was specified as a pluggable interface precisely so a channel can be
handed to something other than the hand-written DSP decoder. DeepCW is the first
credible candidate: a small ONNX model that decodes CW from audio, taking mono
PCM at **3200 Hz** (`ffmpeg -ac 1 -ar 3200 -sample_fmt s16`), with the model and
its metadata (`model.onnx`, `model.onnx.json`) shipped in the repo.

Why it is interesting for a skimmer specifically: a threshold decoder decides
per sample whether the tone is above a level, so any interferer at the same
level breaks it. A trained model decides on the *rhythm* of the keying instead —
the same thing an experienced operator's ear does — which is exactly the regime
a band-wide skimmer lives in, where most channels are weak and crowded.

**Author's own figures, not reproduced here:** 0.00% character error from 0 down
to -4 dB SNR at all tested speeds, below 1.5% at -8 dB and below 8% at -10 dB,
measured in AWGN at roughly 50% keying duty cycle. Treat as a claim to verify,
not a specification.

## Evaluation (2026-09-12) — measured, not read off the README

*What the published model is.* `deepcw-engine` (2 commits, 2026-06-16,
AGPL-3.0-only) ships ONE model: `model.onnx`, 3.61 M parameters (15 MB), a
Conformer encoder (3 Conv2d front-end layers striding only in frequency, then
6 blocks of FFN + multi-head self-attention + depthwise conv k=17, d=192),
CTC head over 42 classes (letters, digits, `, . / ?`, space, blank), opset 18,
exported from torch 2.10. Input `[batch, 1, time, 65]` = `log1p |STFT|` of
3200 Hz audio, FFT 256 / hop 48 (15 ms), the 65 bins covering 400–1200 Hz at
12.5 Hz; output `[batch, time, 42]` log-probs, ONE frame per 15 ms, greedy CTC
collapse. The attention is global over the whole window (no causal variant),
which is why the author's example wants 5–20 s clips and the web app streams
by re-inferring a sliding window and committing text only up to a word gap
≥ 1.25 s before the tail. **The web app (`web-deep-cw-decoder`) is a different
deployment:** 9600 Hz / FFT 768 / hop 192 / per-window CMVN, and its four
models (`en`, `en_narrow` 5-bin pileup, `ja`, `cw_detect`) are fetched from
deepcw.cc by UUID and are NOT published — only the standard engine model is
usable, and the README's CER heat-map is that deployment's claim. The web repo
carries no licence file (all rights reserved by default), so any streaming /
stitching logic here must be our own implementation, not a port.

*Synthetic checks (own keyer, SNR in 2500 Hz):* 25 WPM exact copy at 0, −6,
−8 dB; −10 dB ≈ 6 % CER; collapse at −12 dB. Tones at 450 and 1150 Hz (band
edges) read exact. **Noise-only input gives an EMPTY output** (six seeds at
4 s and 12 s, also after level normalisation, also on an empty fixture
frequency) — no phantom text on dead channels, the July reader's failure.
Two tones 100 Hz apart in one window give plausible-looking nonsense
(`CQ51AKCXC1BK 2KA`) — that is the phantom mechanism, so a window must hold one
station. A 65-bin tile with everything outside ±3 bins (±37.5 Hz) zeroed reads
exactly as the full audio; ±1 bin degrades — so our 125 Hz channelizer output
IS a sufficient input, no audio path needed. Hard-cut 4 s windows garble their
edges (`Q CQ DE G`) — the sliding-window commit rule is needed, and it puts
text on the pane ~2–3 s after the keying, unlike v2's per-element draft.
**Amplitude trap:** the model is scale-invariant only over ~×0.001–×10 of a
full-scale WAV; real IQ from our probe sits below that (tile log1p 0.00–0.02),
and the first `InstanceNormalization`'s epsilon then silences weak channels —
IZ4ECE (25 dB, v2 reads it for 180 s) decoded NOTHING until each window was
normalised to peak 0.5. Normalise per window; after that every station reads.

*Cost (CPU, Core Ultra 7 265, onnxruntime CPU EP):* one thread ≈ 6 ms per
audio-second at 1–4 s windows, 8–10 ms/s at 12–20 s (attention is O(T²)).
Batched, 4 intra-op threads: 2.2–2.8 ms per channel-second; 20 threads is
WORSE per channel than 4. Budget: an 8 s window re-run every 2 s ≈ 3 % of one
core per active channel → 50 active channels ≈ 1.5 of 20 cores. All 1536 CW
channels continuously is impossible on CPU — an energy pre-gate (a keyed line
above the tile floor) decides which channels get inference. GPU (RTX 5070,
CUDA 13.3 installed, `onnxruntime-cuda` in extra) unmeasured; the batch
dimension is dynamic so all active channels can go as one batch.

*Real IQ, 20 m fixture `iq-20260911-ua6hnu-192k.cf32` (12 s windows, hop 6 s,
125 Hz tile, normalised; v2's own decode log beside it):* IZ4ECE and ON4AEO
read with FEWER mutations than v2 (`CQ CQ DE IZ4ECE IZ4ECE K` vs v2's
`IZ4NE`/`IGEECE`/`IZIAECE`; `CQ CQ DE ON4AEO ON4AEO PSE K` vs
`ON4AEOWNHEOMEEAEOPSE`); EA6NB, TA5ARU, EA5JN equal; **the QSO on 14012.97,
which v2 logged for 180 s as `CQ CQ DE EAAOY EAAOY PSE K` and never tabled,
reads `EA6AOY`** — a station the classical path lost; EA5JQF (issue #4) comes
out as the SAME fused token `CQCQCQDEEA5JQFEA5JQFK` — the fused/torn fist
classes (issues #3/#4) are lexical, a neural front-end does not remove them
(it also tears `OL4 AB B`, `O K1 M G3 W` on 80 m). *80 m contest recording
`iq-20260912-80m-cw-contest-192k.cf32` (300 s, 28 v2 stations, 12 busiest
frequencies):* on every channel DeepCW reads at least as well as v2 on the
running station (OK1FHI, OK1MDK, OK1DOL, OM5AA, OK2PGY, OK7PY, SQ100PKP…);
it reads the QSO PARTNER where v2 gets fragments (OK1XC's callers `OK1MWW
AHOJ 5NN T84 OL1ADZ`, `OL5AJU`, `OM7CF`, `OK2PKD` where v2 logged `MO`, `OM`,
`MAWW`; OK1MDK 156–186 s `CQ OK1MDK OK1MDK` where v2 has `CQOKS`, `SKTESTE`);
and it emits short low-confidence junk in QRM windows where v2 stays silent
(3550.89 kHz 0–48 s, `M IH NW370V4UKI` at 0.3–0.8; TA5ARU 108–150 s on 20 m)
— the per-character CTC posterior separates most of it (clean text 0.96–1.00,
junk mostly < 0.7) but not all, so DeepCW text may reach the extractor ONLY
through the existing validation/repetition gates plus a measured confidence
bar, and the tone splitter's contested rule still applies. Counter-cases,
same tables: 3529.95 kHz 12–36 s v2 read `K1KN AHOJ 5NN 9 … ML2BIL` where
DeepCW gave `H9 N` / `?5H?` at 0.5 confidence. Caveat on the rendering: the
comparison script joined v2's log characters after stripping each entry, so
the v2 strings quoted here LOST their word gaps (the 80 m log holds 1455
word-gap entries) — v2 does segment words; the mutations are the real
difference. No ground truth was available; these are side-by-side readings
of 12 s hindsight windows, not CER, and a streaming build commits only up to
a word gap, so it reads window edges worse than these tables and lands text
2–3 s after the keying.

*Licence, both texts read:* AGPL-3.0 §13 second paragraph and GPL-3.0 §13
each grant permission to "link or combine" a work under the other licence
"into a single combined work, and to convey the resulting work"; the AGPL's
network-interaction clause then applies to the combination. The runtime
(ONNX Runtime) is MIT. Whether a weights file is a copyrightable "work" at
all was NOT verified here (an open question by reputation, unchecked); the
author's stated terms are AGPL-3.0-only and that is what we would honour
(notice + source offer, which the public repo already gives).
Richard's go: 2026-09-12.

*Runtime and packaging.* ONNX Runtime is used through its C API and `dlopen`-ed
at run time (`libonnxruntime.so.1`, `OrtGetApiBase`) with a vendored MIT header —
Debian trixie and Fedora ship it (`libonnxruntime1.21`, `onnxruntime` 1.26), Arch
`extra` has 1.29, Ubuntu 24.04 has NONE — so the binary must run without it and
the engine row says "not available" instead of failing. The model and its
metadata are NOT in git: they resolve from
`~/.local/share/skimmer-for-linux/models/deepcw/` with the AGPL notice beside
them (the digest is recorded — sha256 `ef120799…fe02` — but no pin is enforced in
code; bundling into AppImage/deb/rpm is a separate, open decision).

## As built

**Built 2026-09-12 night (Richard's "ano"), offline-proven.** In the tree:
`vendor/onnxruntime/` (the MIT C API header, v1.21 = `ORT_API_VERSION 21`,
`VENDOR.md`), `src/engine/ort_shim.c` (dlopen of `libonnxruntime.so.1` or
`SKIM_ORT_LIB`, `GetApi(21)` — verified against the 1.30.0 library: same
API table, `CreateEnv` OK), `src/engine/decode_deepcw.c` behind the
`SkimDecodeBackend` vtable, `SkimCwEngine` in `SkimPipelineConfig` +
`SKIM_CW_ENGINE=v1|v2|deepcw` (the pipeline falls back to v2 with a
warning when the runtime or the model is missing; `skimmer-replay` prints
the engine in its header), gate `skimmer-deepcw-test` (the model sections run when `SKIM_ORT_LIB` +
`SKIM_DEEPCW_MODEL` resolve; exit 77 = SKIP otherwise). Design as built: 20-point
Hann DFT per 4 samples on the 250 Hz channel (80 ms window, 16 ms hop —
6.7 % time stretch, inside the model's speed range), bins −5..+5 kept per
frame, 10 s ring, a tick every 1.6 s per channel (staggered), gate = line
≥ 6 dB over the inner-bin floor with a keyed duty in 3–97 %, window
peak-normalised, inference INLINE on the engine thread (replays stay
deterministic), greedy CTC, commit up to the last word gap ≥ 1.5 s before
the window end (≥ 2 s in), committed audio leaves the window, one WORD per
`process()` so the extractor/station table see v2's hit cadence. *(This first
commit rule, the 1.6 s tick and the inline inference were replaced later —
see the workers and the sliding window below.)* Found and
fixed by the fixtures, each a measurement: (a) a fast level EMA decayed to
noise in every word gap → peak hold, 2 s; (b) offset updates on noise frames
dragged +30 Hz to 14 → mark frames only (≥ 4× floor and ≥ ½ peak);
(c) SNR from the line bin's own minima saturated (an 80 ms window never
reaches the noise inside a 48 ms gap) and from the outer bins was inflated
(they sit on the filter roll-off) — now window peak over the inner-bin floor
against a BAND-WIDE floor EMA, −10 dB (bin → 125 Hz), which lands on v2's
scale (20 m: IZ4ECE 28 vs v2 25, EH1SDC 66 vs 64, TA5ARU 15 vs 15);
(d) **weak word gaps tear callsigns** — the model splits `OK2B TK`,
`OK1C Z`, both halves validate and the extractor keeps the valid short
part (OK2B 13 reports at 0.95 on 80 m): a gap with posterior < 0.8
(`SKIM_DEEPCW_SPACE_P`) is dropped ONLY where it would tear a ≤ 2-char piece
off a token (dropping every weak gap fused weak stations' text and lost
TA5ARU on 20 m) — OK1CZ then reads right, OK2B falls to 3 reports;
(e) WPM from the on-run histogram read dah-heavy fists at a third (OK1XC
10) → character rate of the committed span (PARIS: 12 × chars/s) with the
histogram as fallback — still crude (OK1XC 10, OK1JAX 12 vs v2 29, 23).

*A/B numbers, final build (`/var/tmp/skimmer-iq/ab/*-H.out`, v2 tables in
`replay-*.v2.out`, logs `*.v2.decodes.log` / `*.deepcw.decodes.log`).*
20 m (180 s): v2 7 stations; DeepCW 10 = all seven + **EA6AOY** (45
reports, CQ — the station v2 logged as EAAOY and never tabled) + EA6ROY
(3 reports, a mutation twin of it, 0.90) + II6IGTO (11 reports, 0.90,
unverified); EA5JQF stays untabled (issue #4 is lexical, as predicted).
80 m contest (300 s): v2 28; DeepCW 33 — 22 in common, new OK5O (36, CQ),
OK1CZ (12), DL3GAK (18), OL6A (13), OE3MM (2), HB9T (5) and singles OK1KN /
DH3Z / TC1MGW (a mutation of OK1MGW); lost SP2NBV, OL2BHZ, OL5A, OL5BOO,
OK1MGW (all read in the side-by-side but out-reported in the station
table's takeovers — DeepCW emits ~5× fewer hits than v2's per-character
stream); OK2B 3 reports (was 13). **OK1DOL sits on its spur** (3537.00,
a +4.57 kHz image 12.5 dB down in the IQ, same for OK1MDK ↔ 3536.44/3541.01):
the table's same-call rule moves a station to whichever report reads
stronger, and an intermittent spur peaks within 1–3 dB of the real signal
for a few seconds at the end of the recording — v2 wins that race only by
report count (258 vs 35). Filed as a station-table item: a same-call report
> merge distance away needs sustained evidence before it re-positions the
station. Cost: 20 m 135 s wall for 180 s (1.3×), 80 m 190 s for 300 s
(1.6×), one inference per gated channel per tick at 4 intra-op threads on
the engine thread, RSS ~400 MB — so **live needs the async worker** (the
engine thread must not block; jobs snapshot the window, results land in a
per-channel mailbox, the commit rule stays on the engine thread), and the
replay keeps the inline path for determinism.

## Workers and the engine switch

**Built later the same night: the worker and the switch.** Inference runs
on `SKIM_DEEPCW_WORKERS` (2) threads: a tick snapshots the window into a
job, a worker runs the model and leaves `logp` in the channel's mailbox,
`process()` on the engine thread applies the commit rule (text order stays
deterministic, no lock on the text path); one job per channel in flight,
a queue > 256 jobs skips ticks with one warning; a channel freed with a job
in flight is freed by the worker (refcount). `skimmer-replay` sets
`SKIM_DEEPCW_SYNC=1` so replays stay inline and bit-stable.
App: Preferences → Decoding → **CW engine** ("Classical (v2)" / "DeepCW
(neural)"), persisted `[decode] engine` = `v2|deepcw`, a change rebuilds
the pipeline like a mode change; the row's subtitle says whether DeepCW
is available on this machine and, if not, why and where the model must be
(`skim_decode_deepcw_available`, `..._model_path`); About's debug_info
carries "CW engine: <resolved name>"; the app logs `app: pipeline engine
<name>` at every pipeline build. The model sits at
`~/.local/share/skimmer-for-linux/models/deepcw/model.onnx` (sha256
`ef120799…fe02`, `NOTICE` + the AGPL text beside it; not in git). Classical v2 stays the default.

## GPU

**GPU (later the same night, Richard: "zkus napřed přidat GPU… uživatel by
měl mít ty možnosti v nastavení").** `onnxruntime-cuda` 1.29.0-3 installed
from extra with Richard's ok (+ cpuinfo, cudnn-frontend, nccl, onednn;
714 MiB download, 1.05 GiB installed) — it provides `libonnxruntime.so.1`
system-wide, so `SKIM_ORT_LIB` is no longer needed for a launch. Shim:
`skim_ort_session_new(…, device)` appends the CUDA execution provider
(`CreateCUDAProviderOptions` / `UpdateCUDAProviderOptions` device_id 0 /
`SessionOptionsAppendExecutionProvider_CUDA_V2`) and on ANY failure builds
the CPU session instead; `skim_ort_session_device()` reports "CUDA:0" or
"CPU (cuda unavailable: <reason>)" and runtime_info carries it. **Arch
packaging quirk, worked around:** the provider library references cuDNN
symbols but does not list `libcudnn.so.9` as NEEDED (only cudart/cublas 13),
so its load failed with "undefined symbol:
cudnnGetConvolutionBackwardDataAlgorithm_v7" although cuDNN 9.26 exports
it (nm-checked) — the shim `dlopen`s `libcudnn.so.9` with `RTLD_GLOBAL`
before appending the provider; harmless elsewhere. Backend: the session is
refcounted so `skim_decode_deepcw_reset()` (device change) can drop it while
workers finish; `skim_decode_deepcw_set_device("cpu"|"cuda")`,
`SKIM_DEEPCW_DEVICE` for replays; **workers now BATCH**: every queued
window goes in one run, zero-padded at the end to the longest
(`SKIM_DEEPCW_BATCH` 32) — one launch for N channels, what a GPU wants.
App: Preferences → Decoding → **Device** ("CPU" / "GPU (CUDA)"), shown only
while the engine row says DeepCW, persisted `[decode] device = cpu|cuda`;
a change resets the session and rebuilds a DeepCW pipeline; the subtitle
carries what actually runs. **Measured:** 80 m contest replay (inline, batch 1)
103 s wall on CUDA:0 vs 190 s CPU (2.9× vs 1.6× realtime); the station
table is the CPU one plus SP3HLM (2 reports) — GPU float paths are not
bit-identical, a marginal weak window flipped. NPU stays open: no Arch
ONNX Runtime package carries the OpenVINO provider (checked), so it needs
either Intel's onnxruntime-openvino build or a second shim on OpenVINO's
own C API (openvino 2026.3.1 + intel-npu-plugin are in extra, the driver
and `/dev/accel0` are on the machine; an earlier measurement found the NPU
no faster than the iGPU, its value is watts).

## Latency — the sliding window

**Latency — the sliding window (2026-09-13 ~00:15, Richard: "dekódování je
dost pomalé, má několik vteřin rezervu… potřeboval bych se dostat k téměř
realtime překladu"; GPU sat at 8 %).** The lag was the commit rule, not
compute: a 1.6 s tick, a 1.5 s tail guard, a wait for a word gap and the
one-word-per-call trickle added up to 3–5 s. Simply speeding the old rule
up (tick 0.5 s, tail 1.0/0.75 s) LOST stations (34 → 27/28) and tore
calls (OL1BI, K2BVX, TU5NN), because every commit dropped the committed
audio from the window and the Conformer was left with 2–3 s of left
context. New rule: the WHOLE ring (10 s) is re-read every tick, a
character is final once it sits ≥ tail (1.0 s) before the window end, a
frame cursor (+ 4-frame margin for characters; spaces on frame order,
doubles squeezed) keeps anything from being emitted twice; no word-gap
wait, no force commit, the cursor never moves on silence. Tick by device:
0.5 s on CUDA, 1.0 s on the CPU (`SKIM_DEEPCW_TICK` overrides) — 20
audio-seconds per channel-second on the GPU, which is what the idle GPU
was for. 80 m fixture, CUDA, inline: 33 stations in 96 s wall (3.1×);
against the old rule the table is v2-like in depth (OK1FHI 294 reports,
OK1MDK 302, DK1WI 109; 9449 fragments vs 2240), OK1DOL and OK1MDK sit on
their REAL frequencies (3532.45 / 3541.00 — the spur race is decided by
report count now, as for v2), OK1MWW and OK2BVX read whole (were OC1MWW /
OK2B), SP2NBV, SQ100PKP, OM5AA 50, OL0CHC, PA2M appear; costs: OL1B →
OL1BIC (the tear rule glues the 2-char "IC" that follows the call, 128
reports at 0.75), OK1C again (a confident gap before the "Z"), OK5O gone
(takeover on the 3540.01 pileup channel). Expected latency now ≈ tick +
tail ≈ 1.5 s on the GPU. Next lever if he wants the pane to feel live: the
model's uncommitted tail as gray DRAFT text through the phase-B pane ops
(`take_pane_op`), firming up at commit — the extractor path unchanged.

## The gray draft

**Gray draft — BUILT (2026-09-13 afternoon; Richard's live verdict on the
engine first: "zatím to překládá dobře… zpoždění tam sice trochu je, ale jak
bude moc nepříjemné se uvidí až při nějakém závodě", then his "zkus jít do
toho dalšího návrhu" with the return point recorded: main 45435e9).**
`skim_deepcw_commit_ex()` = the commit rule plus a `draft` string: every
spike inside the tail guard (beyond the cursor/margin, spaces squeezed on
from the final text) is the model's CURRENT reading of the not-yet-final
tail; the cursor and last_space move only for final characters, and the
plain `skim_deepcw_commit()` is the same call with draft = NULL (gate: bit-
identical output). The backend composes an over region through the decode.h
pane ops at the END of `process()`, after the one-word extraction: region =
the committed words still waiting in `out` (plain, `final_len`) + the draft
(dim) — so the pipeline's one-word-per-call trickle never leaves a hole
between the appended text and the draft; OPEN when a region appears, SET on
change only, CLOSE("") when it empties, on a gate-closed tick and in
`resync`; no op carries `fresh` (the decode log never sees a draft),
`pane_own` stays FALSE (d.text is appended by the pipeline as before), so
extractor, station table, spot path and log are untouched by construction.
Kill switch `SKIM_DEEPCW_DRAFT=0`. **Pipeline trap found by design review
and fixed:** an ops-only hit (draft, no decode) used to carry a zeroed
decode → eff_off 0 → before the first text pins the frequency lock the draft
routed at the channel CENTRE; with the tone > 25 Hz off centre (the app's
pane-slot merge radius) the region opened in ANOTHER pane slot than the text
and was never closed — stale gray text. `hit_placeholder()` gives such hits
the backend's `tone_offset_hz`; the lock still updates on decoded-text hits
only. `SKIM_PANE_DEBUG` now also logs every over
op (kind, Hz, len, final, routed/history). Known and inherent: the tail
reading is unstable while it is draft ("1E" → "1ME" → "MDE" → "DK" on
OK1MDK) and a noise tail on a gated channel can show a flickering single
gray char — that is what gray means; Richard's live look decides whether
the flicker is acceptable (the switch is the fallback). NOT built: a
draft for v2 (its per-element draft is already live per char).

**Live verdict on the raw draft (Richard, 2026-09-13 ~14:37): "tohle není
nic čitelné".** Measured why on the 80 m fixture (`SKIM_DEEPCW_TAILDUMP`,
every draft spike paired with the final character that later landed on the
same frame ±4): the model's reading of the last 384 ms of the window is
11–44 % right (mostly insertions that never become final), 71 % at
384–512 ms, 85–92 % beyond — so the raw tail flickered garbage at 2 Hz.
**Reliable-prefix rule, shipped:** the draft is the tail reading cut at the
first spike closer than `SKIM_DEEPCW_DRAFT_MIN` (0.384 s) to the window end
or below posterior `SKIM_DEEPCW_DRAFT_P` (0.9) — 98.3 % right, 99 % beyond
512 ms, a third of the raw draft's characters, lead over the final ≈ 0.6 s.
Finals bit-identical with the draft on/off (decode log cmp). His look at the rule: "dobrý, je to lepší,
ale teď to má zase tendenci nedělat mezery mezi slovy".

## Word gaps

**Word gaps (same afternoon).** Not the draft: finals identical either way;
widget mirror == FreqLog in 52/52 checks (`SKIM_PANE_DEBUG` now logs
routed text and both tails). The DeepCW finals themselves lose gaps, three
ways, measured with the dump (unique gaps, re-reads deduped): (1)
`drop_weak_tears` glued every weak gap next to a ≤ 2-char piece — but a
short LEFT piece is a word (DE, CQ, TU, K, R, 73: 1488 gaps dropped per
replay, "DEOK1FHI", "CQOL"); (2) a gap the model places ON or just before
the last committed character's frame was squeezed as a re-read (~400 real
losses); (3) the model emits NO space at all after a long pause or at an
over boundary (a fifth of consecutive committed characters sit > 1.9 s
apart with no gap; the glued "DE" + call cases had 0.64–1.0 s of silence
and no space spike in any read). Fixed (1): glue only a torn call TAIL
(left ≥ 3, right ≤ 2) and never when the right piece is a known short
word/prosign (`SKIM_DEEPCW_GAP_WORDS`, default K R CQ DE TU 73 88 GL GM GA
GE GN KN SK AR BK UR ES DX RR). Fixed (2): a gap within 2×margin before
the cursor passes unless it is a re-read of the last committed gap
(`space_t` memory); the cursor never moves back. Measured on the fixture:
spaces 1914 → 2011 (+5 %), 34 stations vs 33 (+DK3GG), no station lost, no
mutation; glued DE unchanged (that class is (3)). **(3) attempted and REMOVED, twice measured:** a
space from the ENVELOPE (longest unkeyed run on the line bin between two
committed characters, keyed = above half-way between the window peak and
the floor, snapshotted per job) — 5 dits at the estimated WPM: 33 → 31
stations, 13 lost, mutations OL1BI/DJ6UXD/K1J, reports 2807 → 1766 (the
WPM estimate overshoots and a real 3-dit gap passes the bar); a fixed
0.6 s bar: spaces +1775 (far more than the ~400 long pauses), 6 stations
lost, DL3GAKF/DJ6UXDJ/OK1DSA minted — the half-way keyed threshold reads
"silence" all over weak and QSB channels. Dead end as built; a real keyed
detector (hysteresis, env_lo floor, mark-frame anchoring) would be the
prerequisite, not a bar. So the model's omitted gaps after long pauses
remain; the pane shows "DE" + a call glued where the operator paused.

## "I hear CW but nothing decodes" — and the keyed copy at 0 Hz

**"Slyším CW, ale nic nepřekládá" (Richard, 2026-09-13 ~16:20, VFO
14022.14 then 14022.007, radio filter ~20 Hz).** Neither the channel nor the
filter: the CW bank is 125 Hz spacing / ±62.5 Hz per channel, overlapping,
and the skimmer reads the wideband IQ ahead of the receiver filter. A 60 s
probe capture at his centre (`/var/tmp/skimmer-iq/iq-20260913-14022-192k.cf32`,
centre 14022007) replayed through BOTH engines reads the station cleanly —
"OK GA DR BRIAN VY 73 TU … CQ CQ CQ DE G4BPJ G4BPJ K", G4BPJ tabled with CQ
by v2 (1.00) and DeepCW (0.90). The garbled live text ("CT P T BMW … DE
G4BPJ RR") was the QSO partner CT1BMW, weaker and torn, interleaved with
G4BPJ — one frequency, two sides; while G4BPJ listens the pane stands still
although the ear hears the weak side. **Side finding, NOT the skimmer's:**
the IQ stream carries a KEYED copy of a strong nearby station exactly at
the DDS centre (0 Hz), ~20 dB below it, with the station's keying rhythm
(20 ms envelope of the 0 Hz component vs the +131 Hz tone: 2.5× higher
during its marks; the fine FFT shows a 0 Hz line with ±10 Hz keying
sidebands; the plain DC offset is 4 dB UNDER a 12.5 Hz bin's noise, so it
is an envelope product, not an offset). The centre channel then decodes
that copy as the neighbour's text (v2 read G4BPJ's whole sign-off on
channel 0 at offset 0.0 Hz while the station sat 131 Hz up) — and the
centre is where the VFO always is without CTUN. Mechanism unknown
(ADC/DDC IMD2? something in the sdr-for-linux IQ path?); to be filed and
measured in sdr-for-linux. New env `SKIM_FLOCK_DEBUG=1` prints per hit:
channel, slot, in-channel offset, raw Hz, lock Hz, arbitration level,
confidence, text — the tool that separated the two channels here.
