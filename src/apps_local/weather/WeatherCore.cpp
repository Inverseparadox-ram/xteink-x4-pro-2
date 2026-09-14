#include "WeatherCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace weather {
namespace {

constexpr const char* kHeader = "crossplay-weather 1";

// WMO 4677, the subset Open-Meteo emits. One table, because a code has to mean
// the same thing on every screen and in the export -- see decision 2.
struct CodeName {
  int code;
  const char* full;
  const char* brief;
};

constexpr CodeName kCodes[] = {
    {0, "Clear sky", "Clear"},
    {1, "Mainly clear", "Mainly clear"},
    {2, "Partly cloudy", "Partly cloudy"},
    {3, "Overcast", "Overcast"},
    {45, "Fog", "Fog"},
    {48, "Depositing rime fog", "Rime fog"},
    {51, "Light drizzle", "Light drizzle"},
    {53, "Moderate drizzle", "Drizzle"},
    {55, "Dense drizzle", "Heavy drizzle"},
    {56, "Light freezing drizzle", "Freezing drizzle"},
    {57, "Dense freezing drizzle", "Freezing drizzle"},
    {61, "Slight rain", "Light rain"},
    {63, "Moderate rain", "Rain"},
    {65, "Heavy rain", "Heavy rain"},
    {66, "Light freezing rain", "Freezing rain"},
    {67, "Heavy freezing rain", "Freezing rain"},
    {71, "Slight snowfall", "Light snow"},
    {73, "Moderate snowfall", "Snow"},
    {75, "Heavy snowfall", "Heavy snow"},
    {77, "Snow grains", "Snow grains"},
    {80, "Slight rain showers", "Showers"},
    {81, "Moderate rain showers", "Showers"},
    {82, "Violent rain showers", "Heavy showers"},
    {85, "Slight snow showers", "Snow showers"},
    {86, "Heavy snow showers", "Snow showers"},
    {95, "Thunderstorm", "Thunderstorm"},
    {96, "Thunderstorm with slight hail", "Storm, hail"},
    {99, "Thunderstorm with heavy hail", "Storm, hail"},
};

const CodeName* findCode(const int code) {
  for (const CodeName& entry : kCodes) {
    if (entry.code == code) return &entry;
  }
  return nullptr;
}

// Sakamoto's method rather than localtime_r: the core is freestanding, and a
// day name that depends on the device's timezone would disagree with the date
// string the API already localised for this place.
int dayOfWeek(const int year, const int month, const int day) {
  static constexpr int kShift[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 1 || month > 12) return -1;
  int y = year;
  if (month < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + kShift[month - 1] + day) % 7;
}

// "2026-09-14" into its three numbers. False when the string is not that
// shape, which is what keeps a format change upstream from printing a
// confident wrong date.
bool splitDate(const std::string& iso, int& year, int& month, int& day) {
  if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') return false;
  for (const size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
    if (iso[i] < '0' || iso[i] > '9') return false;
  }
  year = std::atoi(iso.substr(0, 4).c_str());
  month = std::atoi(iso.substr(5, 2).c_str());
  day = std::atoi(iso.substr(8, 2).c_str());
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

void appendEscaped(std::string& out, const std::string& value) {
  for (const char c : value) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        break;
      default:
        out += c;
    }
  }
}

std::string unescape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 >= value.size()) {
      out += value[i];
      continue;
    }
    switch (value[++i]) {
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case '\\':
        out += '\\';
        break;
      default:
        out += '\\';
        out += value[i];
    }
  }
  return out;
}

std::vector<std::string> fields(const std::string& line, const size_t max) {
  std::vector<std::string> out;
  out.reserve(max);
  size_t start = 0;
  while (out.size() + 1 < max) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string::npos) break;
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  out.push_back(line.substr(start));
  return out;
}

// A line of the export: "  Humidity            64%". Two columns, because a
// report read on a phone or in a terminal is a table and a table needs its
// values to line up.
void reportLine(std::string& out, const char* label, const std::string& value) {
  if (value.empty()) return;
  char row[96];
  std::snprintf(row, sizeof(row), "  %-22s %s\n", label, value.c_str());
  out += row;
}

std::string number(const Value& value, const char* unit, const int decimals = 0) {
  if (!value.has) return "";
  char text[48];
  std::snprintf(text, sizeof(text), "%.*f%s", decimals, static_cast<double>(value.v), unit);
  return text;
}

}  // namespace

// --- The vocabulary ------------------------------------------------------

const char* describeCode(const int code) {
  if (code < 0) return "--";
  const CodeName* entry = findCode(code);
  return entry ? entry->full : "Unknown conditions";
}

const char* shortCode(const int code) {
  if (code < 0) return "--";
  const CodeName* entry = findCode(code);
  return entry ? entry->brief : "Unknown";
}

