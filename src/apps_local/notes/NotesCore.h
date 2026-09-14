#pragma once

// Notes: the model, the store format, and the readable export.
//
// Freestanding C++17 -- no renderer, no Activity, no storage -- so
// host-tests/notes/ builds it with nothing but a compiler. Everything that
// can be got wrong about a note (what its title is when the user never typed
// one, what happens to a tab inside an item, which file it exports as after a
// rename) is decided here, where a test can see it.
//
// ---------------------------------------------------------------------------
// Two decisions worth stating.
//
// 1. A text note has no separate title field. Its FIRST LINE is its title,
//    which is how every plain-text note on every machine already works. That
//    removes a screen (a title prompt before the body), a control (rename),
//    and the class of bug where the two disagree. A checklist does carry a
//    name, because its body is rows rather than prose and there is no first
//    line to borrow.
//
// 2. The store is a line format, not JSON. ArduinoJson is already linked, so
//    this is not about the dependency -- it is that a half-written JSON file
//    is unparseable in its entirety, while a truncated line format loses the
//    tail and keeps every note before it. Notes are the one thing on this
//    device the user typed by hand and cannot re-sync from anywhere.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace notes {

enum class Kind : uint8_t { Text = 0, Checklist = 1 };

// Caps, all enforced at the edges (the Activity clamps what the keyboard
// returns; parse() drops what overruns). They are bounds, not budgets: the
// point is that a corrupt or hostile file on the card cannot make this app
// allocate without limit on a device with no OOM story worth the name.
inline constexpr size_t kMaxNotes = 200;
inline constexpr size_t kMaxItems = 120;
inline constexpr size_t kMaxTitleChars = 96;
inline constexpr size_t kMaxItemChars = 160;
inline constexpr size_t kMaxBodyChars = 4000;

struct Item {
  std::string text;
  bool done = false;
};

struct Note {
  uint32_t id = 0;
  Kind kind = Kind::Text;
  // Checklists only. A text note's title is the first line of its body; see
  // displayTitle().
  std::string name;
  std::string body;
  std::vector<Item> items;
  // Epoch seconds, or 0 when the clock has never been set. Zero is kept as
  // zero rather than faked: a note stamped with a wrong time sorts wrongly
  // forever, and the list says "NO DATE" honestly instead.
  uint32_t updated = 0;
  // The /Notes file this note was last exported as, so a rename can delete the
  // old one instead of leaving a stale copy on the card beside the new. Empty
  // until the first successful export.
  std::string exportName;
};

// How many items are ticked, and how many there are. Checklists only; a text
// note answers 0 of 0.
int doneCount(const Note& note);

// The name a list row shows. A checklist's own name; a text note's first
// non-empty line; "UNTITLED" when there is neither. Never empty, because a
// row with no label is a row nobody can tell apart from the one above it.
std::string displayTitle(const Note& note);

// The body below the title line, for a text note's reading screen. A text
// note's first line is its title and the band already carries it, so drawing
// it again at the top of the body is the same string twice on one screen.
std::string displayBody(const Note& note);

// --- The store file ------------------------------------------------------

// Serialize every note, newest-first order preserved as given.
std::string serialize(const std::vector<Note>& notes);

// Parse a store file. Returns false only when the text is not this format at
// all (no recognisable header); a file with damaged lines parses to whatever
// survived, which is the whole reason for a line format. Notes beyond
// kMaxNotes, items beyond kMaxItems and strings beyond their caps are dropped
// rather than truncated silently into something the user did not write.
bool parse(const std::string& text, std::vector<Note>& out);

// One more than the largest id in use, never 0. Ids are never reused: an
// exported file is named after one, and reuse would silently overwrite a note
// the user still has on the card.
uint32_t nextId(const std::vector<Note>& notes);

// --- The readable export -------------------------------------------------

// What gets written to /Notes. A text note is its own text. A checklist is its
// name, a blank line, then "[x] " or "[ ] " per item -- the Markdown task
// convention, which reads correctly as plain text and renders as tickboxes in
// anything that knows Markdown.
std::string exportText(const Note& note);

// "<slug>-<id>.txt". The id is in the name on purpose: two notes called
// "Shopping" must not be one file, and a rename must not be able to land on
// another note's export. The slug is ASCII lowercase, non-alphanumerics
// collapsed to single hyphens, capped -- so it survives FAT and every desktop
// that will read this card.
std::string exportFileName(const Note& note);

// Trim and clamp a string the user typed. Applied to everything the keyboard
// returns, so the caps above hold no matter what comes back.
std::string sanitize(std::string text, size_t maxChars);

}  // namespace notes
