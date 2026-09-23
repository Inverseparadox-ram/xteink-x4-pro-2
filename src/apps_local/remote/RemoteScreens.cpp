#include "RemoteScreens.h"

#include <FreeInkUIIcon.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../ui/ToyboxIcons.h"
#include "../ui/ToyboxText.h"
#include "RemoteVault.h"

namespace remoteui {
namespace {

constexpr int kBodyTop = toybox::kBodyTop;
constexpr int kFooterHeight = toybox::kPillHeight;

fui::TextStyle plain(const fui::FontId font, const fui::TextAlign align = fui::TextAlign::Left,
                     const fui::Color color = fui::Color::Black, const uint8_t maxLines = 1) {
  fui::TextStyle style;
  style.font = font;
  style.align = align;
  style.color = color;
  style.maxLines = maxLines;
  return style;
}

void chrome(toybox::Screen& screen, const char* title, const char* rightLabel, const bool offerForget) {
  fui::HeaderProps header;
  header.title = title;
  header.rightLabel = rightLabel;
  header.borderEdges = fui::EdgesNone;
  if (rightLabel != nullptr) {
    // Paper, not ink: the band is black and subtitleText defaults to Black.
    header.subtitleText = screen.theme().smallText;
    header.subtitleText.color = fui::Color::White;
    header.subtitleText.align = fui::TextAlign::Right;
  }
  if (offerForget) {
    header.trailingIcon = fui::bitmapFromIcon(icon_remote_unlink_32);
    header.trailingAction = ActionForget;
    // Styled for the BAND. Left unset it resolves to the page palette, which
    // is a black glyph on a black fill -- drawn, tappable and invisible.
    header.trailingStyles = toybox::bandOutlineStyles();
  }
  toybox::absoluteChrome(screen);
  toybox::headerBand(screen, header);
  screen.insetContent(fui::Insets{toybox::kGutter * 3, toybox::kMargin, toybox::kMargin, toybox::kMargin});
}

// A button that is a mark and nothing else. The component centres an icon
// when there is no label, which is exactly what every control on this screen
// wants.
void iconButton(toybox::Screen& screen, const fui::Rect& rect, const freeink::Icon& icon, const int16_t size,
                const fui::ActionId action, const bool primary) {
  fui::ButtonProps button;
  button.icon = fui::bitmapFromIcon(icon);
  button.iconSize = size;
  button.action = action;
  if (!primary) button.styles = toybox::rowStyles();
  screen.button(button, rect);
}

// A mark with a number beside it, for the two seek buttons. `label` may be
// null, and then this is an icon button: a profile that cannot promise a
// number must not print one.
void iconLabelButton(toybox::Screen& screen, const fui::Rect& rect, const freeink::Icon& icon, const char* label,
                     const fui::ActionId action) {
  fui::ButtonProps button;
  button.icon = fui::bitmapFromIcon(icon);
  button.iconSize = 40;
  button.label = label != nullptr && label[0] != '\0' ? label : nullptr;
  button.action = action;
  button.styles = toybox::rowStyles();
  screen.button(button, rect);
}

// Which padlock the unlock button wears. The face says what a tap will DO --
// an open shackle unlocks, a closed one locks -- so it is read from the Mac's
// last answer and never from what the reader last did.
const freeink::Icon& unlockIcon(const UnlockFace face) {
  switch (face) {
    case UnlockFace::Unlock:
      return icon_rc_unlock_40;
    case UnlockFace::Lock:
      return icon_rc_lock_40;
    case UnlockFace::Ask:
    default:
      return icon_rc_ask_40;
  }
}

}  // namespace

void buildRemote(toybox::Screen& screen, const RemoteModel& model) {
  chrome(screen, "REMOTE", nullptr, true);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t gutter = static_cast<int16_t>(toybox::kGutter);
  int16_t y = static_cast<int16_t>(kBodyTop);

  // The link state is a MARK, not a sentence. Connected needs no words at all
  // -- the controls under it are the whole message -- so it is one small
  // bluetooth glyph and nothing else. Unpaired is the one case that has to
  // use words, because a remote the Mac cannot see has to say where to look,
  // and no icon says "System Settings > Bluetooth".
  //
  // The unlock button borrows the same slot for the one thing it has to be
  // able to say. "Connected" and "the helper that answers the unlock button is
  // running" are different facts and the mark only carries the first, so when
  // the second fails the row has to use words.
  const int16_t statusSize = 32;
  const bool haveWords = model.pairingHint != nullptr && model.pairingHint[0] != '\0';
  if (!haveWords) {
    screen.target().bitmap(
        fui::makeRect(static_cast<int16_t>(toybox::kMargin + width - statusSize), y, statusSize, statusSize),
        fui::bitmapFromIcon(model.connected ? icon_rc_btok_32 : icon_rc_bt_32), fui::BitmapMode::Contain,
        fui::Paint::solid(fui::Color::Black));
    y = static_cast<int16_t>(y + statusSize + gutter);
  } else {
    const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
    screen.target().bitmap(fui::makeRect(toybox::kMargin, y, statusSize, statusSize),
                           fui::bitmapFromIcon(model.connected ? icon_rc_btok_32 : icon_rc_bt_32),
                           fui::BitmapMode::Contain, fui::Paint::solid(fui::Color::Black));
    const int16_t textX = static_cast<int16_t>(toybox::kMargin + statusSize + gutter);
    screen.target().text(
        fui::makeRect(textX, y, static_cast<int16_t>(width - statusSize - gutter), static_cast<int16_t>(lineH * 4)),
        model.pairingHint, plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::Black, 4));
    y = static_cast<int16_t>(y + lineH * 4 + gutter);
  }
  screen.target().fill(fui::makeRect(toybox::kMargin, y, width, toybox::kRule), fui::Paint::solid(fui::Color::Black));
  y = static_cast<int16_t>(y + toybox::kRule + gutter);

