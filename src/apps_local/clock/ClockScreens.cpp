#include "ClockScreens.h"

#include <cstdio>

#include "../ui/ToyboxText.h"

namespace clockui {
namespace {

constexpr int kBodyTop = toybox::kBodyTop;

fui::TextStyle plain(const fui::FontId font, const fui::TextAlign align = fui::TextAlign::Left,
                     const fui::Color color = fui::Color::Black, const uint8_t maxLines = 1) {
  fui::TextStyle style;
  style.font = font;
  style.align = align;
  style.color = color;
  style.maxLines = maxLines;
  return style;
}

void chrome(toybox::Screen& screen) {
  fui::HeaderProps header;
  header.title = "CLOCK";
  header.borderEdges = fui::EdgesNone;
  toybox::absoluteChrome(screen);
  toybox::headerBand(screen, header);
  screen.insetContent(fui::Insets{toybox::kGutter * 3, toybox::kMargin, toybox::kMargin, toybox::kMargin});
}

// One counter: a name, a readout, and its two controls. Both rows are built
// from this so the stopwatch and the timer cannot drift apart in a way only a
// screenshot would show.
//
// The two text lines are given REAL heights rather than halves of the row. The
// readout is set in the display cut, whose ink is taller than half a 58px row,
// so halving it put the timer's digits through the label underneath -- visible
// in a render and in nothing else.
void counterRow(toybox::Screen& screen, const int16_t y, const char* name, const char* readout, const bool running,
                const fui::ActionId toggleAction, const fui::ActionId resetAction) {
  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t gutter = static_cast<int16_t>(toybox::kGutter);

  const int16_t nameH = screen.target().lineHeight(toybox::kTileFont);
  const int16_t readoutH = screen.target().lineHeight(toybox::kDisplayFont);

  // The two controls are the fixed part and the readout takes what is left, so
  // a stopwatch that grows an hours field pushes nothing off the row.
  const int16_t buttonW = 112;
  const int16_t labelW = static_cast<int16_t>(width - 2 * (buttonW + gutter));

  screen.target().text(fui::makeRect(toybox::kMargin, y, labelW, nameH), name,
                       plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray));
  screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(y + nameH), labelW, readoutH), readout,
                       plain(toybox::kDisplayFont, fui::TextAlign::Left));

  // The buttons span both text lines and are centred against them.
  const int16_t rowH = static_cast<int16_t>(nameH + readoutH);
  const int16_t buttonH = static_cast<int16_t>(rowH - 8);
  const int16_t buttonY = static_cast<int16_t>(y + (rowH - buttonH) / 2);

  // START and STOP rather than a glyph: this row has room for the word, and a
  // running counter is the one state on the panel worth naming outright. The
  // label is set in the BUTTON cut (the small slot here), not the UI cut a
  // button defaults to -- at the UI cut "START" clipped to "STA..." inside a
  // box wide enough to hold it at the size a button is supposed to use.
  fui::TextStyle face = plain(toybox::kTileFont, fui::TextAlign::Center);

  fui::ButtonProps toggle;
  toggle.label = running ? "STOP" : "START";
  toggle.action = toggleAction;
  toggle.text = face;
  if (!running) toggle.styles = toybox::rowStyles();
  screen.button(toggle,
                fui::makeRect(static_cast<int16_t>(toybox::kMargin + labelW + gutter), buttonY, buttonW, buttonH));

  fui::ButtonProps reset;
  reset.label = "RESET";
  // Live even at zero, where it is a no-op. Disabling it would UNREGISTER it
  // (fui::button only takes a hit rect when enabled), so a control that looked
  // merely greyed would be a hole in the table -- and the row beside it would
  // have to move to say so, which on e-ink is a whole repaint to communicate
  // nothing.
  reset.action = resetAction;
  reset.text = face;
  reset.styles = toybox::rowStyles();
  screen.button(reset, fui::makeRect(static_cast<int16_t>(toybox::kMargin + labelW + buttonW + 2 * gutter), buttonY,
                                     buttonW, buttonH));
}

