// Freestanding tests for NotesCore: the store format, the title rules, and the
// export.
//
// The store format gets a ROUND TRIP rather than a spot check on its output,
// because the failure it exists to prevent is not a wrong byte, it is a note
// that comes back as two notes -- or as none. Every string a user can type
// goes through it here: newlines, tabs, backslashes, and the three together.

#include <cstdio>
#include <string>
#include <vector>

#include "../../src/apps_local/notes/NotesCore.h"

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

static notes::Note textNote(const uint32_t id, const std::string& body) {
  notes::Note note;
  note.id = id;
  note.kind = notes::Kind::Text;
  note.body = body;
  note.updated = 1700000000;
  return note;
}

static notes::Note listNote(const uint32_t id, const std::string& name, const std::vector<notes::Item>& items) {
  notes::Note note;
  note.id = id;
  note.kind = notes::Kind::Checklist;
  note.name = name;
  note.items = items;
  note.updated = 1700000000;
  return note;
}

static void testTitles() {
  CHECK(notes::displayTitle(textNote(1, "Shopping\nmilk\neggs")) == "Shopping", "first line is the title");
  CHECK(notes::displayTitle(textNote(1, "\n\n  Late start  \nrest")) == "Late start",
        "leading blank lines are skipped and the title is trimmed");
  CHECK(notes::displayTitle(textNote(1, "")) == "UNTITLED", "an empty note still has a row label");
  CHECK(notes::displayTitle(textNote(1, "   \n\t\n")) == "UNTITLED", "whitespace is not a title");
  CHECK(notes::displayTitle(listNote(1, "Errands", {})) == "Errands", "a checklist uses its own name");
  CHECK(notes::displayTitle(listNote(1, "", {})) == "UNTITLED", "an unnamed checklist still has a row label");

  // The body a text note SHOWS is what is left after the title line, because
  // the band already carries the title. A note that is only a title shows
  // nothing, which is what drives the EMPTY screen.
  CHECK(notes::displayBody(textNote(1, "Shopping\nmilk\neggs")) == "milk\neggs", "the title line is not drawn twice");
  CHECK(notes::displayBody(textNote(1, "Shopping\n\n\nmilk")) == "milk",
        "blank lines under the title do not push the body off the screen");
  CHECK(notes::displayBody(textNote(1, "Shopping")).empty(), "a title-only note has an empty body");
}

static void testDoneCount() {
  const notes::Note list = listNote(1, "Errands", {{"post", true}, {"bank", false}, {"bread", true}});
  CHECK(notes::doneCount(list) == 2, "two of three ticked");
  CHECK(notes::doneCount(textNote(1, "words")) == 0, "a text note is 0 done");
}

static void testRoundTrip() {
  std::vector<notes::Note> in;
  in.push_back(textNote(1, "Plain\nbody"));
  // Every character the format itself uses, in content, plus the three
  // together. A tab inside an item used to end the field; a newline used to
  // split one item into two.
  in.push_back(textNote(2, "Tricky\nwith\ta tab\\and a backslash\nand \\n literal"));
  in.push_back(listNote(3, "Errands\twith a tab", {{"post\noffice", true}, {"back\\slash", false}}));

  const std::string text = notes::serialize(in);
  std::vector<notes::Note> out;
  CHECK(notes::parse(text, out), "a file we just wrote parses");
  CHECK(out.size() == in.size(), "got %d notes back, wrote %d", static_cast<int>(out.size()),
        static_cast<int>(in.size()));
  for (size_t i = 0; i < out.size() && i < in.size(); ++i) {
    CHECK(out[i].id == in[i].id, "note %d id", static_cast<int>(i));
    CHECK(out[i].kind == in[i].kind, "note %d kind", static_cast<int>(i));
    CHECK(out[i].name == in[i].name, "note %d name: got '%s'", static_cast<int>(i), out[i].name.c_str());
    CHECK(out[i].body == in[i].body, "note %d body: got '%s'", static_cast<int>(i), out[i].body.c_str());
    CHECK(out[i].items.size() == in[i].items.size(), "note %d item count", static_cast<int>(i));
    for (size_t j = 0; j < out[i].items.size() && j < in[i].items.size(); ++j) {
      CHECK(out[i].items[j].text == in[i].items[j].text, "note %d item %d text: got '%s'", static_cast<int>(i),
            static_cast<int>(j), out[i].items[j].text.c_str());
      CHECK(out[i].items[j].done == in[i].items[j].done, "note %d item %d done", static_cast<int>(i),
            static_cast<int>(j));
    }
  }
}

static void testDamagedFile() {
  // Not our format at all: refused, so the caller leaves the file alone rather
  // than overwriting something it cannot read.
  std::vector<notes::Note> out;
  CHECK(!notes::parse("{\"notes\":[]}", out), "JSON is not this format");
  CHECK(!notes::parse("", out), "an empty file is not this format");

  // Truncated mid-file: everything before the cut survives. This is the whole
  // reason the store is a line format instead of one JSON document.
  std::vector<notes::Note> in;
  in.push_back(textNote(1, "First"));
  in.push_back(textNote(2, "Second"));
  in.push_back(textNote(3, "Third"));
  std::string text = notes::serialize(in);
  text.resize(text.size() - 4);  // chop the tail of the last body line
  std::vector<notes::Note> survived;
  CHECK(notes::parse(text, survived), "a truncated file still parses");
  CHECK(survived.size() == 3, "every note header survived, got %d", static_cast<int>(survived.size()));
  CHECK(survived[0].body == "First", "the first note is untouched by damage at the end");

  // A body or item line with no note above it has nothing to attach to and is
  // dropped rather than crashing on an empty vector.
  std::vector<notes::Note> orphan;
  CHECK(notes::parse("crossplay-notes 1\nB\tstray\nI\t1\tstray\n", orphan), "orphan lines parse");
  CHECK(orphan.empty(), "orphan lines make no notes");
}

