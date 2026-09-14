#include "NotesScreens.h"

#include <FreeInkUIIcon.h>

#include <string>

#include "../ui/ToyboxIcons.h"
#include "../ui/ToyboxText.h"

namespace notesui {
namespace {

// The top of any body: below the header band AND the rule Toybox draws under
// it, which is what kChromeHeight names. Shared by every screen here so they
// line up with each other and with the shelf the user just came from.
constexpr int kBodyTop = toybox::kBodyTop;
constexpr int kFooterHeight = toybox::kPillHeight;

// What the footer costs the body: the bar, the margin under it, and the gutter
// above it. Named once because three screens subtract it.
constexpr int kFooterReserve = toybox::kMargin + kFooterHeight + toybox::kGutter;

void chrome(toybox::Screen& screen, const char* title, const char* rightLabel,
            const fui::TextStyle* titleText = nullptr) {
  fui::HeaderProps header;
  header.title = title;
  header.rightLabel = rightLabel;
  header.borderEdges = fui::EdgesNone;
  if (titleText != nullptr) header.titleText = *titleText;
  if (rightLabel != nullptr) {
    // Paper, not ink. The band is solid black and the header draws rightLabel
    // in subtitleText, whose default colour is Black -- a label left at the
    // default is painted black on black and simply is not there. Study,
    // Hacker News and Instapaper each paid for this once.
    header.subtitleText = screen.theme().smallText;
    header.subtitleText.color = fui::Color::White;
    header.subtitleText.align = fui::TextAlign::Right;
  }
  toybox::absoluteChrome(screen);
  toybox::headerBand(screen, header);
  screen.insetContent(fui::Insets{toybox::kGutter * 3, toybox::kMargin, toybox::kMargin, toybox::kMargin});
}

fui::TextStyle plain(const fui::FontId font, const fui::TextAlign align = fui::TextAlign::Left,
                     const fui::Color color = fui::Color::Black, const uint8_t maxLines = 1) {
  fui::TextStyle style;
  style.font = font;
  style.align = align;
  style.color = color;
  style.maxLines = maxLines;
  return style;
}

fui::TextStyle rowSubtitleStyle(const fui::ThemeTokens& tokens) {
  fui::TextStyle subtitle = tokens.smallText;
  subtitle.font = toybox::kTileFont;
  // FONT_SLOT_SMALL is 0, and textStyleUnset() reads a style whose font is 0
  // and every other field default as "the caller did not set this" -- so
  // Screen::list() would put the theme's value back and the row would draw at
  // full size. Naming the alignment the component applies anyway is what marks
  // this style as owned.
  subtitle.align = fui::TextAlign::Left;
  return subtitle;
}

// The two lines every empty screen in this app draws, at the sizes the panel's
// own metrics give rather than at chosen ones. Instapaper's empty queue
// hardcoded a 40px box for a cut whose advanceY is 63 and the two lines
// touched on the panel; asking the target is what stops that happening again.
void emptyState(toybox::Screen& screen, const char* headline, const char* sentence) {
  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t headlineH = screen.target().lineHeight(toybox::kDisplayFont);
  const int16_t bodyH = screen.target().lineHeight(toybox::kUiFont);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);
  screen.target().text(fui::makeRect(toybox::kMargin, top, width, headlineH), headline,
                       plain(toybox::kDisplayFont, fui::TextAlign::Center));
  screen.target().text(
      fui::makeRect(toybox::kMargin, static_cast<int16_t>(top + headlineH + toybox::kGutter), width,
                    static_cast<int16_t>(bodyH * 3)),
      sentence, plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::DarkGray, 3));
}

