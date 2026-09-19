// Freestanding tests for RemoteCore: the seek profiles, the volume arithmetic
// and the words on the buttons.
//
// The thing under test is HONESTY. This app cannot read anything back from the
// host -- not the track, not the volume, not whether the music is playing --
// so every string and every number here is a claim the remote makes on its own
// authority. The tests are mostly about not making claims it cannot keep.

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/apps_local/remote/RemoteCore.h"

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

static bool has(const char* haystack, const char* needle) { return std::strstr(haystack, needle) != nullptr; }

// Only the browser profile knows what "+10s" means, because only YouTube
// defines it. The others must not print a number they cannot keep.
static void testOnlyTheProfileThatKnowsTheNumbersPrintsThem() {
  CHECK(std::string(remote::forwardLabel(remote::Profile::Browser)) == "+10s", "browser forward is exact");
  CHECK(std::string(remote::backLabel(remote::Profile::Browser)) == "-5s", "browser back is exact");

  for (const remote::Profile p : {remote::Profile::Player, remote::Profile::MediaKey}) {
    CHECK(!has(remote::forwardLabel(p), "10"), "a profile that cannot promise 10s does not print it");
    CHECK(!has(remote::backLabel(p), "5s"), "a profile that cannot promise 5s does not print it");
  }
  // And every profile says, in one line, what its buttons will really do.
  for (int i = 0; i < static_cast<int>(remote::Profile::Count); ++i) {
    const remote::Profile p = static_cast<remote::Profile>(i);
    CHECK(remote::profileName(p)[0] != '\0', "profile %d is named", i);
    CHECK(remote::profileNote(p)[0] != '\0', "profile %d explains itself", i);
    CHECK(std::string(remote::profileName(p)) != "UNKNOWN", "profile %d is a real profile", i);
  }
}

// The browser profile is the whole reason the seek buttons are keystrokes: L
// is ten seconds on in YouTube and the left arrow is five back, which is
// exactly the asymmetry that was asked for.
static void testBrowserSeekTypesTheYouTubeKeys() {
  const remote::KeyChord fwd = remote::forwardChord(remote::Profile::Browser);
  const remote::KeyChord back = remote::backChord(remote::Profile::Browser);
  CHECK(fwd.key == 0x0F, "forward is L, got 0x%02X", fwd.key);
  CHECK(back.key == 0x50, "back is the left arrow, got 0x%02X", back.key);
  CHECK(fwd.modifiers == 0 && back.modifiers == 0, "no modifiers: YouTube's are bare keys");

  const remote::KeyChord playerFwd = remote::forwardChord(remote::Profile::Player);
  CHECK(playerFwd.key == 0x4F, "the player profile types the right arrow");

  // The fallback has no chord at all, which is how the Activity knows to hold
  // the scrub key instead. A chord here would silently type into whatever has
  // focus.
  CHECK(remote::forwardChord(remote::Profile::MediaKey).key == 0, "the scrub profile sends no keystroke");
  CHECK(remote::backChord(remote::Profile::MediaKey).key == 0, "in both directions");
}

static void testVolumeIsRelativeAndResyncsAtTheEnds() {
  CHECK(remote::volumeStepsBetween(8, 11) == 3, "three notches up is three presses");
  CHECK(remote::volumeStepsBetween(11, 8) == -3, "and three down is three the other way");
  CHECK(remote::volumeStepsBetween(8, 8) == 0, "no move, no presses");

  // The ends are the resync. The remote's count is a guess the moment anyone
  // touches the Mac's own volume, and only a full sixteen lands somewhere both
  // sides agree about.
  CHECK(remote::volumeStepsBetween(8, 0) == -remote::kVolumeSteps, "dragging to 0 sends a full sixteen down");
  CHECK(remote::volumeStepsBetween(15, 16) == remote::kVolumeSteps, "and to the top, a full sixteen up");
  CHECK(remote::volumeStepsBetween(0, 0) == -remote::kVolumeSteps, "even when it thinks it is already there");

  CHECK(remote::clampVolume(-5) == 0, "clamped below");
  CHECK(remote::clampVolume(999) == remote::kVolumeSteps, "clamped above");
  // Sixteen, because that is how many steps macOS itself moves in: one notch
  // here has to be one press there or a drag lands somewhere else.
  CHECK(remote::kVolumeSteps == 16, "the slider matches macOS's own granularity");
}

static void testTheVolumeCaptionNamesItsOwnCount() {
  char buffer[32];
  CHECK(std::string(remote::formatVolume(11, buffer, sizeof(buffer))) == "VOLUME 11 / 16", "got '%s'", buffer);
  CHECK(std::string(remote::formatVolume(-3, buffer, sizeof(buffer))) == "VOLUME 0 / 16", "clamped in the caption");
  // "/ 16" is the load-bearing half: it reads as the remote's own sixteen
  // notches rather than as a percentage of the Mac's volume, which is a number
  // this app has no way to know.
  CHECK(has(remote::formatVolume(8, buffer, sizeof(buffer)), "/ 16"), "the caption says what the scale is");
}

int main() {
  testOnlyTheProfileThatKnowsTheNumbersPrintsThem();
  testBrowserSeekTypesTheYouTubeKeys();
  testVolumeIsRelativeAndResyncsAtTheEnds();
  testTheVolumeCaptionNamesItsOwnCount();
  std::printf("%s  remote core: %d checks, %d failed\n", failures ? "FAIL" : "ok  ", checks, failures);
  return failures == 0 ? 0 : 1;
}
