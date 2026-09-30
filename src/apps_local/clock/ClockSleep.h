#pragma once

// The Clock sleep screen: the time, the date and the month, repainted once a
// minute while the device sleeps.
//
// A sleeping device repaints by waking itself. The deep-sleep timer is armed
// for one second past the next minute; the wake boots only as far as the
// display (main.cpp's unattended path, the one Live's scheduled check uses),
// draws the new minute and goes straight back down. See ClockCore.h, "The
// sleep screen", for how that timer is shared with Live and why the wake is
// told whose it was.
//
// The refresh is a FAST one over the minute before it, so the panel does not
// flash every minute. After a deep sleep the controller's idea of what is on
// the glass cannot be trusted, so the previous minute is drawn again first as
// the baseline -- it is exactly what is already on the glass, so that pass
// changes nothing visible -- and the new minute is diffed against it. Once an
// hour, and on the sleep a person asked for, it is a clean HALF refresh
// instead, which is what clears the ghosting fast refreshes leave.

#include <cstdint>

class GfxRenderer;

namespace clockapp {
namespace sleep {

// Whether the Clock sleep screen is the one chosen in Settings.
bool enabled();

// Set by main.cpp before the sleep screen is drawn: true on a wake nobody
// asked for, which is what allows the fast refresh.
void setUnattended(bool unattended);

// Draws the clock and puts it on the panel.
void draw(GfxRenderer& renderer);

// Whether the minute on the glass is not the minute it is now. False when the
// Clock sleep screen is off.
bool repaintDue();

// The RTC alarm to arm, in microseconds, for Live's number (0 = none) and the
// build's fallback, with the clock's own minute folded in. Records whether the
// wake it ends belongs to Live (liveOwnsTimer). Every deep sleep arms through
// this, so the clock never loses its minute to a sleep that forgot it.
uint64_t armMicros(uint32_t liveSeconds, uint64_t fallbackMicros);

// Whether the timer that ended this sleep was armed for Live. True when
// nothing is known, which is how every timer wake behaved before the clock.
bool liveOwnsTimer();

// A boot that shows somebody a UI has put something else on the glass, so the
// next minute must not diff against a face that is no longer there.
void forgetGlass();

}  // namespace sleep
}  // namespace clockapp