  // --- Transport: three marks, no words -----------------------------------
  // Equal thirds now, because nothing here has a label to be squeezed by. The
  // 64px art is why they can be: a mark this size needs no caption to be read
  // across a room.
  // The rows are sized from the room that is LEFT, not from fixed heights.
  // Fixed ones left about a hundred and fifty pixels of nothing above the
  // footer -- on a screen whose every control is a touch target, empty space
  // is target that was not given to anything.
  const int16_t footerTop = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  // Exactly four gutters come out: three between the four rows and one above
  // the footer. Counting them wrong is how a band of dead space appears under
  // the last row, which on a panel of touch targets is target given to
  // nothing.
  const int16_t rowsHeight = static_cast<int16_t>(footerTop - y - gutter * 4);
  // The transport takes a little under a third and the three rows under it
  // share the rest: it holds the three controls a hand goes to without looking.
  const int16_t transportH = static_cast<int16_t>(rowsHeight * 28 / 100);
  const int16_t rowH = static_cast<int16_t>((rowsHeight - transportH) / 3);
  const int16_t third = static_cast<int16_t>((width - 2 * gutter) / 3);
  iconButton(screen, fui::makeRect(toybox::kMargin, y, third, transportH), icon_rc_prev_64, 64, ActionPrevious, false);
  iconButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + third + gutter), y, third, transportH),
             icon_rc_toggle_64, 64, ActionPlayPause, true);
  iconButton(screen,
             fui::makeRect(static_cast<int16_t>(toybox::kMargin + 2 * (third + gutter)), y,
                           static_cast<int16_t>(width - 2 * (third + gutter)), transportH),
             icon_rc_next_64, 64, ActionNext, false);
  y = static_cast<int16_t>(y + transportH + gutter);

  // --- Seek ---------------------------------------------------------------
  // A circular arrow, and a NUMBER only where the profile can keep one. Under
  // a profile that cannot promise seconds the button is the mark alone rather
  // than a word standing in for a number -- which is the same rule the labels
  // followed before, with less text.
  const int16_t half = static_cast<int16_t>((width - gutter) / 2);
  iconLabelButton(screen, fui::makeRect(toybox::kMargin, y, half, rowH), icon_rc_back_40, model.backSeconds,
                  ActionBack);
  iconLabelButton(screen,
                  fui::makeRect(static_cast<int16_t>(toybox::kMargin + half + gutter), y,
                                static_cast<int16_t>(width - half - gutter), rowH),
                  icon_rc_fwd_40, model.forwardSeconds, ActionForward);
  y = static_cast<int16_t>(y + rowH + gutter);

  // --- The three Mac shortcuts --------------------------------------------
  // A microphone for Siri, Claude's mark, and a padlock. None of the three is
  // a media key: the first two type a keyboard shortcut, which is the only
  // thing a HID peripheral can do about an application, and the third is the
  // one control on this panel that will not act until the Mac has answered it.
  //
  // This row replaced explicit PLAY, PAUSE and STOP. Those were the transport
  // toggle spelled out three times, and the toggle above is what macOS honours
  // most reliably anyway -- so they cost a third of the panel and added one
  // control the toggle did not already give.
  const int16_t trio = static_cast<int16_t>((width - 2 * gutter) / 3);
  iconButton(screen, fui::makeRect(toybox::kMargin, y, trio, rowH), icon_rc_siri_40, 40, ActionSiri, false);
  iconButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + trio + gutter), y, trio, rowH),
             icon_rc_claude_40, 40, ActionClaude, false);
  // The face is the last VERIFIED answer, and the band is filled while a
  // challenge is out -- four seconds of a locked Mac waking its helper is long
  // enough that an unchanged button reads as one that did nothing.
  iconButton(screen,
             fui::makeRect(static_cast<int16_t>(toybox::kMargin + 2 * (trio + gutter)), y,
                           static_cast<int16_t>(width - 2 * (trio + gutter)), rowH),
             unlockIcon(model.unlockFace), 40, ActionUnlock, model.unlockBusy);
  y = static_cast<int16_t>(y + rowH + gutter);

  // --- Volume: two buttons and a mute --------------------------------------
  // A slider drew a position this app had guessed, and the guess was wrong the
  // moment anyone touched the volume on the Mac -- HID sends steps and cannot
  // read a level back. Two buttons claim nothing: one tap is one step, which
  // is exactly what goes over the wire.
  //
  // The speaker mark stays as the row's label so a bare minus and plus are not
  // left to say on their own which of several things they change.
  const int16_t markSize = 32;
  screen.target().bitmap(
      fui::makeRect(toybox::kMargin, static_cast<int16_t>(y + (rowH - markSize) / 2), markSize, markSize),
      fui::bitmapFromIcon(icon_rc_vol_32), fui::BitmapMode::Contain, fui::Paint::solid(fui::Color::Black));

  const int16_t volLeft = static_cast<int16_t>(toybox::kMargin + markSize + gutter);
  const int16_t volWidth = static_cast<int16_t>(width - markSize - gutter);
  const int16_t volThird = static_cast<int16_t>((volWidth - 2 * gutter) / 3);
  iconButton(screen, fui::makeRect(volLeft, y, volThird, rowH), icon_rc_voldn_40, 40, ActionVolumeDown, false);
  iconButton(screen, fui::makeRect(static_cast<int16_t>(volLeft + volThird + gutter), y, volThird, rowH),
             icon_rc_volup_40, 40, ActionVolumeUp, false);
  // Filled while muted, so the button's own band carries the one piece of
  // state the remote is entitled to remember: that IT sent a mute.
  iconButton(screen,
             fui::makeRect(static_cast<int16_t>(volLeft + 2 * (volThird + gutter)), y,
                           static_cast<int16_t>(volWidth - 2 * (volThird + gutter)), rowH),
             icon_rc_mute_32, 32, ActionMute, model.muted);

  // --- The profile, which is the only word left -------------------------
  // One short name, because no mark distinguishes YouTube from IINA from a
  // blind scrub, and the seek buttons above mean different things under each.
  fui::ButtonProps profile;
  profile.label = model.profileName;
  profile.action = ActionProfile;
  profile.styles = toybox::rowStyles();
  screen.button(profile, fui::makeRect(toybox::kMargin, footerTop, width, kFooterHeight));
}

