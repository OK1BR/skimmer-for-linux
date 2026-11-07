# The dB of a spot: SNR in 500 Hz

What the number after a callsign means — on the panadapter label, in the
tooltip, in the decode pane header and on the telnet/RBN line — how each CW
backend arrives at it, and what was measured (gh#27, 2026-11-07).

## The scale

**SNR in 500 Hz: the key-down carrier power over the noise density times
500 Hz.** This is CW Skimmer's scale, so a station's dB from this skimmer
stands beside the other skimmers' on RBN, which lists them side by side for
the same station.

The reference is Alex VE3NEA's description of CW Skimmer and SkimServ,
quoted by Pete N4ZR on the RBN blog on 2014-03-05
(<https://reversebeacon.blogspot.com/2014/03/understanding-signal-to-noise-ratio-snr.html>):

> The signal is extracted from the I/Q stream using a filter with a bandwidth
> of 50 Hz. First the key-on/key-off decision is made for every sample in the
> signal, as part of CW detection. The samples corresponding to the key-off
> state and on/off transitions are discarded, the key-on samples are analyzed.
> The signal strength is computed assuming the Rayleigh fading model […]. The
> noise density is estimated from the flat part of the power spectrum. The
> noise power in 500 Hz is then computed by multiplying the power density by
> the bandwidth. Finally, the ratio of the signal power to the 500-Hz noise
> power is computed.

and, from the same exchange: "the signal strength is estimated in the 50-Hz
filter passband, while the noise is estimated globally in the whole receiver
bandwidth of 48/96/192 kHz", over about 45 s of history. Whether the key-on
"signal strength" is a peak or a mean is not said; this tree takes the
key-down carrier power, which for a steady carrier is the same thing.

Two consequences shape the implementation:

- **The noise is a property of the band, not of the station's channel.** A
  station's own channel cannot measure the noise under it: between its
  elements the channel filter is still ringing (the CW prototype is 64 ms
  long against a 48 ms dit at 25 WPM), and its keying sidebands lift the
  bins beside the line. Measuring there compresses the scale at the top —
  OK1HRA's estimate read 22 dB for a 30 dB station, ours 25.
- **The reference bandwidth is the flat part of the receiver's passband.** A
  tone away from the channel centre reaches the decoder through the channel
  filter's skirt (−6 dB at the channel edge), the noise density does not;
  the difference has to be added back.

RTTY and the classical v1 CW backend keep their own peak-over-floor ratio
(`decode.h` `snr_in_500hz` = FALSE); nothing refers them to 500 Hz.

## How the number is made

**The band's noise** (`pipeline.c` `band_noise_update`): once a second of
stream time, every channel's wide-lane decoder reports a noise statistic for
the last second in its own units (`decode.h` `noise()`), the pipeline takes
the **median over the bank** and hands it back to every lane
(`set_band_noise()`) together with the channels' equivalent noise bandwidth
(`channelizer.h` `enbw_hz`, from the taps: 105.9 Hz for the CW bank's
125 Hz spacing, 0.85 of the spacing; white noise through the bank measured
104.4 Hz). The channels with a station read high and fall out of the median.
A mean would not do: it rises with every strong station in a contest, and the
process-global EMA DeepCW used until gh#27 did exactly that.

**v2** (`decode_cw_v2.c`, after OK1HRA's estimate in his fork, commit
5b21199): at every committed mark, carrier power over noise power from the
discriminator's two means — µ_mark rides the Rician marks (E[m²] = A² + 2σ²),
the noise is a Rayleigh mean (1.2533·σ, so 2σ² = 4/π·mean²). The mean is the
band's (scaled from the channels' noise bandwidth to the lane's — a narrow
slot of the tone splitter reports its filter's, `tone_split.h`
`slot_enbw`); until the pipeline has supplied it, the channel's own µ_space,
which is OK1HRA's original. The reported value is the **median of the last
24 marks' estimates** (about 2 s at 25 WPM): the first marks of an over come
out of the replayed pre-roll with the trackers still settling, and a median
forgets them as soon as it outnumbers them. The band ratio is referred to
500 Hz by 10·log10(ENBW/500). The reader's arming bar keeps the old
peak/floor ratio (`snr_latch`).

**DeepCW** (`decode_deepcw.c` `window_stats`): the key-down power of the
line — the frames within 3 dB of the window's peak (a dit at 25 WPM is
shorter than the 80 ms DFT window, so only the solid frames inside dahs and
long dits show the carrier whole) — summed over the line bin and its two
neighbours: for a Hann window that sum is 1.5× the tone's bin-centre power
wherever the tone sits between bins (1.47–1.50), so no scalloping term is
needed. The noise power per bin is the band's density times a Hann bin's
1.5 bins (18.75 Hz) of noise bandwidth, subtracted bin for bin; the same
18.75 Hz refers the ratio to 500 Hz. The channel's noise statistic is the
total power in its 11 bins (Hann bins at their own spacing cover the band
1.5× over, so a quiet channel's total is 1.5·N0·ENBW). Until gh#27 the column
was the window peak over the channel's inner-bin floor minus 10 dB — about
8 dB high, 2 dB of that because the inner bins (±25…37.5 Hz) already sit on
the channel filter's skirt and read the noise low.

**The skirt** (`pipeline.c` `dispatch_hits`): for a backend on the 500 Hz
scale the pipeline adds back the prototype's power response at the tone's
in-channel offset (`channelizer.h` `response_db`, tabulated from the taps),
capped a little past the channel edge so a runaway offset estimate cannot
mint a +20 dB station. Without it a 12 dB tone 55 Hz off the centre read 8.4
on both backends.

**The station table** (`station.c`) used to keep the strongest report's dB
— the rule that positions a station on its peak against ghost reports. Over
a minute of reports that is the upper edge of the estimate's scatter (+1 dB
over the raw mean on synthetic stations) and, on real air, the peak of the
station's QSB (+2 dB against the EU skimmers on the 2026-11-01 recording).
The record's dB is now an EMA of the reports' POWER (`SKIM_STATION_SNR_GAIN`
0.2 per report, a few seconds of keying) — CW Skimmer's "signal strength
assuming the Rayleigh fading model" is a mean power; a mean of dB values
would sit 2.5 dB under it on a fully fading signal. The positioning rule
keeps comparing against the peak report (`snr_peak`), so the record's
frequency — and with it every feed decision — moves exactly as before (the
replay of the 2026-11-01 tap gives the same 144 records, only the dB column
differs; letting the EMA drive the positioning gave 156, with OZ1HDF and
SO5CW re-spotted six times each as their records drifted 700 Hz).

