#include "WeatherFetch.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(FREEINK_NET_WOLFSSL)
#include <Arduino.h>
#include <HalStorage.h>
#include <SecureHttpClient.h>

#include "../study/StudySyncRoots.h"
#else
#include <unistd.h>

#include <cstdlib>
#endif

namespace weather {
namespace {

constexpr const char* kTag = "WEATHER";

// The variables asked for, spelled once. Kept deliberately short of
// everything Open-Meteo offers: each hourly variable is 24 more numbers in a
// body this device has to hold in RAM and hand to a JSON parser, and the
// screens can only show what a 480px panel can show. These are the ones that
// earn their bytes.
constexpr const char* kCurrentVars =
    "temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,rain,showers,snowfall,"
    "weather_code,cloud_cover,pressure_msl,surface_pressure,wind_speed_10m,wind_direction_10m,wind_gusts_10m";
constexpr const char* kHourlyVars =
    "temperature_2m,apparent_temperature,relative_humidity_2m,dew_point_2m,precipitation_probability,"
    "weather_code,visibility,wind_speed_10m,uv_index";
constexpr const char* kDailyVars =
    "weather_code,temperature_2m_max,temperature_2m_min,apparent_temperature_max,apparent_temperature_min,"
    "sunrise,sunset,daylight_duration,uv_index_max,precipitation_sum,precipitation_hours,"
    "precipitation_probability_max,wind_speed_10m_max,wind_gusts_10m_max,wind_direction_10m_dominant";

// forecast_hours caps the hourly block at one day. Without it the response
// carries 24 numbers per variable per day for seven days, which is an order of
// magnitude more JSON than this device should be asked to hold at once.
constexpr int kForecastHours = 24;
constexpr int kForecastDays = 7;

// A body larger than this is refused before ArduinoJson sees it. The expected
// forecast is about 12KB; the ceiling exists because a proxy, a captive portal
// or a changed API can answer with something unbounded, and running the device
// out of heap mid-parse is a reboot rather than an error message.
constexpr size_t kMaxBody = 192 * 1024;

std::string baseForecast() {
#if !defined(FREEINK_NET_WOLFSSL)
  // The simulator points at a local fixture server, which is how these screens
  // get rendered at all in an environment with no route to the real host. The
  // device build has no such door.
  if (const char* env = std::getenv("CROSSPLAY_WEATHER_BASE")) return env;
#endif
  return "https://api.open-meteo.com";
}

std::string baseGeocode() {
#if !defined(FREEINK_NET_WOLFSSL)
  if (const char* env = std::getenv("CROSSPLAY_GEOCODE_BASE")) return env;
#endif
  return "https://geocoding-api.open-meteo.com";
}

// Percent-encoding for the one field a person types. A place name can carry a
// space, an accent, an ampersand or a plus, and every one of those changes the
// query if it goes in raw.
std::string urlEncode(const std::string& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size() * 3);
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-' || u == '_' ||
        u == '.' || u == '~') {
      out += static_cast<char>(u);
    } else {
      out += '%';
      out += kHex[u >> 4];
      out += kHex[u & 0x0F];
    }
  }
  return out;
}

// A JSON number that may be null. `null` is the normal way a model says it did
// not produce this hour, and it must not become 0.
Value take(JsonVariantConst value) {
  if (value.isNull() || !value.is<float>()) return Value{};
  const float v = value.as<float>();
  // NaN reaches here from a malformed body and would poison every comparison
  // downstream; treat it as not reported.
  if (std::isnan(v) || std::isinf(v)) return Value{};
  return Value{v};
}

int takeCode(JsonVariantConst value) {
  if (value.isNull() || !value.is<int>()) return -1;
  return value.as<int>();
}

std::string takeString(JsonVariantConst value, const size_t cap = kMaxNameChars) {
  if (!value.is<const char*>()) return "";
  const char* text = value.as<const char*>();
  return text ? sanitize(text, cap) : "";
}

#if defined(FREEINK_NET_WOLFSSL)

// TLS wants roughly 35KB free with a 20KB block; the parse then wants room for
// the document on top. Checked before the handshake so the message can say
// what to do rather than the device rebooting mid-parse.
bool insufficientHeap(std::string& message) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxBlock = ESP.getMaxAllocHeap();
  if (freeHeap < 70000 || maxBlock < 20000) {
    LOG_ERR(kTag, "heap too low: free=%u block=%u", static_cast<unsigned>(freeHeap), static_cast<unsigned>(maxBlock));
    message = "Not enough memory free to fetch a forecast. Leave the app and open it again.";
    return true;
  }
  return false;
}