static void testCaps() {
  // A hostile or corrupt file cannot make this allocate without limit.
  std::string text = "crossplay-notes 1\n";
  for (size_t i = 0; i < notes::kMaxNotes + 50; ++i) {
    text += "N\t" + std::to_string(i + 1) + "\t0\t0\t\t\nB\tbody\n";
  }
  std::vector<notes::Note> out;
  CHECK(notes::parse(text, out), "an oversized file parses");
  CHECK(out.size() == notes::kMaxNotes, "capped at %d notes, got %d", static_cast<int>(notes::kMaxNotes),
        static_cast<int>(out.size()));

  // sanitize cuts on a UTF-8 boundary. Cutting mid-sequence leaves a byte the
  // panel draws as a replacement box and the export writes as an invalid file.
  const std::string euro = "\xE2\x82\xAC";  // 3 bytes
  const std::string six = euro + euro;
  CHECK(notes::sanitize(six, 4) == euro, "a cut lands on a code point boundary, got %d bytes",
        static_cast<int>(notes::sanitize(six, 4).size()));
  CHECK(notes::sanitize("  trailing   ", 100) == "  trailing", "trailing space is trimmed, leading is not");
  CHECK(notes::sanitize("a\r\nb", 100) == "a\nb", "a CR is never content here");

  // An id of 0 is the "never saved" value; two of them off the card must not
  // stay 0, or both notes export to the same file.
  std::vector<notes::Note> zeros;
  CHECK(notes::parse("crossplay-notes 1\nN\t0\t0\t0\t\t\nB\tone\nN\t0\t0\t0\t\t\nB\ttwo\n", zeros), "parses");
  CHECK(zeros.size() == 2 && zeros[0].id != zeros[1].id && zeros[0].id != 0 && zeros[1].id != 0,
        "zero ids are replaced with distinct ones");
}

static void testExport() {
  const notes::Note list = listNote(7, "Errands", {{"post office", true}, {"bank", false}});
  CHECK(notes::exportText(list) == "Errands\n\n[x] post office\n[ ] bank\n",
        "checklist export is a Markdown task list");
  CHECK(notes::exportText(textNote(7, "Shopping\nmilk")) == "Shopping\nmilk\n", "a text note exports as itself");
  CHECK(notes::exportText(textNote(7, "ends already\n")) == "ends already\n", "no double newline at the end");

  // The id is in the file name so two notes with the same title are two files,
  // and a rename can never land on another note's export.
  CHECK(notes::exportFileName(list) == "errands-7.txt", "got '%s'", notes::exportFileName(list).c_str());
  const notes::Note same = listNote(8, "Errands", {});
  CHECK(notes::exportFileName(same) == "errands-8.txt", "same title, different file");
  CHECK(notes::exportFileName(textNote(9, "Buy: milk, eggs & bread!")) == "buy-milk-eggs-bread-9.txt", "got '%s'",
        notes::exportFileName(textNote(9, "Buy: milk, eggs & bread!")).c_str());
  // A title with no ASCII letters slugs to nothing, which is why the fallback
  // is not optional -- the alternative is a file called "-12.txt".
  CHECK(notes::exportFileName(textNote(12, "\xE4\xB8\xAD\xE6\x96\x87")) == "note-12.txt", "got '%s'",
        notes::exportFileName(textNote(12, "\xE4\xB8\xAD\xE6\x96\x87")).c_str());
  CHECK(notes::exportFileName(textNote(13, "")) == "untitled-13.txt", "got '%s'",
        notes::exportFileName(textNote(13, "")).c_str());
  // A slug is capped, so a long first line cannot make a name FAT refuses.
  const std::string longName = notes::exportFileName(textNote(14, std::string(300, 'a')));
  CHECK(longName.size() <= 48, "a long title makes a short file name, got %d", static_cast<int>(longName.size()));
}

static void testNextId() {
  std::vector<notes::Note> notes;
  CHECK(notes::nextId(notes) == 1, "the first id is 1, never 0");
  notes.push_back(textNote(4, "a"));
  notes.push_back(textNote(2, "b"));
  CHECK(notes::nextId(notes) == 5, "ids are never reused, even after a deletion in the middle");
}

int main() {
  testTitles();
  testDoneCount();
  testRoundTrip();
  testDamagedFile();
  testCaps();
  testExport();
  testNextId();
  // The exact shape scripts_local/check.sh counts with grep -c "checks, 0
  // failed". A suite that passes but is not countable is left out of the
  // 'ok (N sub-suites)' tally, which makes a missing suite look exactly like
  // one nobody ever added. host-tests/checksh guards it.
  std::printf("%s  notes core: %d checks, %d failed\n", failures ? "FAIL" : "ok  ", checks, failures);
  return failures == 0 ? 0 : 1;
}
