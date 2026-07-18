# cw-reader.bin — the neural CW reader weights (CWRD v3)

The in-repo blob the app loads when the **Preferences → CW reader** switch
is on (default OFF — a plain launch never arms the reader; the decode pane
belongs to the operator's ear). `SKIM_CW_READER=<path>` still overrides for
offline analyses. DISPLAY ONLY either way: reader text reaches the pane and
the decode log as aux lines, never the extractor/spot path.

Format: `ml/export_c.py` blob v3 (per-layer bounded lookahead, streaming) —
loaded by `src/engine/cw_reader.c`. The `cw-reader` gate runs its torch
parity + stream==batch checks against THIS file on every `meson test`.

## Provenance — run4, 2026-07-18

12-layer dilated TCN + CTC (~310k params) over symbolic run durations,
causal windowed-median features, lookahead 22 runs (~2–4 s commit lag).
Trained by `ml/train_ctc.py`: run3 (30k steps, synthetic fists,
`ml/fist_synth.py`) warm-started + 12k fine-tune steps mixing 12 % real
consensus-labeled pairs (`ml/harvest_real.py`; 20 m contest 2026-07-18,
blocks A–D, 274 pairs).

Measured (offline): synthetic CER clean 0.029 / ugly 0.039; held-out
contest blocks F+G 0.046; EA3BP ragchew calls exact (CER 0.094 vs
reconstructed truth); EA1EYL torn-CQ over 21 exact callsign copies.
Training environment: /var/tmp/skimmer-cw-ml (not in the repo).