// Verified TLS against the roots this firmware already bakes for its own
// services: ISRG Root X1 and X2 (Let's Encrypt) and GTS Root R1 and R4. An SD
// override wins, so a CA rotation is a file copy rather than a reflash -- the
// same escape hatch the Study sync has, and it is not decoration here: this
// app talks to a host the firmware does not control.
const char* caRoots() {
  static std::string sdRoots;
  static bool probed = false;
  if (!probed) {
    probed = true;
    HalFile file;
    if (Storage.openFileForRead(kTag, "/.crosspoint/weather/roots.pem", file)) {
      const size_t size = file.size();
      if (size > 512 && size < 65536) {
        sdRoots.resize(size);
        if (file.read(reinterpret_cast<uint8_t*>(sdRoots.data()), size) == static_cast<int>(size) &&
            sdRoots.find("-----BEGIN CERTIFICATE-----") != std::string::npos) {
          LOG_INF(kTag, "using SD root bundle (%u bytes)", static_cast<unsigned>(size));
        } else {
          sdRoots.clear();
          LOG_ERR(kTag, "SD root bundle unreadable; using baked roots");
        }
      } else {
        sdRoots.clear();
        LOG_ERR(kTag, "SD root bundle size %u rejected; using baked roots", static_cast<unsigned>(size));
      }
    }
  }
  return sdRoots.empty() ? study::kBridgeCaRoots : sdRoots.c_str();
}

int httpGet(const std::string& url, std::string& body, std::string& message) {
  if (insufficientHeap(message)) return 0;
  freeink::SecureHttpClient http;
  http.setCACert(caRoots());
  http.setTimeout(30000);
  http.setFollowRedirects(2);
  if (!http.begin(url)) {
    message = "That forecast address did not make sense. Update the firmware.";
    return 0;
  }
  http.setUserAgent("CrossPlay-ESP32-" CROSSPOINT_VERSION);
  http.addHeader("Accept", "application/json");
  const int status = http.sendRequest("GET", std::string());
  if (status <= 0) {
    LOG_ERR(kTag, "GET failed: %d", status);
    // Naming the certificate file is the difference between a dead app and a
    // fixable one: this host's issuing CA can rotate out from under the baked
    // roots, and when it does every other symptom looks like "no internet".
    message =
        "Could not reach the forecast service. Check Wi-Fi. If Wi-Fi is fine, the service may have changed "
        "certificate: put a current root bundle at /.crosspoint/weather/roots.pem on the card.";
    http.end();
    return 0;
  }
  body = http.getString();
  http.end();
  return status;
}

#else  // simulator: curl, the same door BridgeHttp uses for the same reason.

int httpGet(const std::string& url, std::string& body, std::string& message) {
  char outPath[] = "/tmp/weatherhttp-XXXXXX";
  const int fd = mkstemp(outPath);
  if (fd < 0) {
    message = "sim: mkstemp failed";
    return 0;
  }
  close(fd);
  const std::string cmd = "curl -sS -m 60 -o '" + std::string(outPath) +
                          "' -w '%{http_code}' -H 'Accept: application/json' '" + url + "'";
  FILE* pipe = popen(cmd.c_str(), "r");
  char statusBuf[8] = {};
  if (pipe) {
    if (fgets(statusBuf, sizeof(statusBuf), pipe) == nullptr) statusBuf[0] = '\0';
    pclose(pipe);
  }
  FILE* out = fopen(outPath, "rb");
  if (out) {
    char chunk[1024];
    size_t n = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), out)) > 0) body.append(chunk, n);
    fclose(out);
  }
  unlink(outPath);
  const int status = std::atoi(statusBuf);
  if (status <= 0) message = "Could not reach the forecast service.";
  return status;
}

#endif

// Open-Meteo reports its own failures as 400 with {"error":true,"reason":".."}.
// Reading that is the difference between "Latitude must be in range of -90 to
// 90" and "something went wrong".
bool takeApiError(const std::string& body, std::string& message) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
  if (!doc["error"].as<bool>()) return false;
  const char* reason = doc["reason"].is<const char*>() ? doc["reason"].as<const char*>() : nullptr;
  message = reason ? std::string("The forecast service refused: ") + sanitize(reason, 160)
                   : std::string("The forecast service refused the request.");
  return true;
}

