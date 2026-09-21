#include "ClockCore.h"

#include <cstdio>

namespace clockapp {
namespace {

constexpr const char* kMonths[] = {"JANUARY", "FEBRUARY", "MARCH",     "APRIL",   "MAY",      "JUNE",
                                   "JULY",    "AUGUST",   "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};

constexpr const char* kWeekdays[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};

constexpr const char* kInitials[] = {"S", "M", "T", "W", "T", "F", "S"};

// Sakamoto's table: the day-of-week offset of the 1st of each month.
constexpr int kSakamoto[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

}  // namespace

// --- Calendar arithmetic --------------------------------------------------

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      return 31;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    case 2:
      return isLeapYear(year) ? 29 : 28;
    default:
      // A month outside 1-12 is a corrupt read, not a date. Zero draws an
      // empty grid; anything else indexes a table with it.
      return 0;
  }
}

uint8_t weekdayOf(uint16_t year, const uint8_t month, const uint8_t day) {
  if (month < 1 || month > 12) return 0;
  if (month < 3) year -= 1;
  const int result = (year + year / 4 - year / 100 + year / 400 + kSakamoto[month - 1] + day) % 7;
  return static_cast<uint8_t>(result);
}

uint8_t firstColumnOf(const uint16_t year, const uint8_t month) { return weekdayOf(year, month, 1); }

uint8_t weekRowsIn(const uint16_t year, const uint8_t month) {
  const uint8_t days = daysInMonth(year, month);
  if (days == 0) return 0;
  const uint8_t lead = firstColumnOf(year, month);
  // Ceiling division: the lead blanks and the days together, in rows of seven.
  return static_cast<uint8_t>((lead + days + 6) / 7);
}

// --- The strings ----------------------------------------------------------

const char* monthName(const uint8_t month) { return (month >= 1 && month <= 12) ? kMonths[month - 1] : ""; }

const char* weekdayInitial(const uint8_t weekday) { return weekday < 7 ? kInitials[weekday] : ""; }

const char* formatClock(const Civil& local, char* out, const size_t size) {
  const bool pm = local.hour >= 12;
  int hour12 = local.hour % 12;
  if (hour12 == 0) hour12 = 12;
  std::snprintf(out, size, "%d:%02d %s", hour12, local.minute, pm ? "PM" : "AM");
  return out;
}

const char* formatDateLine(const Civil& local, char* out, const size_t size) {
  const char* weekday = local.weekday < 7 ? kWeekdays[local.weekday] : "";
  std::snprintf(out, size, "%s %d %s", weekday, local.day, monthName(local.month));
  return out;
}

const char* formatMonthTitle(const Civil& local, char* out, const size_t size) {
  std::snprintf(out, size, "%s %u", monthName(local.month), static_cast<unsigned>(local.year));
  return out;
}

const char* formatStopwatch(const uint32_t elapsedMs, char* out, const size_t size) {
  const uint32_t total = elapsedMs / 1000u;
  const uint32_t hours = total / 3600u;
  const uint32_t minutes = (total % 3600u) / 60u;
  const uint32_t seconds = total % 60u;
  if (hours > 0) {
    std::snprintf(out, size, "%u:%02u:%02u", static_cast<unsigned>(hours), static_cast<unsigned>(minutes),
                  static_cast<unsigned>(seconds));
  } else {
    std::snprintf(out, size, "%02u:%02u", static_cast<unsigned>(minutes), static_cast<unsigned>(seconds));
  }
  return out;
}

const char* formatTimer(const uint32_t remainingSeconds, char* out, const size_t size) {
  const uint32_t clamped = remainingSeconds > kTimerMaxSeconds ? kTimerMaxSeconds : remainingSeconds;
  std::snprintf(out, size, "%02u:%02u", static_cast<unsigned>(clamped / 60u), static_cast<unsigned>(clamped % 60u));
  return out;
}

// --- The timer's fields ---------------------------------------------------

uint32_t addTimerMinutes(const uint32_t seconds, const int deltaMinutes) {
  const uint32_t clamped = seconds > kTimerMaxSeconds ? kTimerMaxSeconds : seconds;
  const int minutes = static_cast<int>(clamped / 60u);
  const int rest = static_cast<int>(clamped % 60u);
  // Add 60 before the modulo so a negative delta wraps up rather than landing
  // on a negative minute the format would print as a huge number.
  const int wrapped = ((minutes + deltaMinutes) % 60 + 60) % 60;
  return static_cast<uint32_t>(wrapped * 60 + rest);
}

uint32_t addTimerSeconds(const uint32_t seconds, const int deltaSeconds) {
  const uint32_t clamped = seconds > kTimerMaxSeconds ? kTimerMaxSeconds : seconds;
  const int minutes = static_cast<int>(clamped / 60u);
  const int rest = static_cast<int>(clamped % 60u);
  const int wrapped = ((rest + deltaSeconds) % 60 + 60) % 60;
  return static_cast<uint32_t>(minutes * 60 + wrapped);
}

// --- The repaint cadence --------------------------------------------------

Cadence cadenceFor(const bool stopwatchRunning, const bool timerRunning) {
  return (stopwatchRunning || timerRunning) ? Cadence::Second : Cadence::Minute;
}

uint32_t pollIntervalMs(const Cadence cadence) { return cadence == Cadence::Second ? 250u : 1000u; }

}  // namespace clockapp
