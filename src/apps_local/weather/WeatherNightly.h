#pragma once

// The nightly fetch, run on a wake nobody asked for. See WeatherCore.h, "The
// nightly fetch", for the schedule; this file is the part that touches the
// clock, the card and the radio.

#include <cstdint>

namespace weather {
namespace nightly {

// Seconds until the device should wake for the nightly fetch: at least 5, or
// 0 for "arm nothing" when there is no saved place or no clock to count by.
uint32_t secondsUntilDue();

// Fetches every saved place when the fetch is due, over the saved Wi-Fi, and
// rewrites each place's cache and report. True when a new forecast was
// written, which is the clock face's cue to redraw its weather line.
bool runIfDue();

}  // namespace nightly
}  // namespace weather