bool checkBody(const int status, const std::string& body, std::string& message) {
  if (status <= 0) return false;
  if (body.size() > kMaxBody) {
    LOG_ERR(kTag, "response too large: %u bytes", static_cast<unsigned>(body.size()));
    message = "The forecast service sent far more data than a forecast. Nothing was changed.";
    return false;
  }
  if (status >= 400) {
    if (takeApiError(body, message)) return false;
    char text[96];
    std::snprintf(text, sizeof(text), "The forecast service answered %d. Try again in a moment.", status);
    message = text;
    return false;
  }
  if (body.empty()) {
    message = "The forecast service sent an empty answer.";
    return false;
  }
  return true;
}

// The hourly row whose time matches the current observation, so the Now screen
// can show dew point, visibility and UV -- three things Open-Meteo does not
// carry in its `current` block at all. Without this the most descriptive
// screen in the app would be missing exactly the fields a person cannot
// estimate by looking out of a window.
const Hour* hourMatching(const Reading& reading, const std::string& when) {
  if (when.empty()) return nullptr;
  const std::string hourKey = when.substr(0, when.size() >= 13 ? 13 : when.size());
  for (const Hour& hour : reading.hours) {
    if (hour.time.compare(0, hourKey.size(), hourKey) == 0) return &hour;
  }
  return nullptr;
}

}  // namespace

bool parseForecast(const std::string& body, Reading& reading, std::string& message) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err != DeserializationError::Ok) {
    LOG_ERR(kTag, "forecast parse failed: %s", err.c_str());
    message = "The forecast did not arrive in one piece. Try again.";
    return false;
  }
  if (doc["error"].as<bool>()) {
    takeApiError(body, message);
    return false;
  }

  reading = Reading{};
  reading.timezone = takeString(doc["timezone"]);
  reading.timezoneAbbr = takeString(doc["timezone_abbreviation"], 16);
  reading.elevation = take(doc["elevation"]);

  JsonObjectConst current = doc["current"];
  if (!current.isNull()) {
    reading.observedAt = takeString(current["time"], 32);
    reading.code = takeCode(current["weather_code"]);
    reading.temperature = take(current["temperature_2m"]);
    reading.apparent = take(current["apparent_temperature"]);
    reading.humidity = take(current["relative_humidity_2m"]);
    reading.pressure = take(current["pressure_msl"]);
    reading.surfacePressure = take(current["surface_pressure"]);
    reading.windSpeed = take(current["wind_speed_10m"]);
    reading.windGust = take(current["wind_gusts_10m"]);
    reading.windDirection = take(current["wind_direction_10m"]);
    reading.cloudCover = take(current["cloud_cover"]);
    reading.precipitation = take(current["precipitation"]);
    reading.rain = take(current["rain"]);
    reading.showers = take(current["showers"]);
    reading.snowfall = take(current["snowfall"]);
    reading.isDay = current["is_day"].as<int>() != 0;
  }

  // Hourly and daily are arrays-of-arrays by variable, not an array of
  // objects: Open-Meteo sends one array per field and they are index-aligned
  // with `time`. `time` is therefore the only one whose length can be trusted,
  // and every other array is read defensively against it -- a short array is
  // how a variable this model does not carry actually arrives.
  JsonObjectConst hourly = doc["hourly"];
  if (!hourly.isNull()) {
    JsonArrayConst times = hourly["time"];
    const size_t count = times.isNull() ? 0 : times.size();
    const size_t wanted = count < kMaxHours ? count : kMaxHours;
    reading.hours.reserve(wanted);
    for (size_t i = 0; i < wanted; ++i) {
      Hour hour;
      hour.time = takeString(times[i], 32);
      hour.code = takeCode(hourly["weather_code"][i]);
      hour.temperature = take(hourly["temperature_2m"][i]);
      hour.apparent = take(hourly["apparent_temperature"][i]);
      hour.humidity = take(hourly["relative_humidity_2m"][i]);
      hour.dewPoint = take(hourly["dew_point_2m"][i]);
      hour.precipProbability = take(hourly["precipitation_probability"][i]);
      hour.visibility = take(hourly["visibility"][i]);
      hour.windSpeed = take(hourly["wind_speed_10m"][i]);
      hour.uvIndex = take(hourly["uv_index"][i]);
      reading.hours.push_back(std::move(hour));
    }
  }

  JsonObjectConst daily = doc["daily"];
  if (!daily.isNull()) {
    JsonArrayConst dates = daily["time"];
    const size_t count = dates.isNull() ? 0 : dates.size();
    const size_t wanted = count < kMaxDays ? count : kMaxDays;
    reading.days.reserve(wanted);
    for (size_t i = 0; i < wanted; ++i) {
      Day day;
      day.date = takeString(dates[i], 32);
      day.code = takeCode(daily["weather_code"][i]);
      day.high = take(daily["temperature_2m_max"][i]);
      day.low = take(daily["temperature_2m_min"][i]);
      day.apparentHigh = take(daily["apparent_temperature_max"][i]);
      day.apparentLow = take(daily["apparent_temperature_min"][i]);
      day.precipitationSum = take(daily["precipitation_sum"][i]);
      day.precipProbability = take(daily["precipitation_probability_max"][i]);
      day.precipHours = take(daily["precipitation_hours"][i]);
      day.windMax = take(daily["wind_speed_10m_max"][i]);
      day.gustMax = take(daily["wind_gusts_10m_max"][i]);
      day.windDirection = take(daily["wind_direction_10m_dominant"][i]);
      day.uvIndexMax = take(daily["uv_index_max"][i]);
      day.sunrise = takeString(daily["sunrise"][i], 32);
      day.sunset = takeString(daily["sunset"][i], 32);
      day.daylightSeconds = take(daily["daylight_duration"][i]);
      reading.days.push_back(std::move(day));
    }
  }

  // Fill the three fields `current` does not carry from the matching hour.
  if (const Hour* now = hourMatching(reading, reading.observedAt)) {
    if (!reading.dewPoint.has) reading.dewPoint = now->dewPoint;
    if (!reading.visibility.has) reading.visibility = now->visibility;
    if (!reading.uvIndex.has) reading.uvIndex = now->uvIndex;
  }

  if (!reading.valid()) {
    message = "The forecast arrived with nothing in it. Try again.";
    return false;
  }
  return true;
}