const char* compassPoint(const float degrees) {
  static constexpr const char* kPoints[16] = {"N",  "NNE", "NE", "ENE", "E",  "ESE", "SE", "SSE",
                                              "S",  "SSW", "SW", "WSW", "W",  "WNW", "NW", "NNW"};
  // 16 points, not 8: eight call a twenty-degree swing the same wind, and a
  // forecast that cannot tell a sea breeze from a land breeze is not
  // descriptive.
  float d = std::fmod(degrees, 360.0f);
  if (d < 0.0f) d += 360.0f;
  const int index = static_cast<int>(std::floor((d + 11.25f) / 22.5f)) % 16;
  return kPoints[index];
}

const char* describeWind(const float kmh) {
  // Beaufort, which is the scale that says what a wind DOES rather than how
  // fast it is going.
  if (kmh < 1.0f) return "Calm";
  if (kmh < 6.0f) return "Light air";
  if (kmh < 12.0f) return "Light breeze";
  if (kmh < 20.0f) return "Gentle breeze";
  if (kmh < 29.0f) return "Moderate breeze";
  if (kmh < 39.0f) return "Fresh breeze";
  if (kmh < 50.0f) return "Strong breeze";
  if (kmh < 62.0f) return "Near gale";
  if (kmh < 75.0f) return "Gale";
  if (kmh < 89.0f) return "Strong gale";
  if (kmh < 103.0f) return "Storm";
  if (kmh < 118.0f) return "Violent storm";
  return "Hurricane force";
}

const char* describeUv(const float index) {
  if (index < 3.0f) return "Low";
  if (index < 6.0f) return "Moderate";
  if (index < 8.0f) return "High";
  if (index < 11.0f) return "Very high";
  return "Extreme";
}

const char* describePressure(const float hPa) {
  if (hPa < 990.0f) return "Very low";
  if (hPa < 1005.0f) return "Low";
  if (hPa < 1020.0f) return "Normal";
  if (hPa < 1035.0f) return "High";
  return "Very high";
}

const char* describeHumidity(const float percent) {
  if (percent < 30.0f) return "Very dry";
  if (percent < 50.0f) return "Dry";
  if (percent < 70.0f) return "Comfortable";
  if (percent < 85.0f) return "Humid";
  return "Very humid";
}

const char* describeVisibility(const float metres) {
  if (metres < 1000.0f) return "Fog";
  if (metres < 4000.0f) return "Poor";
  if (metres < 10000.0f) return "Moderate";
  if (metres < 20000.0f) return "Good";
  return "Excellent";
}

const char* describeCloudCover(const float percent) {
  if (percent < 10.0f) return "Clear";
  if (percent < 25.0f) return "Mostly clear";
  if (percent < 50.0f) return "Partly cloudy";
  if (percent < 85.0f) return "Mostly cloudy";
  return "Overcast";
}

// --- Formatting ----------------------------------------------------------

std::string clockOf(const std::string& isoTime) {
  if (isoTime.size() < 16 || isoTime[10] != 'T' || isoTime[13] != ':') return isoTime;
  return isoTime.substr(11, 5);
}

std::string weekdayOf(const std::string& isoDate) {
  static constexpr const char* kDays[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  int year = 0;
  int month = 0;
  int day = 0;
  if (!splitDate(isoDate, year, month, day)) return "--";
  const int index = dayOfWeek(year, month, day);
  return index < 0 ? "--" : kDays[index];
}

std::string dayLabelOf(const std::string& isoDate) {
  static constexpr const char* kMonths[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                              "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  int year = 0;
  int month = 0;
  int day = 0;
  if (!splitDate(isoDate, year, month, day)) return isoDate;
  char label[24];
  std::snprintf(label, sizeof(label), "%s %d %s", weekdayOf(isoDate).c_str(), day, kMonths[month - 1]);
  return label;
}

std::string durationOf(const float seconds) {
  if (seconds <= 0.0f) return "";
  const int total = static_cast<int>(seconds + 0.5f);
  // Sized from the FORMAT, not from the durations a day of daylight produces.
  // An int is eleven characters with its sign, and "%dh %02dm" can therefore
  // print 26 -- host-tests/fmtwidth counts exactly this, and a buffer sized by
  // what the data "obviously" holds is how a truncation ships.
  static constexpr int kIntChars = 11;
  static constexpr int kDurationChars = 2 * kIntChars + 3 + 1;  // "%dh %02dm" plus NUL
  char text[kDurationChars];
  std::snprintf(text, sizeof(text), "%dh %02dm", total / 3600, (total % 3600) / 60);
  return text;
}

std::string describePlace(const Place& place) {
  std::string out = place.name.empty() ? std::string("Unknown place") : place.name;
  // The region is skipped when it merely repeats the city, which happens for
  // every city that is also its own province -- "Singapore, Singapore,
  // Singapore" was the first thing this app ever drew.
  if (!place.region.empty() && place.region != place.name) {
    out += ", ";
    out += place.region;
  }
  if (!place.country.empty() && place.country != place.name && place.country != place.region) {
    out += ", ";
    out += place.country;
  }
  return out;
}

// --- The places store ----------------------------------------------------

std::string sanitize(std::string text, const size_t maxChars) {
  // Control characters, at the boundary. A place name arrives from a web
  // service rather than a keyboard, and the renderer draws nothing at all for
  // a codepoint it has no glyph for -- the Notes app paid for this lesson with
  // a tab.
  std::string clean;
  clean.reserve(text.size());
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == '\n') {
      clean += '\n';
    } else if (u == '\t') {
      clean += ' ';
    } else if (u >= 0x20 && u != 0x7F) {
      clean += c;
    }
  }
  text = std::move(clean);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
  size_t lead = 0;
  while (lead < text.size() && text[lead] == ' ') ++lead;
  text.erase(0, lead);
  if (text.size() <= maxChars) return text;
  size_t cut = maxChars;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
  text.resize(cut);
  return text;
}

