# Skimmer for Linux — backlog

The single work queue for this app: shortcomings, ideas for new features and
bugs reported from real operation. `docs/SCOPE.md` says what the app **is** and
why it is built that way; this file says what is **queued, in progress or just
done**. When the two disagree, that is itself a backlog item.

## How things get in here

- **Found during a contest or live operation** — first written up in
  `docs/CONTEST-NOTES-<date>.md` (raw observation + analysis, no code touched
  while operating), then triaged into an item here.
- **Reported by someone else** — GitHub issue stays the conversation with the
  reporter; the item here mirrors it and carries the `gh#N` link, so one list
  still shows all the work.
- **Own idea / design gap** — straight in, marked `idea`.

## Item format

```
### SKM-N — one-line title
- **Type:** bug | idea | debt · **Severity:** high | medium | low · **Status:** open | doing | done | deferred
- **Source:** who/where/when
- **Detail:** pointer to the full write-up
Short statement of the problem and where in the code it lives.
```

Severity is about the damage, not the effort: `high` = wrong data or something
that leaves the machine wrong; `medium` = gets in the operator's way;
`low` = cosmetic or log noise.

---

## Open — bugs

### SKM-1 — Periodic timers keep firing after the window is destroyed
- **Type:** bug · **Severity:** medium (latent) · **Status:** done — reproduced, fixed, gate-green (2026-09-05)
- **Source:** stderr of the YO DX HF run, 2026-08-22 (not an operator report)
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §N1

Closing the app produced two criticals in the same millisecond —
`gtk_label_set_text: assertion 'GTK_IS_LABEL (self)' failed` and
`adw_window_title_set_subtitle: assertion 'ADW_IS_WINDOW_TITLE (self)' failed`
— 17 minutes after the last operational message, so this is teardown, not
operation. `main.c:1718–1740` registers four periodic sources (`status_tick`
1 s, `age_tick` 2 s, `scan_tick` 3 s, debug `lag_tick` 250 ms) and none is
removed when the window goes away; their callbacks reach into widgets held in
`app`. Today GTK catches it with an assert, but the app is touching destroyed
objects — a different destruction order turns that into a crash.

`log-for-linux` already solves exactly this, twice over: a strong reference on
the window for anything that may outlive it (`win.c:484`) and a teardown guard
at the top of the callback (`win.c:433`). Same pattern applies here — keep the
`g_timeout_add*` ids and `g_clear_handle_id()` them in teardown, plus a
`GTK_IS_LABEL()`-style guard returning `G_SOURCE_REMOVE`.

**Reproduction is cheap and still pending:** the 2026-08-23 instance was still
running when these notes were written, with stderr going to
`/var/tmp/contest-2026-08-23-logy/skimmer.log` — closing it says whether the
criticals appear again before any code is touched.