## Measured: synthetic stations (`skimmer-snr-test`, `meson test snr-scale`)

Stations of known SNR500 — complex AWGN of density 2σ²/rate at the 48 kHz
input, a carrier of power A² — through the whole offline pipeline, 25 WPM,
"CQ TEST DE call call K" six times, one station per channel. The table's
value against the truth, in dB (the gate holds every row to ±2 dB, the 3 dB
row to ±3):

| true SNR500 | 3 | 6 | 10 | 15 | 20 | 25 | 30 |
|---|---|---|---|---|---|---|---|
| v2, before gh#27 | 13.4 | 15.7 | 18.6 | 21.8 | 24.6 | 27.4 | 29.5 |
| DeepCW, before | 13.3 | 15.4 | 19.0 | 23.7 | 28.3 | 33.0 | 37.9 |
| v2, OK1HRA's estimate as ported (the channel's own noise) | 5.9 | 7.3 | 10.2 | 14.3 | 18.5 | 22.4 | 24.9 |
| v2, band noise + skirt, table = strongest report | 4.9 | 7.1 | 10.4 | 15.0 | 19.5 | 25.0 | 29.3 |
| **v2, the same, table = EMA** | **4.3** | **6.7** | **10.2** | **14.7** | **19.2** | **24.3** | **29.2** |
| DeepCW, band noise + skirt, table = strongest report | 3.7 | 6.2 | 10.2 | 15.0 | 19.9 | 24.9 | 29.9 |
| **DeepCW, the same, table = EMA** | **3.4** | **6.1** | **10.1** | **14.9** | **19.8** | **24.8** | **29.8** |

