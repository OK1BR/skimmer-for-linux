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
- **Type:** bug · **Severity:** medium (latent) · **Status:** open
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

### SKM-2 — GtkImage baseline warnings flood stderr, non-deterministically
- **Type:** bug · **Severity:** low · **Status:** open (diagnose first)
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

## Roadmap

Milestones and their order live in `docs/SCOPE.md`. Nothing in this backlog
blocks them: both open items are hygiene, and neither was noticed by the
operator during 5 hours of contest operation across two days.
