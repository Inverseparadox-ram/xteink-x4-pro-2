#pragma once

// Notes: write things down, and tick things off.
//
// ---------------------------------------------------------------------------
// The shape of it, and the three decisions worth knowing.
//
// 1. There is one text editor in this app and it is the firmware's keyboard.
//    Every piece of typing -- a whole note, a list's name, one item -- is
//    KeyboardEntryActivity started for a result, with `editing_` recording
//    what the answer is for. Nothing here re-implements a caret.
//
// 2. Deleting anything is reached the SAME way everywhere: the keyboard's
//    header button, on the screen that edits the thing. That is one rule to
//    learn instead of a trash icon per screen, and it keeps every destructive
//    control off the screens where taps are frequent -- a checklist is tapped
//    once per item, and a trash anywhere near those rows is a matter of time.
//    The button never deletes on its own; it opens the confirm.
//
// 3. Every change is written to the card as it happens, tick included. The
//    resource protocol says to throttle SD persistence, and that rule is about
//    REDUNDANT writes -- saving state nothing changed. These writes are all
//    somebody's typing, the panel is already spending half a second repainting
//    around them, and the alternative is a debounce window in which a sleep or
//    a flat battery loses the only copy of something handwritten. See save().
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../activities/Activity.h"
#include "../ui/ToyboxFormat.h"
#include "../ui/ToyboxScreen.h"
#include "../ui/ToyboxWrappedText.h"
#include "NotesScreens.h"
#include "NotesStore.h"

class NotesActivity final : public Activity {
 public:
  NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Notes", renderer, mappedInput) {}
  ~NotesActivity() override = default;

  static std::unique_ptr<Activity> create(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // What is on the screen right now.
  enum class Phase : uint8_t {
    List,       // every note
    KindPick,   // text or checklist, for a note that does not exist yet
    Text,       // one text note, read
    Checklist,  // one checklist, ticked
    Confirm,    // "delete?", before anything is destroyed
  };

  // What the keyboard now on top of us is editing, so its result can be
  // applied. None whenever no keyboard is up.
  enum class Editing : uint8_t {
    None,
    NewText,   // the body of a text note that has not been created yet
    Body,      // the body of an existing text note
    NewList,   // the name of a checklist that has not been created yet
    ListName,  // the name of an existing checklist
    NewItem,   // an item being added
    ItemText,  // an existing item being reworded
  };

  // What the confirm screen would destroy.
  enum class Target : uint8_t { None, Note, Item };

  void showList();
  void openNote(uint32_t id);
  void toggleItem(int index);
  void turnPage(int delta);
  void pageList(int delta);

  // Starts the keyboard for `what`, prefilled with `initial`. `deletable`
  // gives it the header button that opens the confirm; a thing that does not
  // exist yet has nothing to delete and never gets one.
  void edit(Editing what, const char* title, const std::string& initial, size_t maxChars, bool deletable);
  void onKeyboardResult(const ActivityResult& result);
  void askDelete(Target target, int item);
  void performDelete();

  // Writes the store and says so when the card refuses. A note that did not
  // save is the one failure this app may not swallow: the user typed it and
  // has no other copy.
  void save();

  // Epoch seconds, or 0 when the clock has never been set.
  static uint32_t nowOrZero();

  // The note the Text/Checklist phases are showing, or nullptr if it has gone.
  notes::Note* currentNote();

  notes::Store store_;

  Phase phase_ = Phase::List;
  uint32_t openId_ = 0;

  Editing editing_ = Editing::None;
  int editItem_ = -1;
  Target target_ = Target::None;
  int targetItem_ = -1;

  // Row strings, owned here because fui::ListItem and ChecklistRow hold
  // pointers rather than copies. Rebuilt on every paint that needs them.
  std::vector<uint32_t> rowIds_;
  std::vector<std::string> rowLabels_;
  std::vector<std::string> rowSubtitles_;
  std::vector<freeink::ui::ListItem> rows_;
  int listTop_ = 0;
  int listVisible_ = 0;
  char countLabel_[24] = "";

  // The open text note, wrapped to the reader's width and kept between paints.
  // It holds no copy of the text and re-wraps itself whenever the panel, the
  // cut or the text stops matching what it wrapped; see ToyboxWrappedText.h.
  toybox::WrappedText wrap_;
  std::string bodyText_;
  std::string bandTitle_;
  uint32_t topLine_ = 0;
  uint32_t lineCount_ = 0;
  uint16_t visibleLines_ = 0;
  // "%lu / %lu", sized from that format rather than from the page counts a
  // reasonable note produces.
  static constexpr int kPageLabelCap = 2 * toybox::kULongChars + toybox::literalChars(" / ") + 1;
  char pageLabel_[kPageLabelCap] = "";

  // The open checklist.
  std::vector<std::string> itemLabels_;
  std::vector<notesui::ChecklistRow> itemRows_;
  int itemTop_ = 0;
  int itemVisible_ = 0;
  bool itemEditMode_ = false;
  char progressLabel_[24] = "";

  std::string confirmTitle_;
  char confirmDetail_[48] = "";

  toybox::Interactions interactions_;
  bool interactionsReady_ = false;

  // A tap that arrives within this window of a screen becoming VISIBLE is
  // answering the screen before it. This app has a confirm whose DELETE sits
  // where nothing destructive was a moment earlier, and e-ink looks like
  // nothing happened for up to two seconds, so second taps are not rare. See
  // InstapaperActivity, which established the mechanism and the number.
  static constexpr uint32_t kSettleMs = 600;
  Phase lastShownPhase_ = Phase::List;
  uint32_t phaseShownAtMs_ = 0;
  bool everShown_ = false;
};
