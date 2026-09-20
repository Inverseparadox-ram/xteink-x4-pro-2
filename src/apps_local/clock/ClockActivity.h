#pragma once

// Clock: the time, a stopwatch, a timer and the month.
//
// ---------------------------------------------------------------------------
// The repaint budget, which is the only interesting thing about this app.
//
// A whole-screen refresh on this panel drives it for roughly 300ms
// (PaintClock.h: "0.3-2s"; HALF_REFRESH is 1720ms), and the display SDK
// exposes no windowed update -- every repaint is the whole 480x800 frame. A
// clock that repainted every second would therefore hold the panel driving
// about a third of the time for as long as the app was open, on a device whose
// premise is lasting weeks, and would also flicker continuously while dropping
// taps against a frame nobody has seen yet.
//
// So the rate follows what is counting (clockapp::cadenceFor):
//
//   idle            -> once a MINUTE. The face reads "10:42 AM"; there is
//                      nothing to show faster. ~0.5% panel duty.
//   either counting -> once a SECOND, and only while it counts.
//
// That rate is earned rather than scheduled: loop() polls cheaply and calls
// requestUpdate() only when a string the panel is showing would differ. Idle,
// the only thing that changes is the clock, once a minute.
//
// Both counters hold milliseconds from millis(), so the READING is always
// exact regardless of how rarely the panel catches up -- the paint rate is a
// display property here and never a timekeeping one. That is what makes a
// once-a-second e-ink stopwatch honest rather than approximate.
//
// preventAutoSleep() follows the same line: true only while something counts,
// because a timer that slept through its own deadline would not fire.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <string>

#include "../../activities/Activity.h"
#include "../ui/ToyboxScreen.h"
#include "ClockCore.h"
#include "ClockScreens.h"

class ClockActivity final : public Activity {
 public:
  ClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Clock", renderer, mappedInput) {}
  ~ClockActivity() override = default;

  static std::unique_ptr<Activity> create(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // True only while a counter is running; see the header note.
  bool preventAutoSleep() override { return stopwatchRunning_ || timerRunning_; }

 private:
  // Elapsed stopwatch time, running or not.
  uint32_t stopwatchElapsedMs() const;
  // Whole seconds left on the timer, 0 once it has fired.
  uint32_t timerRemainingSeconds() const;

  void toggleStopwatch();
  void resetStopwatch();
  void toggleTimer();
  void resetTimer();
  void adjustTimer(int deltaMinutes, int deltaSeconds);
  void fireTimer();
  // Re-reads the RTC and rebuilds every string the panel shows. Cheap: one
  // I2C transaction and three snprintfs, which is what makes polling four
  // times a second affordable when a paint is not.
  void refreshModel();

  // The timer's target survives leaving the app; nothing else here does.
  void loadSettings();
  void saveSettings();

  // --- Stopwatch ---
  bool stopwatchRunning_ = false;
  uint32_t stopwatchStartedAtMs_ = 0;   // millis() when it last started
  uint32_t stopwatchAccumulatedMs_ = 0; // banked by every stop

  // --- Timer ---
  bool timerRunning_ = false;
  uint32_t timerTargetSeconds_ = 5 * 60;  // what START will count down from
  uint32_t timerDeadlineMs_ = 0;          // millis() it reaches zero at
  // Set when it reaches zero and cleared by the next thing the user does. It
  // is what keeps the readout at 00:00 after the alarm instead of snapping
  // back to the target -- which would make the flash look like it fired at a
  // time still on the clock.
  bool timerFired_ = false;
  // The alarm is raised here and performed inside render(), which already
  // holds the RenderLock. Inverting the panel from loop() would race the
  // render task for the framebuffer.
  bool flashPending_ = false;

  // --- Paint pacing ---
  uint32_t nextPollAtMs_ = 0;
  // What the panel is currently showing, so a due repaint that would draw the
  // identical frame is skipped. On e-ink an unnecessary repaint is not free:
  // it is ~300ms of panel drive and a visible flash.
  char shownTime_[12] = "";
  char shownStopwatch_[12] = "";
  char shownTimer_[8] = "";

  std::string dateLine_;
  std::string monthTitle_;
  char timeText_[12] = "";
  char stopwatchText_[12] = "";
  char timerText_[8] = "";
  clockapp::Civil local_{};
  bool clockValid_ = false;

  toybox::Interactions interactions_;
  bool interactionsReady_ = false;
};
