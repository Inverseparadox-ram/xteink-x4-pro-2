#include "NotesStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

namespace notes {
namespace {

constexpr const char* kDir = "/.crosspoint/notes";
constexpr const char* kStore = "/.crosspoint/notes/notes.txt";
constexpr const char* kExportDir = "/Notes";
constexpr const char* kTag = "NOTES";

bool readWholeFile(const char* path, std::string& out) {
  HalFile file;
  if (!Storage.openFileForRead(kTag, path, file)) return false;
  out.clear();
  char buffer[512];
  int n = 0;
  while ((n = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer))) > 0) {
    out.append(buffer, static_cast<size_t>(n));
  }
  file.close();
  return true;
}

bool writeWholeFile(const char* path, const std::string& text) {
  HalFile file;
  if (!Storage.openFileForWrite(kTag, path, file)) return false;
  const bool ok =
      file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == static_cast<int>(text.size());
  file.close();
  return ok;
}

// Write beside, then rename into place. A note file half-written by a card
// pulled mid-save is indistinguishable from a note file the user emptied.
bool writeAtomically(const char* path, const std::string& text) {
  const std::string temp = std::string(path) + ".part";
  if (!writeWholeFile(temp.c_str(), text)) {
    LOG_ERR(kTag, "cannot write %s", temp.c_str());
    return false;
  }
  Storage.remove(path);
  if (!Storage.rename(temp.c_str(), path)) {
    LOG_ERR(kTag, "cannot rename %s into place", temp.c_str());
    return false;
  }
  return true;
}

}  // namespace

const char* Store::exportDirectory() { return kExportDir; }

void Store::load() {
  if (loaded_) return;
  loaded_ = true;
  std::string text;
  if (!readWholeFile(kStore, text)) return;  // nothing written yet is not an error
  if (!parse(text, notes_)) {
    // Not our format at all. The file is LEFT ALONE rather than replaced: it
    // is the only copy of something a person typed, and a build that cannot
    // read it is not entitled to delete it. The app opens empty; the next
    // save writes over it, which is the user's own decision to make.
    LOG_ERR(kTag, "%s is not in our format; leaving it alone", kStore);
    notes_.clear();
    return;
  }
  LOG_INF(kTag, "loaded %d notes", static_cast<int>(notes_.size()));
}

Note* Store::find(const uint32_t id) {
  for (Note& note : notes_) {
    if (note.id == id) return &note;
  }
  return nullptr;
}

Note& Store::create(const Kind kind) {
  Note note;
  note.id = nextId(notes_);
  note.kind = kind;
  // Newest first, which is where the user is looking: they just made it.
  notes_.insert(notes_.begin(), std::move(note));
  return notes_.front();
}

void Store::remove(const uint32_t id) {
  const auto it = std::find_if(notes_.begin(), notes_.end(), [id](const Note& n) { return n.id == id; });
  if (it == notes_.end()) return;
  removeExport(*it);
  notes_.erase(it);
}

bool Store::touchAndSave(const uint32_t id, const uint32_t now) {
  const auto it = std::find_if(notes_.begin(), notes_.end(), [id](const Note& n) { return n.id == id; });
  if (it == notes_.end()) return save();
  it->updated = now;
  // Most-recently-touched first, and it is a rotate rather than a sort because
  // the clock may never have been set: `updated` is 0 on a device that has not
  // seen a network, so sorting by it would leave the order to chance. Moving
  // the touched note to the front orders the list by what the user actually
  // did, with or without a clock.
  std::rotate(notes_.begin(), it, it + 1);
  writeExport(notes_.front());
  return save();
}

bool Store::save() {
  Storage.ensureDirectoryExists(kDir);
  if (!writeAtomically(kStore, serialize(notes_))) {
    LOG_ERR(kTag, "could not save %d notes", static_cast<int>(notes_.size()));
    return false;
  }
  return true;
}

void Store::writeExport(Note& note) {
  Storage.ensureDirectoryExists(kExportDir);
  const std::string name = exportFileName(note);
  // A rename moves the file. Remove the old one first, so the card does not
  // accumulate a copy per title the note has ever had.
  if (!note.exportName.empty() && note.exportName != name) removeExport(note);
  const std::string path = std::string(kExportDir) + "/" + name;
  if (!writeWholeFile(path.c_str(), exportText(note))) {
    // Logged, not surfaced: the note itself is about to be saved to the store
    // either way, and an export that failed is a missing convenience rather
    // than lost work. Not written atomically for the same reason -- a torn
    // export is rewritten by the next edit, and the .part files would
    // otherwise litter a directory the user browses by hand.
    LOG_ERR(kTag, "cannot export %s", path.c_str());
    return;
  }
  note.exportName = name;
}

void Store::removeExport(const Note& note) {
  if (note.exportName.empty()) return;
  const std::string path = std::string(kExportDir) + "/" + note.exportName;
  Storage.remove(path.c_str());
}

}  // namespace notes
