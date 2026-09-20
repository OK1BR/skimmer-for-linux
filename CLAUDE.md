# Skimmer for Linux — project context

Instructions and context for Claude Code working in this repo. (Richard's global
`~/.claude/CLAUDE.md` rules also apply — consent before major/irreversible
changes, work in Czech with Richard, etc.)

## What this is

A native **GTK4 multi-channel skimmer** for Linux — CW first, then RTTY and PSK
(BPSK31/63). It is a **TCI client**: it pulls a wideband IQ stream from the
**TCI server in [`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux)**,
channelizes it, decodes every signal in the segment in parallel, and feeds spots
back to the radio panadapter and to the RBN. What is still to do lives in
GitHub Issues.

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

## Current state

What the tree does today. Anything still to DO is a GitHub Issue, not a line
here; what was done and why is in git.

- **Milestones M0–M8 are in:** TCI ingest, channelizer, CW (v1 classical, v2
  semi-Markov Viterbi), callsign extraction/validation, the spot pipeline, the
  local telnet feed, RTTY (45.45 Bd), and the waterfall with its callsign
  column and click-to-tune.
- **CW backend: v2 is the default.** `SKIM_CW_V1=1` selects the classical v1.
  A third backend, DeepCW (neural, ONNX Runtime via `dlopen`, model outside
  git), is chosen in Preferences → Decoding; without the runtime or the model
  the pipeline falls back to v2.
- **`meson test` = 14 gates.** It does NOT relink the app — build
  `ninja skimmer-for-linux` explicitly or an old binary keeps running.
- Richard's live instance runs from `builddir`; recorded IQ fixtures live in
  `/var/tmp/skimmer-iq/` (a fresh one takes minutes:
  `skimmer-tci-probe <host> <port> <rate> <secs> <dump.cf32>`).
- Opt-in and without a live verdict: the tone splitter (`SKIM_TONE_SPLIT=1`)
  and the narrow focus slot (`SKIM_TONE_FOCUS=1`, implies the splitter).

## Releasing

Tag FIRST, then take the tarball's sha256 from the published tag — the
checksum cannot exist before it. `gh release create --verify-tag
--notes-file` runs right after the tag push, so CI attaches its artifacts to
the curated release instead of an empty one. Then `packaging/PKGBUILD`
(`pkgver`, `_pkgtag`, sha256) → `makepkg --printsrcinfo` → commit and push to
`ssh://aur@aur.archlinux.org/skimmer-for-linux.git` (key `~/.ssh/aur_ed25519`);
this repo's PKGBUILD is the single source, the AUR clone is a copy. Release
titles carry no epithet: "Skimmer for Linux 0.4.1", tag message
"release: v0.4.1".

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
```
