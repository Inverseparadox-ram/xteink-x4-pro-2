#include "ClockSleep.h"

#include <Arduino.h>
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "../../CrossPointSettings.h"
#include "../../components/themes/BaseTheme.h"  // Rect, which ToyboxTheme.h uses and does not include
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxIcons.h"
#include "../ui/ToyboxScreen.h"
#include "../ui/ToyboxTheme.h"
#include "../weather/WeatherCore.h"
#include "../weather/WeatherFetch.h"
#include "../weather/WeatherStore.h"
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
constexpr uint32_t kMagic = 0xC10C5EED;

// The weather line as drawn, kept as text so the baseline redraw of the last
// minute prints exactly what the glass does without reading the forecast again.
struct WeatherShown {
  uint8_t present;
  uint8_t sky;  // weather::Sky
  char temperature[16];
  char rain[16];
  char highLow[32];
};

struct Glass {
  uint32_t magic;
  int64_t minute;       // minuteStamp of the face on the glass
  int64_t weatherHour;  // minute / 60 when the weather line was last read, -1 = never
  int8_t battery;       // the percentage it printed, or -1
  uint8_t liveOwns;     // the armed timer is Live's
  WeatherShown weather;
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

const freeink::Icon* iconFor(const weather::Sky sky) {
  switch (sky) {
    case weather::Sky::Sunny:
      return &icon_wx_sun_32;
    case weather::Sky::Clear:
      return &icon_wx_moon_32;
    case weather::Sky::PartlyCloudy:
      return &icon_wx_partly_32;
    case weather::Sky::Cloudy:
      return &icon_wx_cloud_32;
    case weather::Sky::Fog:
      return &icon_wx_fog_32;
    case weather::Sky::Drizzle:
      return &icon_wx_drizzle_32;
    case weather::Sky::Rain:
      return &icon_wx_rain_32;
    case weather::Sky::Snow:
      return &icon_wx_snow_32;
    case weather::Sky::Storm:
      return &icon_wx_storm_32;
    case weather::Sky::Unknown:
      break;
  }
  return nullptr;
}

// The glance for now from the forecast the Weather app last saved for its
// FIRST place. Read from the card and never fetched: see WeatherCore.h, "A
// glance, for the Clock sleep screen".
WeatherShown readWeather(const Civil& now) {
  WeatherShown out = {};
  weather::Store store;
  store.load();
  if (store.places().empty()) return out;
  const std::string body = store.cachedBody(store.places().front().id);
  if (body.empty()) return out;
  weather::Reading reading;
  std::string message;
  if (!weather::parseForecast(body, reading, message)) return out;
  char nowIso[24];
  std::snprintf(nowIso, sizeof(nowIso), "%04u-%02u-%02uT%02u:%02u", now.year, now.month, now.day, now.hour, now.minute);
  const weather::Glance glance = weather::glanceAt(reading, nowIso);
  if (!glance.valid) return out;
  out.present = 1;
  out.sky = static_cast<uint8_t>(glance.sky);
  if (glance.temperature.has) {
    std::snprintf(out.temperature, sizeof(out.temperature), "%d C",
                  static_cast<int>(std::lround(glance.temperature.v)));
  }
  if (glance.rainChance.has) {
    std::snprintf(out.rain, sizeof(out.rain), "%d%%", static_cast<int>(std::lround(glance.rainChance.v)));
  }
  if (glance.high.has && glance.low.has) {
    std::snprintf(out.highLow, sizeof(out.highLow), "%d / %d", static_cast<int>(std::lround(glance.high.v)),
                  static_cast<int>(std::lround(glance.low.v)));
  }
  return out;
}

// Renders one face into the framebuffer. Nothing reaches the panel here.
void render(GfxRenderer& renderer, const bool valid, const Civil& local, const int battery, const WeatherShown& shown) {
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
  clockui::SleepWeather weatherLine;
  if (valid && shown.present) {
    const auto sky = static_cast<weather::Sky>(shown.sky);
    weatherLine.present = true;
    weatherLine.icon = iconFor(sky);
    weatherLine.temperature = shown.temperature;
    weatherLine.sky = weather::skyWord(sky);
    weatherLine.rain = shown.rain;
    weatherLine.highLow = shown.highLow;
  }
  const clockui::ClockLayout layout = clockui::buildSleepScreen(screen, model, charge, weatherLine);
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
  // The weather line changes by the hour, so the forecast is read from the
  // card only when the hour has moved or nothing is known; every other minute
  // reuses the line already on the glass.
  const WeatherShown onGlass = glassKnown() ? glass.weather : WeatherShown{};
  WeatherShown next = onGlass;
  const int64_t nowHour = valid ? minuteStamp(now) / 60 : -1;
  if (valid && (!glassKnown() || glass.weatherHour != nowHour)) next = readWeather(now);

  const bool fast = unattendedWake && valid && glassKnown() && !sleepWantsCleanRefresh(now);
  if (fast) {
    Civil shown;
    const int64_t nowMinute = minuteStamp(now);
    const int back = static_cast<int>(nowMinute - glass.minute);
    if (back > 0 && back < 24 * 60 && minutesBefore(now, back, shown)) {
      // The face already on the glass goes into the controller's previous-
      // frame plane WITHOUT a refresh, then the new minute is diffed against
      // it: one waveform, no flash. Painting the baseline instead is what
      // flashed every minute -- after power-up the driver promotes the first
      // paint to a full refresh, so the baseline itself was the black flash.
      render(renderer, true, shown, glass.battery, onGlass);
      renderer.cleanupGrayscaleWithFrameBuffer();
      render(renderer, true, now, battery, next);
#if defined(SIMULATOR)
      // The simulator's display has no controller planes to seed.
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
#else
      display.displayWholeFrameDifferential();
#endif
    } else {
      render(renderer, valid, now, battery, next);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  } else {
    render(renderer, valid, now, battery, next);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  glass.magic = kMagic;
  glass.minute = valid ? minuteStamp(now) : -1;
  glass.battery = static_cast<int8_t>(battery);
  glass.weather = next;
  glass.weatherHour = nowHour;
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
    glass = Glass{};
    glass.magic = kMagic;
    glass.minute = -1;
    glass.weatherHour = -1;
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
  // The weather too: a person awake may have refreshed the forecast.
  if (glassKnown()) {
    glass.minute = -1;
    glass.weatherHour = -1;
  }
}

bool liveOwnsTimer() { return !glassKnown() || glass.liveOwns != 0; }

}  // namespace sleep
}  // namespace clockapp
