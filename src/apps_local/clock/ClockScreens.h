#pragma once

// The clock app's screen. One view: the time, the two counters, and the month.
//
// ---------------------------------------------------------------------------
// Why this is built in two passes.
//
// The panel has three font slots and this screen needs four sizes: a clock
// readable from across a desk, a 30px cut for the band and the two readouts, a
// 20px cut for labels and buttons, and a dense cut for forty-two calendar
// cells. Rebinding a slot is one assignment on the target (Connections does
// the same thing between its chrome and its tiles), so the builder splits: the
// first pass draws everything at the bound faces and hands back the rect it
// reserved for the time, the Activity rebinds the title slot to the large cut,
// and the second pass draws the time into that rect.
//
// A freestanding builder cannot ask for the rebind itself, which is why the
// rect comes back out rather than staying private.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "../ui/ToyboxScreen.h"

namespace clockui {

namespace fui = freeink::ui;

// Chess 1-4, link 200s, Hacker News 300s, Instapaper 320s, Notes 340s,
// Weather 360s, Remote 380s. The clock takes the 400s.
enum : fui::ActionId {
  ActionStopwatchToggle = 400,
  ActionStopwatchReset = 401,
  ActionTimerToggle = 402,
  ActionTimerReset = 403,
  ActionTimerPlus5m = 404,
  ActionTimerPlus1m = 405,
  ActionTimerPlus10s = 406,
  ActionTimerPlus1s = 407,
};

struct ClockModel {
  // "10:42 AM" and "SUNDAY 20 SEPTEMBER". Empty when the RTC has never been
  // set, which is a state the screen says out loud rather than drawing a
  // plausible wrong time.
  const char* time = "";
  const char* dateLine = "";
  bool clockValid = false;

  const char* stopwatch = "00:00";
  bool stopwatchRunning = false;

  const char* timer = "00:00";
  bool timerRunning = false;

  // "SEPTEMBER 2026", and the grid that goes under it.
  const char* monthTitle = "";
  uint8_t firstColumn = 0;  // blank cells before day 1, 0=Sunday column
  uint8_t daysInMonth = 0;
  uint8_t weekRows = 0;
  uint8_t today = 0;  // 1-31, or 0 when this is not the current month
};

// The rect the big time goes in, handed back so the Activity can rebind the
// title slot before the second pass fills it.
struct ClockLayout {
  fui::Rect timeRect{};
};

// Pass one: chrome, date, counters, calendar. Everything but the time itself.
ClockLayout buildClockScreen(toybox::Screen& screen, const ClockModel& model);

// Pass two: the time, in whatever the title slot is bound to now.
void buildClockFace(toybox::Screen& screen, const ClockModel& model, const ClockLayout& layout);

}  // namespace clockui