// A footer of one wide primary button and one square icon button at its right,
// which is the shape three of these screens want. The wide button keeps its
// LEFT edge wherever the square is present or not, so the pixels a remembered
// tap lands on never change meaning -- Instapaper's account control earned
// that rule.
void footerWithSquare(toybox::Screen& screen, const char* label, const fui::ActionId action,
                      const freeink::Icon* squareIcon, const fui::ActionId squareAction) {
  const fui::DeviceContext& device = screen.device();
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  const int16_t usable = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t side = squareIcon != nullptr ? static_cast<int16_t>(kFooterHeight) : 0;
  const int16_t gap = squareIcon != nullptr ? static_cast<int16_t>(toybox::kGutter) : 0;
  const int16_t barWidth = static_cast<int16_t>(usable - side - gap);

  fui::ButtonProps primary;
  primary.label = label;
  primary.action = action;
  screen.button(primary, fui::makeRect(toybox::kMargin, footerY, barWidth, kFooterHeight));

  if (squareIcon == nullptr) return;
  fui::ButtonProps square;
  square.icon = fui::bitmapFromIcon(*squareIcon);
  square.iconSize = toybox::kIconSize;
  square.action = squareAction;
  // Outlined, so it reads as secondary to the filled primary rather than as a
  // second control of equal weight.
  square.styles = toybox::rowStyles();
  screen.button(square, fui::makeRect(static_cast<int16_t>(toybox::kMargin + barWidth + gap), footerY, side,
                                      kFooterHeight));
}

}  // namespace

// --- The note list -------------------------------------------------------

fui::Rect listBand(const fui::DeviceContext& device) {
  return fui::makeRect(toybox::kMargin, kBodyTop, static_cast<int16_t>(device.width - 2 * toybox::kMargin),
                       static_cast<int16_t>(device.height - kFooterReserve - kBodyTop));
}

int16_t listRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& tokens) {
  // One line of title and one of subtitle, plus air. ONE line of title, and
  // that is forced rather than chosen: a row's title band is one line tall
  // whenever a subtitle is set, so a wrapping label draws straight through the
  // subtitle beneath it. The Activity fits the title to one line for the same
  // reason.
  return static_cast<int16_t>(target.lineHeight(tokens.bodyText.font) + target.lineHeight(toybox::kTileFont) +
                              toybox::kGutter);
}

int16_t listTitleWidth(const fui::DeviceContext& device, const fui::ThemeTokens& tokens) {
  return static_cast<int16_t>(listBand(device).width - 2 * tokens.listSidePadding);
}

void buildList(toybox::Screen& screen, const ListModel& model) {
  chrome(screen, "NOTES", model.countLabel);

  // Taken before the list so the list can never grow into it. The door is
  // always here, including on an empty shelf -- an empty shelf is precisely
  // when somebody wants to make a note, and a screen whose only control
  // appears once there is something to look at teaches nobody where it lives.
  footerWithSquare(screen, "NEW NOTE", ActionNewNote, nullptr, fui::NO_ACTION);

  if (model.count <= 0) {
    emptyState(screen, "NO NOTES YET",
               "Tap NEW NOTE to write one. Everything you write is also saved to /Notes on the card.");
    return;
  }

  fui::ListProps list;
  list.items = model.items;
  list.count = static_cast<uint16_t>(model.count);
  list.topIndex = static_cast<uint16_t>(model.topIndex);
  list.selectedIndex = -1;  // touch-first; nothing is "selected" until it is opened
  list.action = ActionOpenNote;
  list.rowHeight = listRowHeight(screen.target(), screen.theme());
  list.labelText = screen.theme().bodyText;
  list.labelText.maxLines = 1;
  list.subtitleText = rowSubtitleStyle(screen.theme());
  // Off, or a title is capped at 60% of the row so it sits prettily beside a
  // value. These rows carry no value and the title IS the content.
  list.balanceWrappedLabelWithValue = false;
  screen.list(list, listBand(screen.device()).height, fui::LayoutAnchor::Top);
}

// --- Pick a kind ---------------------------------------------------------

