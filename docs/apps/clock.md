# Clock

The time, a stopwatch, a timer and the month, on one screen.

## The layout

Top to bottom: the time in 12-hour with the meridiem, the weekday and date
under it, then a rule; a STOPWATCH row and a TIMER row, each with its readout,
START/STOP and RESET; the timer's four adjust buttons; then a rule and the
current month.

The side keys work the stopwatch without looking at the panel -- **Up** starts
and stops it, **Down** resets it -- because that is the control a hand reaches
for while doing something else.

## What a repaint costs, and what this app does about it

**A whole-screen refresh on this panel drives it for roughly 300ms**
(`lib/GfxRenderer/PaintClock.h`: "0.3-2s"; `HALF_REFRESH` is 1720ms), and the
display SDK exposes **no windowed update** -- `HalDisplay` offers
`displayBuffer`, `displayBufferAsync` and `refreshDisplay`, all of them the
whole 480x800 frame. There is no way to repaint just the seconds.

So a clock that ticked every second would hold the panel driving about a third
of the time for as long as the app was open, on a device whose whole premise is
lasting weeks. It would also flicker continuously and drop taps against frames
nobody had seen yet, which is the failure `PaintClock.h` exists to describe.

**The panel rate therefore follows what is actually counting:**

| State | Panel repaints | Duty |
| --- | --- | --- |
| Nothing running | once a **minute** | ~0.5% |
| Stopwatch or timer running | once a **second** | ~30%, and only while it counts |

The expensive rate is bounded by you deliberately timing something, which is
the one case where you are looking at the screen anyway.

**That rate is earned, not scheduled.** `loop()` polls cheaply -- one I2C read
and three string compares, once a second idle and four times a second while
counting -- and calls `requestUpdate()` only when a string the panel is showing
would actually differ. Idle, the only thing that changes is the clock face, and
it changes once a minute, so the minute rate falls out exactly.

Scheduling to a wall-clock boundary instead would need the sub-second phase,
which an RTC reporting whole seconds cannot give: the paint would land up to a
second late and stay there. A change check has no phase to get wrong.

**The counters are milliseconds; only the display is seconds.** Both hold
elapsed `millis()`, so the reading is exact however rarely the panel catches
up. The paint rate is a display property here and never a timekeeping one --
that is what makes a once-a-second e-ink stopwatch honest rather than
approximate.

`preventAutoSleep()` returns true only while something counts, because a timer
that slept through its own deadline would not fire.

## The timer

Settable from **0:00 to 59:59**, from four buttons: `+5m`, `+1m`, `+10s`,
`+1s`.

**Plus only, and each field wraps within itself** -- minutes mod 60, seconds
mod 60, with no carry between them. A plus-and-minus pair would be eight
buttons on a row with room for four, and reaching 25 minutes costs the same
five taps either way. Wrapping makes every value from 0:00 to 59:59 reachable,
and the ceiling the app advertises is the ceiling the arithmetic enforces.

The adjust buttons grey out while it counts: changing the target mid-run would
leave the remaining time and the target disagreeing, and there is no reading of
that anyone would predict.

- **STOP** banks what is left as the new target, so START resumes rather than
  restarting. A pause that silently reloaded the original would be a reset
  wearing a different word.
- **When it ends** the readout holds at `00:00` until you touch something. The
  target is untouched, so START runs the same interval again.
- The target survives leaving the app, in `/.crosspoint/clock/settings.txt`.

## The alarm

**The screen flashes black and white three times** -- six whole-panel refreshes,
under two seconds.

It works by inverting the *driver's output polarity* (`HalDisplay::setInverted`)
rather than the framebuffer, so the whole panel flips without redrawing
anything. On a device with no speaker and no buzzer
(`FREEINK_CAP_AUDIO` names Murphy and M5; this board has neither), a full-frame
black is the loudest thing available.

The flash runs inside `render()`, which already holds the `RenderLock` --
inverting from `loop()` would race the render task for the framebuffer.
`onExit()` clears the inversion, so a crash mid-flash cannot hand the next
activity an inverted screen.

## The calendar, and the two things it gets right

**Six-row months.** August 2026 is 31 days starting on a Saturday and spans six
week rows; a grid built for five clips the 30th and 31st off the bottom. The
layout asks `weekRowsIn()` rather than assuming, and `host-tests/ui` renders
three months and fails if any day is missing. Forcing the row count to five
fails that test with exactly two missing days.

**The RTC holds UTC and the panel shows local.** `HalClock::begin()` says so
and the NTP path writes it that way, so the offset (`SETTINGS.clockUtcOffsetQ`)
is applied to the *whole timestamp*, not just the hours. Shifting only the
clock face would put the right time above a calendar highlighting the wrong
day -- for five and a half hours out of every day in Bengaluru, and across a
month or year boundary at the turn of one.

`host-tests/clock` checks the shift both ways across midnight, across a month
end and across a year end, and checks Sakamoto's weekday against 1900 (not a
leap year) and 2000 (one).

## When the clock has never been set

An RTC that was never set reads as the year 2000. Rather than draw a confident
`12:00 AM` under a January 2000 calendar, the face reads `--:--` and the line
under it says **SET THE CLOCK IN SETTINGS**.

The stopwatch and the timer still work: they come from `millis()`, not from the
RTC, so a device with a dead clock is still a usable stopwatch.

## Two font passes

The panel needs four sizes and there are three font slots, so the builder
splits the way Connections splits its chrome from its tiles: the first pass
draws everything at the bound faces and hands back the rect reserved for the
time, the Activity rebinds the title slot to the large cut, and the second pass
draws the time into it. Rebinding a slot is one assignment on the target.
