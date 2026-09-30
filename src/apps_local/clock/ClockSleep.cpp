#include "ClockSleep.h"

#include <Arduino.h>
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <cstdio>
#include <ctime>

#include "../../CrossPointSettings.h"
#include "../../components/themes/BaseTheme.h"  // Rect, which ToyboxTheme.h uses and does not include
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxScreen.h"
#include "../ui/ToyboxTheme.h"
#include "ClockCore.h"
#include "ClockScreens.h"

namespace clockapp {
namespace sleep {
namespace {

namespace fui = freeink::ui;

constexpr const char* kTag = "CLKSLP";

// What is on the glass and who armed the timer, kept across deep sleep.
// RTC_NOINIT rather than RTC_DATA so it also survives the software restart an
// aborted sleep entry ends in; the magic is what tells a real record from the
// noise a power-on leaves there.
constexpr uint32_t kMagic = 0xC10C5EEB;
struct Glass {
  uint32_t magic;
  int64_t minute;    // minuteStamp of the face on the glass
  int8_t battery;    // the percentage it printed, or -1
  uint8_t liveOwns;  // the armed timer is Live's
};
RTC_NOINIT_ATTR Glass glass;

bool unattendedWake = false;

bool glassKnown() { return glass.magic == kMagic; }

bool readNow(Civil& out) {
  struct tm now = {};
  // tm_year 120 is 2020: below it the RTC was never set.
  if (!halClock.localTime(now) || now.tm_year < 120) return false;
  out.year = static_cast<uint16_t>(now.tm_year + 1900);
  out.month = static_cast<uint8_t>(now.tm_mon + 1);
  out.day = static_cast<uint8_t>(now.tm_mday);
  out.hour = static_cast<uint8_t>(now.tm_hour);
  out.minute = static_cast<uint8_t>(now.tm_min);
  out.second = static_cast<uint8_t>(now.tm_sec);
  out.weekday = static_cast<uint8_t>(now.tm_wday);
  return true;
}

// The civil time `minutes` before `local`, for redrawing the face already on
// the glass. Through mktime so a day, month or year boundary comes out right.
bool minutesBefore(const Civil& local, const int minutes, Civil& out) {
  struct tm t = {};
  t.tm_year = local.year - 1900;
  t.tm_mon = local.month - 1;
  t.tm_mday = local.day;
  t.tm_hour = local.hour;
  t.tm_min = local.minute - minutes;
  t.tm_isdst = -1;
  if (mktime(&t) == static_cast<time_t>(-1)) return false;
  out.year = static_cast<uint16_t>(t.tm_year + 1900);
  out.month = static_cast<uint8_t>(t.tm_mon + 1);
  out.day = static_cast<uint8_t>(t.tm_mday);
  out.hour = static_cast<uint8_t>(t.tm_hour);
  out.minute = static_cast<uint8_t>(t.tm_min);
  out.second = 0;
  out.weekday = static_cast<uint8_t>(t.tm_wday);
  return true;
}

// Renders one face into the framebuffer. Nothing reaches the panel here.
void render(GfxRenderer& renderer, const bool valid, const Civil& local, const int battery) {
  char time[16] = "";
  char date[48] = "";
  char month[32] = "";
  char charge[16] = "";
  clockui::ClockModel model;
  model.clockValid = valid;
  if (valid) {
    formatClock(local, time, sizeof(time));
    formatDateLine(local, date, sizeof(date));
    formatMonthTitle(local, month, sizeof(month));
    model.time = time;
    model.dateLine = date;
    model.monthTitle = month;
    model.firstColumn = firstColumnOf(local.year, local.month);
    model.daysInMonth = daysInMonth(local.year, local.month);
    model.weekRows = weekRowsIn(local.year, local.month);
    model.today = local.day;
  }
  if (battery >= 0) std::snprintf(charge, sizeof(charge), "%d%%", battery);

  renderer.clearScreen();
  fui::GfxRendererTarget target = toybox::makeTarget(renderer, toybox::proseMenuFaces());
  const fui::InputSnapshot noInput{};
  toybox::Interactions interactions;
  toybox::Frame frame(target, target.deviceContext(), noInput, interactions);
  toybox::Screen screen(frame);
  const clockui::ClockLayout layout = clockui::buildSleepScreen(screen, model, charge);
  target.setFont(fui::GfxRendererTarget::FONT_TITLE, layout.hugeTime ? toybox::kHugeFontId : toybox::kLargeFontId);
  clockui::buildClockFace(screen, model, layout);
}

}  // namespace

bool enabled() { return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CLOCK; }

void setUnattended(const bool unattended) { unattendedWake = unattended; }

void draw(GfxRenderer& renderer) {
  toybox::ensureFonts(renderer);
  Civil now;
  const bool valid = readNow(now);
  // Rounded to five so the baseline redraw of the last minute prints the same
  // number the glass does far more often than an exact reading would.
  const int battery = (static_cast<int>(powerManager.getBatteryPercentage()) + 2) / 5 * 5;

  // The fast path needs to know exactly what is on the glass: the minute and
  // the battery it printed, redrawn below as the baseline.
  const bool fast = unattendedWake && valid && glassKnown() && !sleepWantsCleanRefresh(now);
  if (fast) {
    Civil shown;
    const int64_t nowMinute = minuteStamp(now);
    const int back = static_cast<int>(nowMinute - glass.minute);
    if (back > 0 && back < 24 * 60 && minutesBefore(now, back, shown)) {
      render(renderer, true, shown, glass.battery);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      render(renderer, true, now, battery);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      render(renderer, valid, now, battery);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  } else {
    render(renderer, valid, now, battery);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  glass.magic = kMagic;
  glass.minute = valid ? minuteStamp(now) : -1;
  glass.battery = static_cast<int8_t>(battery);
  LOG_INF(kTag, "drew %02u:%02u (%s)", now.hour, now.minute, fast ? "fast" : "clean");
}

bool repaintDue() {
  if (!enabled()) return false;
  Civil now;
  if (!readNow(now)) return false;
  return sleepRepaintDue(glassKnown() ? glass.minute : -1, minuteStamp(now));
}

uint64_t armMicros(const uint32_t liveSeconds, const uint64_t fallbackMicros) {
  uint32_t clockSeconds = 0;
  Civil now;
  if (enabled() && readNow(now)) clockSeconds = secondsToNextMinute(now.second);
  const uint32_t fallbackSeconds = static_cast<uint32_t>(fallbackMicros / 1000000ULL);
  const SleepAlarm alarm = sleepAlarm(liveSeconds, clockSeconds, fallbackSeconds);
  if (!glassKnown()) {
    glass.magic = kMagic;
    glass.minute = -1;
    glass.battery = -1;
  }
  glass.liveOwns = alarm.liveOwns ? 1 : 0;
  if (clockSeconds > 0) {
    LOG_DBG(kTag, "alarm %us (%s)", static_cast<unsigned>(alarm.seconds), alarm.liveOwns ? "live" : "clock");
  }
  // The fallback keeps its exact microseconds; the others are whole seconds.
  if (alarm.liveOwns && liveSeconds == 0) return fallbackMicros;
  return static_cast<uint64_t>(alarm.seconds) * 1000000ULL;
}

void forgetGlass() {
  if (glassKnown()) glass.minute = -1;
}

bool liveOwnsTimer() { return !glassKnown() || glass.liveOwns != 0; }

}  // namespace sleep
}  // namespace clockapp
