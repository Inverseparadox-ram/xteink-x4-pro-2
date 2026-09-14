#include "NotesActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <ctime>

#include "../../activities/util/KeyboardEntryActivity.h"
#include "../../components/UITheme.h"
#include "../Shelf.h"
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxIcons.h"
#include "../ui/ToyboxText.h"
#include "../ui/ToyboxTheme.h"

namespace fui = freeink::ui;

namespace {

// Below this, the clock has never been set: the RTC comes up somewhere in
// 1970 and a note stamped from it would sort before every real one forever.
// 2020-01-01, the same floor Instapaper uses.
constexpr int64_t kClockFloor = 1577836800;

// "12 MAR", or nothing at all when the clock has never been set. A date this
// device invented is worse than no date: the row would claim the note was
// written on a day it was not.
void formatDate(const uint32_t when, char* out, const size_t size) {
  if (when == 0) {
    out[0] = '\0';
    return;
  }
  static constexpr char kMonths[][4] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  const time_t stamp = static_cast<time_t>(when);
  struct tm parts {};
  localtime_r(&stamp, &parts);
  if (parts.tm_mon < 0 || parts.tm_mon > 11) {
    out[0] = '\0';
    return;
  }
  std::snprintf(out, size, "%d %s", parts.tm_mday, kMonths[parts.tm_mon]);
}

}  // namespace

std::unique_ptr<Activity> NotesActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return makeUniqueNoThrow<NotesActivity>(renderer, mappedInput);
}

uint32_t NotesActivity::nowOrZero() {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  return now > kClockFloor ? static_cast<uint32_t>(now) : 0u;
}

// --- Lifecycle -----------------------------------------------------------

void NotesActivity::onEnter() {
  Activity::onEnter();
  toybox::ensureFonts(renderer);
  store_.load();
  phase_ = Phase::List;
  listTop_ = 0;
  LOG_INF("NOTES", "opened: %d notes", static_cast<int>(store_.notes().size()));
  // Without this the app enters, loads, logs that it did, and draws NOTHING:
  // the panel keeps showing the shelf it was opened from, and nothing in a
  // build or a log says so. See docs/building-apps.md.
  requestUpdate();
}

notes::Note* NotesActivity::currentNote() { return openId_ == 0 ? nullptr : store_.find(openId_); }

void NotesActivity::save() {
  // Every change, as it happens -- see decision 3 in the header. A failure is
  // logged loudly rather than shown, because the only honest thing a dialog
  // could say here is "your card did not take it", and the user is already
  // looking at the note: it is still in memory and still on the screen, and
  // the next edit tries again.
  if (!store_.save()) LOG_ERR("NOTES", "the card refused the write; notes are unsaved");
}

void NotesActivity::showList() {
  RenderLock lock(*this);
  phase_ = Phase::List;
  openId_ = 0;
  itemEditMode_ = false;
  bodyText_.clear();
  topLine_ = 0;
  lineCount_ = 0;
}

void NotesActivity::openNote(const uint32_t id) {
  const notes::Note* note = store_.find(id);
  if (note == nullptr) return;
  {
    RenderLock lock(*this);
    openId_ = id;
    itemEditMode_ = false;
    itemTop_ = 0;
    topLine_ = 0;
    lineCount_ = 0;
    phase_ = note->kind == notes::Kind::Text ? Phase::Text : Phase::Checklist;
  }
  requestUpdate();
}

void NotesActivity::toggleItem(const int index) {
  notes::Note* note = currentNote();
  if (note == nullptr || index < 0 || index >= static_cast<int>(note->items.size())) return;
  {
    RenderLock lock(*this);
    note->items[static_cast<size_t>(index)].done = !note->items[static_cast<size_t>(index)].done;
  }
  store_.touchAndSave(openId_, nowOrZero());
  requestUpdate();
}

