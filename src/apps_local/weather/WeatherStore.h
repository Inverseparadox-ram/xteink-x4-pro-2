#pragma once

// The card side of Weather: the places, the last forecast for each, and the
// readable export.
//
//   /.crosspoint/weather/places.txt   the saved places
//   /.crosspoint/weather/<id>.json    the last body fetched for that place
//   /Weather/<slug>-<id>.txt          the forecast as a report a person reads
//
// The cache is the RAW response, not a second serialization of the parsed
// model, and that is deliberate: it is re-read through the same
// parseForecast() the live fetch uses, so the stored copy and the live one
// cannot drift apart. It also means the app opens on the last forecast
// instantly and with no radio -- which on a panel that takes a second to
// repaint and a device that is usually asleep is the difference between an
// app you check and an app you wait for.

#include <cstdint>
#include <string>
#include <vector>

#include "WeatherCore.h"

namespace weather {

class Store {
 public:
  void load();
  bool save();

  std::vector<Place>& places() { return places_; }
  const std::vector<Place>& places() const { return places_; }
  Place* find(uint32_t id);

  // Appends a place and returns it, assigning the next id. The reference is
  // valid until the next add() or remove(); callers keep the id, not it.
  Place& add(const Place& place);
  void remove(uint32_t id);

  // True when this place is already saved, by coordinates rather than by name:
  // a geocoder answers several spellings for one town, and saving the same
  // point twice gives two rows that always agree.
  bool alreadySaved(const Place& place) const;

  // The cached body for a place, or empty when there is none.
  std::string cachedBody(uint32_t id) const;
  void writeCache(uint32_t id, const std::string& body);

  // Writes /Weather/<slug>-<id>.txt and removes the previous one when a rename
  // moved it. Never fails a save: an export that did not land is a missing
  // convenience, not lost data -- the forecast is on the service.
  void writeExport(Place& place, const Reading& reading);

  static const char* exportDirectory();

 private:
  void removeExport(const Place& place);

  std::vector<Place> places_;
  bool loaded_ = false;
};

}  // namespace weather
