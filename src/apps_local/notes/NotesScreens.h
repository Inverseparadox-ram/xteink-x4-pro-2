#pragma once

// The Notes screens. Freestanding builders in the InstapaperScreens mould: a
// model in, a drawn frame out, no renderer and no Activity, so
// host-tests/ui/ can assert what they drew and what they made tappable.
//
// ---------------------------------------------------------------------------
// The one interaction decision worth stating, because it is the whole app.
//
// On a checklist, A TAP ON A ROW TICKS IT. Not "opens it", not "selects it" --
// ticks it, immediately, with no confirm. That is the action a to-do list is
// for, it is what every finger already expects, and on a panel that takes a
// second to repaint a two-tap tick would be the slowest thing in the fork.
//
// Everything that is NOT ticking -- rewording an item, deleting one, renaming
// the list -- lives behind an explicit EDIT MODE with its own band title. That
// split is the fork's same-pixel-different-action rule applied where it
// actually bites: the alternative designs all put "tick" and "edit" on the
// same row and separated them by an invisible line down the middle of it, or
// by a long press nothing on the screen announces.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "../ui/ToyboxScreen.h"
#include "../ui/ToyboxWrappedText.h"

namespace notesui {

namespace fui = freeink::ui;

// Chess uses 1-4, the link layer the 200s, Hacker News the 300s, Instapaper
// the 320s. Notes takes the 340s, one range per family.
enum : fui::ActionId {
  ActionOpenNote = 340,
  ActionNewNote = 341,
  ActionKindText = 342,
  ActionKindChecklist = 343,
  ActionToggleItem = 344,
  ActionAddItem = 345,
  ActionEditMode = 346,
  ActionEditItem = 347,
  ActionRenameList = 348,
  ActionDoneEditing = 349,
  ActionEditBody = 350,
  ActionDelete = 351,
  ActionDeleteConfirm = 352,
  ActionDeleteCancel = 353,
  ActionPagePrev = 354,
  ActionPageNext = 355,
};

// --- The note list -------------------------------------------------------

struct ListModel {
  // Built by the Activity, which owns the strings: label is the title,
  // subtitle is "CHECKLIST . 3 OF 7" or "NOTE . 12 MAR", value is unused.
  const fui::ListItem* items = nullptr;
  int count = 0;
  int topIndex = 0;
  // "12 NOTES", drawn on the band. nullptr on an empty shelf, where the band
  // should not reserve room for a label saying nothing.
  const char* countLabel = nullptr;
};

void buildList(toybox::Screen& screen, const ListModel& model);

// The band the list draws into and the height of a row, shared with the
// Activity so its paging arithmetic and the drawn rows come from one function
// rather than two that can only agree by accident.
fui::Rect listBand(const fui::DeviceContext& device);
int16_t listRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& tokens);
// The width a title is really drawn into, so the Activity fits it to the space
// the component will give it rather than to a second guess at it.
int16_t listTitleWidth(const fui::DeviceContext& device, const fui::ThemeTokens& tokens);

// --- Pick a kind ---------------------------------------------------------

// Two buttons, and no third. "What kind of note" is a question with two
// answers here, so it is two large targets rather than a list with two rows in
// it -- a two-row list on this panel is mostly empty space above a scroll
// track that does not scroll.
void buildKindPick(toybox::Screen& screen);

// --- A text note ---------------------------------------------------------

// The words, the cut they are set in, and the wrap that counts AND draws
// them. One object rather than three arguments, because a style handed to the
// counting but not to the drawing makes the two fingerprints differ and the
// note is re-wrapped on every paint. See instapaper::ReaderBody, which paid
// for this lesson first.
struct TextBody {
  const char* text = "";
  fui::TextStyle style{};
  toybox::WrappedText* wrap = nullptr;
};

struct TextModel {
  const char* title = "";
  uint32_t topLine = 0;
  const char* pageLabel = nullptr;  // "2 / 3", or nullptr on a single page
  bool canPagePrev = false;
  bool canPageNext = false;
  // An empty note says so rather than showing a blank sheet, which on e-ink is
  // indistinguishable from a note that failed to load.
  bool empty = false;
};

// Returns the line count the panel was ACTUALLY drawn from, which is not
// necessarily the one textLineCount() gave a moment ago: drawing is where a
// wrap that no longer describes this panel is caught and rebuilt. Take this
// value; do not keep the earlier one.
uint32_t buildText(toybox::Screen& screen, const TextModel& model, TextBody& body);

uint32_t textLineCount(const fui::DrawTarget& target, const fui::DeviceContext& device, TextBody& body);
fui::Rect textBodyRect(const fui::DeviceContext& device);

// --- A checklist ---------------------------------------------------------

struct ChecklistRow {
  const char* text = "";
  bool done = false;
};

struct ChecklistModel {
  const char* title = "";
  const ChecklistRow* rows = nullptr;
  int count = 0;     // rows on THIS page
  int firstIndex = 0;  // absolute index of rows[0], so a tap reports the item
  const char* progressLabel = nullptr;  // "3 OF 7", on the band
  const char* pageLabel = nullptr;      // "2 / 3", under the progress
  bool editing = false;
  bool empty = false;
};

void buildChecklist(toybox::Screen& screen, const ChecklistModel& model);

fui::Rect checklistBand(const fui::DeviceContext& device);
int16_t checklistRowHeight(const fui::ThemeTokens& tokens);

// --- Delete confirm ------------------------------------------------------

// The gate in front of a deletion, built the way Instapaper's disconnect
// confirm is: the SAFE answer is the prominent one. KEEP IT sits on the
// primary action band where a thumb expects "the button", and DELETE is a
// smaller outlined control set apart from it -- so a stray tap, or one
// remembered from the screen before, keeps the note.
struct ConfirmModel {
  const char* title = "";
  // "7 items" or "212 words", so the thing being destroyed has a size on
  // screen before the tap rather than after it.
  const char* detail = "";
};

void buildDeleteConfirm(toybox::Screen& screen, const ConfirmModel& model);

}  // namespace notesui
