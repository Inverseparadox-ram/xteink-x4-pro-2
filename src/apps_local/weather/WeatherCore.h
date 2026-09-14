#pragma once

// Weather: the model, the vocabulary, and everything that turns numbers into
// words.
//
// Freestanding C++17 -- no renderer, no Activity, no storage, and no
// ArduinoJson -- so host-tests/weather builds it with nothing but a compiler.
// The JSON lives in WeatherFetch, which fills these structs; that split is the
// same one HackerNews and Instapaper use, and it is what keeps the part with
// all the judgement in it testable.
//
// ---------------------------------------------------------------------------
// Two decisions worth stating.
//
// 1. EVERY FIELD IS OPTIONAL. Not as defensive habit -- because it is true.
//    Open-Meteo answers with the variables a given model actually carries, so
//    visibility is absent from some models, gusts from others, and any field
//    can be JSON null for an hour the model did not produce. A struct of plain
//    floats would render those as 0, and 0 is a real temperature, a real wind
//    speed and a real pressure. `Value` makes "not reported" a state the
//    screen can say out loud instead of a number it invents.
//
// 2. The WMO code table is the app's vocabulary and it lives here, once. A
//    code means the same thing on the Today screen, in the hourly rows, on a
//    daily line and in the exported text file; four copies of a 28-entry table
//    is four chances for them to disagree about what 45 means.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace weather {

// A number the forecast may or may not carry. Absent is not zero: see
// decision 1.
struct Value {
  float v = 0.0f;
  bool has = false;

  Value() = default;
  Value(const float value) : v(value), has(true) {}  // NOLINT(google-explicit-constructor)

  float or_(const float fallback) const { return has ? v : fallback; }
};

// Caps. Bounds rather than budgets: a hostile or truncated response must not
// make this allocate without limit.
inline constexpr size_t kMaxPlaces = 24;
inline constexpr size_t kMaxHours = 48;
inline constexpr size_t kMaxDays = 7;
inline constexpr size_t kMaxResults = 12;
inline constexpr size_t kMaxNameChars = 64;

// A saved location. `id` is assigned by the store and never reused: the cache
// file and the export are named after it.
struct Place {
  uint32_t id = 0;
  std::string name;     // "Bengaluru"
  std::string region;   // "Karnataka" -- admin1, may be empty
  std::string country;  // "India"
  float latitude = 0.0f;
  float longitude = 0.0f;
  std::string timezone;   // "Asia/Kolkata", as the forecast reports it
  Value elevation;        // metres, as the forecast reports it
  std::string exportName; // the /Weather file last written for this place
};

// "Bengaluru, Karnataka, India", skipping the parts that are empty or that
// merely repeat what is already there.
std::string describePlace(const Place& place);

// One hour of the forecast.
struct Hour {
  std::string time;  // "2026-09-14T15:00", local to the place
  int code = -1;     // WMO code, -1 when not reported
  Value temperature;
  Value apparent;
  Value humidity;
  Value precipitation;      // mm
  Value precipProbability;  // %
  Value windSpeed;
  Value windGust;
  Value windDirection;  // degrees
  Value cloudCover;     // %
  Value pressure;       // hPa, mean sea level
  Value visibility;     // metres
  Value uvIndex;
  Value dewPoint;
  bool isDay = true;
};

// One day of the forecast.
struct Day {
  std::string date;  // "2026-09-14"
  int code = -1;
  Value high;
  Value low;
  Value apparentHigh;
  Value apparentLow;
  Value precipitationSum;   // mm
  Value precipProbability;  // % max over the day
  Value precipHours;
  Value windMax;
  Value gustMax;
  Value windDirection;
  Value uvIndexMax;
  std::string sunrise;  // "2026-09-14T06:12"
  std::string sunset;
  Value daylightSeconds;
};

// Everything one refresh produced for one place.
struct Reading {
  uint32_t placeId = 0;
  // Epoch seconds when this was fetched, or 0 when the clock has never been
  // set. Zero stays zero: a reading stamped from an unset RTC would claim to
  // be current forever.
  uint32_t fetchedAt = 0;
  std::string observedAt;  // the API's own "current.time", local to the place
  std::string timezone;
  std::string timezoneAbbr;
  Value elevation;

  // Current conditions.
  int code = -1;
  Value temperature;
  Value apparent;
  Value humidity;
  Value dewPoint;
  Value pressure;         // hPa at mean sea level
  Value surfacePressure;  // hPa at the station
  Value windSpeed;
  Value windGust;
  Value windDirection;
  Value cloudCover;
  Value visibility;
  Value precipitation;
  Value rain;
  Value showers;
  Value snowfall;
  Value uvIndex;
  bool isDay = true;

  std::vector<Hour> hours;
  std::vector<Day> days;

  bool valid() const { return temperature.has || code >= 0 || !days.empty(); }
};

// --- The vocabulary ------------------------------------------------------

// The WMO 4677 present-weather code as a sentence: "Thunderstorm with slight
// hail". Codes the table does not carry answer "Unknown"; -1 answers "--".
const char* describeCode(int code);

// The same code in the two or three words a row can hold: "Heavy snow".
const char* shortCode(int code);

// "NNE". 16-point compass, because 8 points call a 20-degree swing the same
// wind and a forecast that cannot tell a sea breeze from a land breeze is not
// descriptive.
const char* compassPoint(float degrees);

// Beaufort description of a wind in km/h: "Fresh breeze". What a number alone
// does not say is whether you can stand up in it.
const char* describeWind(float kmh);

// "Very high" for a UV index, "Stagnant" for a pressure, and so on. Each
// returns a fixed string; none allocates.
const char* describeUv(float index);
const char* describePressure(float hPa);
const char* describeHumidity(float percent);
const char* describeVisibility(float metres);
const char* describeCloudCover(float percent);

// --- Formatting ----------------------------------------------------------

// "14:30" out of "2026-09-14T14:30". Empty in, empty out; anything that is not
// that shape comes back unchanged, so a format change upstream shows the raw
// string rather than a confidently wrong time.
std::string clockOf(const std::string& isoTime);

// "SAT 14 SEP" out of "2026-09-14". Same rule on malformed input.
std::string dayLabelOf(const std::string& isoDate);

// "MON", for a daily row. "--" when the date is not that shape.
std::string weekdayOf(const std::string& isoDate);

// "13h 48m" out of a count of seconds.
std::string durationOf(float seconds);

// --- The places store ----------------------------------------------------

std::string serializePlaces(const std::vector<Place>& places);
bool parsePlaces(const std::string& text, std::vector<Place>& out);
uint32_t nextPlaceId(const std::vector<Place>& places);

// Trim, strip control characters, and clamp. The same boundary rule the Notes
// app learned the hard way: the renderer has no glyph for a control character,
// and a place name arrives from a web service rather than from a keyboard.
std::string sanitize(std::string text, size_t maxChars);

// --- The readable export -------------------------------------------------

// The whole reading as plain text, the way it would read in a notebook:
// current conditions in full, then the next hours, then the week. This is what
// lands in /Weather/<slug>.txt so a forecast can be read off the card on a
// computer.
std::string exportText(const Place& place, const Reading& reading);

// "bengaluru-3.txt" -- slug plus the place id, so two places with the same
// name are two files and a rename can never land on another place's export.
std::string exportFileName(const Place& place);

}  // namespace weather