**Resolution (2026-09-05, `docs/CONTEST-NOTES-2026-08-22.md` "Rozbor 5. 9."):**
the day-2 log, instance closed, has zero criticals — the bug is a coincidence,
not a constant. Mechanism read in the GTK 4.22 / GLib 2.88 sources and then
reproduced deterministically under gdb (headless Broadway, isolated config
whose TCI host is `skimmer-test.invalid` — the live `sdr-for-linux` binds
`0.0.0.0:40001` and a test client would have streamed IQ and spotted onto the
operator's panadapter): `gtk_window_close` destroys the widget tree
synchronously inside the close-request dispatch, and `g_application_run`
finishes the SAME iteration's dispatch list before it notices the application
released its last window — so every tick that was due together with the close
event ran on finalized widgets. Stop the process 4 s inside `status_tick` (all
seconds timers become due), close the window from the next `age_tick`, and
`scan_tick` in the same list prints the exact day-1
`adw_window_title_set_subtitle` critical; after the close
`g_type_check_instance_is_a()` reads 0 for both `app->status` and
`app->title`, so the `GTK_IS_LABEL (app->status)` guard proposed above would
have read freed memory — UB, not a guard. (The first attempts reproduced
nothing because the gdb script itself leaked a window reference through
`g_list_model_get_item()` and the widgets survived — a trap worth recording.)

Fix in `src/app/main.c`: the four source ids are kept and
`g_clear_handle_id()`-ed in `app_teardown()`, hooked on the window's
`close-request` (widgets still intact) and on the application's `shutdown`
as the idempotent backstop; `app->closing` is the ONE sentinel checked by the
ticks, the event drain, the port probe and the pipeline-start completion; the
running pipeline is stopped there (engine thread joined) and the telnet feed
freed after it, so nothing is minted past the close. Verified: the same gdb
reproduction → zero criticals, one `app: window closed — engine stopped,
timers cleared` line (both hooks fired, one teardown), close→exit 4 ms in the
disconnected state; 11 gates green. **Connected close LIVE-VERIFIED the same
afternoon:** Richard launched `builddir/skimmer-for-linux` against his running
sdr-for-linux (TCI session on 127.0.0.1:40001, CW mode, telnet feed on) and
closed the window — exit code 0, stderr holds exactly one line
(`13:04:20 app: window closed — engine stopped, timers cleared`), zero
criticals or warnings, TCI socket gone. By reading, the same holds in
general: `skim_pipeline_stop` joins the engine thread before its own state
callback, so no engine-thread event can race the teardown.

### SKM-2 — GtkImage baseline warnings flood stderr, non-deterministically
- **Type:** bug · **Severity:** low · **Status:** done — diagnosed, upstream GTK/Pango, no code change (2026-09-05)
- **Source:** stderr of the YO DX HF runs, 2026-08-22 / 23
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §N2 and its day-2 update

18 warnings of the form *"GtkImage … reported baselines of minimum -1 and
natural -1, but sizes of minimum N and natural N"*, each from a different
object address. Nothing visibly broken. The same warning appears in
`sdr-for-linux` (65×) — see its `SDR-3`.

The day-2 run is the interesting part: **zero** warnings from the same binary,
the same GTK (4.22.4, no package transaction between the two days) and a
verified-identical environment. So the warning is not deterministic across runs
and cannot be chased by widget address.

**Do not fix blind.** First step is a backtrace: run under `gdb` with
`G_DEBUG=fatal-warnings` and catch the first occurrence — that says whether the
cause is ours at all or a GTK 4.22 regression. Only then decide where a fix
belongs.

**Resolution (2026-09-05, `docs/CONTEST-NOTES-2026-08-22.md` "Rozbor 5. 9."):**
the numbers above were off — grep over the day-1 log gives **16** warnings
(8 `GtkImage` addresses × 2), baseline `-2147483648` (INT_MIN, not −1), size
always 16/16, in TWO bursts: 14:08:46 (window open — the log's first line) and
14:26:06 (four NEW images; a guess, not pinned: the primary menu's two
`GtkModelButton`s carry two `GtkImage`s each). Same signature as
`sdr-for-linux`'s `SDR-3` (64 = 32 × 2 in one 70 ms burst), whose write-up
reads the chain in GTK 4.22.4: `gtk_image_get_baseline_align()` divides
ascent by ascent + descent from Pango metrics, zero metrics give NaN, the int
cast gives INT_MIN, `gtksizerequest.c` catches it, prints this text and clamps
the baseline to −1 — cosmetic by construction. Confirmed on THIS binary
(headless Broadway, isolated config, scratch font caches — the user's
untouched): with an empty fontset (`FONTCONFIG_FILE` without a single
`<dir>`) `builddir/skimmer-for-linux` prints the byte-identical warning for 5
images at window open (count differs by backend, signature does not),
`G_DEBUG=fatal-warnings` under gdb traps it inside a
`gtk_layout_manager_measure` pass, and the first `pango_context_get_metrics`
call returns ascent 0 / descent 0 where normal fonts return 14550 / 3623 (the
same figures as on the sdr side); the control run with normal fonts, same
steps: 0 lines. We create no `GtkImage` (`grep gtk_image_ src/` = 0; the only
icon touches are `gtk_menu_button_set_icon_name` and
`gtk_button_set_icon_name`) and our CSS sets only the decode-pane font size in
pt. Upstream: GNOME/gtk#5926 reports the same text and numbers from a stale
fontconfig cache; the code is unchanged in GTK main. What upset the metrics at
14:08 on 22.8. cannot be recovered from the log; what is known is that it is
not ours. **No code change.** Reopen recipe, catches the first occurrence with
a backtrace:

```sh
G_DEBUG=fatal-warnings gdb -batch -ex run -ex bt --args builddir/skimmer-for-linux
```

A trace through `gtk_layout_manager_measure` with zero Pango metrics is this
again — check the font cache (`fc-cache -rv`, `~/.cache/fontconfig`), not the
code.

### SKM-6 — A second launch opens a second window inside the primary instance
- **Type:** bug · **Severity:** low · **Status:** done — reproduced, fixed, harness-verified (2026-09-05)
- **Source:** noticed while fixing SKM-1, 2026-09-05
- **Detail:** `src/app/main.c`, `on_activate`

With `G_APPLICATION_DEFAULT_FLAGS` a second `skimmer-for-linux` launch forwards
`activate` to the running primary, and `on_activate` runs again
unconditionally: a second window, a second `App`, a second set of timers and
a second `rbn_apply` against the port the first one already holds. Nobody has
reported it — during a contest only one instance is ever started.
Reproduced headless (Broadway, isolated config, feed on a spare port): the
second launch returns at once with exit 0 and the PRIMARY's stderr gains
`RBN feed on port 7399: … Adresa je užívána` — its second `on_activate`
trying to bind the port its first one holds.

**Resolution (2026-09-05, Richard's "ano"):** `on_activate` now starts with
the standard GNOME idiom — if `gtk_application_get_active_window()` already
exists, present it and return; nothing else is built. One `g_message`
(`app: second launch — presenting the existing window`) marks the event so a
log can show it happened. Verified on the same headless harness: second AND
third launch each return exit 0 at once, the primary logs the message twice,
the feed port is bound exactly once, zero warnings; 11 gates green. SKM-1's
one-teardown-per-window design now holds by construction — one process, one
`App`, one window.

## Open — ideas

### SKM-3 — Evaluate DeepCW as a neural decode backend alongside the DSP one
- **Type:** idea · **Severity:** — · **Status:** open (evaluate, nothing decided)
- **Source:** own research, 2026-08-25
- **Detail:** upstream <https://github.com/e04/deepcw-engine> (model + minimal
  Python/Node example), demo front-end <https://github.com/e04/web-deep-cw-decoder>

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

Open questions, in the order they would have to be answered:

1. **Licence gate, first and blocking.** DeepCW is **AGPL-3.0-only**; this app
   is GPLv3. Whether the model and the inference code can be linked into a
   GPLv3 binary — and what §13 would then oblige for the RBN feed, which is a
   network service — has to be settled before any code is written. If the answer
   is no, the idea ends here regardless of how well it decodes. An
   out-of-process backend talking over a pipe is the obvious fallback shape, but
   that too needs the licence question answered first.
2. **Where it sits in the pipeline.** The channelizer already produces complex
   baseband per channel; DeepCW wants real audio at 3200 Hz, so a channel would
   need a tone-detect + decimate stage in front of it. That throws away the
   phase the channelizer is careful to keep — fine for CW, but it means this
   backend is CW-only by construction and cannot be the path RTTY/PSK reuse.
3. **Cost per channel.** A skimmer runs hundreds of channels at once, so the
   question is not "does it decode" but "what does one channel-second cost".
   Unknown until measured. If it is too expensive to run everywhere, the useful
   shape may be a second-opinion decoder on channels the DSP backend already
   flagged as active but could not resolve into a valid callsign.
4. **Where it would run.** The dev machine has an Intel NPU (Core Ultra 7 265,
   `vpu_37xx`, ~13 TOPS) reachable through OpenVINO, which is attractive because
   it is a few watts and leaves the GPU alone — but **nothing has been compiled
   or measured**: whether the ONNX graph converts to OpenVINO IR and whether its
   operators are covered on that NPU generation are both open. CPU is the
   baseline to measure against first. And an NPU-only backend would be useless
   to anyone else, so any dependency must stay optional.

Nothing here is committed to a milestone. The first cheap step is offline: run
the upstream Python example over recorded contest audio and compare its output
against the DSP backend on the same recording.

### SKM-4 — In-app waterfall with decodes placed by frequency, click to set TX
- **Type:** idea · **Severity:** — · **Status:** doing — half 1 (waterfall + callsign column, click tunes RX) since 2026-09-05; half 2 (click sets TX) deferred to sdr-for-linux `SDR-12`
- **Source:** e-mail from Roy Andre Løntjern, LB0EI, 2026-08-29; answered 2026-08-30
  with a request for a step-by-step description of his workflow, which he sent the
  same day
- **Detail:** the whole thread is archived in the personal mailbox, subject
  *"Skimmer for Linux – pileup use and IC-7610 IQ support"* — his step-by-step
  description and a screenshot of his CW Skimmer setup are there; read it from the
  archive when work on this item starts

CW Skimmer is one of the main reasons the reporter still keeps Windows in the
shack. His use is DX pileups: he watches where stations send `5NN`/`599` inside
the split window — that spot is where the DX station was just listening — then
point-and-clicks to move **his TX frequency** there, and follows the DX as its
listening spot drifts across the window. He asks whether this app will eventually
offer a similar visual pileup/waterfall display with decoded CW positioned by
frequency, including point-and-click tuning.

Two halves, and only one of them is missing:

1. **Decodes placed by frequency, clickable** — already exists, but in the other
   window: validated callsigns are pushed back as spots onto the `sdr-for-linux`
   panadapter, click to tune. What the reporter wants is that view inside the
   skimmer itself. A horizontal waterfall as an **option** is most likely
   feasible; the open question is where it fits in the UI, not whether the data
   is there.
2. **The click has to set TX, not RX** — in a split pileup the RX stays on the DX
   frequency. Whether the tuning path can address the TX VFO (over TCI) has not
   been checked; that is the part that decides whether this workflow is
   supportable at all.

Nothing is designed yet, and nothing from his answer is summarised here on
purpose — the detail lives in the archived mail thread (see **Detail** above).

**Decision (Richard, 2026-09-05):** build half 1 now — CW Skimmer's layout
(`ContestShot.gif`) as the reference: frequency vertical, time sideways, kHz
scale, callsign column to the right with a dot per station on its frequency,
click tunes the (single) VFO like a station row. Half 2 is settled as NOT
supportable today, verified in sdr-for-linux's sources: the TCI `vfo:rx,ch,f`
handler ignores the channel index and sets the one frequency,
`split_enable`/`rit_*`/`xit_*` are echo-only (no backend), and the SDR has no
VFO B or split at all — filed there as `SDR-12`. Design and progress live in
`docs/SCOPE.md` under **M8**. The mail thread was not re-read for this (it is
not in the Gmail archive; the screenshot Richard pointed to is the brief).

### SKM-5 — IC-7610 wideband IQ as a source (`ic7610ftdi`)
- **Type:** idea · **Severity:** — · **Status:** open (not investigated)
- **Source:** same mail, LB0EI, 2026-08-29
- **Detail:** reporter's pointer only — DF7CB's `ic7610ftdi`, a Linux driver/tool
  said to receive the IC-7610 wideband IQ stream over USB. **Not verified here.**
  Same archived mail thread as SKM-4, in the personal mailbox.

The reporter runs an IC-7610 and asks whether that IQ source is relevant or
feasible for this project. Architecturally the work does not land in this repo:
the skimmer is a TCI client and takes its IQ from `sdr-for-linux`, so supporting a
big-three radio (Icom, Yaesu, Kenwood) means getting its wideband IQ into TCI on
the SDR side first. TCI stays the universal protocol between the programs.

With Icom it is realistic in principle and the hardware for testing is on the
bench — an IC-705 and an IC-7610. Not ruled out for the future; it needs study
before anything is promised, starting with what `ic7610ftdi` actually delivers
(sample rate, bandwidth, format, licence).

## Roadmap

Milestones and their order live in `docs/SCOPE.md`. Nothing in this backlog
blocks them: all three bugs are closed (SKM-1 fixed, SKM-2 explained, SKM-6
fixed), and none of them was noticed by the operator during 5 hours of
contest operation across two days.
