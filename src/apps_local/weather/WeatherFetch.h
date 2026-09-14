#pragma once

// Talking to Open-Meteo.
//
// ---------------------------------------------------------------------------
// Why Open-Meteo, out of everything that sells weather data.
//
// NO API KEY. That is the whole argument and the rest is detail. A key means a
// signup, a secret stored on a card that is also a USB drive, and a screen for
// typing 32 hex characters on an on-screen keyboard -- and the first time the
// key expires the app is a brick with a nice font. Open-Meteo answers an
// anonymous GET.
//
// It is also not a scraper's target of convenience: it publishes the output of
// national weather services (DWD ICON, NOAA GFS, ECMWF, Meteo-France), which
// is the same modelling everyone else resells, and it is free for
// non-commercial use. And it ships its own geocoding endpoint, so "choose a
// place" needs no second provider with a second set of terms.
//
// Two endpoints, and both are plain GETs:
//   api.open-meteo.com/v1/forecast        the numbers
//   geocoding-api.open-meteo.com/v1/search  the places
// ---------------------------------------------------------------------------
//
// This layer owns ArduinoJson and the HTTP client; WeatherCore owns the model
// and knows about neither. That is the split HackerNews and Instapaper use and
// it is what keeps the judgement host-testable.

#include <string>
#include <vector>

#include "WeatherCore.h"

namespace weather {

// Fetches the forecast for `place` and fills `reading`. On failure returns
// false and puts a sentence in `message` that is fit to show a person -- the
// screens print it verbatim, so it says what to do rather than what the code
// saw.
//
// `rawJson`, when non-null, receives the response body so the caller can cache
// it. The cache is re-parsed through parseForecast() rather than through a
// second format, so the stored copy and the live one cannot drift.
bool fetchForecast(const Place& place, Reading& reading, std::string& message, std::string* rawJson = nullptr);

// Searches Open-Meteo's geocoder. Returns true when the request succeeded,
// even if nothing matched -- an empty `results` is an answer, not a failure,
// and the screen says so differently.
bool searchPlaces(const std::string& query, std::vector<Place>& results, std::string& message);

// The parser, exposed so a cached body can be re-read on a device with no
// network. Returns false when the body is not a forecast at all; a body
// missing individual variables parses to whatever it carried, because that is
// the normal case (see WeatherCore's decision 1).
bool parseForecast(const std::string& body, Reading& reading, std::string& message);

}  // namespace weather
