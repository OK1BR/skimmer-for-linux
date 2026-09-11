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

**Update 2026-09-05 (observation, item stays closed for now):** during the M8
live look the warning came back on Richard's desktop in BOTH launches of the
day — 8 lines (4 `GtkImage` × 2) within 10 ms of window open at 14:14:03, and
in the earlier instance 4 × 2 at 13:59:57 plus 4 NEW × 2 at 14:06:27 (the
day-1 shape exactly) — while the same binary headless on Broadway with the
same fonts printed none across a dozen runs. So "transient font-metrics
failure" is at best half the story: on the live Wayland session it is
reproducible at launch. Still cosmetic. Next time the desktop instance is
restarted anyway, launch it once as
`G_DEBUG=fatal-warnings gdb -batch -ex "catch signal SIGTRAP" -ex run -ex bt
--args builddir/skimmer-for-linux` and keep the first backtrace; only then
decide whether to reopen.

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

### SKM-7 — IQ blocks are not filtered by receiver
- **Type:** bug · **Severity:** high · **Status:** done — fixed, gate-proven (2026-09-11)
- **Source:** TCI compatibility audit for gh#2 (SM0ONR asks for SunSDR / ExpertSDR3), 2026-09-11 — not reproduced, no ExpertSDR3 here
- **Detail:** `src/engine/tci_client.c` `drain_binary` (~183); Thetis `Project Files/Source/Console/TCIServer.cs` `wantsIQStream()` (ramdor/Thetis @852bf0ef)

`drain_binary` accepts every block with type IQ, 2 channels and float32, but
never looks at the header's `receiver` word (h[0]). We only send `iq_start:0`,
yet a server may push other receivers' IQ to us too: Thetis has an
`AlwaysStreamIQ` option that streams IQ of all receivers to every client
(verified in its source), and a SunSDR2 reports `trx_count: 2`. RX1 blocks
would be fed into the RX0 channelizer — two bands mixed into one waterfall and
decoder bank, spots on wrong frequencies. Fix: drop blocks with h[0] != 0 and
log it once.

**Resolution (2026-09-11):** `handle_block()` in `tci_client.c` drops every
Stream block whose h[0] is not 0 before the type/format checks and logs the
receiver number once per session. Gate: the mock queues a receiver-1 block
at a telltale 96 kHz followed by a MARKER block — the marker's callback
arrives, the 96 kHz one never does (red on the pre-fix client: rx1 = 1).

### SKM-8 — Centre stamps are read from reserved header words without the server's echo
- **Type:** bug · **Severity:** high · **Status:** done — fixed, gate-proven (2026-09-11)
- **Source:** TCI compatibility audit for gh#2, 2026-09-11
- **Detail:** `src/engine/tci_client.c` ~204-208 and `handle_command` (~101-148); TCI spec Ver. 2.0 §3.4 p.9; sdr-for-linux `docs/TCI-SCOPE.md` (IQ centre stamps)

`iq_stamp:1` is our family extension: sdr-for-linux echoes it and fills
reserv[0..2] (h[8..10]). The client uses h[8] as the block centre whenever it
is non-zero and never checks that the server echoed `iq_stamp:1`. The spec only
calls these words "reserved" — it does not promise zeros. Thetis zeroes them
(verified); ExpertSDR3 is closed source. If a server puts anything there, every
decode and spot lands on a wrong frequency with no warning. Fix: set a flag on
the `iq_stamp:1` echo and honour h[8..10] only then; otherwise keep the `dds`
fallback.

**Resolution (2026-09-11):** `handle_command()` sets `stamp_ok` on the
`iq_stamp:1` echo (cleared in `stop()`); `handle_block()` reads h[8..10]
only while it is set. Gate: a SECOND client session against the mock with
the echo withheld and junk in h[8] (7021000) and a junk boundary (700) —
every callback carries the dds label and the whole 2048-frame block (both
checks red on the pre-fix client). `iq_stamp:1` is queued LAST so its echo
precedes the first stamped block.