// How tall counterRow() draws, so the caller can stack two of them without
// either knowing the other's internals.
int16_t counterRowHeight(toybox::Screen& screen) {
  return static_cast<int16_t>(screen.target().lineHeight(toybox::kTileFont) +
                              screen.target().lineHeight(toybox::kDisplayFont));
}

// The timer's four adjust buttons. Plus only, each wrapping its own field --
// see ClockCore.h for why there is no minus.
void adjustRow(toybox::Screen& screen, const int16_t y, const int16_t rowH, const bool enabled) {
  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t gutter = static_cast<int16_t>(toybox::kGutter);
  const int16_t cell = static_cast<int16_t>((width - 3 * gutter) / 4);

  struct Step {
    const char* label;
    fui::ActionId action;
  };
  const Step steps[4] = {
      {"+5m", ActionTimerPlus5m},
      {"+1m", ActionTimerPlus1m},
      {"+10s", ActionTimerPlus10s},
      {"+1s", ActionTimerPlus1s},
  };

  for (int i = 0; i < 4; ++i) {
    fui::ButtonProps button;
    button.label = steps[i].label;
    button.action = steps[i].action;
    button.text = plain(toybox::kTileFont, fui::TextAlign::Center);
    button.styles = toybox::rowStyles();
    // Greyed while the timer counts: changing the target mid-run would mean
    // the remaining time and the target disagree, and there is no reading of
    // that a user would predict.
    button.enabled = enabled;
    const int16_t x = static_cast<int16_t>(toybox::kMargin + i * (cell + gutter));
    // The last cell absorbs the rounding so the row ends flush with the margin.
    const int16_t w = i == 3 ? static_cast<int16_t>(width - 3 * (cell + gutter)) : cell;
    screen.button(button, fui::makeRect(x, y, w, rowH));
  }
}

// The month. A fixed seven columns and as many rows as the month asks for --
// August 2026 needs six, and a grid built for five clips its last two days.
void calendar(toybox::Screen& screen, const ClockModel& model, const int16_t top, const int16_t bottom) {
  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t titleH = screen.target().lineHeight(toybox::kDisplayFont);
  const int16_t headH = screen.target().lineHeight(toybox::kTileFont);

  screen.target().text(fui::makeRect(toybox::kMargin, top, width, titleH), model.monthTitle,
                       plain(toybox::kDisplayFont, fui::TextAlign::Center));

  const int16_t gridTop = static_cast<int16_t>(top + titleH + toybox::kGutter / 2);
  const int16_t column = static_cast<int16_t>(width / 7);
  // Centred on the real column span rather than the margin, so the seven
  // columns and the heading above them share one grid however the division
  // rounds.
  const int16_t left = static_cast<int16_t>(toybox::kMargin + (width - column * 7) / 2);

  static constexpr const char* kInitials[7] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int i = 0; i < 7; ++i) {
    screen.target().text(fui::makeRect(static_cast<int16_t>(left + i * column), gridTop, column, headH), kInitials[i],
                         plain(toybox::kTileFont, fui::TextAlign::Center, fui::Color::DarkGray));
  }

  const int16_t cellsTop = static_cast<int16_t>(gridTop + headH + 4);
  const int16_t rows = model.weekRows == 0 ? 1 : model.weekRows;
  const int16_t available = static_cast<int16_t>(bottom - cellsTop);
  const int16_t cellH = static_cast<int16_t>(available / rows);
  if (cellH <= 0) return;

  // Sized for any unsigned rather than for 1-31: host-tests/fmtwidth sizes a
  // buffer from what its format CAN print, not from what the caller happens
  // to pass, and it is right to -- the day bound lives in another function.
  char label[12];
  for (uint8_t day = 1; day <= model.daysInMonth; ++day) {
    const int index = model.firstColumn + day - 1;
    const int16_t col = static_cast<int16_t>(index % 7);
    const int16_t row = static_cast<int16_t>(index / 7);
    if (row >= rows) break;
    const fui::Rect cell = fui::makeRect(static_cast<int16_t>(left + col * column),
                                         static_cast<int16_t>(cellsTop + row * cellH), column, cellH);
    std::snprintf(label, sizeof(label), "%u", static_cast<unsigned>(day));

    if (day == model.today) {
      // Today is filled, not ringed. A ring on a 1-bit panel at this cell size
      // is two pixels of outline that the panel's own ghosting eats; a filled
      // block survives a fast refresh and reads from arm's length.
      screen.target().fill(cell, fui::Paint::solid(fui::Color::Black));
      screen.target().text(cell, label, plain(toybox::kTileFont, fui::TextAlign::Center, fui::Color::White));
    } else {
      screen.target().text(cell, label, plain(toybox::kTileFont, fui::TextAlign::Center));
    }
  }
}

}  // namespace

