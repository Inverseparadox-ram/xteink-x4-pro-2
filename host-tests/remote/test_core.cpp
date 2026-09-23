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

// --- Now playing -----------------------------------------------------------

static remote::NowPlaying track(const remote::NowPlayingState state, const char* title, const char* artist) {
  remote::NowPlaying np;
  np.state = state;
  std::snprintf(np.title, sizeof(np.title), "%s", title);
  std::snprintf(np.artist, sizeof(np.artist), "%s", artist);
  return np;
}

// The bytes the Swift helper has to produce, pinned. If this test changes, the
// helper has to change with it or the reader stops understanding it.
static void testTheNowPlayingFrameIsPinnedByteForByte() {
  const remote::NowPlaying np = track(remote::NowPlayingState::Playing, "Hey", "Me");
  uint8_t frame[remote::kNowPlayingFrameMax];
  const size_t len = remote::encodeNowPlaying(np, frame, sizeof(frame));
  const uint8_t want[] = {1, 1, 3, 'H', 'e', 'y', 2, 'M', 'e'};
  CHECK(len == sizeof(want), "frame is %zu bytes, want %zu", len, sizeof(want));
  CHECK(std::memcmp(frame, want, sizeof(want)) == 0, "frame bytes match the documented layout");

  remote::NowPlaying back;
  CHECK(remote::decodeNowPlaying(frame, len, back), "a frame it wrote is a frame it reads");
  CHECK(remote::sameNowPlaying(np, back), "and it round-trips");
  CHECK(back.present(), "a playing track with a title is worth drawing");
}

// A frame that fails to parse must leave what is on screen alone. Blanking it
// would read as the music stopping; half-overwriting it would print a title
// from one song beside the artist of another.
static void testABadFrameChangesNothing() {
  remote::NowPlaying shown = track(remote::NowPlayingState::Playing, "Keep", "This");
  const remote::NowPlaying before = shown;

  const uint8_t wrongVersion[] = {2, 1, 1, 'x', 0};
  const uint8_t badState[] = {1, 7, 1, 'x', 0};
  const uint8_t titleOverruns[] = {1, 1, 9, 'x', 0};
  const uint8_t trailingJunk[] = {1, 1, 1, 'x', 0, 'z'};
  const uint8_t tooShort[] = {1, 1, 0};
  uint8_t tooLong[remote::kNowPlayingFrameMax + 1] = {1, 1, 0, 0};

  CHECK(!remote::decodeNowPlaying(wrongVersion, sizeof(wrongVersion), shown), "a version it does not speak");
  CHECK(!remote::decodeNowPlaying(badState, sizeof(badState), shown), "a state it does not know");
  CHECK(!remote::decodeNowPlaying(titleOverruns, sizeof(titleOverruns), shown), "a title longer than the frame");
  CHECK(!remote::decodeNowPlaying(trailingJunk, sizeof(trailingJunk), shown), "bytes after the artist");
  CHECK(!remote::decodeNowPlaying(tooShort, sizeof(tooShort), shown), "a frame with no artist length");
  CHECK(!remote::decodeNowPlaying(tooLong, sizeof(tooLong), shown), "a frame longer than any valid one");
  CHECK(!remote::decodeNowPlaying(nullptr, 4, shown), "no frame at all");
  CHECK(remote::sameNowPlaying(shown, before), "and after all of that the screen still says what it said");
}

// Nothing playing clears the line, and a frame that says so carries no text
// the reader should believe even if a buggy sender included some.
static void testNothingPlayingClearsTheLine() {
  const uint8_t nothing[] = {1, 0, 3, 'o', 'l', 'd', 0};
  remote::NowPlaying np = track(remote::NowPlayingState::Playing, "Old", "Song");
  CHECK(remote::decodeNowPlaying(nothing, sizeof(nothing), np), "a Nothing frame parses");
  CHECK(np.state == remote::NowPlayingState::Nothing, "and says nothing is playing");
  CHECK(np.title[0] == '\0' && np.artist[0] == '\0', "with no stale text left behind");
  CHECK(!np.present(), "so the row draws nothing");

  const remote::NowPlaying untitled = track(remote::NowPlayingState::Playing, "", "Someone");
  CHECK(!untitled.present(), "a player reporting Playing with no name has nothing to show");

  const remote::NowPlaying paused = track(remote::NowPlayingState::Paused, "Held", "");
  CHECK(paused.present(), "a paused track is still the one that is loaded, so it stays");
}

