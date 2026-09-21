#include "ClockActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "../../components/UITheme.h"
#include "../Shelf.h"
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxTheme.h"

namespace fui = freeink::ui;

namespace {
constexpr const char* kTag = "CLOCK";
constexpr const char* kDir = "/.crosspoint/clock";
constexpr const char* kSettings = "/.crosspoint/clock/settings.txt";

// How many black/white inversions the timer fires on. Each one is two
// whole-panel refreshes at roughly 300ms, so three cycles is under two seconds
// -- long enough to catch from across a room, short enough that the panel is
// not still flashing when you pick the device up.
constexpr int kFlashCycles = 3;
}  // namespace

std::unique_ptr<Activity> ClockActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return makeUniqueNoThrow<ClockActivity>(renderer, mappedInput);
}

// --- Settings ------------------------------------------------------------

void ClockActivity::loadSettings() {
  HalFile file;
  if (!Storage.openFileForRead(kTag, kSettings, file)) return;
  char buffer[32] = {};
  const int n = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer) - 1);
  file.close();
  if (n <= 0) return;
  unsigned seconds = 0;
  if (std::sscanf(buffer, "timer=%u", &seconds) == 1) {
    timerTargetSeconds_ = seconds > clockapp::kTimerMaxSeconds ? clockapp::kTimerMaxSeconds : seconds;
  }
}

void ClockActivity::saveSettings() {
  Storage.ensureDirectoryExists(kDir);
  char text[32];
  const int n = std::snprintf(text, sizeof(text), "timer=%u\n", static_cast<unsigned>(timerTargetSeconds_));
  if (n <= 0) return;
  HalFile file;
  if (!Storage.openFileForWrite(kTag, kSettings, file)) return;
  file.write(reinterpret_cast<const uint8_t*>(text), static_cast<size_t>(n));
  file.close();
}

// --- The counters --------------------------------------------------------

uint32_t ClockActivity::stopwatchElapsedMs() const {
  if (!stopwatchRunning_) return stopwatchAccumulatedMs_;
  // Unsigned subtraction, so the 49-day millis() rollover costs one wrong
  // reading rather than a negative elapsed time printed as four billion.
  return stopwatchAccumulatedMs_ + (millis() - stopwatchStartedAtMs_);
}

uint32_t ClockActivity::timerRemainingSeconds() const {
  if (timerFired_) return 0;
  if (!timerRunning_) return timerTargetSeconds_;
  const uint32_t now = millis();
  if (static_cast<int32_t>(timerDeadlineMs_ - now) <= 0) return 0;
  // Round UP, so a timer set to 1:00 shows 1:00 for the first instant rather
  // than 0:59. A countdown that skips its own start value reads as broken.
  return (timerDeadlineMs_ - now + 999u) / 1000u;
}

void ClockActivity::toggleStopwatch() {
  RenderLock lock(*this);
  if (stopwatchRunning_) {
    stopwatchAccumulatedMs_ += millis() - stopwatchStartedAtMs_;
    stopwatchRunning_ = false;
  } else {
    stopwatchStartedAtMs_ = millis();
    stopwatchRunning_ = true;
  }
  nextPollAtMs_ = millis();
  requestUpdate();
}

void ClockActivity::resetStopwatch() {
  RenderLock lock(*this);
  stopwatchRunning_ = false;
  stopwatchAccumulatedMs_ = 0;
  stopwatchStartedAtMs_ = 0;
  nextPollAtMs_ = millis();
  requestUpdate();
}

void ClockActivity::toggleTimer() {
  if (timerRunning_) {
    // Stopping banks what is left as the new target, so START resumes rather
    // than restarting. A pause that silently reloaded the original target
    // would be a reset wearing a different word.
    const uint32_t left = timerRemainingSeconds();
    RenderLock lock(*this);
    timerRunning_ = false;
    timerTargetSeconds_ = left;
    nextPollAtMs_ = millis();
    requestUpdate();
    return;
  }
  // A start after the alarm repeats the same interval: the target was never
  // consumed, only the countdown was.
  if (timerFired_) {
    RenderLock lock(*this);
    timerFired_ = false;
  }
  if (timerTargetSeconds_ == 0) {
    // Nothing to count. Silent rather than a toast: the readout says 00:00 and
    // the four adjust buttons are the answer, right under the thumb.
    LOG_INF(kTag, "timer start ignored: nothing set");
    return;
  }
  {
    RenderLock lock(*this);
    timerDeadlineMs_ = millis() + timerTargetSeconds_ * 1000u;
    timerRunning_ = true;
  }
  saveSettings();
  nextPollAtMs_ = millis();
  requestUpdate();
}