bool fetchForecast(const Place& place, Reading& reading, std::string& message, std::string* rawJson) {
  char query[1024];
  std::snprintf(query, sizeof(query),
                "%s/v1/forecast?latitude=%.5f&longitude=%.5f&current=%s&hourly=%s&daily=%s"
                "&timezone=auto&forecast_days=%d&forecast_hours=%d&wind_speed_unit=kmh",
                baseForecast().c_str(), static_cast<double>(place.latitude), static_cast<double>(place.longitude),
                kCurrentVars, kHourlyVars, kDailyVars, kForecastDays, kForecastHours);

  std::string body;
  const int status = httpGet(query, body, message);
  if (!checkBody(status, body, message)) return false;

  if (!parseForecast(body, reading, message)) return false;
  reading.placeId = place.id;
  if (rawJson != nullptr) *rawJson = body;
  LOG_INF(kTag, "forecast for %s: %d hours, %d days", place.name.c_str(), static_cast<int>(reading.hours.size()),
          static_cast<int>(reading.days.size()));
  return true;
}

bool searchPlaces(const std::string& query, std::vector<Place>& results, std::string& message) {
  results.clear();
  const std::string trimmed = sanitize(query, kMaxNameChars);
  if (trimmed.empty()) {
    message = "Type a place name to search for.";
    return false;
  }

  const std::string url =
      baseGeocode() + "/v1/search?name=" + urlEncode(trimmed) + "&count=10&language=en&format=json";
  std::string body;
  const int status = httpGet(url, body, message);
  if (!checkBody(status, body, message)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    message = "The place search did not arrive in one piece. Try again.";
    return false;
  }

  JsonArrayConst matches = doc["results"];
  // No "results" key at all is how this geocoder says "nothing matched". That
  // is a successful request with an empty answer, and the screen says so
  // differently from a failure.
  if (matches.isNull()) return true;

  results.reserve(matches.size() < kMaxResults ? matches.size() : kMaxResults);
  for (JsonObjectConst match : matches) {
    if (results.size() >= kMaxResults) break;
    Place place;
    place.name = takeString(match["name"]);
    if (place.name.empty()) continue;
    place.region = takeString(match["admin1"]);
    place.country = takeString(match["country"]);
    place.timezone = takeString(match["timezone"]);
    place.elevation = take(match["elevation"]);
    const Value latitude = take(match["latitude"]);
    const Value longitude = take(match["longitude"]);
    if (!latitude.has || !longitude.has) continue;
    place.latitude = latitude.v;
    place.longitude = longitude.v;
    if (place.latitude < -90.0f || place.latitude > 90.0f) continue;
    if (place.longitude < -180.0f || place.longitude > 180.0f) continue;
    results.push_back(std::move(place));
  }
  LOG_INF(kTag, "search '%s': %d matches", trimmed.c_str(), static_cast<int>(results.size()));
  return true;
}

}  // namespace weather
