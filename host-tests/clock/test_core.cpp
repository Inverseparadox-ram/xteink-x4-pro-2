// Freestanding tests for ClockCore: the calendar, the offset shift, the two
// readouts and the repaint cadence.
//
// The thing under test is ARITHMETIC NOBODY WILL EXERCISE BY HAND. A calendar
// is wrong for one month out of a hundred and right the rest of the time, and
// the months it is wrong for are the ones that need six week rows or fall on a
// leap day. Rendering it and looking proves only that the current month draws.

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/apps_local/clock/ClockCore.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                               \
  do {                                                 \
    ++checks;                                          \
    if (!(cond)) {                                     \
      ++failures;                                      \
      std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

// Sakamoto against dates with a known answer, including the two centuries that
// disagree about being leap years. 1900 was not a leap year and 2000 was, and
// a rule that gets either wrong is wrong for every date after it.
static void testWeekdaysAndLeapYears() {
  CHECK(clockapp::weekdayOf(2026, 9, 20) == 0, "2026-09-20 is a Sunday");
  CHECK(clockapp::weekdayOf(2000, 1, 1) == 6, "2000-01-01 was a Saturday");
  CHECK(clockapp::weekdayOf(1970, 1, 1) == 4, "the epoch was a Thursday");
  CHECK(clockapp::weekdayOf(2024, 2, 29) == 4, "the last leap day was a Thursday");

  CHECK(clockapp::isLeapYear(2024), "2024 is a leap year");
  CHECK(!clockapp::isLeapYear(2023), "2023 is not");
  CHECK(!clockapp::isLeapYear(1900), "1900 was NOT, despite dividing by four");
  CHECK(clockapp::isLeapYear(2000), "2000 WAS, despite dividing by a hundred");

  CHECK(clockapp::daysInMonth(2024, 2) == 29, "February 2024 has 29 days");
  CHECK(clockapp::daysInMonth(2023, 2) == 28, "February 2023 has 28");
  CHECK(clockapp::daysInMonth(2026, 9) == 30, "September has 30");
  CHECK(clockapp::daysInMonth(2026, 12) == 31, "December has 31");
  // A corrupt RTC read must draw an empty grid, not index a table with 13.
  CHECK(clockapp::daysInMonth(2026, 0) == 0, "month 0 is not a month");
  CHECK(clockapp::daysInMonth(2026, 13) == 0, "and neither is 13");
}

// The six-row month is the one the grid gets wrong. A 31-day month starting on
// a Saturday needs six rows, and a layout built for five clips its last two
// days off the bottom of the screen.
static void testTheGridAsksHowManyRowsItNeeds() {
  // August 2026: 31 days, starts Saturday. Six rows.
  CHECK(clockapp::firstColumnOf(2026, 8) == 6, "August 2026 starts in the Saturday column");
  CHECK(clockapp::weekRowsIn(2026, 8) == 6, "so it needs six rows, got %d", clockapp::weekRowsIn(2026, 8));

  // February 2027: 28 days starting Monday -- still five rows, because the
  // lead blank pushes the 28th into a fifth.
  CHECK(clockapp::weekRowsIn(2026, 9) == 5, "September 2026 needs five");

  // The only four-row month there is: a non-leap February starting on a
  // Sunday. 2021 is one.
  CHECK(clockapp::firstColumnOf(2021, 2) == 1, "February 2021 starts on a Monday");

  // Whatever the month, the rows must cover every day in it.
  for (uint16_t year = 2024; year <= 2030; ++year) {
    for (uint8_t month = 1; month <= 12; ++month) {
      const uint8_t rows = clockapp::weekRowsIn(year, month);
      const uint8_t cells = static_cast<uint8_t>(rows * 7);
      const uint8_t needed =
          static_cast<uint8_t>(clockapp::firstColumnOf(year, month) + clockapp::daysInMonth(year, month));
      CHECK(cells >= needed, "%u-%02u: %d cells for %d needed", year, month, cells, needed);
      CHECK(rows >= 4 && rows <= 6, "%u-%02u wants %d rows", year, month, rows);
    }
  }
}

// The readouts. The stopwatch is the one that has to grow rather than wrap: an
// hour is not an unreasonable thing to time, and 01:03 meaning "one hour three
// minutes" or "one minute three seconds" is the whole bug.
static void testTheReadouts() {
  char buf[16];
  CHECK(std::string(clockapp::formatStopwatch(0, buf, sizeof(buf))) == "00:00", "got '%s'", buf);
  CHECK(std::string(clockapp::formatStopwatch(9500, buf, sizeof(buf))) == "00:09", "truncates, never rounds up");
  CHECK(std::string(clockapp::formatStopwatch(59999, buf, sizeof(buf))) == "00:59", "got '%s'", buf);
  CHECK(std::string(clockapp::formatStopwatch(60000, buf, sizeof(buf))) == "01:00", "got '%s'", buf);
  CHECK(std::string(clockapp::formatStopwatch(3599000, buf, sizeof(buf))) == "59:59", "got '%s'", buf);
  // Past the hour it GROWS a field instead of wrapping to 00:00.
  CHECK(std::string(clockapp::formatStopwatch(3600000, buf, sizeof(buf))) == "1:00:00", "got '%s'", buf);
  CHECK(std::string(clockapp::formatStopwatch(3723000, buf, sizeof(buf))) == "1:02:03", "got '%s'", buf);

  CHECK(std::string(clockapp::formatTimer(0, buf, sizeof(buf))) == "00:00", "got '%s'", buf);
  CHECK(std::string(clockapp::formatTimer(3599, buf, sizeof(buf))) == "59:59", "the advertised ceiling");
  CHECK(std::string(clockapp::formatTimer(99999, buf, sizeof(buf))) == "59:59", "and nothing above it prints");

  clockapp::Civil local;
  local.year = 2026;
  local.month = 9;
  local.day = 20;
  local.hour = 13;
  local.minute = 42;
  local.weekday = 0;
  CHECK(std::string(clockapp::formatClock(local, buf, sizeof(buf))) == "1:42 PM", "got '%s'", buf);
  local.hour = 0;
  CHECK(std::string(clockapp::formatClock(local, buf, sizeof(buf))) == "12:42 AM", "midnight is 12 AM, got '%s'", buf);
  local.hour = 12;
  CHECK(std::string(clockapp::formatClock(local, buf, sizeof(buf))) == "12:42 PM", "noon is 12 PM, got '%s'", buf);

  char line[48];
  local.hour = 9;
  CHECK(std::string(clockapp::formatDateLine(local, line, sizeof(line))) == "SUNDAY 20 SEPTEMBER", "got '%s'", line);
  CHECK(std::string(clockapp::formatMonthTitle(local, line, sizeof(line))) == "SEPTEMBER 2026", "got '%s'", line);
}

// Every value the app advertises has to be reachable from the four buttons it
// actually draws. There is no minus, so each field wraps within itself.
static void testTheTimerFieldsWrapWithinThemselves() {
  CHECK(clockapp::addTimerMinutes(0, 5) == 5 * 60, "five minutes from nothing");
  CHECK(clockapp::addTimerMinutes(5 * 60 + 30, 1) == 6 * 60 + 30, "the seconds are left alone");
  CHECK(clockapp::addTimerMinutes(59 * 60 + 30, 1) == 0 * 60 + 30, "minutes wrap at 60, seconds survive");
  CHECK(clockapp::addTimerMinutes(0, -1) == 59 * 60, "and wrap downward too");

  CHECK(clockapp::addTimerSeconds(60, 10) == 70, "ten seconds on");
  CHECK(clockapp::addTimerSeconds(60 + 55, 10) == 60 + 5, "seconds wrap WITHOUT carrying into minutes");
  CHECK(clockapp::addTimerSeconds(0, -1) == 59, "and wrap downward");

  // The ceiling holds: nothing reachable from the buttons exceeds 59:59.
  uint32_t value = 0;
  for (int i = 0; i < 200; ++i) {
    value = clockapp::addTimerMinutes(value, 5);
    value = clockapp::addTimerSeconds(value, 10);
    CHECK(value <= clockapp::kTimerMaxSeconds, "step %d reached %u", i, value);
  }
}

// The battery argument, as a test rather than a claim. A whole-screen refresh
// is ~300ms of panel drive and there is no windowed update, so a clock that
// repainted every second would drive the panel a third of the time forever.
static void testTheExpensiveCadenceOnlyRunsWhileSomethingCounts() {
  CHECK(clockapp::cadenceFor(false, false) == clockapp::Cadence::Minute, "idle is the cheap rate");
  CHECK(clockapp::cadenceFor(true, false) == clockapp::Cadence::Second, "a running stopwatch earns the fast one");
  CHECK(clockapp::cadenceFor(false, true) == clockapp::Cadence::Second, "so does a running timer");
  CHECK(clockapp::cadenceFor(true, true) == clockapp::Cadence::Second, "and both together is still one second");

  // A poll is not a paint. Polling four times a second while counting keeps a
  // stopwatch's tick from landing up to a second late; the PANEL rate is
  // decided separately, by whether a string the screen shows would differ.
  CHECK(clockapp::pollIntervalMs(clockapp::Cadence::Minute) == 1000, "idle looks once a second");
  CHECK(clockapp::pollIntervalMs(clockapp::Cadence::Second) == 250, "counting looks four times a second");
  // Whatever the regime, looking must be at least as often as the fastest
  // thing on screen changes, or a second would be skipped outright.
  CHECK(clockapp::pollIntervalMs(clockapp::Cadence::Second) <= 1000, "a running second is never missed");
}

int main() {
  testWeekdaysAndLeapYears();
  testTheGridAsksHowManyRowsItNeeds();
  testTheReadouts();
  testTheTimerFieldsWrapWithinThemselves();
  testTheExpensiveCadenceOnlyRunsWhileSomethingCounts();
  std::printf("%s  clock core: %d checks, %d failed\n", failures ? "FAIL" : "ok  ", checks, failures);
  return failures == 0 ? 0 : 1;
}