void NotesActivity::turnPage(const int delta) {
  if (visibleLines_ == 0 || lineCount_ == 0) return;
  const uint32_t step = visibleLines_;
  RenderLock lock(*this);
  if (delta > 0) {
    if (topLine_ + step >= lineCount_) return;  // already on the last page
    topLine_ += step;
  } else {
    topLine_ = topLine_ > step ? topLine_ - step : 0;
  }
  requestUpdate();
}

void NotesActivity::pageList(const int delta) {
  const int count = static_cast<int>(rowIds_.size());
  if (count == 0 || listVisible_ <= 0) return;
  const int pages = (count + listVisible_ - 1) / listVisible_;
  const int page = listTop_ / listVisible_;
  // Wraps: a page key that stops working at the last page reads as broken.
  listTop_ = ((page + (delta > 0 ? 1 : pages - 1)) % pages) * listVisible_;
  requestUpdate();
}

// --- Typing --------------------------------------------------------------

void NotesActivity::edit(const Editing what, const char* title, const std::string& initial, const size_t maxChars,
                         const bool deletable) {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, title, initial, maxChars,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("NOTES", "OOM: keyboard");
    return;
  }
  // The one way to delete anything in this app. It finishes the keyboard with
  // headerAction set and the typed text preserved, so a tap on it that was
  // meant for a letter still has somewhere to go back to: the confirm's KEEP
  // IT returns to the note with the edit NOT applied, which is the same place
  // Back would have gone.
  if (deletable) keyboard->setHeaderAction(fui::bitmapFromIcon(icon_notes_trash_32));
  editing_ = what;
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) { onKeyboardResult(result); });
}

void NotesActivity::onKeyboardResult(const ActivityResult& result) {
  const Editing what = editing_;
  editing_ = Editing::None;
  const int item = editItem_;
  editItem_ = -1;

  if (result.isCancelled) {
    // A cancelled NEW note is not a note. Nothing was created, so there is
    // nothing to clean up -- which is the reason creation waits for the
    // keyboard to come back rather than making an empty note first.
    requestUpdate();
    return;
  }

  const auto& typed = std::get<KeyboardResult>(result.data);
  if (typed.headerAction) {
    switch (what) {
      case Editing::Body:
      case Editing::ListName:
        askDelete(Target::Note, -1);
        return;
      case Editing::ItemText:
        askDelete(Target::Item, item);
        return;
      default:
        // A thing that does not exist yet has no delete button, so this is
        // unreachable; falling through to "nothing happened" is still the
        // right answer if it ever becomes reachable.
        requestUpdate();
        return;
    }
  }

  const uint32_t now = nowOrZero();

  switch (what) {
    case Editing::NewText: {
      const std::string body = notes::sanitize(typed.text, notes::kMaxBodyChars);
      // An empty note is not created. Somebody who opened the keyboard, typed
      // nothing and pressed OK has made no note, and a row called UNTITLED
      // with nothing in it is litter they then have to delete.
      if (body.empty()) {
        showList();
        break;
      }
      if (store_.notes().size() >= notes::kMaxNotes) {
        LOG_ERR("NOTES", "at the %d-note cap; not creating another", static_cast<int>(notes::kMaxNotes));
        showList();
        break;
      }
      notes::Note& note = store_.create(notes::Kind::Text);
      note.body = body;
      const uint32_t id = note.id;
      store_.touchAndSave(id, now);
      openNote(id);
      return;
    }
    case Editing::Body: {
      notes::Note* note = currentNote();
      if (note == nullptr) break;
      {
        RenderLock lock(*this);
        note->body = notes::sanitize(typed.text, notes::kMaxBodyChars);
        // The edit may have changed the title line, which changes how long the
        // note is and where its pages fall. Back to the top rather than to a
        // line number that meant something about the previous text.
        topLine_ = 0;
        lineCount_ = 0;
      }
      store_.touchAndSave(openId_, now);
      break;
    }
    case Editing::NewList: {
      const std::string name = notes::sanitize(typed.text, notes::kMaxTitleChars);
      if (name.empty()) {
        showList();
        break;
      }
      if (store_.notes().size() >= notes::kMaxNotes) {
        LOG_ERR("NOTES", "at the %d-note cap; not creating another", static_cast<int>(notes::kMaxNotes));
        showList();
        break;
      }
      notes::Note& note = store_.create(notes::Kind::Checklist);
      note.name = name;
      const uint32_t id = note.id;
      store_.touchAndSave(id, now);
      openNote(id);
      return;
    }
    case Editing::ListName: {
      notes::Note* note = currentNote();
      if (note == nullptr) break;
      const std::string name = notes::sanitize(typed.text, notes::kMaxTitleChars);
      // An empty name is refused rather than stored: displayTitle() would show
      // UNTITLED and the export would be renamed to match, which is a lot of
      // consequence for a keystroke.
      if (!name.empty()) {
        RenderLock lock(*this);
        note->name = name;
      }
      store_.touchAndSave(openId_, now);
      break;
    }
    case Editing::NewItem: {
      notes::Note* note = currentNote();
      if (note == nullptr) break;
      const std::string text = notes::sanitize(typed.text, notes::kMaxItemChars);
      if (text.empty()) break;
      if (note->items.size() >= notes::kMaxItems) {
        LOG_ERR("NOTES", "at the %d-item cap; not adding another", static_cast<int>(notes::kMaxItems));
        break;
      }
      {
        RenderLock lock(*this);
        note->items.push_back(notes::Item{text, false});
        // A new item goes on the end, so show the end: adding something and
        // being left looking at a page that does not contain it reads as the
        // add having failed.
        itemTop_ = itemVisible_ > 0 ? (static_cast<int>(note->items.size()) - 1) / itemVisible_ * itemVisible_ : 0;
      }
      store_.touchAndSave(openId_, now);
      break;
    }
    case Editing::ItemText: {
      notes::Note* note = currentNote();
      if (note == nullptr || item < 0 || item >= static_cast<int>(note->items.size())) break;
      const std::string text = notes::sanitize(typed.text, notes::kMaxItemChars);
      // Emptying an item is a delete by another name, and it goes through the
      // same confirm rather than happening because a backspace was held.
      if (text.empty()) {
        askDelete(Target::Item, item);
        return;
      }
      {
        RenderLock lock(*this);
        note->items[static_cast<size_t>(item)].text = text;
      }
      store_.touchAndSave(openId_, now);
      break;
    }
    case Editing::None:
      break;
  }
  requestUpdate();
}

