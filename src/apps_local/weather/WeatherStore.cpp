#include "WeatherStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

namespace weather {
namespace {

constexpr const char* kDir = "/.crosspoint/weather";
constexpr const char* kPlaces = "/.crosspoint/weather/places.txt";
constexpr const char* kExportDir = "/Weather";
constexpr const char* kTag = "WEATHER";

bool readWholeFile(const char* path, std::string& out) {
  HalFile file;
  if (!Storage.openFileForRead(kTag, path, file)) return false;
  out.clear();
  char buffer[512];
  int n = 0;
  while ((n = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer))) > 0) {
    out.append(buffer, static_cast<size_t>(n));
  }
  file.close();
  return true;
}

bool writeWholeFile(const char* path, const std::string& text) {
  HalFile file;
  if (!Storage.openFileForWrite(kTag, path, file)) return false;
  const bool ok =
      file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == static_cast<int>(text.size());
  file.close();
  return ok;
}

// Write beside, then rename. A places file half-written by a card pulled
// mid-save is indistinguishable from one the user emptied.
bool writeAtomically(const char* path, const std::string& text) {
  const std::string temp = std::string(path) + ".part";
  if (!writeWholeFile(temp.c_str(), text)) {
    LOG_ERR(kTag, "cannot write %s", temp.c_str());
    return false;
  }
  Storage.remove(path);
  if (!Storage.rename(temp.c_str(), path)) {
    LOG_ERR(kTag, "cannot rename %s into place", temp.c_str());
    return false;
  }
  return true;
}

std::string cachePath(const uint32_t id) {
  char path[64];
  std::snprintf(path, sizeof(path), "%s/%lu.json", kDir, static_cast<unsigned long>(id));
  return path;
}

}  // namespace

const char* Store::exportDirectory() { return kExportDir; }

void Store::load() {
  if (loaded_) return;
  loaded_ = true;
  std::string text;
  if (!readWholeFile(kPlaces, text)) return;  // no places yet is not an error
  if (!parsePlaces(text, places_)) {
    // Left alone rather than replaced: it is the only record of places
    // somebody chose, and a build that cannot read it is not entitled to
    // delete it. The next save overwrites it, which is the user's decision.
    LOG_ERR(kTag, "%s is not in our format; leaving it alone", kPlaces);
    places_.clear();
    return;
  }
  LOG_INF(kTag, "loaded %d places", static_cast<int>(places_.size()));
}

Place* Store::find(const uint32_t id) {
  for (Place& place : places_) {
    if (place.id == id) return &place;
  }
  return nullptr;
}

Place& Store::add(const Place& place) {
  Place copy = place;
  copy.id = nextPlaceId(places_);
  copy.exportName.clear();
  places_.push_back(std::move(copy));
  return places_.back();
}

bool Store::alreadySaved(const Place& place) const {
  for (const Place& saved : places_) {
    // Roughly a kilometre. Two geocoder rows for one town differ in the fourth
    // decimal and in spelling; matching on the name would save both.
    if (std::fabs(saved.latitude - place.latitude) < 0.01f && std::fabs(saved.longitude - place.longitude) < 0.01f) {
      return true;
    }
  }
  return false;
}

void Store::remove(const uint32_t id) {
  const auto it = std::find_if(places_.begin(), places_.end(), [id](const Place& p) { return p.id == id; });
  if (it == places_.end()) return;
  removeExport(*it);
  Storage.remove(cachePath(id).c_str());
  places_.erase(it);
}

bool Store::save() {
  Storage.ensureDirectoryExists(kDir);
  if (!writeAtomically(kPlaces, serializePlaces(places_))) {
    LOG_ERR(kTag, "could not save %d places", static_cast<int>(places_.size()));
    return false;
  }
  return true;
}

std::string Store::cachedBody(const uint32_t id) const {
  std::string body;
  if (!readWholeFile(cachePath(id).c_str(), body)) return "";
  return body;
}

void Store::writeCache(const uint32_t id, const std::string& body) {
  Storage.ensureDirectoryExists(kDir);
  // Atomically, because a torn cache is read back on the next cold open and
  // would look like a forecast that arrived broken.
  writeAtomically(cachePath(id).c_str(), body);
}

void Store::writeExport(Place& place, const Reading& reading) {
  Storage.ensureDirectoryExists(kExportDir);
  const std::string name = exportFileName(place);
  if (!place.exportName.empty() && place.exportName != name) removeExport(place);
  const std::string path = std::string(kExportDir) + "/" + name;
  if (!writeWholeFile(path.c_str(), exportText(place, reading))) {
    LOG_ERR(kTag, "cannot export %s", path.c_str());
    return;
  }
  place.exportName = name;
}

void Store::removeExport(const Place& place) {
  if (place.exportName.empty()) return;
  const std::string path = std::string(kExportDir) + "/" + place.exportName;
  Storage.remove(path.c_str());
}

}  // namespace weather
