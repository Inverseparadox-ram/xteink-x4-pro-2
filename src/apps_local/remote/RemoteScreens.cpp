#include "RemoteScreens.h"

#include <FreeInkUIIcon.h>

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
  const int16_t statusSize = 32;
  if (model.connected) {
    screen.target().bitmap(
        fui::makeRect(static_cast<int16_t>(toybox::kMargin + width - statusSize), y, statusSize, statusSize),
        fui::bitmapFromIcon(icon_rc_btok_32), fui::BitmapMode::Contain, fui::Paint::solid(fui::Color::Black));
    y = static_cast<int16_t>(y + statusSize + gutter);
  } else {
    const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
    screen.target().bitmap(fui::makeRect(toybox::kMargin, y, statusSize, statusSize),
                           fui::bitmapFromIcon(icon_rc_bt_32), fui::BitmapMode::Contain,
                           fui::Paint::solid(fui::Color::Black));
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
  const int16_t avail = static_cast<int16_t>(footerTop - gutter * 2 - y);
  const int16_t rowsHeight = static_cast<int16_t>(avail - gutter * 3);
  // The transport is a third of it and the three rows under it share the
  // rest: it holds the three controls a hand goes to without looking.
  const int16_t transportH = static_cast<int16_t>(rowsHeight * 34 / 100);
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

  // --- Explicit play, pause, stop -----------------------------------------
  // Kept, and now wordless. The toggle above is what macOS honours most
  // reliably and is still a toggle; when the two ends disagree about what is
  // playing -- which they always might, since nothing comes back -- only a
  // control that means one thing gets you out.
  const int16_t trio = static_cast<int16_t>((width - 2 * gutter) / 3);
  iconButton(screen, fui::makeRect(toybox::kMargin, y, trio, rowH), icon_rc_play_40, 40, ActionPlay, false);
  iconButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + trio + gutter), y, trio, rowH),
             icon_rc_pause_40, 40, ActionPause, false);
  iconButton(screen,
             fui::makeRect(static_cast<int16_t>(toybox::kMargin + 2 * (trio + gutter)), y,
                           static_cast<int16_t>(width - 2 * (trio + gutter)), rowH),
             icon_rc_stop_40, 40, ActionStop, false);
  y = static_cast<int16_t>(y + rowH + gutter);

  // --- Volume -------------------------------------------------------------
  // The speaker mark replaces the "VOLUME 8 / 16" caption entirely. The
  // slider's own knob says where the count is, and the caption was the text
  // most obviously doing a picture's job.
  const int16_t markSize = 32;
  const int16_t muteW = static_cast<int16_t>(rowH * 3 / 2);
  const int16_t sliderW = static_cast<int16_t>(width - markSize - muteW - gutter * 2);
  screen.target().bitmap(
      fui::makeRect(toybox::kMargin, static_cast<int16_t>(y + (rowH - markSize) / 2), markSize, markSize),
      fui::bitmapFromIcon(icon_rc_vol_32), fui::BitmapMode::Contain, fui::Paint::solid(fui::Color::Black));

  fui::SliderProps slider;
  slider.value = model.volume;
  slider.max = model.volumeMax;
  slider.action = ActionVolume;
  // Taller than the component's default: the one control here that is dragged
  // rather than tapped, on a panel held at arm's length.
  slider.knobHeight = 34;
  slider.knobWidth = 18;
  slider.trackHeight = 6;
  fui::slider(screen.frame(),
              fui::makeRect(static_cast<int16_t>(toybox::kMargin + markSize + gutter), y, sliderW, rowH), slider);

  // Filled while muted, so the button's own band carries the one piece of
  // state the remote is entitled to remember: that IT sent a mute.
  iconButton(screen,
             fui::makeRect(static_cast<int16_t>(toybox::kMargin + markSize + sliderW + gutter * 2), y, muteW, rowH),
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

}  // namespace remoteui
