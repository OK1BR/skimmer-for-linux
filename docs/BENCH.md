# The bench — recorded air, replayed, scored against RBN

How a decoder, extractor or feed change is judged before it becomes a default
(#28). A change is replayed over recorded IQ and its RBN feed output is scored
against the spots other skimmers sent to RBN at the same minutes. A replay
result is a candidate, not a verdict: a change becomes a default only after a
live session (in OK1HRA's fork, offline wins twice lost on live air — #28).

## Record

    skimmer-iq-record [-t seconds] ~/.local/share/skimmer-for-linux/fixtures/iq-<date>-<HHMM>z-192k.cf32

- Runs beside the app as a second TCI client. sdr-for-linux (≥ 0.6.0,
  `tci_server.c`) keeps the IQ subscription, the rate and `iq_stop` per
  client: the recorder sends no `iq_samplerate` by default, and its
  `iq_stop` on exit leaves the app's stream running (seen live 2026-11-01:
  the app and a second recording kept their IQ through a third client's stop).
- `<out>.meta` is written at the first IQ block (`start_unix_us` is the
  time RBN spots are aligned to) and completed on exit; `<out>.events`
  holds the DDC centre per frame (retunes, including the client's split of
  a block retuned mid-way), the holes the TCI 2.1 packet counter saw, and
  the TX edges. Retuning and transmitting during a recording are fine.
- 192 kHz cf32 is 1.5 MB/s, 5.5 GB an hour. Keep fixtures under `$HOME`:
  `/var/tmp` is cleaned after 30 days, which is how every fixture before
  2026-10 was lost.

## Replay

    SKIM_REPLAY_OUT=<prefix> skimmer-replay <file.cf32>

- Applies the `.events` sidecar: per-block centre, `skim_pipeline_feed_gap`
  for holes, TX hold from the edges (`SKIM_REPLAY_HOLDS` replaces them).
  A block never straddles an event.
- `<prefix>.rbn.log`: every record the RBN feed would have broadcast — the
  feed's own policy (CQ only, score bar 0.85 or `SKIM_REPLAY_RBN_MIN`, ham
  bands, 600 s re-spot, 100 Hz QSY, 5/s), via `SkimPipelineConfig.rbn_tap`.
  UTC = `start_unix_us` + stream time, holes included.
- Deterministic: two runs over the same input are byte-identical (cw-v2,
  2026-11-01). About 36× real time on cw-v2 at 192 kHz on the development
  machine, so an evening of air replays in minutes.

## Score

    skimmer-rbn-score -r <file.cf32> <prefix>.rbn.log <YYYYMMDD.csv>…
    skimmer-rbn-score --diff <a.rbn.log> <b.rbn.log>

- Referee: RBN's public daily archive,
  `https://data.reversebeacon.net/rbn_history/<YYYYMMDD>.zip` (published
  shortly after 00:00 UTC for the day before). One CSV row per spot:
  `callsign,de_pfx,de_cont,freq,band,dx,dx_pfx,dx_cont,mode,db,date,speed,tx_mode`,
  time to the second, frequency to 0.1 kHz. No login, no telnet session
  under the operator's call. Rows with `tx_mode` CW and `mode` CQ or DX
  count; beacons do not. The own skimmer (`--self`, OK1BR) is left out —
  on 2026-10-31 it had no spot in the archive anyway (the feed is local).
- Unit: call × band × UTC hour. Ours are *confirmed* (any skimmer spotted the
  call on that band that hour), *look-alike* (not confirmed, but a spotted
  call within two edits, or containing ours, sat within ±1 kHz that hour —
  a bust) or *unconfirmed*.
- Recall = our confirmed units / the referee set: the units EU skimmers
  (`--cont`, or `--spotter` for named ones) heard inside the span the
  recording covered at that moment, minus a 5 kHz edge. The referee hears
  stations this antenna cannot, so recall is an A/B figure — only the
  difference between two runs over one recording means something.

## Noise floor

Measured before anything is judged: two replays of one recording that
differ only by a warm-up shift. A difference between two runs smaller than
this is noise, and the two sides of an A/B must start the same way.

(to be filled in from the first recording, 2026-11-01)
