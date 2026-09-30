#include "RemoteCore.h"

#include <cstring>

namespace remote {
namespace {

// HID keyboard usage ids (HUT 1.12, section 10).
constexpr uint8_t kKeyL = 0x0F;
constexpr uint8_t kKeyJ = 0x0D;
constexpr uint8_t kKeyRight = 0x4F;
constexpr uint8_t kKeyLeft = 0x50;

}  // namespace

const char* profileName(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      return "YOUTUBE";
    case Profile::Player:
      return "PLAYER KEYS";
    case Profile::MediaKey:
      return "SCRUB";
    case Profile::Count:
      break;
  }
  return "UNKNOWN";
}

KeyChord forwardChord(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      return KeyChord{0, kKeyL};
    case Profile::Player:
      return KeyChord{0, kKeyRight};
    case Profile::MediaKey:
    case Profile::Count:
      break;
  }
  return KeyChord{};  // no chord: the caller holds the media scrub key
}

KeyChord backChord(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      // The left arrow, not J. J is ten seconds back and the ask was five, and
      // five back with ten forward is the asymmetry people actually use: you
      // overshoot forward and nudge back.
      return KeyChord{0, kKeyLeft};
    case Profile::Player:
      return KeyChord{0, kKeyLeft};
    case Profile::MediaKey:
    case Profile::Count:
      break;
  }
  return KeyChord{};
}

const char* forwardSeconds(const Profile profile) {
  // Only YouTube defines the numbers, so only YouTube prints them. Under the
  // others the mark stands alone: "FWD" was a word doing an arrow's job, and
  // an arrow was already there.
  return profile == Profile::Browser ? "10" : nullptr;
}

const char* backSeconds(const Profile profile) { return profile == Profile::Browser ? "5" : nullptr; }

// --- Now playing ---------------------------------------------------------

