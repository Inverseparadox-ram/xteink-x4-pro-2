#pragma once

// The card side of Notes: one store file the app owns, and one readable
// export per note that the user owns.
//
// ---------------------------------------------------------------------------
// Two locations, on purpose, and they are not the same kind of thing.
//
//   /.crosspoint/notes/notes.txt   the store. Written atomically, read once
//                                  per opening, and the only file this app
//                                  ever reads back. Under /.crosspoint
//                                  because it is app bookkeeping.
//
//   /Notes/<slug>-<id>.txt         the export. Written after every change and
//                                  never read. It exists so a note typed on
//                                  the device can be opened on a computer by
//                                  plugging the card in -- which is the whole
//                                  difference between notes you keep and
//                                  notes you are holding hostage.
//
// The export is deliberately one-way. Reading it back would mean deciding what
// happens when both copies changed, on a device with no clock it can trust,
// and getting that wrong loses the thing the user typed. If an export fails --
// a full card, a card pulled mid-write -- the note is still saved and the app
// says nothing, because the store is what the app is for and the export is a
// convenience that must never be able to block it.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

#include "NotesCore.h"

namespace notes {

class Store {
 public:
  // Reads the store file. Idempotent: the second call is free, so an Activity
  // can call it on entry without tracking whether it already did.
  void load();

  // Serializes every note and replaces the store file atomically. False when
  // the card refused the write, which the caller is expected to surface --
  // silently losing a note the user just typed is the one failure this app
  // cannot have.
  bool save();

  std::vector<Note>& notes() { return notes_; }
  const std::vector<Note>& notes() const { return notes_; }

  Note* find(uint32_t id);

  // Appends a new note and returns it. The reference is valid until the next
  // create() or remove(); callers take the id, not the pointer.
  Note& create(Kind kind);

  // Deletes the note and its export. Silent when the id is unknown.
  void remove(uint32_t id);

  // Stamps `updated`, moves the note to the front (the list shows
  // most-recently-touched first), rewrites the export, and saves.
  // `now` is epoch seconds, or 0 when the clock has never been set.
  bool touchAndSave(uint32_t id, uint32_t now);

  // Where the exports go, so the Activity can name it on an empty screen --
  // a user who cannot find the files is in the same position as one who has
  // none.
  static const char* exportDirectory();

 private:
  // Writes /Notes/<slug>-<id>.txt and removes the note's previous export when
  // a rename moved it. Never fails the save; see the header note.
  void writeExport(Note& note);
  void removeExport(const Note& note);

  std::vector<Note> notes_;
  bool loaded_ = false;
};

}  // namespace notes
