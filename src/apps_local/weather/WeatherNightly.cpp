#include "WeatherNightly.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <ctime>
#include <string>

#include "../live/LiveEngine.h"
#include "WeatherCore.h"
#include "WeatherFetch.h"
#include "WeatherStore.h"

namespace weather {
namespace nightly {
namespace {

constexpr const char* kTag = "WXNIGHT";
constexpr const char* kPath = "/.crosspoint/weather/nightly.txt";
// A long list is the owner's choice, but a wake that fetches twenty places is
// a minute of radio at 1am. The first few are what a morning looks at.
constexpr size_t kMaxNightlyPlaces = 4;

struct Now {
  std::string today;  // "2026-09-30", local
  int32_t secondsIntoDay = 0;
  int64_t epoch = 0;
};

bool readNow(Now& out) {
  struct tm local = {};
  if (!halClock.localTime(local) || local.tm_year < 120) return false;
  char date[40];
  std::snprintf(date, sizeof(date), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  out.today = date;
  out.secondsIntoDay = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
  out.epoch = static_cast<int64_t>(std::time(nullptr));
  return true;
}

Nightly load() {
  Nightly nightly;
  HalFile file;
  if (!Storage.openFileForRead(kTag, kPath, file)) return nightly;
  char buffer[128] = {};
  const int n = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer) - 1);
  if (n > 0) parseNightly(std::string(buffer, static_cast<size_t>(n)), nightly);
  return nightly;
}

void save(const Nightly& nightly) {
  HalFile file;
  if (!Storage.openFileForWrite(kTag, kPath, file)) {
    LOG_ERR(kTag, "cannot write %s", kPath);
    return;
  }
  const std::string text = serializeNightly(nightly);
  file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool anyPlace() {
  Store store;
  store.load();
  return !store.places().empty();
}

}  // namespace

uint32_t secondsUntilDue() {
  Now now;
  if (!readNow(now) || !anyPlace()) return 0;
  const uint32_t wait = nightlySecondsUntilDue(load(), now.today, now.secondsIntoDay, now.epoch);
  return wait < 5 ? 5 : wait;
}

bool runIfDue() {
  Now now;
  if (!readNow(now)) return false;
  Store store;
  store.load();
  if (store.places().empty()) return false;
  Nightly nightly = load();
  if (nightlySecondsUntilDue(nightly, now.today, now.secondsIntoDay, now.epoch) != 0) return false;

  // The attempt is written down first, so a wake that dies in the radio does
  // not come straight back and try again: the retry rule counts from here.
  nightly.lastAttempt = now.epoch;
  save(nightly);

  std::string message;
  live::engine::RadioLease lease(message);
  if (!lease.held()) {
    LOG_ERR(kTag, "no network: %s", message.c_str());
    return false;
  }
  size_t fetched = 0;
  const size_t count = store.places().size() < kMaxNightlyPlaces ? store.places().size() : kMaxNightlyPlaces;
  for (size_t i = 0; i < count; ++i) {
    Place& place = store.places()[i];
    Reading reading;
    std::string body;
    if (!fetchForecast(place, reading, message, &body)) {
      LOG_ERR(kTag, "%s: %s", place.name.c_str(), message.c_str());
      continue;
    }
    reading.fetchedAt = static_cast<uint32_t>(now.epoch);
    store.writeCache(place.id, body);
    store.writeExport(place, reading);
    ++fetched;
  }
  if (fetched > 0) {
    store.save();
    nightly.doneDate = now.today;
    save(nightly);
  }
  LOG_INF(kTag, "nightly fetch: %u of %u places", static_cast<unsigned>(fetched), static_cast<unsigned>(count));
  return fetched > 0;
}

}  // namespace nightly
}  // namespace weather
