#include "NotesCore.h"

#include <algorithm>
#include <cstdio>

namespace notes {
namespace {

constexpr const char* kHeader = "crossplay-notes 1";

// Escaping, and why it is here rather than left to a quoting convention: an
// item is one line in the store file and a user can type a newline into the
// keyboard's text field. Without this, one multi-line item silently becomes
// two items on the next load -- the kind of corruption that looks like the app
// inventing content.
void appendEscaped(std::string& out, const std::string& value) {
  for (const char c : value) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        break;  // dropped: a CR is never content here, only a line ending
      default:
        out += c;
    }
  }
}

std::string unescape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 >= value.size()) {
      out += value[i];
      continue;
    }
    switch (value[++i]) {
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case '\\':
        out += '\\';
        break;
      default:
        // An escape this version does not know is kept verbatim, both
        // characters of it, rather than dropped. A future field written by a
        // newer build then survives a round trip through an older one.
        out += '\\';
        out += value[i];
    }
  }
  return out;
}

// Splits on '\t' into at most `max` fields; the last field keeps any further
// tabs, which is what lets an escaped body sit in the final column.
std::vector<std::string> fields(const std::string& line, const size_t max) {
  std::vector<std::string> out;
  out.reserve(max);
  size_t start = 0;
  while (out.size() + 1 < max) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string::npos) break;
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  out.push_back(line.substr(start));
  return out;
}

uint32_t toUint(const std::string& text) {
  uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return value;
    // Saturate rather than wrap. A wrapped id collides with a live note and
    // an export overwrites somebody's file.
    if (value > (UINT32_MAX - static_cast<uint32_t>(c - '0')) / 10u) return UINT32_MAX;
    value = value * 10u + static_cast<uint32_t>(c - '0');
  }
  return value;
}

// The first line with something on it, trimmed. Empty when there is none.
std::string firstLine(const std::string& text) {
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
    size_t lead = 0;
    while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
    line.erase(0, lead);
    if (!line.empty()) return line;
    start = end + 1;
  }
  return "";
}

}  // namespace

int doneCount(const Note& note) {
  int done = 0;
  for (const Item& item : note.items) {
    if (item.done) ++done;
  }
  return done;
}

std::string displayTitle(const Note& note) {
  if (note.kind == Kind::Checklist) {
    const std::string name = firstLine(note.name);
    return name.empty() ? std::string("UNTITLED") : name;
  }
  const std::string line = firstLine(note.body);
  return line.empty() ? std::string("UNTITLED") : line;
}

std::string displayBody(const Note& note) {
  if (note.kind != Kind::Text) return note.body;
  // Everything after the title line, with the blank lines that separated them
  // eaten -- a note typed as "Title\n\nWords" should not open on an empty
  // screen with the words pushed below the fold.
  size_t start = 0;
  while (start < note.body.size()) {
    size_t end = note.body.find('\n', start);
    if (end == std::string::npos) end = note.body.size();
    if (!firstLine(note.body.substr(start, end - start)).empty()) {
      start = end < note.body.size() ? end + 1 : note.body.size();
      break;
    }
    start = end + 1;
  }
  while (start < note.body.size() && (note.body[start] == '\n' || note.body[start] == '\r')) ++start;
  return note.body.substr(start);
}

uint32_t nextId(const std::vector<Note>& notes) {
  uint32_t highest = 0;
  for (const Note& note : notes) {
    if (note.id > highest) highest = note.id;
  }
  return highest + 1;
}

std::string sanitize(std::string text, const size_t maxChars) {
  // A CR is a line ending the keyboard never produces and a pasted file might.
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n')) text.pop_back();
  if (text.size() <= maxChars) return text;
  // Cut on a UTF-8 boundary. Cutting mid-sequence leaves a byte the renderer
  // draws as a replacement box and the export writes as an invalid file.
  size_t cut = maxChars;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
  text.resize(cut);
  return text;
}