A 12 dB station across the channel, table value (EMA):

| offset from the channel centre | 0 | +15 | +30 | +45 | +55 Hz |
|---|---|---|---|---|---|
| v2, before the skirt term | 12.1 | 11.9 | 11.3 | 10.7 | 8.4 |
| **v2** | **11.7** | **11.7** | **11.7** | **11.5** | **10.7** |
| DeepCW, before the skirt term | 12.0 | 12.1 | 11.7 | 10.4 | 8.3 |
| **DeepCW** | **11.8** | **11.9** | **11.8** | **11.6** | **11.2** |

The old columns were not a fixed offset from the truth: v2 read a 27 dB span
as 16 dB, DeepCW ran a constant 8 dB high. What is left: v2 reads the 3 dB
row 1.3 dB high (the decaying-peak µ_mark rides the Rician scatter of a weak
mark) and the rest within 0.8 dB; DeepCW is within 0.4 dB everywhere.

## Measured: real air against RBN

The 2026-11-01 80 m recording (`docs/BENCH.md`; 18:03–23:30 UTC) replayed
and its RBN lines matched with the spots EU skimmers gave the same station
within ±10 min and ±1 kHz (the archive's `db`, one value per skimmer, the
median over skimmers). Different QTHs, antennas and noise floors put the
skimmers themselves −15…+16 dB apart from one another, so the absolute
offset says little; what the comparison can show is the shape of our
column against theirs, before and after, on the same 130 matched lines (v2;
the 144 feed records are identical before and after — only the dB changed):

| EU skimmers' median dB | < 10 | 10–15 | 15–20 | 20–25 | 25–30 |
|---|---|---|---|---|---|
| ours − theirs, v2 before gh#27 | +9.8 | +6.8 | +5.0 | +3.0 | −2.0 |
| ours − theirs, v2 after | +2.2 | −3.0 | −3.0 | −5.0 | −10.0 |

Before, the column floored near 10 dB whatever the station — OK1HRA's
finding on his own antenna against CW Skimmer Server ("+8 dB at our floor
to −3 dB above 40") reproduced on ours. After, the floor is gone: the
column spans the stations' range (its spread over the matched stations is
0.79 of a single skimmer's, 0.56 before) and sits 4 dB under the EU median
overall (median of ours − theirs; −2 with the old strongest-report table
rule). The top rows (n = 11 at 25–30) are DJ5AZ and OZ4UN mostly — strong
at the DL/PA-heavy median QTH, 8 dB weaker here; the row by OUR dB reads
the other way (+6 at 25–30, OZ1HDF and DL9ABM), which is what two QTHs do
to a binned comparison. The band's noise reference behind the column is
steady on this recording: the bank's median moves ±0.4 dB over the hour,
quartiles at −1.0/+1.5 dB around it (`SKIM_NOISE_DEBUG=1`).

DeepCW on CPU replays this recording at 0.7× real time, so only its first
18 minutes were run: 23 lines, 21 matched, ours − theirs median −2.0 dB,
and station for station within about 2 dB of what v2 read on the same
minutes (SN40KZE 31/27 against v2's 28/26, DL9ABM 20 against 18, HA8WY 16
against 17, IZ2FME 15/20 against 13/18) — the two backends are on one
scale on real air as on the bench.

What this does not settle: whether the remaining −2…−4 dB against the EU
median is our QTH or a bias. A comparison on one antenna against CW Skimmer
Server, as OK1HRA ran, would.