// --- Deleting ------------------------------------------------------------

void NotesActivity::askDelete(const Target target, const int item) {
  const notes::Note* note = currentNote();
  if (note == nullptr) {
    showList();
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    target_ = target;
    targetItem_ = item;
    if (target == Target::Item && item >= 0 && item < static_cast<int>(note->items.size())) {
      confirmTitle_ = note->items[static_cast<size_t>(item)].text;
      std::snprintf(confirmDetail_, sizeof(confirmDetail_), "One item off this list.");
    } else {
      confirmTitle_ = notes::displayTitle(*note);
      if (note->kind == notes::Kind::Checklist) {
        std::snprintf(confirmDetail_, sizeof(confirmDetail_), "%d items. This cannot be undone.",
                      static_cast<int>(note->items.size()));
      } else {
        std::snprintf(confirmDetail_, sizeof(confirmDetail_), "%d characters. This cannot be undone.",
                      static_cast<int>(note->body.size()));
      }
    }
    phase_ = Phase::Confirm;
  }
  requestUpdate();
}

void NotesActivity::performDelete() {
  notes::Note* note = currentNote();
  if (note == nullptr) {
    showList();
    requestUpdate();
    return;
  }
  if (target_ == Target::Item) {
    if (targetItem_ >= 0 && targetItem_ < static_cast<int>(note->items.size())) {
      RenderLock lock(*this);
      note->items.erase(note->items.begin() + targetItem_);
      // The page the deleted item was on may no longer exist.
      if (itemVisible_ > 0 && itemTop_ >= static_cast<int>(note->items.size())) {
        itemTop_ = itemTop_ >= itemVisible_ ? itemTop_ - itemVisible_ : 0;
      }
      phase_ = Phase::Checklist;
    }
    target_ = Target::None;
    targetItem_ = -1;
    store_.touchAndSave(openId_, nowOrZero());
    requestUpdate();
    return;
  }

  // The whole note, and its export with it.
  const uint32_t id = openId_;
  target_ = Target::None;
  targetItem_ = -1;
  store_.remove(id);
  save();
  showList();
  requestUpdate();
}

