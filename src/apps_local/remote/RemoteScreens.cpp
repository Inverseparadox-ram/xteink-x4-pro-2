#include "RemoteScreens.h"

#include <FreeInkUIIcon.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../ui/ToyboxIcons.h"
#include "../ui/ToyboxText.h"

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

void chrome(toybox::Screen& screen, const char* title, const char* rightLabel, const bool offerNextPage) {
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
  if (offerNextPage) {
    // The page arrow. Unpair used to live here and moved to the MAC page's
    // foot: it is used once per Mac, and this is the control used every visit.
    header.trailingIcon = fui::bitmapFromIcon(icon_rc_page_32);
    header.trailingAction = ActionNextPage;
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
  chrome(screen, "REMOTE", pageLabel(0), true);

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
  const bool haveSong = !haveWords && model.connected && model.nowTitle != nullptr && model.nowTitle[0] != '\0';
  if (haveSong) {
    // Title over artist, beside the connected mark. One line each and cut with
    // an ellipsis rather than wrapped: this row sits above the transport, and
    // a title that grew a second line would push every control down with it.
    const fui::TextStyle titleStyle = plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::Black, 1);
    const fui::TextStyle artistStyle = plain(toybox::kSmallFont, fui::TextAlign::Left, fui::Color::Black, 1);
    const int16_t titleH = screen.target().lineHeight(toybox::kUiFont);
    const int16_t artistH = screen.target().lineHeight(toybox::kSmallFont);
    const int16_t textW = static_cast<int16_t>(width - statusSize - gutter);
    const std::string title = toybox::fitLines(screen.target(), model.nowTitle, textW, 1, titleStyle);
    screen.target().text(fui::makeRect(toybox::kMargin, y, textW, titleH), title.c_str(), titleStyle);
    const bool haveArtist = model.nowArtist != nullptr && model.nowArtist[0] != '\0';
    if (haveArtist) {
      const std::string artist = toybox::fitLines(screen.target(), model.nowArtist, textW, 1, artistStyle);
      screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(y + titleH), textW, artistH),
                           artist.c_str(), artistStyle);
    }
    screen.target().bitmap(
        fui::makeRect(static_cast<int16_t>(toybox::kMargin + width - statusSize), y, statusSize, statusSize),
        fui::bitmapFromIcon(icon_rc_btok_32), fui::BitmapMode::Contain, fui::Paint::solid(fui::Color::Black));
    // Both lines reserved even when there is no artist, so the controls sit in
    // the same place for every song rather than jumping when one lacks a name.
    const int16_t rowH = static_cast<int16_t>(titleH + artistH);
    y = static_cast<int16_t>(y + (rowH > statusSize ? rowH : statusSize) + gutter);
  } else if (!haveWords && model.connected) {
    // Nothing playing: the reader's own time over its charge, in the same two
    // lines a song uses, so the controls under them never move between the
    // two.
    const int16_t timeH = screen.target().lineHeight(toybox::kUiFont);
    const int16_t chargeH = screen.target().lineHeight(toybox::kSmallFont);
    const int16_t textW = static_cast<int16_t>(width - statusSize - gutter);
    if (model.clockText != nullptr && model.clockText[0] != '\0') {
      screen.target().text(fui::makeRect(toybox::kMargin, y, textW, timeH), model.clockText,
                           plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::Black, 1));
    }
    if (model.batteryPercent >= 0) {
      // A gauge drawn to the real level, then the number. The number is the
      // fact; the gauge is what reads at a glance.
      const int16_t bodyW = 26;
      const int16_t bodyH = 13;
      const int16_t gaugeY = static_cast<int16_t>(y + timeH + (chargeH - bodyH) / 2);
      const fui::Rect body = fui::makeRect(toybox::kMargin, gaugeY, bodyW, bodyH);
      screen.target().stroke(body, fui::Paint::solid(fui::Color::Black), 2, 2);
      screen.target().fill(fui::makeRect(static_cast<int16_t>(toybox::kMargin + bodyW),
                                         static_cast<int16_t>(gaugeY + 4), 3, static_cast<int16_t>(bodyH - 8)),
                           fui::Paint::solid(fui::Color::Black));
      const int percent = model.batteryPercent > 100 ? 100 : model.batteryPercent;
      const int16_t inner = static_cast<int16_t>((bodyW - 6) * percent / 100);
      if (inner > 0) {
        screen.target().fill(fui::makeRect(static_cast<int16_t>(toybox::kMargin + 3), static_cast<int16_t>(gaugeY + 3),
                                           inner, static_cast<int16_t>(bodyH - 6)),
                             fui::Paint::solid(fui::Color::Black));
      }
      char charge[16];
      std::snprintf(charge, sizeof(charge), "%d%%", percent);
      screen.target().text(fui::makeRect(static_cast<int16_t>(toybox::kMargin + bodyW + 3 + gutter / 2),
                                         static_cast<int16_t>(y + timeH), textW, chargeH),
                           charge, plain(toybox::kSmallFont, fui::TextAlign::Left, fui::Color::Black, 1));
    }
    screen.target().bitmap(
        fui::makeRect(static_cast<int16_t>(toybox::kMargin + width - statusSize), y, statusSize, statusSize),
        fui::bitmapFromIcon(icon_rc_btok_32), fui::BitmapMode::Contain, fui::Paint::solid(fui::Color::Black));
    const int16_t rowH = static_cast<int16_t>(timeH + chargeH);
    y = static_cast<int16_t>(y + (rowH > statusSize ? rowH : statusSize) + gutter);
  } else if (!haveWords) {
    screen.target().bitmap(
        fui::makeRect(static_cast<int16_t>(toybox::kMargin + width - statusSize), y, statusSize, statusSize),
        fui::bitmapFromIcon(icon_rc_bt_32), fui::BitmapMode::Contain, fui::Paint::solid(fui::Color::Black));
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

  // --- F8, the microphones, the padlock -----------------------------------
  // F8 is a plain key held across two taps. The other two need the Mac to
  // act and answer, so they ride the unlock helper's GATT service.
  //
  // Each band is filled only for a state the remote actually knows: F8 while
  // IT is holding the key, the microphone while the MAC has said every input
  // is muted. The microphone stays outlined until the Mac has said anything,
  // rather than guessing.
  const int16_t trio = static_cast<int16_t>((width - 2 * gutter) / 3);
  // A pen, for what holding F8 is for here: dictation.
  iconButton(screen, fui::makeRect(toybox::kMargin, y, trio, rowH), icon_rc_pen_40, 40, ActionHoldKey, model.keyHeld);
  iconButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + trio + gutter), y, trio, rowH),
             icon_rc_micoff_40, 40, ActionMicrophone, model.micKnown && model.micMuted);
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
  // The same three columns as the row above, edge to edge, so the panel reads
  // as a grid rather than two rows that almost line up.
  const char* volumeLabel = model.tvVolume ? "TV" : nullptr;
  iconLabelButton(screen, fui::makeRect(toybox::kMargin, y, trio, rowH), icon_rc_voldn_40, volumeLabel,
                  ActionVolumeDown);
  iconLabelButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + trio + gutter), y, trio, rowH),
                  icon_rc_volup_40, volumeLabel, ActionVolumeUp);
  // Filled while muted, so the button's own band carries the one piece of
  // state the remote is entitled to remember: that IT sent a mute.
  iconButton(screen,
             fui::makeRect(static_cast<int16_t>(toybox::kMargin + 2 * (trio + gutter)), y,
                           static_cast<int16_t>(width - 2 * (trio + gutter)), rowH),
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

  // Either side key does the same as this button; see the detail line.
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  fui::ButtonProps done;
  done.label = "TYPED IT";
  done.action = ActionPairDone;
  screen.button(done, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));
}

