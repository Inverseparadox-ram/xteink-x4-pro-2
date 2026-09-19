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

// A button with an icon above its word. The transport row is hit without
// looking, so the mark carries it and the word is there for the first week.
void tallButton(toybox::Screen& screen, const fui::Rect& rect, const freeink::Icon* icon, const char* label,
                const fui::ActionId action, const bool primary) {
  fui::ButtonProps button;
  // The mark is optional, and the middle of the transport row goes without
  // one. PREV and NEXT are one short word each and a chevron helps them; the
  // toggle's label is two words and the icon beside it cost exactly the pixels
  // that turned "PLAY/PAUSE" into "PLAY/PA...". A word that survives beats a
  // picture that truncates it.
  if (icon != nullptr) {
    button.icon = fui::bitmapFromIcon(*icon);
    button.iconSize = toybox::kIconSize;
  }
  button.label = label;
  button.action = action;
  if (!primary) button.styles = toybox::rowStyles();
  screen.button(button, rect);
}

void wideButton(toybox::Screen& screen, const fui::Rect& rect, const char* label, const fui::ActionId action,
                const bool primary) {
  fui::ButtonProps button;
  button.label = label;
  button.action = action;
  if (!primary) button.styles = toybox::rowStyles();
  screen.button(button, rect);
}

}  // namespace

void buildRemote(toybox::Screen& screen, const RemoteModel& model) {
  chrome(screen, "REMOTE", model.linkLabel, true);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t gutter = static_cast<int16_t>(toybox::kGutter);
  int16_t y = static_cast<int16_t>(kBodyTop);

  // Nothing connected: say so, and say what to do about it, before any
  // control. A row of buttons that silently do nothing is the worst screen
  // this app could show.
  if (!model.connected && model.pairingHint[0] != '\0') {
    const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
    // Four lines. Three cut the sentence at "It stays di...", and the half it
    // cut is the half that says the device only advertises while this screen
    // is open -- which is the one thing a person needs to know when the Mac
    // cannot find it.
    screen.target().text(fui::makeRect(toybox::kMargin, y, width, static_cast<int16_t>(lineH * 4)),
                         model.pairingHint, plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::Black, 4));
    y = static_cast<int16_t>(y + lineH * 4 + gutter);
    screen.target().fill(fui::makeRect(toybox::kMargin, y, width, toybox::kRule),
                         fui::Paint::solid(fui::Color::Black));
    y = static_cast<int16_t>(y + toybox::kRule + gutter);
  }

  // --- Transport: the three a hand finds without looking ------------------
  // Not three equal thirds. The middle is the button this app exists for and
  // the only one whose label is two words, and an even split elided it to
  // "PLAY/P...". The sides carry a mark and one short word and need less.
  const int16_t transportH = static_cast<int16_t>(kFooterHeight * 2);
  const int16_t side = static_cast<int16_t>(width * 27 / 100);
  const int16_t middle = static_cast<int16_t>(width - 2 * side - 2 * gutter);
  tallButton(screen, fui::makeRect(toybox::kMargin, y, side, transportH), &icon_remote_prev_32, "PREV",
             ActionPrevious, false);
  tallButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + side + gutter), y, middle, transportH),
             nullptr, "PLAY/PAUSE", ActionPlayPause, true);
  tallButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + side + middle + 2 * gutter), y, side,
                                   transportH),
             &icon_remote_next_32, "NEXT", ActionNext, false);
  y = static_cast<int16_t>(y + transportH + gutter);

  // --- Seek ---------------------------------------------------------------
  const int16_t half = static_cast<int16_t>((width - gutter) / 2);
  wideButton(screen, fui::makeRect(toybox::kMargin, y, half, kFooterHeight), model.backLabel, ActionBack, false);
  wideButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + half + gutter), y,
                                   static_cast<int16_t>(width - half - gutter), kFooterHeight),
             model.forwardLabel, ActionForward, false);
  y = static_cast<int16_t>(y + kFooterHeight + gutter);

  // --- Explicit PLAY / PAUSE / STOP ---------------------------------------
  // Separate from the toggle on purpose. The toggle is the one macOS honours
  // most reliably, but it is a toggle: when the remote and the Mac disagree
  // about what is playing -- which they always might, since nothing comes back
  // -- the only way to be sure is a button that means one thing.
  const int16_t trio = static_cast<int16_t>((width - 2 * gutter) / 3);
  wideButton(screen, fui::makeRect(toybox::kMargin, y, trio, kFooterHeight), "PLAY", ActionPlay, false);
  wideButton(screen, fui::makeRect(static_cast<int16_t>(toybox::kMargin + trio + gutter), y, trio, kFooterHeight),
             "PAUSE", ActionPause, false);
  wideButton(screen,
             fui::makeRect(static_cast<int16_t>(toybox::kMargin + 2 * (trio + gutter)), y,
                           static_cast<int16_t>(width - 2 * (trio + gutter)), kFooterHeight),
             "STOP", ActionStop, false);
  y = static_cast<int16_t>(y + kFooterHeight + gutter * 3);

  // --- Volume -------------------------------------------------------------
  const int16_t capH = screen.target().lineHeight(toybox::kTileFont);
  screen.target().text(fui::makeRect(toybox::kMargin, y, width, capH), model.volumeCaption,
                       plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray));
  y = static_cast<int16_t>(y + capH + gutter / 2);

  const int16_t muteW = static_cast<int16_t>(kFooterHeight * 2);
  const int16_t sliderW = static_cast<int16_t>(width - muteW - gutter);
  fui::SliderProps slider;
  slider.value = model.volume;
  slider.max = model.volumeMax;
  slider.action = ActionVolume;
  // Taller than the component's default: this is the one control on the screen
  // that is dragged rather than tapped, and a 22px knob on a panel held at
  // arm's length is a control you miss.
  slider.knobHeight = 34;
  slider.knobWidth = 18;
  slider.trackHeight = 6;
  fui::slider(screen.frame(), fui::makeRect(toybox::kMargin, y, sliderW, kFooterHeight), slider);

  fui::ButtonProps mute;
  mute.label = model.muted ? "UNMUTE" : "MUTE";
  mute.action = ActionMute;
  // Filled while muted, so the band of the button itself carries the state the
  // remote is allowed to remember: that IT sent a mute. It cannot know the
  // Mac's real mute, and the caption below says as much.
  if (!model.muted) mute.styles = toybox::rowStyles();
  screen.button(mute, fui::makeRect(static_cast<int16_t>(toybox::kMargin + sliderW + gutter), y, muteW,
                                    kFooterHeight));

  // --- The profile row, which is the footer -------------------------------
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  const int16_t noteH = screen.target().lineHeight(toybox::kTileFont);
  screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(footerY - noteH - gutter / 2), width,
                                     noteH),
                       model.profileNote, plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray));
  fui::ButtonProps profile;
  profile.label = model.profileName;
  profile.action = ActionProfile;
  profile.styles = toybox::rowStyles();
  screen.button(profile, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));
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
                                      static_cast<int16_t>(footerY - kFooterHeight - toybox::kMargin * 2),
                                      unpairWidth, kFooterHeight));
}

}  // namespace remoteui