void buildKindPick(toybox::Screen& screen) {
  chrome(screen, "NEW NOTE", nullptr);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  // Two targets of equal weight, because the question has two equal answers.
  // Tall rather than pill-height: this is the only thing on the screen and a
  // 52px bar floating in 600px of paper reads as an accident.
  const int16_t height = static_cast<int16_t>(kFooterHeight * 2);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);

  const int16_t captionH = screen.target().lineHeight(toybox::kTileFont);
  const auto choice = [&](const char* label, const char* caption, const fui::ActionId action, const int16_t y) {
    fui::ButtonProps button;
    button.label = label;
    button.action = action;
    screen.button(button, fui::makeRect(toybox::kMargin, y, width, height));
    // The caption sits UNDER the button rather than inside it: a two-line
    // button label draws at the button's own cut and the second line is as
    // loud as the first, which makes both choices shout.
    screen.target().text(
        fui::makeRect(toybox::kMargin, static_cast<int16_t>(y + height + toybox::kGutter / 2), width, captionH),
        caption, plain(toybox::kTileFont, fui::TextAlign::Center, fui::Color::DarkGray));
  };

  choice("TEXT NOTE", "PROSE. THE FIRST LINE BECOMES THE TITLE.", ActionKindText, top);
  const int16_t second = static_cast<int16_t>(top + height + captionH + toybox::kMargin * 2);
  choice("CHECKLIST", "THINGS TO DO. TAP A ROW TO TICK IT.", ActionKindChecklist, second);
}

// --- A text note ---------------------------------------------------------

fui::Rect textBodyRect(const fui::DeviceContext& device) {
  return fui::makeRect(toybox::kMargin, kBodyTop, static_cast<int16_t>(device.width - 2 * toybox::kMargin),
                       static_cast<int16_t>(device.height - kFooterReserve - kBodyTop));
}

uint32_t textLineCount(const fui::DrawTarget& target, const fui::DeviceContext& device, TextBody& body) {
  if (body.wrap == nullptr) return 0;
  return body.wrap->lineCount(target, textBodyRect(device).width, body.text, body.style);
}

uint32_t buildText(toybox::Screen& screen, const TextModel& model, TextBody& body) {
  // The band carries the note's own first line, which is somebody's sentence
  // in their own case -- so it borrows the reading cut in paper, the way a
  // book's running header borrows the text's, rather than shouting it in the
  // chrome face.
  fui::TextStyle bandTitle = screen.theme().bodyText;
  bandTitle.color = fui::Color::White;
  bandTitle.maxLines = 1;
  const fui::TextStyle& labelStyle = screen.theme().smallText;
  const int16_t labelWidth =
      model.pageLabel != nullptr && model.pageLabel[0] != '\0'
          ? static_cast<int16_t>(screen.target().measureText(labelStyle.font, model.pageLabel, labelStyle).width +
                                 toybox::kGutter)
          : 0;
  const int16_t room = static_cast<int16_t>(screen.device().width - 2 * toybox::kMargin - labelWidth);
  const std::string headline = toybox::fitLines(screen.target(), model.title, room, 1, bandTitle);
  chrome(screen, headline.c_str(), model.pageLabel, &bandTitle);

  const fui::DeviceContext& device = screen.device();
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  const int16_t usable = static_cast<int16_t>(device.width - 2 * toybox::kMargin);

  // EDIT keeps the left edge of the bar on every page, and the two arrows take
  // the right. A single page draws EDIT across the whole bar: there is nothing
  // to page, and two permanently dead arrows are the documented failure mode
  // this fork calls a dead gesture.
  const bool paged = model.canPagePrev || model.canPageNext;
  if (!paged) {
    fui::ButtonProps edit;
    edit.label = "EDIT";
    edit.action = ActionEditBody;
    screen.button(edit, fui::makeRect(toybox::kMargin, footerY, usable, kFooterHeight));
  } else {
    const int16_t arrow = static_cast<int16_t>(fui::ButtonProps{}.minTouchSize + toybox::kGutter);
    const int16_t editWidth = static_cast<int16_t>(usable - 2 * arrow - 2 * toybox::kGutter);
    fui::ButtonProps edit;
    edit.label = "EDIT";
    edit.action = ActionEditBody;
    screen.button(edit, fui::makeRect(toybox::kMargin, footerY, editWidth, kFooterHeight));

    const auto arrowButton = [&](const char* label, const fui::ActionId action, const int16_t x, const bool enabled) {
      fui::ButtonProps button;
      button.label = label;
      button.action = enabled ? action : fui::NO_ACTION;
      // Dimmed rather than removed: a control that vanishes moves its
      // neighbours, and on e-ink that costs a repaint of the whole bar.
      button.styles = enabled ? toybox::rowStyles() : toybox::disabledButtonStyles();
      screen.button(button, fui::makeRect(x, footerY, arrow, kFooterHeight));
    };
    const int16_t prevX = static_cast<int16_t>(toybox::kMargin + editWidth + toybox::kGutter);
    arrowButton("<", ActionPagePrev, prevX, model.canPagePrev);
    arrowButton(">", ActionPageNext, static_cast<int16_t>(prevX + arrow + toybox::kGutter), model.canPageNext);
  }

  if (model.empty) {
    emptyState(screen, "EMPTY", "This note is just a title. Tap EDIT to write the rest of it.");
    return 0;
  }

  // Drawn into textBodyRect() rather than into the Screen's running content
  // rect, because the Activity counts the lines that fit in this exact
  // rectangle to decide what a page turn does. Two ways of arriving at the
  // same rectangle is how a page turn starts eating a line.
  //
  // Through the wrap rather than fui::textArea(), which walks the text from
  // byte zero to find the lines it is about to draw -- so drawing page ten
  // would cost wrapping pages one to ten as well, again on every turn.
  if (body.wrap == nullptr) return 0;
  body.wrap->draw(screen.target(), textBodyRect(device), body.text, body.style, model.topLine);
  // Asked AFTER the drawing, and cheap because the wrap has just answered it.
  return body.wrap->lineCount(screen.target(), textBodyRect(device).width, body.text, body.style);
}