namespace {

// Copies a field and makes it safe to hand to the renderer: cut to whole UTF-8
// characters, and every control character -- a newline in a track name is the
// realistic one -- replaced with a space so it cannot break the row's layout.
template <size_t N>
void copyField(const uint8_t* src, size_t len, char (&dst)[N]) {
  if (len > N - 1) len = N - 1;
  const size_t keep = utf8CompleteLength(reinterpret_cast<const char*>(src), len);
  for (size_t i = 0; i < keep; ++i) {
    const uint8_t c = src[i];
    dst[i] = (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
  }
  dst[keep] = '\0';
}

}  // namespace

size_t utf8CompleteLength(const char* text, const size_t len) {
  if (len == 0) return 0;
  // Walk back over continuation bytes (10xxxxxx) to the lead byte of the last
  // character, then check that the lead byte's sequence fits in what is left.
  size_t lead = len;
  size_t continuation = 0;
  while (lead > 0 && continuation < 4) {
    const uint8_t c = static_cast<uint8_t>(text[lead - 1]);
    if ((c & 0xC0) != 0x80) break;
    --lead;
    ++continuation;
  }
  if (lead == 0) return 0;  // nothing but continuation bytes: no character at all
  const uint8_t c = static_cast<uint8_t>(text[lead - 1]);
  size_t need = 1;
  if ((c & 0x80) == 0x00) {
    need = 1;
  } else if ((c & 0xE0) == 0xC0) {
    need = 2;
  } else if ((c & 0xF0) == 0xE0) {
    need = 3;
  } else if ((c & 0xF8) == 0xF0) {
    need = 4;
  } else {
    // Not a lead byte UTF-8 allows. Drop it and whatever trailed it.
    return lead - 1;
  }
  const size_t have = continuation + 1;
  return have >= need ? len : lead - 1;
}

bool decodeNowPlaying(const uint8_t* data, const size_t len, NowPlaying& out) {
  if (data == nullptr || len < 4 || len > kNowPlayingFrameMax) return false;
  if (data[0] != kNowPlayingVersion) return false;
  if (data[1] > static_cast<uint8_t>(NowPlayingState::Paused)) return false;

  const size_t titleLen = data[2];
  if (titleLen > kNowPlayingFieldMax || 3 + titleLen >= len) return false;
  const size_t artistLen = data[3 + titleLen];
  if (artistLen > kNowPlayingFieldMax) return false;
  if (3 + titleLen + 1 + artistLen != len) return false;

  // Everything checked before anything is written, so a bad frame leaves the
  // line on screen exactly as it was.
  NowPlaying next;
  next.state = static_cast<NowPlayingState>(data[1]);
  if (next.state != NowPlayingState::Nothing) {
    copyField(data + 3, titleLen, next.title);
    copyField(data + 3 + titleLen + 1, artistLen, next.artist);
  }
  out = next;
  return true;
}

size_t encodeNowPlaying(const NowPlaying& in, uint8_t* out, const size_t size) {
  const bool empty = in.state == NowPlayingState::Nothing;
  size_t titleLen = empty ? 0 : std::strlen(in.title);
  size_t artistLen = empty ? 0 : std::strlen(in.artist);
  if (titleLen > kNowPlayingFieldMax) titleLen = kNowPlayingFieldMax;
  if (artistLen > kNowPlayingFieldMax) artistLen = kNowPlayingFieldMax;
  titleLen = utf8CompleteLength(in.title, titleLen);
  artistLen = utf8CompleteLength(in.artist, artistLen);

  const size_t need = 3 + titleLen + 1 + artistLen;
  if (out == nullptr || size < need) return 0;
  out[0] = kNowPlayingVersion;
  out[1] = static_cast<uint8_t>(in.state);
  out[2] = static_cast<uint8_t>(titleLen);
  std::memcpy(out + 3, in.title, titleLen);
  out[3 + titleLen] = static_cast<uint8_t>(artistLen);
  std::memcpy(out + 3 + titleLen + 1, in.artist, artistLen);
  return need;
}

bool sameNowPlaying(const NowPlaying& a, const NowPlaying& b) {
  return a.state == b.state && std::strcmp(a.title, b.title) == 0 && std::strcmp(a.artist, b.artist) == 0;
}

// --- The microphone button -----------------------------------------------

void encodeCommand(const MacCommand command, uint8_t out[kCommandLen]) {
  out[0] = kMacLinkVersion;
  out[1] = static_cast<uint8_t>(command);
}

uint8_t serviceCheck(const char* title) {
  // FNV-1a over the title's bytes, low byte. The Swift helper computes the
  // same over the first kStatusTitleMax bytes of the name it sent.
  uint32_t hash = 2166136261u;
  for (const char* p = title; p != nullptr && *p != '\0'; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619u;
  }
  return static_cast<uint8_t>(hash & 0xFF);
}

void encodeRestart(const uint8_t row, const char* title, uint8_t out[kRestartLen]) {
  out[0] = kMacLinkVersion;
  out[1] = kCommandRestartService;
  out[2] = row;
  out[3] = serviceCheck(title);
}

bool decodeCommand(const uint8_t* data, const size_t len, MacCommand& out) {
  if (data == nullptr || len != kCommandLen || data[0] != kMacLinkVersion) return false;
  if (data[1] != static_cast<uint8_t>(MacCommand::MuteMicrophones) &&
      data[1] != static_cast<uint8_t>(MacCommand::UnmuteMicrophones)) {
    return false;
  }
  out = static_cast<MacCommand>(data[1]);
  return true;
}

void encodeMacState(const MacState& state, uint8_t out[kMacStateLen]) {
  out[0] = kMacLinkVersion;
  out[1] = state.microphonesMuted ? kMacStateMicMuted : 0;
}

bool decodeMacState(const uint8_t* data, const size_t len, MacState& out) {
  // Exactly two bytes today. A longer frame from a newer helper is refused
  // rather than half-read, so a flag this reader does not know about can
  // never be mistaken for one it does.
  if (data == nullptr || len != kMacStateLen || data[0] != kMacLinkVersion) return false;
  out.known = true;
  out.microphonesMuted = (data[1] & kMacStateMicMuted) != 0;
  return true;
}

// --- Status boards --------------------------------------------------------

const char* statusWord(const StatusCode code) {
  switch (code) {
    case StatusCode::InProcess:
      return "in process";
    case StatusCode::AwaitingInput:
      return "awaiting input";
    case StatusCode::Completed:
      return "completed";
    case StatusCode::Failed:
      return "failed";
    case StatusCode::Running:
      return "running";
    case StatusCode::Stopped:
      return "stopped";
    case StatusCode::Unknown:
      break;
  }
  return "unknown";
}

bool statusNeedsAttention(const StatusCode code) {
  return code == StatusCode::AwaitingInput || code == StatusCode::Failed;
}

size_t encodeStatusRow(const StatusBoardId board, const uint8_t count, const uint8_t index, const StatusRow& row,
                       uint8_t* out, const size_t size) {
  size_t titleLen = std::strlen(row.title);
  size_t detailLen = std::strlen(row.detail);
  if (titleLen > kStatusTitleMax) titleLen = kStatusTitleMax;
  if (detailLen > kStatusDetailMax) detailLen = kStatusDetailMax;
  titleLen = utf8CompleteLength(row.title, titleLen);
  detailLen = utf8CompleteLength(row.detail, detailLen);
  const size_t need = 6 + titleLen + 1 + detailLen;
  if (out == nullptr || size < need) return 0;
  out[0] = kStatusVersion;
  out[1] = static_cast<uint8_t>(board);
  out[2] = count;
  out[3] = index;
  out[4] = static_cast<uint8_t>(row.status);
  out[5] = static_cast<uint8_t>(titleLen);
  std::memcpy(out + 6, row.title, titleLen);
  out[6 + titleLen] = static_cast<uint8_t>(detailLen);
  std::memcpy(out + 6 + titleLen + 1, row.detail, detailLen);
  return need;
}

bool StatusAssembler::feed(const uint8_t* data, const size_t len) {
  if (data == nullptr || len < 7 || len > kStatusFrameMax || data[0] != kStatusVersion) return false;
  const uint8_t boardId = data[1];
  if (boardId != static_cast<uint8_t>(StatusBoardId::Claude) &&
      boardId != static_cast<uint8_t>(StatusBoardId::Services)) {
    return false;
  }
  const size_t b = boardId - 1;
  const uint8_t count = data[2];
  const uint8_t index = data[3];
  const uint8_t status = data[4];
  const size_t titleLen = data[5];
  if (count > kStatusRowsMax || status > static_cast<uint8_t>(StatusCode::Stopped)) return false;
  if (titleLen > kStatusTitleMax || 6 + titleLen >= len) return false;
  const size_t detailLen = data[6 + titleLen];
  if (detailLen > kStatusDetailMax || 6 + titleLen + 1 + detailLen != len) return false;

  if (count == 0) {
    // An empty board is one frame, and it is complete on arrival.
    shown_[b] = StatusBoard{};
    shown_[b].known = true;
    expected_[b] = 0;
    return true;
  }
  if (index >= count) return false;
  if (index == 0) {
    pending_[b] = StatusBoard{};
    pending_[b].count = count;
    expected_[b] = 0;
  }
  if (index != expected_[b] || pending_[b].count != count) {
    // Out of step: drop what was collected and wait for the next board.
    expected_[b] = 0;
    pending_[b] = StatusBoard{};
    return false;
  }
  StatusRow& row = pending_[b].rows[index];
  row.status = static_cast<StatusCode>(status);
  copyField(data + 6, titleLen, row.title);
  copyField(data + 6 + titleLen + 1, detailLen, row.detail);
  expected_[b] = static_cast<uint8_t>(index + 1);
  if (expected_[b] < count) return false;
  shown_[b] = pending_[b];
  shown_[b].known = true;
  expected_[b] = 0;
  return true;
}

const StatusBoard& StatusAssembler::board(const StatusBoardId id) const {
  return shown_[id == StatusBoardId::Claude ? 0 : 1];
}

void StatusAssembler::forget() {
  for (size_t b = 0; b < 2; ++b) {
    shown_[b] = StatusBoard{};
    pending_[b] = StatusBoard{};
    expected_[b] = 0;
  }
}

}  // namespace remote
