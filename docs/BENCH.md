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
  UTC = `start_unix_us` + stream time, holes included. The gh#24 gate takes
  its knobs from the environment too: `SKIM_REPLAY_RBN_READS` (readings a
  call needs, default 2; 1 = any), `SKIM_REPLAY_RBN_SETTLE` (the hold before
  the first line in s, default 8; -1 = none) and
  `SKIM_REPLAY_RBN_SETTLE_NODICT` (the hold of a call MASTER.SCP does not
  know). `READS=1 SETTLE=-1` is the feed as it was before gh#24, so the two
  policies replay over one tap side by side. `SKIM_RBN_DEBUG=1` traces every
  hold, release and drop with the readings and the dictionary verdict.
- Deterministic: two runs over the same input are byte-identical (cw-v2,
  2026-11-01). About 36× real time on cw-v2 at 192 kHz on the development
  machine, so an evening of air replays in minutes.
- `<prefix>.tap`: the decode tap — everything the stages behind the
  decoders consumed, for a replay in seconds (see "Decode tap" below).

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

Measured before anything is judged: replays of one recording that differ
only by where they start (`SKIM_REPLAY_FROM`), a warm-up shift. A
difference between two runs smaller than this is noise, and the two sides
of an A/B must start at the same frame.

First measurement, 2026-11-01: the first hour of `iq-20261101-1803z-192k`
(80 m, Sunday evening 18:03–19:03 UTC, cw-v2, no retune, no TX), replayed
from 0, 5, 10, 20 and 30 s. Each run put 23–26 calls (24–27 units, 48–57
records) on the feed. Over the ten pairs 1 to 7 units differ, 3.7 to
24.1 % of the union. 21 of the 29 calls seen are in all five runs; the
other 8 are where the noise lives:

- seven are readings of four stations whose call the decoder is unsure
  of: DM8EO or DK8EO at 3566.00; SA8AR beside HA8AR (in every run) at
  3535.02; OK1FLK and OK1FLKO together at 3551.91, or neither; OK2BVX,
  read whole in one run, otherwise only as K2B and the fragments OK2B and
  K2BVX, which are in every run;
- one, DH2IF, is a single spot that is there or not.

The runs fall into two families, 0/5/20 s (DM8EO, OK1FLK + OK1FLKO) and
10/30 s (DK8EO, SA8AR): 3.7–7.7 % apart inside a family, 17–24 % across.
The noise looks like a fork between a few trajectories, not a drift that
grows with the shift.

For an A/B on a recording this size: both sides start at the same frame;
a difference of up to five units a side, in calls like these, is not a
signal; a change in the stable set is. Not every bust is noise — OK2B and
K2BVX are in every run, so a fix for them (#29) can be measured.

The whole recording (18:03–23:30 UTC, 5 h 27 min) replayed from 0, 10 and
20 s and scored against the 2026-11-01 archive, referee EU:

    shift   ours   confirmed   look-alike   unconfirmed   recall
     0 s     112       71          22            19        17.3 %
    10 s     105       67          20            18        16.3 %
    20 s     112       72          22            18        17.5 %

The same two families again: 0 and 20 s differ in 2 units of 113, 10 s
sits 14–16 % away from both. In the first hour the five shifts gave 17 or
18 confirmed units (recall 16.7–17.6 %) where the raw units moved by up to
seven. So the score moves less than the feed: over an evening on this band
a change has to move confirmed units by more than about five (recall by
more than about 1.2 points) or look-alikes by more than two before it says
anything. A contest recording will have its own floor.

## Decode tap

    SKIM_REPLAY_OUT=<prefix> skimmer-replay <file.cf32>    # writes <prefix>.tap too
    skimmer-replay <prefix>.tap                            # → <prefix>.tap.rbn.log

- The cut sits in the pipeline's dispatch AFTER ghost arbitration and the
  frequency lock and BEFORE the extractor is fed (`SkimPipelineConfig.
  decode_tap`, `decode_tap.h`). One line per event: every decode with its
  channel and lane, the locked frequency, confidence, speed, SNR, the
  contested flag and the text; the bank geometry; every extractor reset (a
  reborn slot, a centre change); every station-table prune tick — each with
  the pipeline's stream frames and clock. The header names the source, the
  engine, the mode, the rate, the origin frame and the MASTER.SCP release.
- Given a `.tap`, `skimmer-replay` runs only the stages behind the cut —
  extractor, station table, spot/RBN policy (`skim_pipeline_feed_tap`) —
  on the recorded clock, through the same code the IQ path runs, and
  writes the same `.rbn.log`. A run under a different MASTER.SCP release
  says so: the extractor's dictionary bonus would differ.
- Exact: the tap of the whole 2026-11-01 recording replayed to the same
  215 RBN records byte for byte and the same station table; the first hour
  alone, 57 records and 10 stations, likewise. The rbn-feed gate holds the
  pipeline to that, and to the two events a replay silently goes wrong
  without — the centre flush and the prune ticks (with the ticks dropped
  the gate's replay diverges: a station pruned live clears its re-spot
  memo and the comeback re-spots; unpruned, the 600 s window swallows it).
- Fast, and free of the decoder's noise: the evening's 271 102 events
  (29.6 MB) replay in 0.2 s against 544 s for the IQ. Both sides of an A/B
  of an extractor, station-table or feed change read the very same
  decodes, so the noise floor above does not apply — a difference is the
  change. What sits before the cut — decoders, ghost arbitration, the
  frequency lock, the tone splitter — still needs the IQ replay, and a tap
  is valid only for the decoder that produced it: re-take it after a
  decoder change (nine minutes for the evening).