### SKM-9 — Binary IQ is parsed as one continuous byte stream across WebSocket messages
- **Type:** bug · **Severity:** medium · **Status:** done — fixed, gate-proven (2026-09-11)
- **Source:** TCI compatibility audit for gh#2, 2026-09-11
- **Detail:** `src/engine/tci_client.c` `LWS_CALLBACK_CLIENT_RECEIVE` (~236-238), `drain_binary` (~170-217); TCI spec Ver. 2.0 §3.4 p.9

Binary frames are appended to one `GByteArray` and cut purely by the header's
`length`, so the code assumes every message is exactly 64 + length×4 bytes.
The spec draws the block as a struct with a fixed `data[16384]`; sdr-for-linux
and Thetis send exact sizes (verified), ExpertSDR3 unknown. If a server pads a
message, the padding is read as the next header — "bogus Stream length"
resets and silent data loss. Fix: one complete WS message = one block (collect
fragments until `lws_is_final_fragment()`), parse its header, ignore trailing
bytes.

**Resolution (2026-09-11):** `LWS_CALLBACK_CLIENT_RECEIVE` collects binary
fragments and parses ONE block when `lws_is_final_fragment() &&
lws_remaining_packet_payload() == 0` (lws reports FIN on every rx-buffer
piece of the final frame, so the payload test is what ends a message);
trailing bytes are ignored with one `g_message` naming their count (a
server concatenating blocks would show up there), a message shorter than
its header claims is dropped with one `g_warning`, and the accumulator
resets per message. Gate: a block with 100 trailing bytes delivers exactly
once and the marker behind it parses; a block cut 1000 bytes short delivers
nothing and the marker still arrives (on the pre-fix client the truncated
case swallowed the marker — desync; the padded case passed there too,
because the old code recovered through its "bogus Stream length" reset).
The gate's fragmentation check (16448-byte blocks over an 8192-byte rx
buffer) still holds.

### SKM-10 — The IQ request goes out as three commands in one text frame
- **Type:** bug · **Severity:** medium · **Status:** done — fixed, gate-proven (2026-09-11)
- **Source:** TCI compatibility audit for gh#2, 2026-09-11
- **Detail:** `src/engine/tci_client.c` `skim_tci_client_start` (~420)

`iq_samplerate:192000;iq_start:0;iq_stamp:1;` is queued as a single WebSocket
text frame. The spec says nothing about several commands per frame.
sdr-for-linux and Thetis split on `;` (Thetis verified), but ftl/tci — a Go
client written for ExpertSDR — sends one command per frame. If ExpertSDR3 reads
only the first command of a frame, `iq_start` is lost and the skimmer sits
"connected" with no IQ. Fix: queue each command as its own message.

**Resolution (2026-09-11):** three `cli_queue()` calls; the WRITEABLE
handler already sends one queued string per frame. Gate: the mock counts
complete text messages (`lws_is_final_fragment && remaining == 0`) — three
by the time `iq_stamp:1` has arrived (one on the pre-fix client).

### SKM-11 — The TCI port cannot be set
- **Type:** bug · **Severity:** medium · **Status:** done — built, headless-verified (2026-09-11)
- **Source:** TCI compatibility audit for gh#2, 2026-09-11; Richard: the port must be configurable, as it is in log-for-linux
- **Detail:** `src/app/main.c` ~1247 (pipeline config), ~1370 (reachability probe), ~1500 (Preferences text); only `[tci] host` is persisted (~930, ~1057)

Only the host is a setting; 40001 is hard-coded in three places. The TCI spec
names no default port and the server side decides it (ExpertSDR3 users are told
to check the port in its settings), so a server on any other port is
unreachable. Fix: a `[tci] port` key (1–65535, default 40001) loaded and saved
next to `host`, a Port row in Preferences → TCI server, used by both the
pipeline and the probe; a change reconnects like a host change. Reference:
log-for-linux `src/app/settings.c` (~145-149).

