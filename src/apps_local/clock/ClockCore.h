#pragma once

// The clock app's model: civil-calendar arithmetic, the two counters, and the
// strings on the panel.
//
// Freestanding C++17 -- no Arduino, no RTC, no renderer -- so
// host-tests/clock builds it with a bare compiler. Everything here is a pure
// function of numbers the Activity hands in, which is the only reason a
// calendar can be tested at all: the alternative is trusting a leap-year rule
// nobody will exercise again until 2100.
//
// ---------------------------------------------------------------------------
// Three decisions worth stating.
//
// 1. THE ZONE IS THE HAL'S JOB, NOT THIS FILE'S. The RTC keeps UTC and
//    HalClock::localTime() applies a POSIX TZ rule through localtime_r, so
//    daylight saving is right year-round. This app used to shift the
//    timestamp itself by a fixed quarter-hour offset, which no DST rule fits;
//    upstream 1.13.9 made that unnecessary and the shift is gone. Everything
//    here takes an ALREADY-LOCAL Civil.
//
// 2. THE COUNTERS ARE MILLISECONDS, THE DISPLAY IS SECONDS. The stopwatch and
//    the timer hold elapsed milliseconds taken from millis(), so the reading
//    stays exact no matter how rarely the panel repaints. That split is the
//    whole reason a once-a-second e-ink panel can carry a stopwatch at all:
//    the paint rate is a display property and never a timekeeping one.
//
// 3. THE TIMER'S FIELDS WRAP, AND THERE IS NO MINUS BUTTON. Setting 25 minutes
//    from a pair of plus buttons is five taps of +5m; from a plus-and-minus
//    pair it is the same five taps plus four buttons on a panel that has room
//    for four, not eight. Each field wraps within itself (minutes mod 60,
//    seconds mod 60), so every value from 0:00 to 59:59 is reachable and the
//    ceiling the app advertises is the ceiling the arithmetic enforces.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>

namespace clockapp {

// The ceiling the timer advertises: 59 minutes 59 seconds.
inline constexpr uint32_t kTimerMaxSeconds = 59 * 60 + 59;

// A civil date and time, already shifted into the user's zone.
struct Civil {
  uint16_t year = 2000;
  uint8_t month = 1;    // 1-12
  uint8_t day = 1;      // 1-31
  uint8_t hour = 0;     // 0-23
  uint8_t minute = 0;   // 0-59
  uint8_t second = 0;   // 0-59
  uint8_t weekday = 0;  // 0=Sunday .. 6=Saturday
};

// --- Calendar arithmetic --------------------------------------------------

bool isLeapYear(uint16_t year);

// Days in `month` (1-12) of `year`. 0 for a month outside 1-12, so a caller
// handed a corrupt RTC read draws an empty grid rather than walking off one.
uint8_t daysInMonth(uint16_t year, uint8_t month);

// Weekday of `year-month-day`, 0=Sunday. Sakamoto's method, which is exact for
// any Gregorian date and is four lines rather than a table.
uint8_t weekdayOf(uint16_t year, uint8_t month, uint8_t day);

// Which column (0=Sunday) the 1st of this month falls in -- the number of
// blank cells before day 1 in a Sunday-first grid.
uint8_t firstColumnOf(uint16_t year, uint8_t month);

// How many week rows the month needs: 4, 5 or 6. A 31-day month that starts on
// a Saturday spans six, and a grid built for five clips its last two days. The
// layout asks rather than assuming, which is the only reason that month draws.
uint8_t weekRowsIn(uint16_t year, uint8_t month);

// --- The strings ----------------------------------------------------------

// "10:42 AM". 12-hour with the meridiem, because that is what the panel asks
// for; `out` needs 10 bytes.
const char* formatClock(const Civil& local, char* out, size_t size);

// "SATURDAY 20 SEPTEMBER". The line under the clock, which is the only place
// the weekday is spelled out -- the calendar header has one letter per column.
const char* formatDateLine(const Civil& local, char* out, size_t size);

// "SEPTEMBER 2026", the calendar's own heading.
const char* formatMonthTitle(const Civil& local, char* out, size_t size);

// Full month name, 1-12; "" outside that.
const char* monthName(uint8_t month);

// One-letter column heading for 0=Sunday .. 6=Saturday.
const char* weekdayInitial(uint8_t weekday);

// The stopwatch readout. Under an hour it is "MM:SS"; past one it grows a
// leading hour ("1:02:03") rather than rolling over silently, because a
// stopwatch that wraps at an hour is a stopwatch that lies. `out` needs 12
// bytes.
const char* formatStopwatch(uint32_t elapsedMs, char* out, size_t size);

// The timer readout, always "MM:SS" -- it cannot exceed 59:59 by
// construction. `out` needs 8 bytes.
const char* formatTimer(uint32_t remainingSeconds, char* out, size_t size);

// --- The timer's fields ---------------------------------------------------

// Adds `deltaMinutes` to the minutes field alone, wrapping within 0-59 and
// leaving the seconds where they are. Returns the new total in seconds.
uint32_t addTimerMinutes(uint32_t seconds, int deltaMinutes);

// The same for the seconds field.
uint32_t addTimerSeconds(uint32_t seconds, int deltaSeconds);

// --- The repaint cadence --------------------------------------------------
//
// This is the whole battery argument, in one function, so it can be tested
// rather than asserted.
//
// A whole-screen refresh on this panel drives it for roughly 300ms (see
// PaintClock.h: "0.3-2s", HALF_REFRESH is 1720ms), and the SDK exposes no
// windowed update -- every repaint is the entire 480x800 frame. Repainting
// once a second therefore means the panel is driving about a third of the time
// for as long as the app is open, on a device whose whole premise is lasting
// weeks. Repainting once a MINUTE costs about 0.5% of that.
//
// So the panel rate follows what is actually counting:
//
//   nothing running  -> once a MINUTE. The clock face is "10:42 AM"; there is
//                       nothing to show faster.
//   either counting  -> once a SECOND, for as long as it counts, and no
//                       longer.
//
// That rate is not scheduled, it is EARNED: the Activity polls cheaply and
// repaints only when a string it is showing would actually differ. Scheduling
// to a wall-clock boundary instead would need the sub-second phase, which an
// RTC reporting whole seconds cannot give -- the paint would land up to a
// second late and stay there. The change check has no phase to get wrong, and
// it collapses to exactly once a minute when idle because the clock string is
// the only thing changing and it changes once a minute.
//
// The expensive rate is bounded by the user deliberately timing something,
// which is the one case where they are looking at the screen anyway.
enum class Cadence : uint8_t {
  Minute,
  Second,
};

Cadence cadenceFor(bool stopwatchRunning, bool timerRunning);

// How often to LOOK, which is not how often to paint. A poll is an I2C read
// and three string compares; a paint is 300ms of panel. Polling four times a
// second while counting keeps a stopwatch's tick from landing up to a second
// late, and costs nothing that matters.
uint32_t pollIntervalMs(Cadence cadence);

}  // namespace clockapp
