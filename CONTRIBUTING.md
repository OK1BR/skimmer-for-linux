# Contributing

Thanks for your interest — but please read this before opening anything.

## Pull requests are not accepted

This project does **not** accept pull requests. Every decoder change is
A/B-tested against yesterday's build on the same corpus of recorded off-air
IQ before it ships, on top of the gates that run in `meson test`. A patch
that looks correct in isolation can quietly cost precision on real signals,
and that only shows up in the replay harness. Unsolicited PRs will be closed
without review, regardless of quality.

## What IS welcome: issues

- **Bad decodes and false spots** — the single most useful report. Tell me
  the callsign the skimmer printed, what was actually sent, and the mode,
  speed and signal level if you know them.
- **Recorded off-air IQ** — a segment that decodes badly is worth more than
  any patch: it goes into the replay corpus and every future decoder change
  is measured against it.
- **Bug reports** — with your distro, the `skimmer-for-linux` version, the
  TCI server you connect to, and steps to reproduce. Logs from a terminal
  run help a lot.
- **Feature requests** — especially from real operating experience
  (contesting, DXing, digimodes).

## Why so strict?

The decoder is measured, not argued about: 11 offline gates plus a replay
harness that pushes recorded band segments through the real pipeline many
times faster than realtime, so that a change either improves the numbers on
the corpus or does not ship. That discipline is incompatible
with drive-by code contributions — but it is why the callsign pipeline holds
RBN-grade precision. Reports, recordings and on-air testing are where outside
help genuinely moves this project forward.

73 de OK1BR