// --- A checklist ---------------------------------------------------------

int16_t checklistRowHeight(const fui::ThemeTokens& tokens) {
  // Screen::checkbox() takes theme rowHeight and uses spaceSm as the gap, so
  // the Activity's paging arithmetic has to use the same sum. Derived here
  // rather than restated there.
  return static_cast<int16_t>(tokens.rowHeight + tokens.spaceSm);
}

fui::Rect checklistBand(const fui::DeviceContext& device) {
  return fui::makeRect(toybox::kMargin, kBodyTop, static_cast<int16_t>(device.width - 2 * toybox::kMargin),
                       static_cast<int16_t>(device.height - kFooterReserve - kBodyTop));
}

void buildChecklist(toybox::Screen& screen, const ChecklistModel& model) {
  // EDITING is shouted in the chrome face and replaces the list's name,
  // because a mode that changes what a tap does has to be visible from across
  // the room. The name is still one tap away (leave the mode) and the progress
  // label keeps its place either way.
  fui::TextStyle bandTitle = screen.theme().bodyText;
  bandTitle.color = fui::Color::White;
  bandTitle.maxLines = 1;
  const fui::TextStyle& labelStyle = screen.theme().smallText;
  const char* right = model.editing ? model.pageLabel : model.progressLabel;
  const int16_t labelWidth =
      right != nullptr && right[0] != '\0'
          ? static_cast<int16_t>(screen.target().measureText(labelStyle.font, right, labelStyle).width +
                                 toybox::kGutter)
          : 0;
  const int16_t room = static_cast<int16_t>(screen.device().width - 2 * toybox::kMargin - labelWidth);

  if (model.editing) {
    chrome(screen, "EDITING", right);
  } else {
    const std::string headline = toybox::fitLines(screen.target(), model.title, room, 1, bandTitle);
    chrome(screen, headline.c_str(), right, &bandTitle);
  }

  // Taken before the rows so the rows can never grow into it.
  if (model.editing) {
    // RENAME keeps its own word's width and DONE takes the rest as the primary:
    // leaving the mode is the common answer and it gets the thumb's half of
    // the bar.
    const fui::DeviceContext& device = screen.device();
    const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
    const int16_t usable = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
    const fui::TextStyle& buttonLabel = screen.theme().bodyText;
    const int16_t renameWanted = static_cast<int16_t>(
        screen.target().measureText(buttonLabel.font, "RENAME", buttonLabel).width + toybox::kMargin * 2);
    const int16_t renameCap = static_cast<int16_t>((usable - toybox::kGutter) / 2);
    const int16_t renameWidth = renameWanted > renameCap ? renameCap : renameWanted;

    fui::ButtonProps rename;
    rename.label = "RENAME";
    rename.action = ActionRenameList;
    rename.styles = toybox::rowStyles();
    screen.button(rename, fui::makeRect(toybox::kMargin, footerY, renameWidth, kFooterHeight));

    fui::ButtonProps done;
    done.label = "DONE";
    done.action = ActionDoneEditing;
    screen.button(done, fui::makeRect(static_cast<int16_t>(toybox::kMargin + renameWidth + toybox::kGutter), footerY,
                                      static_cast<int16_t>(usable - renameWidth - toybox::kGutter), kFooterHeight));
  } else {
    footerWithSquare(screen, "ADD ITEM", ActionAddItem, &icon_notes_edit_32, ActionEditMode);
  }

  if (model.empty) {
    emptyState(screen, "NOTHING ON IT", "Tap ADD ITEM to put the first thing on this list.");
    return;
  }

  for (int i = 0; i < model.count; ++i) {
    fui::CheckboxProps row;
    row.label = model.rows[i].text;
    row.checked = model.rows[i].done;
    // The whole point of the mode: the same row reports a different action, and
    // the band says which. In edit mode a tap opens the item for rewording (and
    // the keyboard's own header button deletes it); out of it, a tap ticks.
    row.action = model.editing ? ActionEditItem : ActionToggleItem;
    row.value = static_cast<int16_t>(model.firstIndex + i);
    row.text = screen.theme().bodyText;
    row.text.maxLines = 1;
    // The box is drawn at the reading cut's own scale rather than the
    // component's 18px default, which is a desktop checkbox on a panel whose
    // rows are 62px tall.
    row.boxSize = 26;
    row.sidePadding = static_cast<int16_t>(toybox::kGutter);
    row.gap = static_cast<int16_t>(toybox::kGutter);
    row.styles = toybox::rowStyles();
    screen.checkbox(row);
  }
}