ClockLayout buildClockScreen(toybox::Screen& screen, const ClockModel& model) {
  chrome(screen);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t gutter = static_cast<int16_t>(toybox::kGutter);

  ClockLayout layout;

  // --- The time, and the date under it ------------------------------------
  // The rect is reserved and handed back; the second pass fills it once the
  // title slot carries the large cut.
  int16_t y = static_cast<int16_t>(kBodyTop);
  const int16_t timeH = 76;
  layout.timeRect = fui::makeRect(toybox::kMargin, y, width, timeH);
  y = static_cast<int16_t>(y + timeH);

  const int16_t dateH = screen.target().lineHeight(toybox::kTileFont);
  screen.target().text(fui::makeRect(toybox::kMargin, y, width, dateH),
                       model.clockValid ? model.dateLine : "SET THE CLOCK IN SETTINGS",
                       plain(toybox::kTileFont, fui::TextAlign::Center, fui::Color::DarkGray));
  y = static_cast<int16_t>(y + dateH + gutter);

  screen.target().fill(fui::makeRect(toybox::kMargin, y, width, toybox::kRule), fui::Paint::solid(fui::Color::Black));
  y = static_cast<int16_t>(y + toybox::kRule + gutter);

  // --- The two counters ---------------------------------------------------
  const int16_t rowH = counterRowHeight(screen);
  counterRow(screen, y, "STOPWATCH", model.stopwatch, model.stopwatchRunning, ActionStopwatchToggle,
             ActionStopwatchReset);
  y = static_cast<int16_t>(y + rowH + gutter);

  counterRow(screen, y, "TIMER", model.timer, model.timerRunning, ActionTimerToggle, ActionTimerReset);
  // Half a gutter: the adjust row belongs to the timer above it, and the wider
  // gap below separates the pair from the calendar.
  y = static_cast<int16_t>(y + rowH + gutter / 2);

  const int16_t adjustH = 48;
  adjustRow(screen, y, adjustH, !model.timerRunning);
  y = static_cast<int16_t>(y + adjustH + gutter);

  screen.target().fill(fui::makeRect(toybox::kMargin, y, width, toybox::kRule), fui::Paint::solid(fui::Color::Black));
  y = static_cast<int16_t>(y + toybox::kRule + gutter);

  // --- The month ----------------------------------------------------------
  calendar(screen, model, y, static_cast<int16_t>(device.height - toybox::kMargin));

  return layout;
}

void buildClockFace(toybox::Screen& screen, const ClockModel& model, const ClockLayout& layout) {
  // "--:--" rather than a plausible wrong time. An RTC that was never set
  // reads as 2000-01-01, and a clock confidently showing 12:00 AM is worse
  // than one admitting it does not know.
  screen.target().text(layout.timeRect, model.clockValid ? model.time : "--:--",
                       plain(toybox::kDisplayFont, fui::TextAlign::Center));
}

}  // namespace clockui