void ClockActivity::resetTimer() {
  {
    RenderLock lock(*this);
    timerRunning_ = false;
    timerFired_ = false;
    timerTargetSeconds_ = 0;
  }
  saveSettings();
  nextPollAtMs_ = millis();
  requestUpdate();
}

void ClockActivity::adjustTimer(const int deltaMinutes, const int deltaSeconds) {
  if (timerRunning_) return;
  {
    RenderLock lock(*this);
    timerFired_ = false;
    if (deltaMinutes != 0) timerTargetSeconds_ = clockapp::addTimerMinutes(timerTargetSeconds_, deltaMinutes);
    if (deltaSeconds != 0) timerTargetSeconds_ = clockapp::addTimerSeconds(timerTargetSeconds_, deltaSeconds);
  }
  saveSettings();
  nextPollAtMs_ = millis();
  requestUpdate();
}

void ClockActivity::fireTimer() {
  {
    RenderLock lock(*this);
    timerRunning_ = false;
    timerDeadlineMs_ = 0;
    // The readout holds at 00:00 until the user does something; the target is
    // untouched, so START repeats the same interval.
    timerFired_ = true;
    flashPending_ = true;
  }
  LOG_INF(kTag, "timer finished");
  nextPollAtMs_ = millis();
  requestUpdate();
}

// --- Lifecycle -----------------------------------------------------------

void ClockActivity::onEnter() {
  Activity::onEnter();
  toybox::ensureFonts(renderer);
  loadSettings();
  nextPollAtMs_ = millis();
  LOG_INF(kTag, "opened; RTC %s", halClock.isAvailable() ? "present" : "absent");
  requestUpdate();
}

void ClockActivity::onExit() {
  saveSettings();
  // Leave the panel the way every other app does. The flash inverts the
  // DRIVER's output polarity, not the framebuffer, so a crash mid-flash would
  // otherwise hand the next activity an inverted screen.
  display.setInverted(false);
  Activity::onExit();
}

// --- Input and pacing ----------------------------------------------------

void ClockActivity::loop() {
  const uint32_t now = millis();

  // The deadline is checked here rather than at paint time: a timer whose
  // alarm waited for the next scheduled repaint would fire up to a second
  // late, and the whole point of a timer is the moment it ends.
  if (timerRunning_ && static_cast<int32_t>(timerDeadlineMs_ - now) <= 0) {
    fireTimer();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    shelf::leave(renderer, mappedInput);
    return;
  }

  // The side keys work the stopwatch without looking at the panel, which is
  // the control a hand reaches for while doing something else.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    toggleStopwatch();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    resetStopwatch();
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY) && interactionsReady_) {
    fui::InputSnapshot input;
    input.touchReleased = true;
    input.touchX = static_cast<int16_t>(tapX);
    input.touchY = static_cast<int16_t>(tapY);
    const fui::ActionEvent event = interactions_.route(input);
    switch (event.action) {
      case clockui::ActionStopwatchToggle:
        toggleStopwatch();
        return;
      case clockui::ActionStopwatchReset:
        resetStopwatch();
        return;
      case clockui::ActionTimerToggle:
        toggleTimer();
        return;
      case clockui::ActionTimerReset:
        resetTimer();
        return;
      case clockui::ActionTimerPlus5m:
        adjustTimer(5, 0);
        return;
      case clockui::ActionTimerPlus1m:
        adjustTimer(1, 0);
        return;
      case clockui::ActionTimerPlus10s:
        adjustTimer(0, 10);
        return;
      case clockui::ActionTimerPlus1s:
        adjustTimer(0, 1);
        return;
      default:
        break;
    }
  }

  if (static_cast<int32_t>(now - nextPollAtMs_) < 0) return;
  const clockapp::Cadence cadence = clockapp::cadenceFor(stopwatchRunning_, timerRunning_);
  nextPollAtMs_ = now + clockapp::pollIntervalMs(cadence);

  {
    RenderLock lock(*this);
    refreshModel();
  }

  // The panel is only driven when what it shows would actually differ. This is
  // the whole battery story: idle, the clock string is the only thing that
  // changes and it changes once a minute, so the ~300ms refresh happens sixty
  // times an hour instead of thirty-six hundred.
  if (std::strcmp(shownTime_, timeText_) != 0 || std::strcmp(shownStopwatch_, stopwatchText_) != 0 ||
      std::strcmp(shownTimer_, timerText_) != 0) {
    requestUpdate();
  }
}

