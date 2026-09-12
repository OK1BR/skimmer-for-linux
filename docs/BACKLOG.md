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
- **Type:** idea · **Severity:** — · **Status:** doing — evaluated 2026-09-12, Richard's "ano" the same evening; backend + gate + replay A/B + async batched workers + the Preferences "CW engine" and "Device" (CPU / GPU CUDA) rows BUILT, headless-verified, GPU measured (below); open: Richard's live look, a station-table QSY rule, mutation twins, NPU (needs an OpenVINO runtime path)
- **Source:** own research, 2026-08-25; Richard 2026-09-12 ("zjisti, co to přesně je a jak bychom to implementovali… přepínač dekódovacího enginu")
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

**Evaluation (2026-09-12, Richard's ask: "what exactly is it and how would we
implement it — the app wants a decoding-engine switch").** Everything below was
measured, not read off the README; scratch (clones, a venv with onnxruntime
1.30, the bench and replay scripts, all outputs) sits in
`/var/tmp/deepcw-research/`.

*What the published model is.* `deepcw-engine` (2 commits, 2026-06-16,
AGPL-3.0-only) ships ONE model: `model.onnx`, 3.61 M parameters (15 MB), a
Conformer encoder (3 Conv2d front-end layers striding only in frequency, then
6 blocks of FFN + multi-head self-attention + depthwise conv k=17, d=192),
CTC head over 42 classes (letters, digits, `, . / ?`, space, blank), opset 18,
exported from torch 2.10. Input `[batch, 1, time, 65]` = `log1p |STFT|` of
3200 Hz audio, FFT 256 / hop 48 (15 ms), the 65 bins covering 400–1200 Hz at
12.5 Hz; output `[batch, time, 42]` log-probs, ONE frame per 15 ms, greedy CTC
collapse. The attention is global over the whole window (no causal variant),
which is why the author's example wants 5–20 s clips and the web app streams
by re-inferring a sliding window and committing text only up to a word gap
≥ 1.25 s before the tail. **The web app (`web-deep-cw-decoder`) is a different
deployment:** 9600 Hz / FFT 768 / hop 192 / per-window CMVN, and its four
models (`en`, `en_narrow` 5-bin pileup, `ja`, `cw_detect`) are fetched from
deepcw.cc by UUID and are NOT published — only the standard engine model is
usable, and the README's CER heat-map is that deployment's claim. The web repo
carries no licence file (all rights reserved by default), so any streaming /
stitching logic here must be our own implementation, not a port.

*Synthetic checks (own keyer, SNR in 2500 Hz):* 25 WPM exact copy at 0, −6,
−8 dB; −10 dB ≈ 6 % CER; collapse at −12 dB. Tones at 450 and 1150 Hz (band
edges) read exact. **Noise-only input gives an EMPTY output** (six seeds at
4 s and 12 s, also after level normalisation, also on an empty fixture
frequency) — no phantom text on dead channels, the July reader's failure.
Two tones 100 Hz apart in one window give plausible-looking nonsense
(`CQ51AKCXC1BK 2KA`) — that is the phantom mechanism, so a window must hold one
station. A 65-bin tile with everything outside ±3 bins (±37.5 Hz) zeroed reads
exactly as the full audio; ±1 bin degrades — so our 125 Hz channelizer output
IS a sufficient input, no audio path needed. Hard-cut 4 s windows garble their
edges (`Q CQ DE G`) — the sliding-window commit rule is needed, and it puts
text on the pane ~2–3 s after the keying, unlike v2's per-element draft.
**Amplitude trap:** the model is scale-invariant only over ~×0.001–×10 of a
full-scale WAV; real IQ from our probe sits below that (tile log1p 0.00–0.02),
and the first `InstanceNormalization`'s epsilon then silences weak channels —
IZ4ECE (25 dB, v2 reads it for 180 s) decoded NOTHING until each window was
normalised to peak 0.5. Normalise per window; after that every station reads.

*Cost (CPU, Core Ultra 7 265, onnxruntime CPU EP):* one thread ≈ 6 ms per
audio-second at 1–4 s windows, 8–10 ms/s at 12–20 s (attention is O(T²)).
Batched, 4 intra-op threads: 2.2–2.8 ms per channel-second; 20 threads is
WORSE per channel than 4. Budget: an 8 s window re-run every 2 s ≈ 3 % of one
core per active channel → 50 active channels ≈ 1.5 of 20 cores. All 1536 CW
channels continuously is impossible on CPU — an energy pre-gate (a keyed line
above the tile floor) decides which channels get inference. GPU (RTX 5070,
CUDA 13.3 installed, `onnxruntime-cuda` in extra) unmeasured; the batch
dimension is dynamic so all active channels can go as one batch.

*Real IQ, 20 m fixture `iq-20260911-ua6hnu-192k.cf32` (12 s windows, hop 6 s,
125 Hz tile, normalised; v2's own decode log beside it):* IZ4ECE and ON4AEO
read with FEWER mutations than v2 (`CQ CQ DE IZ4ECE IZ4ECE K` vs v2's
`IZ4NE`/`IGEECE`/`IZIAECE`; `CQ CQ DE ON4AEO ON4AEO PSE K` vs
`ON4AEOWNHEOMEEAEOPSE`); EA6NB, TA5ARU, EA5JN equal; **the QSO on 14012.97,
which v2 logged for 180 s as `CQ CQ DE EAAOY EAAOY PSE K` and never tabled,
reads `EA6AOY`** — a station the classical path lost; EA5JQF (SKM-13) comes
out as the SAME fused token `CQCQCQDEEA5JQFEA5JQFK` — the fused/torn fist
classes (SKM-12/13) are lexical, a neural front-end does not remove them
(it also tears `OL4 AB B`, `O K1 M G3 W` on 80 m). *80 m contest recording
`iq-20260912-80m-cw-contest-192k.cf32` (300 s, 28 v2 stations, 12 busiest
frequencies):* on every channel DeepCW reads at least as well as v2 on the
running station (OK1FHI, OK1MDK, OK1DOL, OM5AA, OK2PGY, OK7PY, SQ100PKP…);
it reads the QSO PARTNER where v2 gets fragments (OK1XC's callers `OK1MWW
AHOJ 5NN T84 OL1ADZ`, `OL5AJU`, `OM7CF`, `OK2PKD` where v2 logged `MO`, `OM`,
`MAWW`; OK1MDK 156–186 s `CQ OK1MDK OK1MDK` where v2 has `CQOKS`, `SKTESTE`);
and it emits short low-confidence junk in QRM windows where v2 stays silent
(3550.89 kHz 0–48 s, `M IH NW370V4UKI` at 0.3–0.8; TA5ARU 108–150 s on 20 m)
— the per-character CTC posterior separates most of it (clean text 0.96–1.00,
junk mostly < 0.7) but not all, so DeepCW text may reach the extractor ONLY
through the existing validation/repetition gates plus a measured confidence
bar, and the tone splitter's contested rule still applies. Counter-cases,
same tables: 3529.95 kHz 12–36 s v2 read `K1KN AHOJ 5NN 9 … ML2BIL` where
DeepCW gave `H9 N` / `?5H?` at 0.5 confidence. Caveat on the rendering: the
comparison script joined v2's log characters after stripping each entry, so
the v2 strings quoted here LOST their word gaps (the 80 m log holds 1455
word-gap entries) — v2 does segment words; the mutations are the real
difference. No ground truth was available; these are side-by-side readings
of 12 s hindsight windows, not CER, and a streaming build commits only up to
a word gap, so it reads window edges worse than these tables and lands text
2–3 s after the keying.

*Licence, both texts read:* AGPL-3.0 §13 second paragraph and GPL-3.0 §13
each grant permission to "link or combine" a work under the other licence
"into a single combined work, and to convey the resulting work"; the AGPL's
network-interaction clause then applies to the combination. The runtime
(ONNX Runtime) is MIT. Whether a weights file is a copyrightable "work" at
all was NOT verified here (an open question by reputation, unchecked); the
author's stated terms are AGPL-3.0-only and that is what we would honour
(notice + source offer, which the public repo already gives).
The go/no-go is Richard's.

*Proposed implementation (not started):* (1) `src/engine/decode_deepcw.c`
implementing `SkimDecodeBackend` — per-channel ring of complex baseband,
tile builder (×4 interpolation of the 250 Hz channel to 1000 Hz, 80-sample
Hann, hop 15 = exactly the model's 80 ms / 15 ms STFT at 12.5 Hz bins, the
channel's bins seated at tile index 32, per-window peak normalisation,
log1p), energy pre-gate, ONE scheduler thread batching all gated channels
every 1–2 s over an 8–12 s window, word-gap commit (own code), per-char
posterior → `confidence`, tile-peak centroid → `freq_offset_hz`,
`level()`/`tone_offset_hz()` from the tile so ghost arbitration and freq
locks keep working; text goes down the SAME extractor/station/spot path.
(2) ONNX Runtime through its C API, `dlopen`-ed at run time (`libonnxruntime
.so.1`, `OrtGetApiBase`) with a vendored MIT header — Debian trixie and
Fedora ship it (`libonnxruntime1.21`, `onnxruntime` 1.26), Arch `extra` has
1.29, Ubuntu 24.04 has NONE — so the binary must run without it and the
engine row must say "not available" instead of failing. (3) Model + metadata
NOT in git: resolved from `~/.local/share/skimmer-for-linux/models/deepcw/`
with a pinned sha256 and the AGPL notice beside it (bundling into
AppImage/deb/rpm is a separate decision). (4) App: Preferences → Decoding →
**CW engine** combo ("Classical (v2)" / "DeepCW (neural)"), persisted
`[decode] engine`, a change rebuilds the pipeline like a mode change;
`SkimPipelineConfig.cw_engine`; `SKIM_CW_ENGINE=deepcw` for `skimmer-replay`.
(5) Proof before any default flips: offline A/B on both fixtures through the
replay harness (station tables, phantom count, CPU), a labelled subset, then
Richard's live look. The July rule holds: gate-proven offline first, and the
classical path stays the default until a live band says otherwise.

**Built 2026-09-12 night (Richard's "ano"), offline-proven.** In the tree:
`vendor/onnxruntime/` (the MIT C API header, v1.21 = `ORT_API_VERSION 21`,
`VENDOR.md`), `src/engine/ort_shim.c` (dlopen of `libonnxruntime.so.1` or
`SKIM_ORT_LIB`, `GetApi(21)` — verified against the 1.30.0 library: same
API table, `CreateEnv` OK), `src/engine/decode_deepcw.c` behind the
`SkimDecodeBackend` vtable, `SkimCwEngine` in `SkimPipelineConfig` +
`SKIM_CW_ENGINE=v1|v2|deepcw` (the pipeline falls back to v2 with a
warning when the runtime or the model is missing; `skimmer-replay` prints
the engine in its header), gate `skimmer-deepcw-test` (27 checks: the pure
commit rule, the tile builder through the vtable — +30/−30 Hz offset sign,
level, gate keyed vs noise, dit estimate — and, when `SKIM_ORT_LIB` +
`SKIM_DEEPCW_MODEL` resolve, the model on a synthetic keyed tone and on
noise; exit 77 = SKIP otherwise). **13 gates.** Design as built: 20-point
Hann DFT per 4 samples on the 250 Hz channel (80 ms window, 16 ms hop —
6.7 % time stretch, inside the model's speed range), bins −5..+5 kept per
frame, 10 s ring, a tick every 1.6 s per channel (staggered), gate = line
≥ 6 dB over the inner-bin floor with a keyed duty in 3–97 %, window
peak-normalised, inference INLINE on the engine thread (replays stay
deterministic), greedy CTC, commit up to the last word gap ≥ 1.5 s before
the window end (≥ 2 s in), committed audio leaves the window, one WORD per
`process()` so the extractor/station table see v2's hit cadence. Found and
fixed by the fixtures, each a measurement: (a) a fast level EMA decayed to
noise in every word gap → peak hold, 2 s; (b) offset updates on noise frames
dragged +30 Hz to 14 → mark frames only (≥ 4× floor and ≥ ½ peak);
(c) SNR from the line bin's own minima saturated (an 80 ms window never
reaches the noise inside a 48 ms gap) and from the outer bins was inflated
(they sit on the filter roll-off) — now window peak over the inner-bin floor
against a BAND-WIDE floor EMA, −10 dB (bin → 125 Hz), which lands on v2's
scale (20 m: IZ4ECE 28 vs v2 25, EH1SDC 66 vs 64, TA5ARU 15 vs 15);
(d) **weak word gaps tear callsigns** — the model splits `OK2B TK`,
`OK1C Z`, both halves validate and the extractor keeps the valid short
part (OK2B 13 reports at 0.95 on 80 m): a gap with posterior < 0.8
(`SKIM_DEEPCW_SPACE_P`) is dropped ONLY where it would tear a ≤ 2-char piece
off a token (dropping every weak gap fused weak stations' text and lost
TA5ARU on 20 m) — OK1CZ then reads right, OK2B falls to 3 reports;
(e) WPM from the on-run histogram read dah-heavy fists at a third (OK1XC
10) → character rate of the committed span (PARIS: 12 × chars/s) with the
histogram as fallback — still crude (OK1XC 10, OK1JAX 12 vs v2 29, 23).

*A/B numbers, final build (`/var/tmp/skimmer-iq/ab/*-H.out`, v2 tables in
`replay-*.v2.out`, logs `*.v2.decodes.log` / `*.deepcw.decodes.log`).*
20 m (180 s): v2 7 stations; DeepCW 10 = all seven + **EA6AOY** (45
reports, CQ — the station v2 logged as EAAOY and never tabled) + EA6ROY
(3 reports, a mutation twin of it, 0.90) + II6IGTO (11 reports, 0.90,
unverified); EA5JQF stays untabled (SKM-13 is lexical, as predicted).
80 m contest (300 s): v2 28; DeepCW 33 — 22 in common, new OK5O (36, CQ),
OK1CZ (12), DL3GAK (18), OL6A (13), OE3MM (2), HB9T (5) and singles OK1KN /
DH3Z / TC1MGW (a mutation of OK1MGW); lost SP2NBV, OL2BHZ, OL5A, OL5BOO,
OK1MGW (all read in the side-by-side but out-reported in the station
table's takeovers — DeepCW emits ~5× fewer hits than v2's per-character
stream); OK2B 3 reports (was 13). **OK1DOL sits on its spur** (3537.00,
a +4.57 kHz image 12.5 dB down in the IQ, same for OK1MDK ↔ 3536.44/3541.01):
the table's same-call rule moves a station to whichever report reads
stronger, and an intermittent spur peaks within 1–3 dB of the real signal
for a few seconds at the end of the recording — v2 wins that race only by
report count (258 vs 35). Filed as a station-table item: a same-call report
> merge distance away needs sustained evidence before it re-positions the
station. Cost: 20 m 135 s wall for 180 s (1.3×), 80 m 190 s for 300 s
(1.6×), one inference per gated channel per tick at 4 intra-op threads on
the engine thread, RSS ~400 MB — so **live needs the async worker** (the
engine thread must not block; jobs snapshot the window, results land in a
per-channel mailbox, the commit rule stays on the engine thread), and the
replay keeps the inline path for determinism.

**Built later the same night: the worker and the switch.** Inference runs
on `SKIM_DEEPCW_WORKERS` (2) threads: a tick snapshots the window into a
job, a worker runs the model and leaves `logp` in the channel's mailbox,
`process()` on the engine thread applies the commit rule (text order stays
deterministic, no lock on the text path); one job per channel in flight,
a queue > 256 jobs skips ticks with one warning; a channel freed with a job
in flight is freed by the worker (refcount). `skimmer-replay` sets
`SKIM_DEEPCW_SYNC=1` so replays stay inline and bit-stable. Gate: the
model section now runs the async path with a paced feed (2 ms per 256 ms
block, then drains the mailbox) and the deferred free; 28 checks; **13
gates green** (`meson test` with the runtime env runs the model part).
App: Preferences → Decoding → **CW engine** ("Classical (v2)" / "DeepCW
(neural)"), persisted `[decode] engine` = `v2|deepcw`, a change rebuilds
the pipeline like a mode change; the row's subtitle says whether DeepCW
is available on this machine and, if not, why and where the model must be
(`skim_decode_deepcw_available`, `..._model_path`); About's debug_info
carries "CW engine: <resolved name>"; the app logs `app: pipeline engine
<name>` at every pipeline build. The model sits at
`~/.local/share/skimmer-for-linux/models/deepcw/model.onnx` (sha256
`ef120799…fe02`, `NOTICE` + the AGPL text beside it; not in git). Headless
check (isolated config on Broadway, private D-Bus, `.invalid` TCI host,
`SKIM_IQ_FILE` = the 20 m fixture): with `engine=deepcw` + the runtime
via `SKIM_ORT_LIB` the app logs `pipeline engine deepcw` and its decode
log fills with word hits (IZ4ECE, EA6NB, ON4AEO…) through the async
path, zero criticals over 70 s; with the model unreachable it warns
`DeepCW engine not available (model file not found: …) — falling back to
the classical v2 decoder`, logs `pipeline engine cw-v2` and decodes per
character as before; `[decode] engine=deepcw` survives the save. The
combo round trip itself was not clicked headless (the settings-file path
was). **For a live look the runtime must be reachable:** no system
`onnxruntime` is installed (Arch `extra/onnxruntime-cpu` 1.29 would be
Richard's call), so today the launch is
`SKIM_ORT_LIB=/var/tmp/deepcw-research/.venv/lib/python3.14/site-packages/onnxruntime/capi/libonnxruntime.so.1.30.0 builddir/skimmer-for-linux`
+ Preferences → CW engine → DeepCW. Classical v2 stays the default.

**GPU (later the same night, Richard: "zkus napřed přidat GPU… uživatel by
měl mít ty možnosti v nastavení").** `onnxruntime-cuda` 1.29.0-3 installed
from extra with Richard's ok (+ cpuinfo, cudnn-frontend, nccl, onednn;
714 MiB download, 1.05 GiB installed) — it provides `libonnxruntime.so.1`
system-wide, so `SKIM_ORT_LIB` is no longer needed for a launch. Shim:
`skim_ort_session_new(…, device)` appends the CUDA execution provider
(`CreateCUDAProviderOptions` / `UpdateCUDAProviderOptions` device_id 0 /
`SessionOptionsAppendExecutionProvider_CUDA_V2`) and on ANY failure builds
the CPU session instead; `skim_ort_session_device()` reports "CUDA:0" or
"CPU (cuda unavailable: <reason>)" and runtime_info carries it. **Arch
packaging quirk, worked around:** the provider library references cuDNN
symbols but does not list `libcudnn.so.9` as NEEDED (only cudart/cublas 13),
so its load failed with "undefined symbol:
cudnnGetConvolutionBackwardDataAlgorithm_v7" although cuDNN 9.26 exports
it (nm-checked) — the shim `dlopen`s `libcudnn.so.9` with `RTLD_GLOBAL`
before appending the provider; harmless elsewhere. Backend: the session is
refcounted so `skim_decode_deepcw_reset()` (device change) can drop it while
workers finish; `skim_decode_deepcw_set_device("cpu"|"cuda")`,
`SKIM_DEEPCW_DEVICE` for replays; **workers now BATCH**: every queued
window goes in one run, zero-padded at the end to the longest
(`SKIM_DEEPCW_BATCH` 32) — one launch for N channels, what a GPU wants.
App: Preferences → Decoding → **Device** ("CPU" / "GPU (CUDA)"), shown only
while the engine row says DeepCW, persisted `[decode] device = cpu|cuda`;
a change resets the session and rebuilds a DeepCW pipeline; the subtitle
carries what actually runs. Gate 35 checks: device=cuda requested → either
"CUDA:0" or the fallback wording (the CPU-only venv runtime answers "CUDA
execution provider is not enabled in this build"), reset back to CPU, three
channels fed in lockstep through the batched worker read their own text
with no cross-talk. **Measured:** 80 m contest replay (inline, batch 1)
103 s wall on CUDA:0 vs 190 s CPU (2.9× vs 1.6× realtime); the station
table is the CPU one plus SP3HLM (2 reports) — GPU float paths are not
bit-identical, a marginal weak window flipped. Headless app check with
`engine=deepcw device=cuda` and the IQ replay: `deepcw: ONNX Runtime 1.29.0
via libonnxruntime.so.1, CUDA:0`, decodes flow. NPU stays open: no Arch
ONNX Runtime package carries the OpenVINO provider (checked), so it needs
either Intel's onnxruntime-openvino build or a second shim on OpenVINO's
own C API (openvino 2026.3.1 + intel-npu-plugin are in extra, the driver
and `/dev/accel0` are on the machine; an earlier measurement found the NPU
no faster than the iGPU, its value is watts).

**Latency — the sliding window (2026-09-13 ~00:15, Richard: "dekódování je
dost pomalé, má několik vteřin rezervu… potřeboval bych se dostat k téměř
realtime překladu"; GPU sat at 8 %).** The lag was the commit rule, not
compute: a 1.6 s tick, a 1.5 s tail guard, a wait for a word gap and the
one-word-per-call trickle added up to 3–5 s. Simply speeding the old rule
up (tick 0.5 s, tail 1.0/0.75 s) LOST stations (34 → 27/28) and tore
calls (OL1BI, K2BVX, TU5NN), because every commit dropped the committed
audio from the window and the Conformer was left with 2–3 s of left
context. New rule: the WHOLE ring (10 s) is re-read every tick, a
character is final once it sits ≥ tail (1.0 s) before the window end, a
frame cursor (+ 4-frame margin for characters; spaces on frame order,
doubles squeezed) keeps anything from being emitted twice; no word-gap
wait, no force commit, the cursor never moves on silence. Tick by device:
0.5 s on CUDA, 1.0 s on the CPU (`SKIM_DEEPCW_TICK` overrides) — 20
audio-seconds per channel-second on the GPU, which is what the idle GPU
was for. 80 m fixture, CUDA, inline: 33 stations in 96 s wall (3.1×);
against the old rule the table is v2-like in depth (OK1FHI 294 reports,
OK1MDK 302, DK1WI 109; 9449 fragments vs 2240), OK1DOL and OK1MDK sit on
their REAL frequencies (3532.45 / 3541.00 — the spur race is decided by
report count now, as for v2), OK1MWW and OK2BVX read whole (were OC1MWW /
OK2B), SP2NBV, SQ100PKP, OM5AA 50, OL0CHC, PA2M appear; costs: OL1B →
OL1BIC (the tear rule glues the 2-char "IC" that follows the call, 128
reports at 0.75), OK1C again (a confident gap before the "Z"), OK5O gone
(takeover on the 3540.01 pileup channel). Gate rewritten for the rule
(33/29 checks async/inline), 13 gates green. Expected latency now ≈ tick +
tail ≈ 1.5 s on the GPU. Next lever if he wants the pane to feel live: the
model's uncommitted tail as gray DRAFT text through the phase-B pane ops
(`take_pane_op`), firming up at commit — the extractor path unchanged.

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

### SKM-14 — Waterfall time span as a setting, default much shorter so keying reads
- **Type:** idea · **Severity:** — · **Status:** open
- **Source:** Richard, 2026-09-11 ("rychlost pohybu spektra" — the speed the spectrum moves), after the Whole Milk demos (OH7LZB, 10 Sep 2026: youtu.be/FSvMRQ1qojw Mac, youtu.be/qqQsqIUnPXw Windows)
- **Detail:** measured off the demo videos by frame cross-correlation (local notes in `~/Downloads/whole-milk/`, not in the repo)

Whole Milk's waterfall shows **~4 s of history across its width** (Mac demo, toolbar selector "4 s";
3.8 s and 4.3 s measured in two segments). The selector *is* the span: in the Windows demo "3 s"
measures 2.8 s and "6 s" 5.5 s across the waterfall. Ours is fixed: `SKIM_SPECTRUM_HOP_DIV` 4 →
93.75 rows/s, `SKIM_WF_ROWS_PER_PX` 2 → 21.3 ms/px ≈ 47 px/s, i.e. **~15 s on a 700 px
waterfall**. At 25 WPM a dit (48 ms) is ~2 px wide here; at their span it is ~4 pt (assuming a 2×
Retina capture) and single dits and dahs read in the picture. `rows_per_px` is an integer ≥ 1
(`wf_compose.c`, `MAX(win->rows_per_px, 1u)`), so a span under ~7.5 s at 700 px needs either a
fractional rows-per-px (one row stretched over more than a pixel) or a higher row rate
(`HOP_DIV` 8 = 187.5 rows/s, twice the FFT work). Make the span a user setting (seconds across the
visible width, like their selector); the default is picked at Richard's live look.

### SKM-15 — A tail of the decoded text next to each call in the waterfall column
- **Type:** idea · **Severity:** — · **Status:** open
- **Source:** Richard, 2026-09-11 ("úryvek dekódovaného textu, vedle volačky"), Whole Milk Mac demo
- **Detail:** demo frames ~35 s, 90 s, 170–175 s (local `~/Downloads/whole-milk/vsechny-snimky/mac/`)

Whole Milk draws one text line per signal to the right of the waterfall: the frequency's holder call in
brackets (`[UA3QGT]`), then the running decoded text (latest overs, `…` between them), joined to the
signal's frequency by a dot and a leader line; the pane underneath holds the full transcript of the
selected signal. Zoomed out over a crowded band the lines stack and the leaders fan out to them.
Ours: the column (`wf_view.c`, `COLUMN_W` 180 px) carries a dot and the call only; kHz / speed / dB /
heard / age live in the tooltip (a dB after the call was tried and taken out on Richard's look,
2026-09-05), and the decode pane follows only the tuned station. Wanted: the last N characters of a
station's decoded text after its call. Open points: a wider or elastic column (or text flowing over
the decode area), where the snippet comes from (tracker's recent text), and a collision policy when
zoomed out — today the weakest labels are hidden over capacity (`wf_view.c` ~321), theirs stack
and fan out.

### SKM-16 — Spectrum zoom in the manner of Whole Milk's
- **Type:** idea · **Severity:** — · **Status:** open
- **Source:** Richard, 2026-09-11 ("zoomování spektra"), Whole Milk Mac demo 170–171 s; the Windows build shows a `zoom 9.3x` / `zoom 11.7x` badge in the waterfall corner
- **Detail:** 20 fps frames of the Mac transition (local `~/Downloads/whole-milk/`)

In the demo a zoom from ~10 kHz to ~50 kHz completes in ~0.1–0.15 s: the waterfall is redrawn at the
new scale including its history, and the text lines re-arrange in a short animation into stacked
rows with fanned leaders (see SKM-15). Ours already keeps full-resolution history and recomposes on
zoom (5.4 ms for 192 k on 700×600): Ctrl+wheel = 1.25× per step from 2 kHz to the band, wheel pans,
the scale strip drags. Which part of theirs to take (transition, badge, gesture, the column following
the zoom) is settled with Richard before any code.

### SKM-17 — Highlight protocol keywords in the decoded text by colour / shade
- **Type:** idea · **Severity:** — · **Status:** open
- **Source:** Richard, 2026-09-11 ("zvýraznění klíčových slov jinou barvou/odstínem"), after the Whole Milk demo

Ours colours only validated callsigns (`scp` tag in the TCI spot colour, `scp-dup` gray once the
logbook has the station, `main.c` ~1985–2000) and shades uncommitted draft text gray. Wanted:
protocol words by role in their own colour or shade — calling (CQ / TEST / QRZ), DE, report
(5NN / RST), closing (TU / 73 / K / BK), the exchange — in the decode pane and in the column snippet
(SKM-15). Note from the demo, inferred from frames and not stated by its author: Whole Milk's
colours seem to encode decoder confidence rather than a word's role — tokens whose tooltip reads
`confidence 1.00` (CQ, 5NN, RDA) are white, the yellow / orange / red ones are mostly garbage
(`3YFLCSM`, `SU5H5HHHE`). Which classes and shades: at Richard's look.

### SKM-18 — Neural decoding, second attempt: bigger, faster models on GPU / NPU (CPU as baseline)
- **Type:** idea · **Severity:** — · **Status:** open (nothing decided)
- **Source:** Richard, 2026-09-11 ("zaměřil bych se znovu na tu neuronku… využití GPU, NPU, případně CPU, ale výkonnější a rychlejší modely"), after the Whole Milk demos
- **Detail:** SKM-3 (DeepCW evaluation and its licence gate); CLAUDE.md "Neural CW reader" (July prototype) and "LIVE VERDIKT run5" (2026-07-19)

Why this is not a rerun of July: the July reader was a ~310k-param dilated TCN + CTC over
**symbolic run durations**. It re-read what the classical demodulator had already decided, so it
inherited every demodulation error. It ran on the CPU in dependency-free C, and two trained
generations failed to be a net positive on a live band ("the AI comes OUT of decoding",
2026-07-19). The neural decoders shown to work today start from the **signal**:

- **DeepCW** (e04/deepcw-engine, AGPL-3.0) is a ~15 MB CTC CNN over a log-magnitude
  spectrogram: 3200 Hz audio, 256-point FFT, 400–1200 Hz → 65 bins (per AetherSDR RFC #4817, open
  since 2026-08-07).
- **Whole Milk's** general decoder, per its own UI tooltip, is `cw_ctc.onnx on trtrtx`: a CTC model
  in ONNX, run through ONNX Runtime's NVIDIA TensorRT-RTX execution provider and fed the whole band
  at 46.9 Hz bins. A second, dearer decoder is called "Whisper" (that is all that is known).
  The file name is **not** DeepCW's (that model ships as `model.onnx`), so it is presumably his own.

Hardware on the dev machine: an RTX 5070 (CUDA / TensorRT), an Intel NPU (Core Ultra 7 265,
`vpu_37xx`, ~13 TOPS; OpenVINO inference verified 2026-07-18; research 2026-08-25 found it no
faster than an iGPU, its value is a few watts and a free GPU) and the CPU as the baseline.

Questions, in order:

1. **Input.** Per-channel baseband or audio (DeepCW's shape) versus band-wide spectrogram tiles
   (Whole Milk's). The second maps onto the spectrum tap that already exists (`spectrum.c`,
   23.4 Hz bins, 93.75 rows/s).
2. **Model and licence.**
   - DeepCW: AGPL-3.0, gated as in SKM-3.
   - lucpaysan/CW-LAB: GPL-3.0, ONNX models of ~7 MB.
   - ag1le/LSTM_morse: MIT, TensorFlow LSTM.
   - Our own training, reusing the July synthetic-fist and consensus-harvest approach.
3. **Runtime.** ONNX Runtime with CUDA / TensorRT-RTX / OpenVINO providers as an *optional*
   dependency, with a CPU fallback (an NPU- or NVIDIA-only backend is useless to anyone else).
4. **Cost.** What one band-second costs on each device.
5. **Proof.** An offline A/B against v2 on recorded contest IQ with the replay harness.
   July's lesson: gains on synthetic CER did not survive a live band.

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
new log lines are meant to answer from his log. SKM-3 (DeepCW) was evaluated
on 2026-09-12: the published Conformer/CTC model reads two real IQ fixtures
(20 m, and a fresh 300 s 80 m contest recording,
`/var/tmp/skimmer-iq/iq-20260912-80m-cw-contest-192k.cf32`) at least as well
as v2 and better on weak QSO partners, at ~3 % of a core per active channel;
an engine switch + backend design is written up there and waits for
Richard's decision (licence: AGPL ↔ GPLv3 §13 both permit the combination).