// --- Delete confirm ------------------------------------------------------

void buildDeleteConfirm(toybox::Screen& screen, const ConfirmModel& model) {
  chrome(screen, "DELETE?", nullptr);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);

  // The name of the thing being destroyed, at the display cut, because the
  // whole job of this screen is to let somebody check they are deleting the
  // note they think they are.
  const int16_t headlineH = screen.target().lineHeight(toybox::kDisplayFont);
  fui::TextStyle name = screen.theme().bodyText;
  name.align = fui::TextAlign::Center;
  name.color = fui::Color::Black;
  name.maxLines = 2;
  const std::string shown = toybox::fitLines(screen.target(), model.title, width, 2, name);
  screen.target().text(fui::makeRect(toybox::kMargin, top, width, static_cast<int16_t>(headlineH * 2)), shown.c_str(),
                       name);

  const int16_t bodyH = screen.target().lineHeight(toybox::kUiFont);
  screen.target().text(
      fui::makeRect(toybox::kMargin, static_cast<int16_t>(top + headlineH * 2 + toybox::kGutter), width,
                    static_cast<int16_t>(bodyH * 3)),
      model.detail, plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::DarkGray, 3));

  // KEEP IT is the prominent one and sits on the fork-wide primary-action
  // band, where a thumb expects "the button". DELETE is a smaller outlined
  // control a full margin above it, on pixels no control on the screen before
  // occupied -- so a stray or remembered tap keeps the note, and only a
  // deliberate press on the marked control destroys it.
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  fui::ButtonProps keep;
  keep.label = "KEEP IT";
  keep.action = ActionDeleteCancel;
  screen.button(keep, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));

  const int16_t deleteWidth = static_cast<int16_t>(width / 2);
  fui::ButtonProps destroy;
  destroy.label = "DELETE";
  destroy.action = ActionDeleteConfirm;
  destroy.styles = toybox::rowStyles();
  screen.button(destroy, fui::makeRect(static_cast<int16_t>(toybox::kMargin + (width - deleteWidth) / 2),
                                       static_cast<int16_t>(footerY - kFooterHeight - toybox::kMargin * 2),
                                       deleteWidth, kFooterHeight));
}

}  // namespace notesui