// --- Pages 2 and 3 -----------------------------------------------------------

const char* pageLabel(const int page) {
  static constexpr const char* kLabels[kPageCount] = {"1/3", "2/3", "3/3"};
  return page >= 0 && page < kPageCount ? kLabels[page] : "";
}

void buildStatusPage(toybox::Screen& screen, const StatusPageModel& model) {
  chrome(screen, model.title, pageLabel(model.page), true);
  const fui::DeviceContext& device = screen.device();
  fui::DrawTarget& target = screen.target();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t gutter = static_cast<int16_t>(toybox::kGutter);
  const int16_t lineH = target.lineHeight(toybox::kUiFont);
  const int16_t smallH = target.lineHeight(toybox::kSmallFont);
  int16_t bottom = static_cast<int16_t>(device.height - toybox::kMargin);

  // The foot: REFRESH, with Unpair beside it on the MAC page. Unpair is
  // outlined because it undoes the pairing; REFRESH is the page's one primary
  // control, since nothing reaches this page unless it is asked for.
  {
    const int16_t y = static_cast<int16_t>(bottom - kFooterHeight);
    const int16_t half = static_cast<int16_t>((width - gutter) / 2);
    const bool connected = model.helperConnected;
    int16_t refreshX = static_cast<int16_t>(toybox::kMargin + (width - half) / 2);
    if (model.offerForget) {
      fui::ButtonProps forget;
      forget.icon = fui::bitmapFromIcon(icon_remote_unlink_32);
      forget.iconSize = 32;
      forget.label = "UNPAIR";
      forget.action = ActionForget;
      forget.styles = toybox::rowStyles();
      screen.button(forget, fui::makeRect(toybox::kMargin, y, half, kFooterHeight));
      refreshX = static_cast<int16_t>(toybox::kMargin + width - half);
    }
    if (connected) {
      fui::ButtonProps refresh;
      refresh.icon = fui::bitmapFromIcon(icon_weather_refresh_32);
      refresh.iconSize = 32;
      refresh.label = model.refreshing ? "ASKING" : "REFRESH";
      refresh.action = ActionRefreshBoard;
      screen.button(refresh, fui::makeRect(refreshX, y, half, kFooterHeight));
    }
    bottom = static_cast<int16_t>(y - gutter);
    if (connected && model.stamp != nullptr && model.stamp[0] != '\0') {
      bottom = static_cast<int16_t>(bottom - smallH);
      target.text(fui::makeRect(toybox::kMargin, bottom, width, smallH), model.stamp,
                  plain(toybox::kSmallFont, fui::TextAlign::Center, fui::Color::DarkGray));
      bottom = static_cast<int16_t>(bottom - gutter);
    }
  }

  int16_t y = static_cast<int16_t>(kBodyTop);
  const auto sentence = [&](const char* text) {
    target.text(fui::makeRect(toybox::kMargin, y, width, static_cast<int16_t>(lineH * 4)), text,
                plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::DarkGray, 4));
  };
  if (!model.helperConnected) {
    sentence("The Mac helper is not connected. Run crossplay-unlock on the Mac, then open this page again.");
    return;
  }
  if (model.board == nullptr || !model.board->known) {
    sentence("Waiting for the Mac...");
    return;
  }
  if (model.board->count == 0) {
    sentence(model.emptyLine);
    return;
  }

  // One row per item: the name and a detail line on the left, the status word
  // on the right. The two that want a person -- awaiting input and failed --
  // are set in reverse, so they are found from across a desk.
  const fui::TextStyle nameStyle = plain(toybox::kUiFont, fui::TextAlign::Left);
  const fui::TextStyle detailStyle = plain(toybox::kSmallFont, fui::TextAlign::Left, fui::Color::DarkGray);
  const fui::TextStyle wordStyle = plain(toybox::kSmallFont, fui::TextAlign::Center);
  const fui::TextStyle wordInverse = plain(toybox::kSmallFont, fui::TextAlign::Center, fui::Color::White);
  const int16_t rowH = static_cast<int16_t>(lineH + smallH + gutter);
  // The widest word decides the column, so every status sits in one column.
  int16_t wordW = 0;
  for (uint8_t i = 0; i < model.board->count; ++i) {
    const char* word = remote::statusWord(model.board->rows[i].status);
    const int16_t w = target.measureText(wordStyle.font, word, wordStyle).width;
    if (w > wordW) wordW = w;
  }
  wordW = static_cast<int16_t>(wordW + 2 * gutter);
  const int16_t nameW = static_cast<int16_t>(width - wordW - gutter);
  const int16_t pillH = static_cast<int16_t>(smallH + 8);

  int shown = 0;
  for (uint8_t i = 0; i < model.board->count; ++i) {
    if (y + rowH > bottom) break;
    const remote::StatusRow& row = model.board->rows[i];
    if (model.restartable && restartOffered(row.status)) {
      screen.frame().hit(fui::makeRect(toybox::kMargin, y, width, static_cast<int16_t>(rowH - gutter / 2)),
                         ActionRestartRow, static_cast<int16_t>(i));
    }
    target.text(fui::makeRect(toybox::kMargin, y, nameW, lineH),
                toybox::fitLines(target, row.title, nameW, 1, nameStyle).c_str(), nameStyle);
    if (row.detail[0] != '\0') {
      target.text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(y + lineH), nameW, smallH),
                  toybox::fitLines(target, row.detail, nameW, 1, detailStyle).c_str(), detailStyle);
    }
    const fui::Rect pill = fui::makeRect(static_cast<int16_t>(toybox::kMargin + width - wordW),
                                         static_cast<int16_t>(y + (lineH + smallH - pillH) / 2), wordW, pillH);
    const char* word = remote::statusWord(row.status);
    if (remote::statusNeedsAttention(row.status)) {
      target.fill(pill, fui::Paint::solid(fui::Color::Black), 6);
      target.text(pill, word, wordInverse);
    } else {
      target.stroke(pill, fui::Paint::solid(fui::Color::Black), 1, 6);
      target.text(pill, word, wordStyle);
    }
    y = static_cast<int16_t>(y + rowH);
    ++shown;
    if (i + 1 < model.board->count && y + rowH <= bottom) {
      target.fill(fui::makeRect(toybox::kMargin, static_cast<int16_t>(y - gutter / 2), width, 1),
                  fui::Paint::solid(fui::Color::Black));
    }
  }
  if (shown < model.board->count) {
    char more[24];
    std::snprintf(more, sizeof(more), "+%d more", model.board->count - shown);
    target.text(fui::makeRect(toybox::kMargin, y, width, smallH), more, detailStyle);
  }
}