// --- Input ---------------------------------------------------------------

void NotesActivity::loop() {
  // An app never names where Back goes; the shelf puts it back in whichever
  // folder opened it. The global back-swipe gesture arrives as Button::Back
  // too, and on the X4 Pro it is the ONLY way out -- the board has no Back key.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    switch (phase_) {
      case Phase::List:
        shelf::leave(renderer, mappedInput);
        return;
      case Phase::KindPick:
        showList();
        break;
      case Phase::Text:
        showList();
        break;
      case Phase::Checklist:
        // Back out of the mode first, then out of the list. A mode Back
        // escapes in one step is a mode people leave by accident.
        if (itemEditMode_) {
          RenderLock lock(*this);
          itemEditMode_ = false;
        } else {
          showList();
        }
        break;
      case Phase::Confirm:
        // Back is the safe answer to "delete?", exactly like KEEP IT.
        {
          RenderLock lock(*this);
          target_ = Target::None;
          targetItem_ = -1;
          phase_ = openId_ == 0 ? Phase::List : (currentNote() != nullptr && currentNote()->kind == notes::Kind::Text
                                                     ? Phase::Text
                                                     : Phase::Checklist);
        }
        break;
    }
    requestUpdate();
    return;
  }

  // The two side keys page whatever is paged. They are the device's only
  // physical buttons; a row cursor would need Confirm, which is an unassigned
  // pin on the X4 Pro and never fires. See docs/buttons.md.
  const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
  if (next || prev) {
    switch (phase_) {
      case Phase::List:
        pageList(next ? 1 : -1);
        return;
      case Phase::Text:
        turnPage(next ? 1 : -1);
        return;
      case Phase::Checklist: {
        const notes::Note* note = currentNote();
        if (note == nullptr || itemVisible_ <= 0) return;
        const int count = static_cast<int>(note->items.size());
        if (count <= itemVisible_) return;
        const int pages = (count + itemVisible_ - 1) / itemVisible_;
        const int page = itemTop_ / itemVisible_;
        itemTop_ = ((page + (next ? 1 : pages - 1)) % pages) * itemVisible_;
        requestUpdate();
        return;
      }
      default:
        return;
    }
  }

  int tapX = 0;
  int tapY = 0;
  if (!mappedInput.wasScreenTapped(tapX, tapY) || !interactionsReady_) return;

  // A tap that arrives within kSettleMs of this screen appearing is answering
  // the PREVIOUS screen -- see the note beside kSettleMs. Dropped rather than
  // queued: re-routing it once the window closes would still act on something
  // the user had not read.
  if (everShown_ && millis() - phaseShownAtMs_ < kSettleMs) return;

  fui::InputSnapshot input;
  input.touchReleased = true;
  input.touchX = static_cast<int16_t>(tapX);
  input.touchY = static_cast<int16_t>(tapY);
  const fui::ActionEvent event = interactions_.route(input);

  switch (event.action) {
    case notesui::ActionOpenNote:
      if (event.value >= 0 && event.value < static_cast<int>(rowIds_.size())) {
        openNote(rowIds_[static_cast<size_t>(event.value)]);
      }
      break;
    case notesui::ActionNewNote: {
      RenderLock lock(*this);
      phase_ = Phase::KindPick;
      requestUpdate();
      break;
    }
    case notesui::ActionKindText:
      edit(Editing::NewText, "NEW NOTE", "", notes::kMaxBodyChars, false);
      break;
    case notesui::ActionKindChecklist:
      edit(Editing::NewList, "LIST NAME", "", notes::kMaxTitleChars, false);
      break;
    case notesui::ActionEditBody: {
      const notes::Note* note = currentNote();
      if (note != nullptr) edit(Editing::Body, "EDIT NOTE", note->body, notes::kMaxBodyChars, true);
      break;
    }
    case notesui::ActionToggleItem:
      toggleItem(event.value);
      break;
    case notesui::ActionAddItem:
      edit(Editing::NewItem, "NEW ITEM", "", notes::kMaxItemChars, false);
      break;
    case notesui::ActionEditMode: {
      RenderLock lock(*this);
      itemEditMode_ = true;
      requestUpdate();
      break;
    }
    case notesui::ActionDoneEditing: {
      RenderLock lock(*this);
      itemEditMode_ = false;
      requestUpdate();
      break;
    }
    case notesui::ActionEditItem: {
      const notes::Note* note = currentNote();
      if (note == nullptr || event.value < 0 || event.value >= static_cast<int>(note->items.size())) break;
      editItem_ = event.value;
      edit(Editing::ItemText, "EDIT ITEM", note->items[static_cast<size_t>(event.value)].text, notes::kMaxItemChars,
           true);
      break;
    }
    case notesui::ActionRenameList: {
      const notes::Note* note = currentNote();
      if (note != nullptr) edit(Editing::ListName, "LIST NAME", note->name, notes::kMaxTitleChars, true);
      break;
    }
    case notesui::ActionPagePrev:
      turnPage(-1);
      break;
    case notesui::ActionPageNext:
      turnPage(1);
      break;
    case notesui::ActionDeleteConfirm:
      performDelete();
      break;
    case notesui::ActionDeleteCancel: {
      const notes::Note* note = currentNote();
      RenderLock lock(*this);
      target_ = Target::None;
      targetItem_ = -1;
      phase_ = note == nullptr ? Phase::List : (note->kind == notes::Kind::Text ? Phase::Text : Phase::Checklist);
      requestUpdate();
      break;
    }
    default:
      break;
  }
}

