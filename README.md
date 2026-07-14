# Skimmer for Linux

**A native GTK4 multi-channel CW/RTTY/PSK skimmer for Linux — decode every
signal in a band segment at once, and spot it.** The free-software counterpart
of CW Skimmer / SDC, built as a **TCI client** for
[`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux).

`skimmer-for-linux` connects to the ExpertSDR-compatible **TCI server** in
`sdr-for-linux`, pulls a wideband IQ stream straight from the radio, splits it
into hundreds of narrow channels, and decodes them in parallel. Valid callsigns
are pushed back as **spots** onto the `sdr-for-linux` panadapter (click to tune)
and, as a goal, out to the **Reverse Beacon Network**.

> **Status: M3 — the CW decoder decodes.** The chain works end-to-end on
> synthetic signals: TCI client (M1, live-verified) → polyphase channelizer
> (M2: −109 dBc isolation, ~1 % of a core for a 192 kHz segment) → CW backend
> (M3: exact copy at 15–35 WPM, still copying at 12 dB SNR, through 10 dB QSB
> and a ±15 % ragged fist, silent on noise-only channels). Everything gated
> offline (`meson test`). Next: M4 — RBN-grade callsign extraction +
> validation. See [`docs/SCOPE.md`](docs/SCOPE.md) for the full plan.

## How it works

```
sdr-for-linux (TCI server) ──IQ──► skimmer-for-linux ──► channelizer ──► CW/RTTY/PSK decode
        ▲                                                                        │
        └──────────────────────── SPOT (callsign @ freq) ◄───────────────────────┘
                                   also ──► Reverse Beacon Network (telnet)
```

The hard half — clean, correctly-oriented wideband IQ out of the radio — already
exists and is live-verified in `sdr-for-linux` (its TCI IQ stream was tested
against SDC and CW Skimmer). This project is the decoder and the spot pipeline.

## Planned modes

1. **CW** (first) — adaptive-WPM Morse, many channels in parallel.
2. **RTTY** — FSK 45.45 bd, Baudot/ITA2.
3. **PSK** — BPSK31 and BPSK63 (Costas loop, varicode).

The channelizer is mode-agnostic and phase-preserving from day one, so each mode
is a pluggable decode backend on shared infrastructure.

## Requirements

- A running [`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux) with its
  **TCI server enabled** (Prefs → Radio → TCI), reachable over the network.
- Linux, GTK4 + libadwaita, GLib, libwebsockets, single-precision FFTW.
- Build: `meson` + `ninja`.

## Build

```sh
meson setup build
meson compile -C build
./build/skimmer-for-linux
```

## Relationship to sdr-for-linux

Same author (OK1BR), same house style: native GTK4/C, GPLv3, in-tree vendoring
of proven DSP (WDSP). This is a separate repo because the skimmer is a distinct
tool that talks to the radio only over TCI — it could in principle run against
any ExpertSDR-compatible TCI server.

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