bool restartOffered(const remote::StatusCode code) {
  return code == remote::StatusCode::Stopped || code == remote::StatusCode::Failed ||
         code == remote::StatusCode::Unknown;
}

void buildRestartConfirm(toybox::Screen& screen, const RestartModel& model) {
  chrome(screen, "RESTART?", nullptr, false);
  const fui::DeviceContext& device = screen.device();
  fui::DrawTarget& target = screen.target();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t lineH = target.lineHeight(toybox::kUiFont);
  int16_t y = static_cast<int16_t>(kBodyTop + toybox::kMargin);
  const fui::TextStyle name = plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::Black, 2);
  target.text(fui::makeRect(toybox::kMargin, y, width, static_cast<int16_t>(lineH * 2)),
              toybox::fitLines(target, model.title, width, 2, name).c_str(), name);
  y = static_cast<int16_t>(y + lineH * 2);
  target.text(fui::makeRect(toybox::kMargin, y, width, lineH), model.detail,
              plain(toybox::kSmallFont, fui::TextAlign::Center, fui::Color::DarkGray));
  y = static_cast<int16_t>(y + lineH + toybox::kGutter);
  target.text(fui::makeRect(toybox::kMargin, y, width, static_cast<int16_t>(lineH * 7)),
              "The Mac starts it again. If it is still down, Claude Code looks into it in the Service doctor "
              "chat. Tap REFRESH to see how it went.",
              plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::DarkGray, 7));

  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  fui::ButtonProps restart;
  restart.label = "RESTART";
  restart.action = ActionRestartConfirm;
  screen.button(restart, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));
  const int16_t cancelW = static_cast<int16_t>(width / 2);
  fui::ButtonProps cancel;
  cancel.label = "CANCEL";
  cancel.action = ActionRestartCancel;
  cancel.styles = toybox::rowStyles();
  screen.button(cancel, fui::makeRect(static_cast<int16_t>(toybox::kMargin + (width - cancelW) / 2),
                                      static_cast<int16_t>(footerY - kFooterHeight - toybox::kMargin * 2), cancelW,
                                      kFooterHeight));
}

}  // namespace remoteui