**Resolution (2026-09-11):** `App.tci_port`, `settings_load_tci_port()`
(`[tci] port`, 1–65535, 40001 otherwise), saved next to `host`; the
pipeline config, the 3 s probe and its "searching for host:port…" subtitle
and the About debug_info all read it; Preferences → Radio → TCI server has
a Port spin row (1–65535) under Host, the group text no longer names 40001
as a fact but as the usual value ("ExpertSDR3 shows its own in its
settings"); `prefs_closed` treats a port change like a host change (save +
pipeline rebuild + rescan). Verified headless (Broadway, private D-Bus,
isolated XDG dirs — Richard's live instance untouched): with `port=40123`
in the isolated settings and a bare TCP listener on 127.0.0.1:40123 the
listener took six connections in 16 s — probe + WebSocket connect every
~5 s, the handshake timeout cadence — so the value reaches both the probe
and the TCI client. NOT exercised: the Preferences round trip (spin row →
save); Richard's look.

**Also added for the remote tester (gh#2, "connects, no output"):** three
log lines in `tci_client.c` — `tci: IQ stream up — receiver, rate, format,
channels, frames/block` on the first accepted block; `tci: server runs IQ
at X Hz (asked for Y)` when the `iq_samplerate` echo differs from the
request (the pipeline builds the bank from the blocks' own rate, so this is
information, not a fault); and a one-shot `g_warning` from the service
thread when 3 s pass after `iq_start:0` with no IQ block, quoting what the
server announced. A missing `iq_start` echo is NOT treated as an error (the
spec marks the command client→server only). All three lines are
gate-exercised: a THIRD client session against a mock that answers
`iq_samplerate` with 96000 and never starts IQ must produce the "server runs
IQ at 96000 Hz (asked for 48000)" line and the 3 s warning (read back through
a `g_log_set_writer_func` tap). Lesson from building it: `lws_service()` in
libwebsockets ≥ 3.2 ignores its timeout and sleeps until an event, so a
watchdog polled from the service loop fired ~5.3 s late on a silent
connection — the alarm is now an `lws_sul_schedule` armed when `iq_start:0`
is written (fires at +3.00 s, measured). Mock lesson: its outbox is global,
so a new session must start with it emptied (the no-IQ session had received
the previous session's tail). `tci-client` gate: 20 → 32 checks.

### SKM-12 — A callsign torn into three or more tokens is spotted as its two-token fragment
- **Type:** bug · **Severity:** high (a wrong callsign on the panadapter) · **Status:** open
- **Source:** Richard's sdr-for-linux screenshot 2026-09-11 18:58 (label `UA6H` at 14038.5) + the live decode log `~/.local/share/skimmer-for-linux/decodes-2026-09-11.log`, 14039.x, 18:49–18:57
- **Detail:** token stream saved to `/var/tmp/skimmer-iq/ua6hnu-live-text-14039-18h49-18h57.txt` (980 chars of joined pane text); mechanism in `src/engine/callsign.c` — the join hypothesis (~393-410) and `skim_callsign_extractor_best_ex` (tie → longer call); `src/engine/station.c` `clip_fold`

The operator on 14039 keys `CQ CQ DE UA6 H NU UA6 H NU PSE K` with inter-word
gaps INSIDE the callsign, so v2 reads it correctly but as three tokens: over
eight minutes the log holds `UA6` 12×, `H` 35×, `NU` 32× and `UA6HNU` never
(sometimes even four pieces, `UA 6 H NU`). The extractor's join hypothesis
glues only two ADJACENT tokens: `UA6`+`H` = `UA6H` validates (UA6 prefix + H
suffix) and `H` alone is no call, so the join is accepted; `H`+`NU` = `HNU`
does not validate, and the join is never chained onto the previous join, so
`UA6HNU` is never even proposed. Repetition lifts `UA6H` past the 0.70
panadapter bar, and the station table's clip fold cannot retire it because
the longer call is never reported. The real call is most likely UA6HNU
(inferred from the repeated pattern, not verified). Same rukopis class as
EA1EYL (2026-07-16), whose fix covered only the CQ chain and the glued DE.

Candidate fix, to be MEASURED before it stays: chain the join through the
previous accepted join (`prev_join` + `tok`, here `UA6H`+`NU`) under the
same guards (no stop word on either side, the join must explain something
the parts do not, tie goes to the longer call) — then repetition makes
`UA6HNU` outscore `UA6H` and the existing clip fold retires the fragment.
Gate: a token-level case in `skimmer-call-test` fed from the saved stream
(`skim_callsign_extractor_feed`), plus phantom guards (a chained join must
not mint a call out of `<call> <stop-word> <fragment>` sequences). Regression
check: the 2026-09-11 IQ fixture (below) must keep its station table
bit-identical with zero phantoms.

### SKM-13 — A whole over keyed as ONE token (`CQCQCQDEEA5JQFEA5JQFK`) yields no candidate
- **Type:** bug · **Severity:** medium (a calling station is missed) · **Status:** open
- **Source:** offline replay of the fresh IQ fixture, 2026-09-11 19:05–19:08, 14040.00
- **Detail:** fixture `/var/tmp/skimmer-iq/iq-20260911-ua6hnu-192k.cf32` (180 s, 192 k, centre 14 016 981 Hz, `.meta` written by the probe; replay outputs `replay-ua6hnu.{out,err}` beside it, decodes in `…cf32.decodes.log`); `src/engine/callsign.c` `cq_run_token` and the DE-strip fallback (~412-420)

EA5JQF on 14040.00 keys the whole over with NO word gaps: the pane reads
`CQCQCQDEEA5JQFEA5JQFK` as a single token, twice in 180 s. Neither fused-fist
rule covers it — `cq_run_token` accepts nothing but `CQ` repeats, the
DE-strip needs the token to START with `DE` — so the extractor emits no
candidate at all (54 evaluations at that frequency, every score 0.00) and
the station is absent from the replay's table (7 stations: IZ4ECE, EH1SDC,
ON4AEO, EA6NB, 4L8A, TA5ARU, EA5JN). Same rukopis family as SKM-12, the
opposite extreme: gaps closed instead of stretched.

Candidate fix, to be measured: a lexical fallback for a long invalid token
that begins with a CQ run and continues with `DE` — strip the run and the
marker, then look for a valid call that repeats inside the remainder
(`EA5JQF EA5JQF K`) — strictly a fallback where the normal path yields
nothing, as variant C was. The fixture above is the end-to-end witness.

**The fixture itself** is the first CW IQ recording since the corpus deletion
of 2026-07-19 and doubles as the regression check for ANY extractor change:
station table identical, zero phantoms. It does NOT contain UA6HNU (the
operator stopped at 18:57:21, the recording started 19:05:46) — SKM-12's
evidence is the live decode log and its saved token stream.

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
- **Type:** idea · **Severity:** — · **Status:** doing — half 1 DONE 2026-09-05 (M8 in SCOPE): engine tap + view + palettes + drag-pan + absolute-frequency history + the waterfall flowing through a retune (SDR HP kick, IQ centre stamps, largest-segment rows — Richard's live verdict on 80 m) + the callsign column with click-to-tune + logbook prefill (LIVE-verified 22:05) + the column's tooltip carrying kHz / speed / dB / heard / age (a dB after the call tried and taken out on his look) — and the station list DELETED on his word (~23:30); half 2 (click sets TX) deferred to sdr-for-linux `SDR-12`; half 1 SHIPPED in v0.4.0 (2026-09-06)
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
blocks them. Closed: SKM-1 fixed, SKM-2 explained, SKM-6 fixed (none noticed
by the operator during 5 hours of contest operation across two days), and
the gh#2 TCI hardening SKM-7..11 fixed on 2026-09-11 with the `tci-client`
gate at 32 checks. Open bugs: SKM-12 and SKM-13, two extractor cases from
the same evening's 20 m band (a callsign torn into three tokens spotted as
its fragment; a whole over fused into one token missed) — decoder-quality
work with a fresh IQ fixture to measure against. What remains for gh#2 is
the first run against a real ExpertSDR3 — SM0ONR reports the skimmer works
on his SunSDR once the device bandwidth is raised (156/312 kHz), so the
"connected, no output" at his default settings is the open question the
new log lines are meant to answer from his log.
