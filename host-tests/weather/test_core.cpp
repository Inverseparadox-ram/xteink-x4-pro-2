// Freestanding tests for WeatherCore: the vocabulary, the formatting, the
// places store and the export.
//
// The thing under test that matters most is ABSENCE. Open-Meteo answers with
// the variables a given model carries, so visibility is missing from some and
// gusts from others, and any hour can be null. A struct of plain floats would
// render every one of those as 0 -- a real temperature, a real wind speed, a
// real pressure -- so most of what follows checks that a missing value stays
// missing all the way to the exported line.

#include <cstdio>
#include <string>
#include <vector>

#include "../../src/apps_local/weather/WeatherCore.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                               \
  do {                                                 \
    ++checks;                                          \
    if (!(cond)) {                                     \
      ++failures;                                      \
      std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

static void testCodes() {
  CHECK(std::string(weather::describeCode(0)) == "Clear sky", "code 0");
  CHECK(std::string(weather::describeCode(95)) == "Thunderstorm", "code 95");
  CHECK(std::string(weather::describeCode(99)) == "Thunderstorm with heavy hail", "code 99");
  CHECK(std::string(weather::shortCode(75)) == "Heavy snow", "short 75");
  // A code the table does not carry must not be silently drawn as clear sky --
  // WMO 4677 has entries Open-Meteo does not emit today and may tomorrow.
  CHECK(std::string(weather::describeCode(42)) == "Unknown conditions", "an unlisted code says so");
  CHECK(std::string(weather::describeCode(-1)) == "--", "no code reported");
  CHECK(std::string(weather::shortCode(-1)) == "--", "no code reported, brief");
}

static void testCompass() {
  // 16 points, and the boundaries are the whole reason for the +11.25 shift.
  CHECK(std::string(weather::compassPoint(0.0f)) == "N", "0 is N");
  CHECK(std::string(weather::compassPoint(11.24f)) == "N", "just under the first boundary");
  CHECK(std::string(weather::compassPoint(11.26f)) == "NNE", "just over it");
  CHECK(std::string(weather::compassPoint(90.0f)) == "E", "90 is E");
  CHECK(std::string(weather::compassPoint(180.0f)) == "S", "180 is S");
  CHECK(std::string(weather::compassPoint(270.0f)) == "W", "270 is W");
  CHECK(std::string(weather::compassPoint(348.75f)) == "N", "the wrap back to N");
  CHECK(std::string(weather::compassPoint(359.9f)) == "N", "just short of 360");
  // Out-of-range degrees are wrapped rather than indexing off the table.
  CHECK(std::string(weather::compassPoint(-10.0f)) == "N", "negative degrees wrap");
  CHECK(std::string(weather::compassPoint(370.0f)) == "N", "over 360 wraps");
  // -100 wraps to 260, and 260 is W: the WSW/W boundary is 258.75. Worked out
  // rather than guessed -- the first version of this line expected E, on the
  // reasoning that -100 looks like 100.
  CHECK(std::string(weather::compassPoint(-100.0f)) == "W", "far negative wraps to the right point");
}

static void testScales() {
  CHECK(std::string(weather::describeWind(0.5f)) == "Calm", "beaufort 0");
  CHECK(std::string(weather::describeWind(25.0f)) == "Moderate breeze", "beaufort 4");
  CHECK(std::string(weather::describeWind(200.0f)) == "Hurricane force", "beaufort 12");
  CHECK(std::string(weather::describeUv(0.0f)) == "Low", "uv low");
  CHECK(std::string(weather::describeUv(7.0f)) == "High", "uv high");
  CHECK(std::string(weather::describeUv(12.0f)) == "Extreme", "uv extreme");
  CHECK(std::string(weather::describePressure(1013.0f)) == "Normal", "standard pressure is normal");
  CHECK(std::string(weather::describePressure(975.0f)) == "Very low", "deep low");
  CHECK(std::string(weather::describeHumidity(20.0f)) == "Very dry", "dry air");
  CHECK(std::string(weather::describeHumidity(90.0f)) == "Very humid", "wet air");
  CHECK(std::string(weather::describeVisibility(500.0f)) == "Fog", "fog");
  CHECK(std::string(weather::describeVisibility(24000.0f)) == "Excellent", "clear air");
  CHECK(std::string(weather::describeCloudCover(0.0f)) == "Clear", "no cloud");
  CHECK(std::string(weather::describeCloudCover(100.0f)) == "Overcast", "full cloud");
}

static void testFormatting() {
  CHECK(weather::clockOf("2026-09-14T14:30") == "14:30", "got '%s'", weather::clockOf("2026-09-14T14:30").c_str());
  CHECK(weather::clockOf("2026-09-14T06:05:00") == "06:05", "seconds are ignored");
  // Not that shape: hand back what came in, so a format change upstream shows
  // the raw string instead of a confidently wrong time.
  CHECK(weather::clockOf("later") == "later", "unparseable passes through");
  CHECK(weather::clockOf("") == "", "empty stays empty");

  // Checked against Python's datetime rather than against my own arithmetic.
  CHECK(weather::weekdayOf("2026-09-14") == "MON", "got '%s'", weather::weekdayOf("2026-09-14").c_str());
  CHECK(weather::weekdayOf("2026-01-01") == "THU", "new year");
  CHECK(weather::weekdayOf("2024-02-29") == "THU", "a leap day");
  CHECK(weather::weekdayOf("2000-03-01") == "WED", "the century leap year");
  CHECK(weather::weekdayOf("1999-12-31") == "FRI", "the day before it");
  CHECK(weather::weekdayOf("not-a-date") == "--", "unparseable says so");
  CHECK(weather::weekdayOf("2026-13-01") == "--", "month 13 is not a date");

  CHECK(weather::dayLabelOf("2026-09-19") == "SAT 19 SEP", "got '%s'", weather::dayLabelOf("2026-09-19").c_str());
  CHECK(weather::durationOf(49680.0f) == "13h 48m", "got '%s'", weather::durationOf(49680.0f).c_str());
  CHECK(weather::durationOf(0.0f) == "", "no daylight reported draws nothing");
}

static void testPlaceNames() {
  weather::Place place;
  place.name = "Bengaluru";
  place.region = "Karnataka";
  place.country = "India";
  CHECK(weather::describePlace(place) == "Bengaluru, Karnataka, India", "got '%s'",
        weather::describePlace(place).c_str());

  // The city that is also its province and its country. Without the skip this
  // drew "Singapore, Singapore, Singapore", which is the first thing this app
  // ever rendered.
  weather::Place city;
  city.name = "Singapore";
  city.region = "Singapore";
  city.country = "Singapore";
  CHECK(weather::describePlace(city) == "Singapore", "got '%s'", weather::describePlace(city).c_str());

  weather::Place noRegion;
  noRegion.name = "Monaco";
  noRegion.country = "Monaco";
  CHECK(weather::describePlace(noRegion) == "Monaco", "a place that is its own country");

  weather::Place nameless;
  CHECK(weather::describePlace(nameless) == "Unknown place", "a row always has a label");
}

static void testSanitize() {
  CHECK(weather::sanitize("  Bengaluru  ", 64) == "Bengaluru", "trimmed both ends");
  CHECK(weather::sanitize("Foo\tBar", 64) == "Foo Bar", "a tab becomes a space, not a missing glyph");
  CHECK(weather::sanitize("a\x01"
                          "\x1F"
                          "b",
                          64) == "ab",
        "other C0 controls are dropped");
  CHECK(weather::sanitize("caf\xC3\xA9", 64) == "caf\xC3\xA9", "UTF-8 continuation bytes are not controls");
  const std::string euro = "\xE2\x82\xAC";
  CHECK(weather::sanitize(euro + euro, 4) == euro, "a cut lands on a code point boundary");
}

static weather::Place made(const uint32_t id, const char* name, const float lat, const float lon) {
  weather::Place place;
  place.id = id;
  place.name = name;
  place.region = "Region";
  place.country = "Country";
  place.latitude = lat;
  place.longitude = lon;
  place.timezone = "Asia/Kolkata";
  place.exportName = "x.txt";
  return place;
}

static void testPlacesStore() {
  std::vector<weather::Place> in;
  in.push_back(made(1, "Bengaluru", 12.97194f, 77.59369f));
  in.push_back(made(2, "Reykjav\xC3\xADk", 64.13548f, -21.89541f));
  // A backslash in a name, which the format itself uses.
  in.push_back(made(3, "Back\\slash", -33.86785f, 151.20732f));

  const std::string text = weather::serializePlaces(in);
  std::vector<weather::Place> out;
  CHECK(weather::parsePlaces(text, out), "a file we just wrote parses");
  CHECK(out.size() == in.size(), "got %d places back, wrote %d", static_cast<int>(out.size()),
        static_cast<int>(in.size()));
  for (size_t i = 0; i < out.size() && i < in.size(); ++i) {
    CHECK(out[i].id == in[i].id, "place %d id", static_cast<int>(i));
    CHECK(out[i].name == in[i].name, "place %d name: got '%s'", static_cast<int>(i), out[i].name.c_str());
    CHECK(out[i].timezone == in[i].timezone, "place %d timezone", static_cast<int>(i));
    // Five decimals is about a metre; anything coarser would move the forecast.
    const float dLat = out[i].latitude - in[i].latitude;
    const float dLon = out[i].longitude - in[i].longitude;
    CHECK(dLat < 0.0001f && dLat > -0.0001f, "place %d latitude survived", static_cast<int>(i));
    CHECK(dLon < 0.0001f && dLon > -0.0001f, "place %d longitude survived", static_cast<int>(i));
  }

  std::vector<weather::Place> rejected;
  CHECK(!weather::parsePlaces("{\"places\":[]}", rejected), "JSON is not this format");
  CHECK(!weather::parsePlaces("", rejected), "an empty file is not this format");

  // A coordinate off the globe is a corrupt line, not a location: answering a
  // forecast for it would quietly describe somewhere else.
  std::vector<weather::Place> bad;
  CHECK(weather::parsePlaces("crossplay-weather 1\nP\t1\t99.0\t0.0\tNowhere\t\t\n", bad), "parses");
  CHECK(bad.empty(), "a latitude of 99 is refused");
  CHECK(weather::parsePlaces("crossplay-weather 1\nP\t1\t0.0\t999.0\tNowhere\t\t\n", bad), "parses");
  CHECK(bad.empty(), "a longitude of 999 is refused");

  // Ids are never reused, and a zero id off the card gets a real one -- two
  // zeros would share a cache file and an export.
  std::vector<weather::Place> zeros;
  CHECK(weather::parsePlaces("crossplay-weather 1\nP\t0\t1.0\t1.0\tA\t\t\nP\t0\t2.0\t2.0\tB\t\t\n", zeros), "parses");
  CHECK(zeros.size() == 2 && zeros[0].id != zeros[1].id && zeros[0].id != 0 && zeros[1].id != 0,
        "zero ids are replaced with distinct ones");

  std::string many = "crossplay-weather 1\n";
  for (size_t i = 0; i < weather::kMaxPlaces + 10; ++i) {
    many += "P\t" + std::to_string(i + 1) + "\t1.0\t1.0\tPlace\t\t\n";
  }
  std::vector<weather::Place> capped;
  CHECK(weather::parsePlaces(many, capped), "an oversized file parses");
  CHECK(capped.size() == weather::kMaxPlaces, "capped at %d, got %d", static_cast<int>(weather::kMaxPlaces),
        static_cast<int>(capped.size()));

  CHECK(weather::nextPlaceId({}) == 1, "the first id is 1, never 0");
  CHECK(weather::exportFileName(made(7, "Bengaluru", 0, 0)) == "bengaluru-7.txt", "got '%s'",
        weather::exportFileName(made(7, "Bengaluru", 0, 0)).c_str());
  CHECK(weather::exportFileName(made(8, "\xE4\xB8\xAD\xE6\x96\x87", 0, 0)) == "place-8.txt",
        "a name with no ASCII still makes a usable file name");
}

// The point of Value: a field the model did not report must not become 0.
static void testAbsenceSurvivesToTheExport() {
  weather::Place place = made(3, "Bengaluru", 12.97f, 77.59f);
  weather::Reading reading;
  reading.code = 61;
  reading.temperature = 23.4f;
  reading.windSpeed = 11.0f;
  reading.windDirection = 205.0f;
  // humidity, pressure, visibility, gusts and UV are all left absent, which is
  // exactly what a model without them answers.

  const std::string report = weather::exportText(place, reading);
  CHECK(contains(report, "Bengaluru"), "the place is named");
  CHECK(contains(report, "Slight rain"), "the code is spelled out");
  CHECK(contains(report, "23.4 C"), "the temperature is there");
  CHECK(contains(report, "SSW"), "the wind direction is a compass point");
  CHECK(contains(report, "Light breeze"), "and it says what the wind does");

  // The absent ones draw NO LINE AT ALL rather than a zero.
  CHECK(!contains(report, "Humidity"), "an unreported humidity prints no row");
  CHECK(!contains(report, "Pressure"), "an unreported pressure prints no row");
  CHECK(!contains(report, "Visibility"), "an unreported visibility prints no row");
  CHECK(!contains(report, "UV index"), "an unreported UV index prints no row");
  CHECK(!contains(report, "0 hPa"), "and certainly not as a zero");

  // With them present, the rows appear and carry their plain-words gloss.
  reading.humidity = 64.0f;
  reading.pressure = 1012.5f;
  reading.visibility = 24000.0f;
  reading.uvIndex = 7.5f;
  const std::string full = weather::exportText(place, reading);
  CHECK(contains(full, "Humidity") && contains(full, "Comfortable"), "humidity gains its description");
  CHECK(contains(full, "Normal"), "pressure gains its description");
  CHECK(contains(full, "24.0 km") && contains(full, "Excellent"), "visibility is metres in, km out");
  CHECK(contains(full, "High"), "uv gains its description");
  CHECK(contains(full, "Open-Meteo"), "the report says where the numbers came from");
}

static void testExportCarriesTheWholeForecast() {
  weather::Place place = made(1, "Bengaluru", 12.97f, 77.59f);
  weather::Reading reading;
  reading.temperature = 21.0f;
  reading.code = 3;

  weather::Hour hour;
  hour.time = "2026-09-14T15:00";
  hour.code = 80;
  hour.temperature = 24.0f;
  hour.precipProbability = 40.0f;
  hour.windSpeed = 12.0f;
  reading.hours.push_back(hour);

  weather::Day day;
  day.date = "2026-09-14";
  day.code = 63;
  day.high = 27.0f;
  day.low = 19.0f;
  day.precipProbability = 70.0f;
  day.precipitationSum = 6.2f;
  day.windMax = 22.0f;
  day.sunrise = "2026-09-14T06:12";
  day.sunset = "2026-09-14T18:24";
  day.daylightSeconds = 43920.0f;
  reading.days.push_back(day);

  const std::string report = weather::exportText(place, reading);
  CHECK(contains(report, "NEXT HOURS"), "the hourly section is there");
  CHECK(contains(report, "15:00"), "an hour is a clock time");
  CHECK(contains(report, "THE WEEK"), "the daily section is there");
  CHECK(contains(report, "MON 14 SEP"), "a day is named");
  CHECK(contains(report, "sunrise 06:12"), "sunrise is a clock time");
  CHECK(contains(report, "12h 12m"), "and daylight is a duration");

  // An empty reading produces a report with no invented sections.
  weather::Reading empty;
  const std::string bare = weather::exportText(place, empty);
  CHECK(!contains(bare, "NEXT HOURS"), "no hours, no hourly heading");
  CHECK(!contains(bare, "THE WEEK"), "no days, no weekly heading");
  CHECK(!empty.valid(), "an empty reading knows it is empty");
}

int main() {
  testCodes();
  testCompass();
  testScales();
  testFormatting();
  testPlaceNames();
  testSanitize();
  testPlacesStore();
  testAbsenceSurvivesToTheExport();
  testExportCarriesTheWholeForecast();
  // The shape scripts_local/check.sh counts with grep -c "checks, 0 failed".
  std::printf("%s  weather core: %d checks, %d failed\n", failures ? "FAIL" : "ok  ", checks, failures);
  return failures == 0 ? 0 : 1;
}