void ClockActivity::refreshModel() {
  // localTime() rather than a shift of our own. Upstream 1.13.9 moved HalClock
  // onto a POSIX TZ rule applied through newlib's localtime_r, so zones with
  // daylight saving are right year-round -- which the fixed quarter-hour
  // offset this app used to apply never was. The RTC still keeps UTC; the HAL
  // is simply the thing that knows the rule now.
  struct tm now = {};
  // tm_year 120 is 2020: a year below it means the chip was never set, which
  // is the same test HalClock::begin() makes before trusting it.
  clockValid_ = halClock.localTime(now) && now.tm_year >= 120;
  if (clockValid_) {
    local_.year = static_cast<uint16_t>(now.tm_year + 1900);
    local_.month = static_cast<uint8_t>(now.tm_mon + 1);
    local_.day = static_cast<uint8_t>(now.tm_mday);
    local_.hour = static_cast<uint8_t>(now.tm_hour);
    local_.minute = static_cast<uint8_t>(now.tm_min);
    local_.second = static_cast<uint8_t>(now.tm_sec);
    local_.weekday = static_cast<uint8_t>(now.tm_wday);
    clockapp::formatClock(local_, timeText_, sizeof(timeText_));
    char line[48];
    dateLine_ = clockapp::formatDateLine(local_, line, sizeof(line));
    monthTitle_ = clockapp::formatMonthTitle(local_, line, sizeof(line));
  } else {
    timeText_[0] = '\0';
    dateLine_.clear();
    monthTitle_.clear();
  }
  clockapp::formatStopwatch(stopwatchElapsedMs(), stopwatchText_, sizeof(stopwatchText_));
  clockapp::formatTimer(timerRemainingSeconds(), timerText_, sizeof(timerText_));
}

// --- Drawing -------------------------------------------------------------

void ClockActivity::render(RenderLock&&) {
  renderer.clearScreen();
  // proseMenuFaces: the button cut in the small slot for forty-two calendar
  // cells and four adjust labels, the UI cut for rows, the display cut for the
  // band. The large cut arrives by rebind, below.
  fui::GfxRendererTarget target = toybox::makeTarget(renderer, toybox::proseMenuFaces());
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};
  interactionsReady_ = false;
  toybox::Frame frame(target, device, noInput, interactions_);
  toybox::Screen screen(frame);

  clockui::ClockModel model;
  model.time = timeText_;
  model.dateLine = dateLine_.c_str();
  model.clockValid = clockValid_;
  model.stopwatch = stopwatchText_;
  model.stopwatchRunning = stopwatchRunning_;
  model.timer = timerText_;
  model.timerRunning = timerRunning_;
  model.monthTitle = monthTitle_.c_str();
  if (clockValid_) {
    model.firstColumn = clockapp::firstColumnOf(local_.year, local_.month);
    model.daysInMonth = clockapp::daysInMonth(local_.year, local_.month);
    model.weekRows = clockapp::weekRowsIn(local_.year, local_.month);
    model.today = local_.day;
  }

  // Two passes with one slot rebound between them, the same way Connections
  // splits its chrome from its tiles: the band needs the display cut and the
  // time needs one three times that size, and there are only three slots.
  const clockui::ClockLayout layout = clockui::buildClockScreen(screen, model);
  target.setFont(fui::GfxRendererTarget::FONT_TITLE, toybox::kLargeFontId);
  clockui::buildClockFace(screen, model, layout);

  interactionsReady_ = true;
  toybox::reportOverflow(interactions_, "Clock");

  const auto labels = mappedInput.mapLabels("Back", "", "SW", "Reset");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  // Remember what the panel now shows, so a due repaint that would redraw the
  // identical frame is skipped.
  std::snprintf(shownTime_, sizeof(shownTime_), "%s", timeText_);
  std::snprintf(shownStopwatch_, sizeof(shownStopwatch_), "%s", stopwatchText_);
  std::snprintf(shownTimer_, sizeof(shownTimer_), "%s", timerText_);

  if (flashPending_) {
    flashPending_ = false;
    // The alarm. Inverting is the driver's output polarity rather than the
    // framebuffer, so the whole panel flips without redrawing anything -- and
    // on e-ink a full-frame black is the loudest thing this device can do,
    // which is the right register for a timer that has run out on a machine
    // with no speaker.
    for (int i = 0; i < kFlashCycles; ++i) {
      display.setInverted(true);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      display.setInverted(false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  }
}