void buildForgetConfirm(toybox::Screen& screen, const ForgetModel& model) {
  chrome(screen, "UNPAIR?", nullptr, false);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);
  const int16_t bodyH = screen.target().lineHeight(toybox::kUiFont);

  screen.target().text(fui::makeRect(toybox::kMargin, top, width, static_cast<int16_t>(bodyH * 6)), model.detail,
                       plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::Black, 6));

  // KEEP IT on the primary band a thumb expects; UNPAIR smaller, outlined and
  // a full margin above it. Same rule as every other confirm in this fork.
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  fui::ButtonProps keep;
  keep.label = "KEEP IT";
  keep.action = ActionForgetCancel;
  screen.button(keep, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));

  const int16_t unpairWidth = static_cast<int16_t>(width / 2);
  fui::ButtonProps unpair;
  unpair.label = "UNPAIR";
  unpair.action = ActionForgetConfirm;
  unpair.styles = toybox::rowStyles();
  screen.button(unpair, fui::makeRect(static_cast<int16_t>(toybox::kMargin + (width - unpairWidth) / 2),
                                      static_cast<int16_t>(footerY - kFooterHeight - toybox::kMargin * 2), unpairWidth,
                                      kFooterHeight));
}

// --- Unlock ----------------------------------------------------------------

