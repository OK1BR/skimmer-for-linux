# cw-reader.bin — the neural CW reader weights (CWRD v3, OFFLINE ONLY)

**The app does not load this — decoding is classical (decode_cw_v2 +
clock re-lock + tone splitter).** The live verdict (2026-07-19, after two
trained generations, margin calibration, dead-air gating and the
same-shape guard): the reader's confident rewrites degraded more text v2
had decoded correctly than they fixed, so the neural path was removed
from the app entirely — no Preferences switch, no arming. The pane shows
v2's decode, full stop.

What remains is research material: `SKIM_CW_READER=<path>` arms the
reader in OFFLINE replay analyses only (`skimmer-replay`), where it never
touched the extractor/spot path to begin with. Whether this blob and the
neural sources stay in the tree at all is a pending explicit decision —
git history keeps them either way.

Format: `ml/export_c.py` blob v3 (per-layer bounded lookahead, streaming) —
loaded by `src/engine/cw_reader.c`. The `cw-reader` gate runs its torch
parity + stream==batch checks against THIS file on every `meson test`
(keeping the dormant code honest while it exists).

## Provenance — run5, 2026-07-18 (late)

12-layer dilated TCN + CTC (~310k params) over symbolic run durations,
causal windowed-median features, lookahead 22 runs (~2–4 s commit lag).
Trained by `ml/train_ctc.py`: run3 (30k steps, synthetic fists,
`ml/fist_synth.py`) warm-started + 20k fine-tune steps mixing 25 %
**v2-labeled** real pairs (`ml/harvest_real.py` after the harvest-bias
fix: labels come from decode_cw_v2's own solid decode via the
`SKIM_CW_DUMP_RUNS` `.chars` sidecar, the reader model is OUT of the
label path; 20 m contest 2026-07-18 blocks A–G + the phase-B live
capture, 5143 pairs, "teach" chunks — the ones run4 misread — 3×
oversampled). Block H held out.

Measured (offline): synthetic CER clean 0.046 / ugly 0.067; held-out
block H (v2 labels) 0.25 vs run4's 0.428; EA3BP ragchew CER 0.008 vs
reconstructed truth (run4 0.016); binec EA1EYL 3 exact accepted call
copies (= run4), mutation "EA1EYLL" now REJECTED at min margin 0.51.
phaseB live capture, per-word gate 6/3: match 219 / conflict 93 vs
run4's 228/214 — and run5's accepted "orphans" are real weak-signal
words (TEST/CQ/calls), not E/T babble. run4's noise-transcription
disease ("EEEAAOEEEIEX" for "PA4O TU 5NN 2") is trained away — run5
blanks what v2's squelch blanks.
Training environment: /var/tmp/skimmer-cw-ml (not in the repo).