uint32_t nextPlaceId(const std::vector<Place>& places) {
  uint32_t highest = 0;
  for (const Place& place : places) {
    if (place.id > highest) highest = place.id;
  }
  return highest + 1;
}

std::string serializePlaces(const std::vector<Place>& places) {
  std::string out;
  out.reserve(places.size() * 128 + 32);
  out += kHeader;
  out += '\n';
  for (const Place& place : places) {
    char head[80];
    // Five decimals is about a metre, which is far finer than any forecast
    // grid and still short enough to read.
    std::snprintf(head, sizeof(head), "P\t%lu\t%.5f\t%.5f\t", static_cast<unsigned long>(place.id),
                  static_cast<double>(place.latitude), static_cast<double>(place.longitude));
    out += head;
    appendEscaped(out, place.name);
    out += '\t';
    appendEscaped(out, place.region);
    out += '\t';
    appendEscaped(out, place.country);
    out += '\t';
    appendEscaped(out, place.timezone);
    out += '\t';
    appendEscaped(out, place.exportName);
    out += '\n';
  }
  return out;
}

bool parsePlaces(const std::string& text, std::vector<Place>& out) {
  out.clear();
  if (text.compare(0, std::char_traits<char>::length(kHeader), kHeader) != 0) return false;

  size_t start = 0;
  bool first = true;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (first) {
      first = false;
      continue;
    }
    if (line.empty() || line[0] != 'P') continue;
    if (out.size() >= kMaxPlaces) break;

    const std::vector<std::string> parts = fields(line, 9);
    if (parts.size() < 7) continue;
    Place place;
    place.id = static_cast<uint32_t>(std::strtoul(parts[1].c_str(), nullptr, 10));
    place.latitude = std::strtof(parts[2].c_str(), nullptr);
    place.longitude = std::strtof(parts[3].c_str(), nullptr);
    place.name = sanitize(unescape(parts[4]), kMaxNameChars);
    place.region = sanitize(unescape(parts[5]), kMaxNameChars);
    place.country = sanitize(unescape(parts[6]), kMaxNameChars);
    if (parts.size() >= 8) place.timezone = sanitize(unescape(parts[7]), kMaxNameChars);
    if (parts.size() >= 9) place.exportName = unescape(parts[8]);
    // A place off the globe is a corrupt line, not a location: the forecast
    // would be answered for somewhere else entirely.
    if (place.latitude < -90.0f || place.latitude > 90.0f) continue;
    if (place.longitude < -180.0f || place.longitude > 180.0f) continue;
    if (place.id == 0) place.id = nextPlaceId(out);
    out.push_back(std::move(place));
  }
  return true;
}

// --- The readable export -------------------------------------------------

std::string exportFileName(const Place& place) {
  std::string slug;
  slug.reserve(40);
  bool pendingHyphen = false;
  for (const char c : place.name) {
    if (slug.size() >= 40) break;
    const unsigned char u = static_cast<unsigned char>(c);
    const bool alnum = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
    if (alnum) {
      if (pendingHyphen && !slug.empty()) slug += '-';
      pendingHyphen = false;
      slug += static_cast<char>((u >= 'A' && u <= 'Z') ? u - 'A' + 'a' : u);
    } else {
      pendingHyphen = true;
    }
  }
  if (slug.empty()) slug = "place";
  char suffix[24];
  std::snprintf(suffix, sizeof(suffix), "-%lu.txt", static_cast<unsigned long>(place.id));
  return slug + suffix;
}