void buildPin(toybox::Screen& screen, const PinModel& model) {
  chrome(screen, model.title, nullptr, false);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t gutter = static_cast<int16_t>(toybox::kGutter);
  int16_t y = static_cast<int16_t>(kBodyTop);

  if (model.detail != nullptr && model.detail[0] != '\0') {
    const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
    screen.target().text(fui::makeRect(toybox::kMargin, y, width, static_cast<int16_t>(lineH * 3)), model.detail,
                         plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::Black, 3));
    y = static_cast<int16_t>(y + lineH * 3 + gutter);
  }

  // How many digits, never which. A PIN drawn back at the person typing it is
  // a PIN readable from across the room, and this one is typed at a lock
  // screen in public more often than anywhere else.
  constexpr int16_t kSlot = 18;
  constexpr int16_t kSlotGap = 14;
  // As many marks as the SHORTEST PIN the pad will take, and then one more per
  // digit beyond it. Drawing all twelve up front reads as an instruction to
  // type twelve, which four-digit PINs are then a failure to follow.
  const int16_t floorSlots = static_cast<int16_t>(remote::vault::kPinMinLen);
  const int16_t slots = model.entered > floorSlots ? static_cast<int16_t>(model.entered) : floorSlots;
  const int16_t slotsWidth = static_cast<int16_t>(slots * kSlot + (slots - 1) * kSlotGap);
  int16_t slotX = static_cast<int16_t>(toybox::kMargin + (width - slotsWidth) / 2);
  for (int16_t i = 0; i < slots; ++i) {
    const fui::Rect where = fui::makeRect(slotX, y, kSlot, kSlot);
    if (i < static_cast<int16_t>(model.entered)) {
      screen.target().fill(where, fui::Paint::solid(fui::Color::Black), kSlot / 2);
    } else {
      // A hairline ring rather than nothing, so the length of the PIN the pad
      // will take is visible before any of it is typed.
      screen.target().stroke(where, fui::Paint::solid(fui::Color::Black), 1, kSlot / 2);
    }
    slotX = static_cast<int16_t>(slotX + kSlot + kSlotGap);
  }
  y = static_cast<int16_t>(y + kSlot + gutter * 2);

  // Ten digits, three to a row, with backspace and confirm on the last -- the
  // same pad the comic number uses, because a second arrangement of the same
  // twelve keys is a second thing to learn.
  const int16_t footerTop = static_cast<int16_t>(device.height - toybox::kMargin);
  const int16_t keyW = static_cast<int16_t>((width - gutter * 2) / 3);
  const int16_t keyH = static_cast<int16_t>((footerTop - y - gutter * 3) / 4);
  static const char* kKeys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "<", "0", "OK"};
  for (int i = 0; i < 12; ++i) {
    const int row = i / 3;
    const int col = i % 3;
    fui::ButtonProps key;
    key.label = kKeys[i];
    // The one key on the pad that is the device agreeing rather than a digit
    // keeps the primary band; everything else is outlined. Same rule as every
    // confirm in this fork.
    const bool primary = i == 11 && model.canConfirm;
    if (!primary) key.styles = toybox::rowStyles();
    if (i == 9) {
      key.action = ActionPinBack;
      key.enabled = model.entered > 0;
    } else if (i == 11) {
      key.action = ActionPinOk;
      key.enabled = model.canConfirm;
    } else {
      key.action = ActionPinDigit;
      key.value = static_cast<int16_t>(i == 10 ? 0 : i + 1);
      key.enabled = model.entered < remote::vault::kPinMaxLen;
    }
    screen.button(key, fui::makeRect(static_cast<int16_t>(toybox::kMargin + col * (keyW + gutter)),
                                     static_cast<int16_t>(y + row * (keyH + gutter)), keyW, keyH));
  }
}

void buildPair(toybox::Screen& screen, const PairModel& model) {
  chrome(screen, "PAIR", nullptr, false);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t gutter = static_cast<int16_t>(toybox::kGutter);
  int16_t y = static_cast<int16_t>(kBodyTop);

  screen.target().bitmap(fui::makeRect(static_cast<int16_t>(toybox::kMargin + (width - 40) / 2), y, 40, 40),
                         fui::bitmapFromIcon(icon_rc_key_40), fui::BitmapMode::Contain,
                         fui::Paint::solid(fui::Color::Black));
  y = static_cast<int16_t>(y + 40 + gutter);

  const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
  screen.target().text(fui::makeRect(toybox::kMargin, y, width, static_cast<int16_t>(lineH * 4)), model.detail,
                       plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::Black, 4));
  y = static_cast<int16_t>(y + lineH * 4 + gutter);

  // Eight groups of four on four lines. Thirty-two unbroken characters is a
  // line people lose their place in halfway across, and this one is read off a
  // screen and typed into another machine exactly once.
  const int16_t codeH = screen.target().lineHeight(toybox::kDisplayFont);
  char line[12];
  const size_t have = std::strlen(model.code);
  for (int row = 0; row < 4; ++row) {
    const size_t at = static_cast<size_t>(row) * 8;
    if (at + 8 > have) break;
    std::snprintf(line, sizeof(line), "%.4s %.4s", model.code + at, model.code + at + 4);
    screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(y + row * codeH), width, codeH), line,
                         plain(toybox::kDisplayFont, fui::TextAlign::Center, fui::Color::Black, 1));
  }
  y = static_cast<int16_t>(y + codeH * 4 + gutter);

  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  fui::ButtonProps done;
  done.label = "TYPED IT";
  done.action = ActionPairDone;
  screen.button(done, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));
}

}  // namespace remoteui