// Titles are cut at 64 BYTES, and a byte limit is exactly where a Japanese or
// an accented title splits a character in half. Half a character is a glyph
// the font draws as garbage, so the cut has to fall on a boundary.
static void testALongTitleIsCutOnACharacterNeverThroughOne() {
  // U+3042 HIRAGANA A is three bytes. Twenty-two of them is 66 bytes, so a
  // plain 64-byte cut would leave one and two-thirds of the last character.
  std::string hiragana;
  for (int i = 0; i < 22; ++i) hiragana += "\xE3\x81\x82";
  remote::NowPlaying np;
  np.state = remote::NowPlayingState::Playing;
  std::memcpy(np.title, hiragana.data(), remote::kNowPlayingFieldMax);
  np.title[remote::kNowPlayingFieldMax] = '\0';

  uint8_t frame[remote::kNowPlayingFrameMax];
  const size_t len = remote::encodeNowPlaying(np, frame, sizeof(frame));
  CHECK(len > 0, "a long title still encodes");
  CHECK(frame[2] == 63, "cut to 63 bytes -- 21 whole characters -- not 64, got %u", frame[2]);

  remote::NowPlaying back;
  CHECK(remote::decodeNowPlaying(frame, len, back), "and decodes");
  CHECK(std::strlen(back.title) == 63, "with no half-character left on the end");

  CHECK(remote::utf8CompleteLength("ab", 2) == 2, "ASCII is always complete");
  CHECK(remote::utf8CompleteLength("\xC3\xA9", 2) == 2, "a whole e-acute is kept");
  CHECK(remote::utf8CompleteLength("a\xC3", 2) == 1, "half an e-acute is dropped");
  CHECK(remote::utf8CompleteLength("a\xE3\x81", 3) == 1, "two-thirds of a kana is dropped");
  CHECK(remote::utf8CompleteLength("\xF0\x9F\x8E\xB5", 4) == 4, "a whole emoji is kept");
  CHECK(remote::utf8CompleteLength("\xF0\x9F\x8E", 3) == 0, "three-quarters of one is not");
  CHECK(remote::utf8CompleteLength("", 0) == 0, "nothing is nothing");
}

// A newline in a track name is the realistic control character, and one that
// reached the renderer would push the artist line off the row.
static void testControlCharactersNeverReachTheScreen() {
  const uint8_t frame[] = {1, 1, 5, 'A', '\n', 'B', '\t', 'C', 1, '\r'};
  remote::NowPlaying np;
  CHECK(remote::decodeNowPlaying(frame, sizeof(frame), np), "a title with control characters parses");
  CHECK(std::strcmp(np.title, "A B C") == 0, "and they become spaces, got '%s'", np.title);
  CHECK(std::strcmp(np.artist, " ") == 0, "in the artist too");
}

int main() {
  testOnlyTheProfileThatKnowsTheNumbersPrintsThem();
  testBrowserSeekTypesTheYouTubeKeys();
  testSiriAndClaudeShareCommandSpace();
  testTheClaudeQueryIsTypeable();
  testTheNowPlayingFrameIsPinnedByteForByte();
  testABadFrameChangesNothing();
  testNothingPlayingClearsTheLine();
  testALongTitleIsCutOnACharacterNeverThroughOne();
  testControlCharactersNeverReachTheScreen();
  std::printf("%s  remote core: %d checks, %d failed\n", failures ? "FAIL" : "ok  ", checks, failures);
  return failures == 0 ? 0 : 1;
}