std::string exportText(const Place& place, const Reading& reading) {
  std::string out;
  out.reserve(4096);
  out += describePlace(place);
  out += '\n';

  char head[128];
  std::snprintf(head, sizeof(head), "%.4f, %.4f", static_cast<double>(place.latitude),
                static_cast<double>(place.longitude));
  reportLine(out, "Coordinates", head);
  reportLine(out, "Elevation", number(reading.elevation, " m"));
  reportLine(out, "Timezone", reading.timezone);
  reportLine(out, "Observed", reading.observedAt);
  out += "\nNOW\n";
  reportLine(out, "Conditions", describeCode(reading.code));
  reportLine(out, "Temperature", number(reading.temperature, " C", 1));
  reportLine(out, "Feels like", number(reading.apparent, " C", 1));
  if (reading.humidity.has) {
    reportLine(out, "Humidity", number(reading.humidity, "%") + " (" + describeHumidity(reading.humidity.v) + ")");
  }
  reportLine(out, "Dew point", number(reading.dewPoint, " C", 1));
  if (reading.windSpeed.has) {
    std::string wind = number(reading.windSpeed, " km/h", 1);
    if (reading.windDirection.has) {
      wind += " from ";
      wind += compassPoint(reading.windDirection.v);
    }
    wind += " (";
    wind += describeWind(reading.windSpeed.v);
    wind += ")";
    reportLine(out, "Wind", wind);
  }
  reportLine(out, "Gusts", number(reading.windGust, " km/h", 1));
  if (reading.pressure.has) {
    reportLine(out, "Pressure",
               number(reading.pressure, " hPa", 1) + " (" + describePressure(reading.pressure.v) + ")");
  }
  reportLine(out, "Surface pressure", number(reading.surfacePressure, " hPa", 1));
  if (reading.cloudCover.has) {
    reportLine(out, "Cloud cover",
               number(reading.cloudCover, "%") + " (" + describeCloudCover(reading.cloudCover.v) + ")");
  }
  if (reading.visibility.has) {
    char vis[48];
    std::snprintf(vis, sizeof(vis), "%.1f km (%s)", static_cast<double>(reading.visibility.v) / 1000.0,
                  describeVisibility(reading.visibility.v));
    reportLine(out, "Visibility", vis);
  }
  reportLine(out, "Precipitation", number(reading.precipitation, " mm", 1));
  reportLine(out, "Rain", number(reading.rain, " mm", 1));
  reportLine(out, "Showers", number(reading.showers, " mm", 1));
  reportLine(out, "Snowfall", number(reading.snowfall, " cm", 1));
  if (reading.uvIndex.has) {
    reportLine(out, "UV index", number(reading.uvIndex, "", 1) + " (" + describeUv(reading.uvIndex.v) + ")");
  }
  reportLine(out, "Daylight", reading.isDay ? "Day" : "Night");

  if (!reading.hours.empty()) {
    out += "\nNEXT HOURS\n";
    for (const Hour& hour : reading.hours) {
      char row[128];
      std::snprintf(row, sizeof(row), "  %-6s %6s  %-18s %4s%%  %5s\n", clockOf(hour.time).c_str(),
                    number(hour.temperature, "C", 0).c_str(), shortCode(hour.code),
                    hour.precipProbability.has ? number(hour.precipProbability, "").c_str() : "--",
                    hour.windSpeed.has ? number(hour.windSpeed, "", 0).c_str() : "--");
      out += row;
    }
  }

  if (!reading.days.empty()) {
    out += "\nTHE WEEK\n";
    for (const Day& day : reading.days) {
      char row[160];
      std::snprintf(row, sizeof(row), "  %-11s %-18s %5s / %-5s  rain %4s%% %6s  wind %5s\n",
                    dayLabelOf(day.date).c_str(), shortCode(day.code),
                    day.high.has ? number(day.high, "C", 0).c_str() : "--",
                    day.low.has ? number(day.low, "C", 0).c_str() : "--",
                    day.precipProbability.has ? number(day.precipProbability, "").c_str() : "--",
                    day.precipitationSum.has ? number(day.precipitationSum, "mm", 1).c_str() : "",
                    day.windMax.has ? number(day.windMax, "", 0).c_str() : "--");
      out += row;
      if (!day.sunrise.empty() || !day.sunset.empty()) {
        char sun[96];
        std::snprintf(sun, sizeof(sun), "              sunrise %s  sunset %s  %s\n", clockOf(day.sunrise).c_str(),
                      clockOf(day.sunset).c_str(), durationOf(day.daylightSeconds.or_(0.0f)).c_str());
        out += sun;
      }
    }
  }

  out += "\nSource: Open-Meteo (open-meteo.com), which publishes forecasts from\n";
  out += "national weather services. Written by CrossPlay on the device.\n";
  return out;
}

}  // namespace weather
