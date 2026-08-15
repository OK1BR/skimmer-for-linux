# TX hold — freeze decode state during own transmission (zadání, 2026-08-15)

> **STATUS: IMPLEMENTED the same evening** (trx/tune parse + tx callback in
> tci_client, block-swallow hold + 0.3 s grace + 30 s cap in the pipeline,
> optional `resync` backend hook — RTTY implements it, `set_tx_hold` public
> for the offline harness). Gate: the RTTY suite grew a hold-vs-control
> pass — WITH the hold the reply decodes complete from its first character
> and NOTHING decodes from the held band; WITHOUT it (the old behaviour)
> 77 garbage decodes leak during the own-TX silence and the reply's head is
> lost to re-acquisition. Full suite 11/11. Live pass with the radio
> (needs sdr-for-linux ≥ cc470af running) pending.

Reported by Richard live, first RTTY QSO attempts with sdr-for-linux's new
RTTY mode: **"when I answer a call, after my over it hangs and doesn't
decode for a while."** Mechanism verified from the radio side the same
evening: while the SDR transmits, its RX is deliberately deafened (T/R
relay + both step attenuators at 31 dB — TX protection), so the whole band
disappears from the IQ stream for the length of the over. Every acquired
RTTY channel then rides its release logic (decode_rtty.c: `lost_s` :200,
release after ~2 s under the bar :384/:391) — observed live as a mass
`rtty release` wave across the band during each over. When the answering
station comes back right after unkey, acquisition must re-converge first →
**the first seconds of the reply are lost.** CW panes suffer the same
physics (their envelope trackers adapt toward the silent band).

## Radio side — DONE (sdr-for-linux cc470af, 2026-08-15)

`trx:0,true/false;` now reports the **real keyed state** (any RF: MOX,
TUNE, CW and RTTY text keying). Before, it reflected only the MOX button —
text keying was invisible to TCI clients, so this skimmer had no way to
know the band blank was self-inflicted. The 500 ms reporter broadcasts
every change; worst-case ~500 ms key-on latency is comfortably inside the
~2 s release bar.

## Changes (proposed)

1. **tci_client.c** (command parser, :94 chain): parse `trx:0,<bool>` into
   the client state + expose it to the pipeline (the `vfo_cb` pattern).
2. **Hold flag in the engines**: while trx is true, and for a short settle
   grace (~300 ms) after it drops:
   - RTTY channels: freeze `lost_s` and all adaptation (AFC/ATC/quality),
     suppress character emission (the deaf band decodes only garbage);
     reset the UART framers at resume (a mid-char phase is meaningless
     across the gap) — the channel itself stays acquired.
   - CW: the same hold for the envelope trackers/keying detectors.
3. **Resume**: channels continue with their pre-TX state — the answering
   station decodes from its first characters instead of after a re-acq.
4. **Backstop**: cap the hold at ~30 s (a stuck trx=true or a long TUNE
   must not freeze the skimmer forever); after the cap, release normally.

## Gate

Extend the offline RTTY gate (rtty_test.c synthesizes IQ): decode ok →
N s of silence with hold asserted → signal resumes → decoding continues
immediately (no re-acquisition); the control case without hold releases
as today. CW twin if CW lands in the same pass.

## Decisions (confirm with Richard / the implementing session)

- **A.** Grace period after trx=false (~300 ms proposed — T/R settle).
- **B.** CW hold in the same pass, or RTTY-first.
- **C.** Hold cap value (~30 s proposed).