// --- Drawing -------------------------------------------------------------

void NotesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  // The reading cut in the body slot. This app is words somebody wrote, not a
  // board, so it is set in the reading face the way the readers are.
  fui::GfxRendererTarget target = toybox::makeTarget(renderer, toybox::readingFaces());
  const fui::DeviceContext device = target.deviceContext();
  const fui::ThemeTokens& tokens = toybox::themeTokens();
  const fui::InputSnapshot noInput{};
  interactionsReady_ = false;
  toybox::Frame frame(target, device, noInput, interactions_);
  toybox::Screen screen(frame);

  const char* what = "Notes";

  switch (phase_) {
    case Phase::List: {
      const std::vector<notes::Note>& all = store_.notes();
      rowIds_.clear();
      rowLabels_.clear();
      rowSubtitles_.clear();
      rows_.clear();

      const int16_t rowHeight = notesui::listRowHeight(target, tokens);
      listVisible_ = fui::listVisibleRows(notesui::listBand(device), rowHeight, tokens.listRowGap);
      if (listVisible_ <= 0) listVisible_ = 1;
      const int count = static_cast<int>(all.size());
      if (listTop_ >= count) listTop_ = 0;
      const int shown = count - listTop_ < listVisible_ ? count - listTop_ : listVisible_;

      fui::TextStyle titleStyle = tokens.bodyText;
      titleStyle.maxLines = 1;
      const int16_t titleWidth = notesui::listTitleWidth(device, tokens);
      rowIds_.reserve(static_cast<size_t>(shown > 0 ? shown : 0));
      rowLabels_.reserve(static_cast<size_t>(shown > 0 ? shown : 0));
      rowSubtitles_.reserve(static_cast<size_t>(shown > 0 ? shown : 0));
      for (int i = 0; i < shown; ++i) {
        const notes::Note& note = all[static_cast<size_t>(listTop_ + i)];
        rowIds_.push_back(note.id);
        rowLabels_.push_back(toybox::fitLines(target, notes::displayTitle(note).c_str(), titleWidth, 1, titleStyle));
        char date[16];
        formatDate(note.updated, date, sizeof(date));
        char subtitle[48];
        if (note.kind == notes::Kind::Checklist) {
          std::snprintf(subtitle, sizeof(subtitle), "%d OF %d DONE%s%s", notes::doneCount(note),
                        static_cast<int>(note.items.size()), date[0] != '\0' ? "  .  " : "", date);
        } else {
          std::snprintf(subtitle, sizeof(subtitle), "NOTE%s%s", date[0] != '\0' ? "  .  " : "", date);
        }
        rowSubtitles_.push_back(subtitle);
      }
      // A second pass, because a push_back can reallocate and ListItem holds
      // pointers rather than copies.
      rows_.reserve(rowLabels_.size());
      for (size_t i = 0; i < rowLabels_.size(); ++i) {
        fui::ListItem row;
        row.label = rowLabels_[i].c_str();
        row.subtitle = rowSubtitles_[i].c_str();
        // The index into rowIds_, which holds THIS PAGE. Both are rebuilt
        // together in this loop, so a tap resolves to the note on the row that
        // was drawn -- never to the note that would be there on page one.
        row.actionValue = static_cast<int16_t>(i);
        rows_.push_back(row);
      }

      if (count > 0) {
        std::snprintf(countLabel_, sizeof(countLabel_), "%d", count);
      } else {
        countLabel_[0] = '\0';
      }

      notesui::ListModel model;
      model.items = rows_.empty() ? nullptr : rows_.data();
      model.count = static_cast<int>(rows_.size());
      model.topIndex = 0;  // the model is handed a PAGE, never the whole list
      model.countLabel = countLabel_[0] != '\0' ? countLabel_ : nullptr;
      notesui::buildList(screen, model);
      what = "Notes list";
      break;
    }

    case Phase::KindPick:
      notesui::buildKindPick(screen);
      what = "Notes kind";
      break;

    case Phase::Text: {
      const notes::Note* note = currentNote();
      if (note == nullptr) {
        phase_ = Phase::List;
        break;
      }
      bandTitle_ = notes::displayTitle(*note);
      bodyText_ = notes::displayBody(*note);

      const fui::Rect body = notesui::textBodyRect(device);
      const int16_t lineHeight = target.lineHeight(tokens.bodyText.font);
      visibleLines_ = fui::textAreaVisibleLines(body, lineHeight);

      notesui::TextBody text;
      text.text = bodyText_.c_str();
      text.style = tokens.bodyText;
      text.wrap = &wrap_;
      const uint32_t measured = notesui::textLineCount(target, device, text);
      lineCount_ = measured;

      // Clamped here rather than where the page was turned, because the count
      // does not exist until something has measured the text -- and an edit
      // can make a note shorter than the page somebody was on.
      if (visibleLines_ > 0 && topLine_ >= lineCount_ && lineCount_ > 0) {
        topLine_ = (lineCount_ - 1) / visibleLines_ * visibleLines_;
      }

      notesui::TextModel model;
      model.title = bandTitle_.c_str();
      model.topLine = topLine_;
      model.empty = bodyText_.empty();
      if (visibleLines_ > 0 && lineCount_ > visibleLines_) {
        const unsigned long page = topLine_ / visibleLines_ + 1;
        const unsigned long pages = (lineCount_ + visibleLines_ - 1) / visibleLines_;
        std::snprintf(pageLabel_, sizeof(pageLabel_), "%lu / %lu", page, pages);
        model.pageLabel = pageLabel_;
        model.canPagePrev = topLine_ > 0;
        model.canPageNext = topLine_ + visibleLines_ < lineCount_;
      }
      const uint32_t drawn = notesui::buildText(screen, model, text);
      // Take the count the panel was really drawn from; see buildText.
      if (drawn > 0) lineCount_ = drawn;
      what = "Notes text";
      break;
    }

    case Phase::Checklist: {
      const notes::Note* note = currentNote();
      if (note == nullptr) {
        phase_ = Phase::List;
        break;
      }
      bandTitle_ = notes::displayTitle(*note);

      const int16_t rowHeight = notesui::checklistRowHeight(tokens);
      const fui::Rect band = notesui::checklistBand(device);
      itemVisible_ = rowHeight > 0 ? band.height / rowHeight : 0;
      if (itemVisible_ <= 0) itemVisible_ = 1;

      const int count = static_cast<int>(note->items.size());
      if (itemTop_ >= count) itemTop_ = 0;
      const int shown = count - itemTop_ < itemVisible_ ? count - itemTop_ : itemVisible_;

      itemLabels_.clear();
      itemRows_.clear();
      fui::TextStyle itemStyle = tokens.bodyText;
      itemStyle.maxLines = 1;
      // The room a label really gets: the row less the box, the two gaps and
      // the side padding buildChecklist hands the component.
      const int16_t labelWidth = static_cast<int16_t>(band.width - 26 - 3 * toybox::kGutter);
      itemLabels_.reserve(static_cast<size_t>(shown > 0 ? shown : 0));
      for (int i = 0; i < shown; ++i) {
        const notes::Item& item = note->items[static_cast<size_t>(itemTop_ + i)];
        itemLabels_.push_back(toybox::fitLines(target, item.text.c_str(), labelWidth, 1, itemStyle));
      }
      itemRows_.reserve(itemLabels_.size());
      for (int i = 0; i < static_cast<int>(itemLabels_.size()); ++i) {
        notesui::ChecklistRow row;
        row.text = itemLabels_[static_cast<size_t>(i)].c_str();
        row.done = note->items[static_cast<size_t>(itemTop_ + i)].done;
        itemRows_.push_back(row);
      }

      std::snprintf(progressLabel_, sizeof(progressLabel_), "%d/%d", notes::doneCount(*note), count);
      char pages[kPageLabelCap] = "";
      if (count > itemVisible_) {
        std::snprintf(pages, sizeof(pages), "%d / %d", itemTop_ / itemVisible_ + 1,
                      (count + itemVisible_ - 1) / itemVisible_);
      }
      std::snprintf(pageLabel_, sizeof(pageLabel_), "%s", pages);

      notesui::ChecklistModel model;
      model.title = bandTitle_.c_str();
      model.rows = itemRows_.empty() ? nullptr : itemRows_.data();
      model.count = static_cast<int>(itemRows_.size());
      model.firstIndex = itemTop_;
      model.progressLabel = count > 0 ? progressLabel_ : nullptr;
      model.pageLabel = pageLabel_[0] != '\0' ? pageLabel_ : nullptr;
      model.editing = itemEditMode_;
      model.empty = count == 0;
      notesui::buildChecklist(screen, model);
      what = "Notes checklist";
      break;
    }

    case Phase::Confirm: {
      notesui::ConfirmModel model;
      model.title = confirmTitle_.c_str();
      model.detail = confirmDetail_;
      notesui::buildDeleteConfirm(screen, model);
      what = "Notes confirm";
      break;
    }
  }

  interactionsReady_ = true;
  toybox::reportOverflow(interactions_, what);
  const bool phaseChanged = !everShown_ || phase_ != lastShownPhase_;

  // Both devices this fork builds for have touch, and drawButtonHints()
  // returns early on any board that does, so these labels reach no panel
  // today. Passed because every app here passes them and a lone exception
  // would read as an oversight.
  const auto labels = mappedInput.mapLabels("Back", "", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  // Stamped AFTER the panel has been written, so the settle window measures
  // from when a person could first have seen this screen rather than from when
  // the state changed. displayBuffer() blocks on the panel, so by here it is
  // showing.
  if (phaseChanged) {
    lastShownPhase_ = phase_;
    phaseShownAtMs_ = millis();
    everShown_ = true;
  }
}
