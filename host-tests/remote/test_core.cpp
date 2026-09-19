// Freestanding tests for RemoteCore: the seek profiles and the three Mac
// shortcuts.
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

// Only the browser profile knows what ten and five seconds mean, because only
// YouTube defines them. The others print no number at all -- the face is the
// arrow alone rather than a promise the host decides.
static void testOnlyTheProfileThatKnowsTheNumbersPrintsThem() {
  CHECK(std::string(remote::forwardSeconds(remote::Profile::Browser)) == "10", "browser forward is exactly ten");
  CHECK(std::string(remote::backSeconds(remote::Profile::Browser)) == "5", "browser back is exactly five");

  for (const remote::Profile p : {remote::Profile::Player, remote::Profile::MediaKey}) {
    CHECK(remote::forwardSeconds(p) == nullptr, "a profile that cannot promise a number prints none");
    CHECK(remote::backSeconds(p) == nullptr, "in both directions");
  }

  // The profile name is the one word left on the screen, so it has to carry
  // the whole distinction on its own.
  for (int i = 0; i < static_cast<int>(remote::Profile::Count); ++i) {
    const remote::Profile p = static_cast<remote::Profile>(i);
    CHECK(remote::profileName(p)[0] != '\0', "profile %d is named", i);
    CHECK(std::string(remote::profileName(p)) != "UNKNOWN", "profile %d is a real profile", i);
    CHECK(std::strlen(remote::profileName(p)) <= 12, "profile %d's name fits the footer", i);
  }
  CHECK(std::string(remote::profileName(remote::Profile::Browser)) == "YOUTUBE", "the browser profile names the site");
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

// Siri and Claude ride the SAME chord, and only the hold tells them apart --
// which is exactly how macOS itself separates Siri from Spotlight. If these
// ever diverge, one of the two buttons is sending something a stock Mac has
// never been told about.
static void testSiriAndClaudeShareCommandSpace() {
  const remote::KeyChord cmdSpace = remote::commandSpace();
  CHECK(cmdSpace.key == 0x2C, "the key is Space, got 0x%02X", cmdSpace.key);
  CHECK(cmdSpace.modifiers == 8, "with Command and nothing else, got %d", cmdSpace.modifiers);

  // Long enough that macOS reads it as a hold rather than a Spotlight tap, and
  // with margin for the BLE round trip at both ends.
  CHECK(remote::kSiriHoldMs >= 1000, "the Siri hold clears macOS's own one second");
}

// Do Not Disturb is the one button with no stock shortcut behind it, so the
// chord has to be one nothing else claims -- a user who binds it must not find
// they have broken something they already had.
static void testDoNotDisturbIsAChordNothingElseClaims() {
  const remote::KeyChord dnd = remote::doNotDisturbChord();
  CHECK(dnd.key == 0x07, "the key is D, got 0x%02X", dnd.key);
  // Control + Option + Command, all three. Two-modifier chords are where
  // macOS and the common applications put their own shortcuts.
  CHECK(dnd.modifiers == (1 | 4 | 8), "Control-Option-Command, got %d", dnd.modifiers);

  // And it must not collide with the one other chord this app sends.
  const remote::KeyChord cmdSpace = remote::commandSpace();
  CHECK(!(dnd.key == cmdSpace.key && dnd.modifiers == cmdSpace.modifiers), "the two chords are distinct");
}

// The Claude button types this into Spotlight, so it has to be typeable: the
// keyboard report this app builds covers letters, digits and space, and
// nothing else.
static void testTheClaudeQueryIsTypeable() {
  const char* query = remote::kClaudeQuery;
  CHECK(query != nullptr && query[0] != '\0', "there is something to type");
  for (const char* c = query; *c != '\0'; ++c) {
    const bool typeable = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == ' ';
    CHECK(typeable, "'%c' is a character the keyboard report can carry", *c);
  }
}

int main() {
  testOnlyTheProfileThatKnowsTheNumbersPrintsThem();
  testBrowserSeekTypesTheYouTubeKeys();
  testSiriAndClaudeShareCommandSpace();
  testDoNotDisturbIsAChordNothingElseClaims();
  testTheClaudeQueryIsTypeable();
  std::printf("%s  remote core: %d checks, %d failed\n", failures ? "FAIL" : "ok  ", checks, failures);
  return failures == 0 ? 0 : 1;
}