std::string serialize(const std::vector<Note>& notes) {
  std::string out;
  // A note averages a couple of hundred bytes; reserving from the count keeps
  // this to one or two allocations instead of a growth cascade on the way out.
  out.reserve(notes.size() * 256 + 32);
  out += kHeader;
  out += '\n';
  for (const Note& note : notes) {
    char head[48];
    std::snprintf(head, sizeof(head), "N\t%lu\t%u\t%lu\t", static_cast<unsigned long>(note.id),
                  static_cast<unsigned>(note.kind), static_cast<unsigned long>(note.updated));
    out += head;
    appendEscaped(out, note.name);
    out += '\t';
    appendEscaped(out, note.exportName);
    out += '\n';
    if (note.kind == Kind::Text) {
      out += "B\t";
      appendEscaped(out, note.body);
      out += '\n';
    } else {
      for (const Item& item : note.items) {
        out += item.done ? "I\t1\t" : "I\t0\t";
        appendEscaped(out, item.text);
        out += '\n';
      }
    }
  }
  return out;
}

bool parse(const std::string& text, std::vector<Note>& out) {
  out.clear();
  if (text.compare(0, std::char_traits<char>::length(kHeader), kHeader) != 0) return false;

  size_t start = 0;
  bool first = true;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (first) {  // the header, already matched
      first = false;
      continue;
    }
    if (line.empty()) continue;

    if (line[0] == 'N') {
      if (out.size() >= kMaxNotes) break;
      const std::vector<std::string> parts = fields(line, 6);
      if (parts.size() < 5) continue;  // not a record this version understands
      Note note;
      note.id = toUint(parts[1]);
      note.kind = toUint(parts[2]) == 1 ? Kind::Checklist : Kind::Text;
      note.updated = toUint(parts[3]);
      note.name = sanitize(unescape(parts[4]), kMaxTitleChars);
      if (parts.size() >= 6) note.exportName = unescape(parts[5]);
      // An id of 0 is the "never saved" value and must not come back off the
      // card as one: two such notes would export to the same file.
      if (note.id == 0) note.id = nextId(out);
      out.push_back(std::move(note));
      continue;
    }

    if (out.empty()) continue;  // a body or item before any note: nothing to attach to
    Note& note = out.back();

    if (line[0] == 'B') {
      const std::vector<std::string> parts = fields(line, 2);
      if (parts.size() == 2) note.body = sanitize(unescape(parts[1]), kMaxBodyChars);
      continue;
    }
    if (line[0] == 'I') {
      if (note.items.size() >= kMaxItems) continue;
      const std::vector<std::string> parts = fields(line, 3);
      if (parts.size() < 3) continue;
      Item item;
      item.done = parts[1] == "1";
      item.text = sanitize(unescape(parts[2]), kMaxItemChars);
      if (!item.text.empty()) note.items.push_back(std::move(item));
    }
  }
  return true;
}

std::string exportText(const Note& note) {
  if (note.kind == Kind::Text) {
    std::string out = note.body;
    if (!out.empty() && out.back() != '\n') out += '\n';
    return out;
  }
  std::string out = displayTitle(note);
  out += "\n\n";
  for (const Item& item : note.items) {
    out += item.done ? "[x] " : "[ ] ";
    out += item.text;
    out += '\n';
  }
  return out;
}

std::string exportFileName(const Note& note) {
  const std::string title = displayTitle(note);
  std::string slug;
  slug.reserve(40);
  bool pendingHyphen = false;
  for (const char c : title) {
    if (slug.size() >= 40) break;
    const unsigned char u = static_cast<unsigned char>(c);
    const bool alnum = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
    if (alnum) {
      if (pendingHyphen && !slug.empty()) slug += '-';
      pendingHyphen = false;
      slug += static_cast<char>((u >= 'A' && u <= 'Z') ? u - 'A' + 'a' : u);
    } else {
      // Anything else -- a space, an emoji, a CJK glyph -- becomes one hyphen.
      // A title with no ASCII letters at all therefore slugs to nothing, which
      // is why the id below is not optional.
      pendingHyphen = true;
    }
  }
  char suffix[24];
  std::snprintf(suffix, sizeof(suffix), "-%lu.txt", static_cast<unsigned long>(note.id));
  if (slug.empty()) slug = "note";
  return slug + suffix;
}

}  // namespace notes
