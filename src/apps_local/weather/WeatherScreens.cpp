#include "WeatherScreens.h"

#include <FreeInkUIIcon.h>

#include <string>

#include "../ui/ToyboxIcons.h"
#include "../ui/ToyboxText.h"

namespace weatherui {
namespace {

constexpr int kBodyTop = toybox::kBodyTop;
constexpr int kFooterHeight = toybox::kPillHeight;
constexpr int kFooterReserve = toybox::kMargin + kFooterHeight + toybox::kGutter;

void chrome(toybox::Screen& screen, const char* title, const char* rightLabel,
            const fui::TextStyle* titleText = nullptr) {
  fui::HeaderProps header;
  header.title = title;
  header.rightLabel = rightLabel;
  header.borderEdges = fui::EdgesNone;
  if (titleText != nullptr) header.titleText = *titleText;
  if (rightLabel != nullptr) {
    // Paper, not ink: the band is solid black and subtitleText defaults to
    // Black, so an unstyled right label is painted black on black.
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
  subtitle.align = fui::TextAlign::Left;
  return subtitle;
}

void emptyState(toybox::Screen& screen, const char* headline, const char* sentence) {
  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t headlineH = screen.target().lineHeight(toybox::kDisplayFont);
  const int16_t bodyH = screen.target().lineHeight(toybox::kUiFont);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);
  screen.target().text(fui::makeRect(toybox::kMargin, top, width, headlineH), headline,
                       plain(toybox::kDisplayFont, fui::TextAlign::Center));
  screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(top + headlineH + toybox::kGutter), width,
                                     static_cast<int16_t>(bodyH * 3)),
                       sentence, plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::DarkGray, 3));
}

// The segmented bar: NOW | HOURS | WEEK, in the same pixels on all three
// views, with the current one filled and the others outlined. Drawn before any
// content so content can never grow into it.
//
// A segmented bar rather than swipes or side keys, because the three views are
// siblings and nothing on a swipe says which of three you are about to get.
// The side keys still page WITHIN a view, which is the axis docs/buttons.md
// already gives to content that continues downward.
void viewBar(toybox::Screen& screen, const View current) {
  const fui::DeviceContext& device = screen.device();
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  const int16_t usable = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t segment = static_cast<int16_t>((usable - 2 * toybox::kGutter) / 3);

  struct Segment {
    const char* label;
    fui::ActionId action;
    View view;
  };
  const Segment segments[3] = {
      {"NOW", ActionViewNow, View::Now},
      {"HOURS", ActionViewHours, View::Hours},
      {"WEEK", ActionViewWeek, View::Week},
  };

  for (int i = 0; i < 3; ++i) {
    fui::ButtonProps button;
    button.label = segments[i].label;
    // The current view still registers its action. A tap on it is a refresh of
    // the same screen rather than a dead control, and a dead control is a
    // documented failure mode in this fork.
    button.action = segments[i].action;
    if (segments[i].view != current) button.styles = toybox::rowStyles();
    const int16_t x = static_cast<int16_t>(toybox::kMargin + i * (segment + toybox::kGutter));
    // The last segment absorbs the rounding, so the bar always ends flush with
    // the right margin rather than a pixel or two short of it.
    const int16_t width = i == 2 ? static_cast<int16_t>(usable - 2 * (segment + toybox::kGutter)) : segment;
    screen.button(button, fui::makeRect(x, footerY, width, kFooterHeight));
  }
}

// The place name on the band, fitted to the room the header really gives it.
//
// `refreshable` puts the one control that fetches anything in the band, where
// it is visible on all three views and shares pixels with nothing. It is
// deliberately NOT the current segment of the view bar: re-tapping the segment
// you are already on is the same-pixel-different-action trap this fork keeps
// paying for, and on e-ink -- where a screen looks unchanged for up to two
// seconds -- a second tap meant as "did that work" would silently spend a
// network round trip.
void placeChrome(toybox::Screen& screen, const char* place, const char* rightLabel, const bool refreshable = false) {
  fui::TextStyle bandTitle = screen.theme().bodyText;
  bandTitle.color = fui::Color::White;
  bandTitle.maxLines = 1;
  // The title is NOT pre-fitted here. toybox::headerBand() fits it itself,
  // against toybox::headerTitleWidth(), which subtracts every term the header
  // component subtracts -- the side padding, the right label AND the trailing
  // button -- in the same order. Doing it again here meant a second copy of
  // that arithmetic, and the copy reached for the band height to size the
  // button: an additive expression on .headerHeight, which is precisely what
  // host-tests/chromeguard exists to stop, because a name for the black band
  // knows nothing about the rule drawn under it.
  fui::HeaderProps header;
  header.title = place;
  header.rightLabel = rightLabel;
  header.borderEdges = fui::EdgesNone;
  header.titleText = bandTitle;
  if (rightLabel != nullptr) {
    header.subtitleText = screen.theme().smallText;
    header.subtitleText.color = fui::Color::White;
    header.subtitleText.align = fui::TextAlign::Right;
  }
  if (refreshable) {
    header.trailingIcon = fui::bitmapFromIcon(icon_weather_refresh_32);
    header.trailingAction = ActionRefresh;
    // Styled for the BAND, not for the page. The header component leaves
    // trailingStyles unset by default, which resolves to the page palette --
    // a black fill carrying a black glyph, on a band that is already black.
    // The control drew, registered its hit rect, and was invisible.
    header.trailingStyles = toybox::bandOutlineStyles();
  }
  toybox::absoluteChrome(screen);
  toybox::headerBand(screen, header);
  screen.insetContent(fui::Insets{toybox::kGutter * 3, toybox::kMargin, toybox::kMargin, toybox::kMargin});
}

}  // namespace

// --- The saved places ----------------------------------------------------

fui::Rect placesBand(const fui::DeviceContext& device) {
  return fui::makeRect(toybox::kMargin, kBodyTop, static_cast<int16_t>(device.width - 2 * toybox::kMargin),
                       static_cast<int16_t>(device.height - kFooterReserve - kBodyTop));
}

int16_t placesRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& tokens) {
  return static_cast<int16_t>(target.lineHeight(tokens.bodyText.font) + target.lineHeight(toybox::kTileFont) +
                              toybox::kGutter);
}

void buildPlaces(toybox::Screen& screen, const PlacesModel& model) {
  fui::HeaderProps header;
  header.title = model.editing ? "REMOVE WHICH?" : "WEATHER";
  header.borderEdges = fui::EdgesNone;
  // The mode is offered only when there is something to remove; a control that
  // leads to an empty list is a control that does nothing.
  if (model.count > 0) {
    header.trailingIcon = fui::bitmapFromIcon(model.editing ? icon_weather_done_32 : icon_weather_remove_32);
    header.trailingAction = ActionEditPlaces;
    // Filled once the mode is on, so the band itself says which way round it
    // is even before the title is read. See the note beside refreshable.
    header.trailingStyles = model.editing ? toybox::bandFilledStyles() : toybox::bandOutlineStyles();
  }
  toybox::absoluteChrome(screen);
  toybox::headerBand(screen, header);
  screen.insetContent(fui::Insets{toybox::kGutter * 3, toybox::kMargin, toybox::kMargin, toybox::kMargin});

  const fui::DeviceContext& device = screen.device();
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  const int16_t usable = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  fui::ButtonProps add;
  add.label = model.editing ? "DONE" : "ADD A PLACE";
  add.action = model.editing ? ActionEditPlaces : ActionAddPlace;
  screen.button(add, fui::makeRect(toybox::kMargin, footerY, usable, kFooterHeight));

  if (model.count <= 0) {
    emptyState(screen, "NO PLACES YET",
               "Tap ADD A PLACE and search for a town. Forecasts come from Open-Meteo and are also saved to "
               "/Weather on the card.");
    return;
  }

  fui::ListProps list;
  list.items = model.items;
  list.count = static_cast<uint16_t>(model.count);
  list.topIndex = static_cast<uint16_t>(model.topIndex);
  list.selectedIndex = -1;
  list.action = model.editing ? ActionRemovePlace : ActionOpenPlace;
  list.rowHeight = placesRowHeight(screen.target(), screen.theme());
  list.labelText = screen.theme().bodyText;
  list.labelText.maxLines = 1;
  list.subtitleText = rowSubtitleStyle(screen.theme());
  list.valueText = rowSubtitleStyle(screen.theme());
  list.valueText.align = fui::TextAlign::Right;
  list.balanceWrappedLabelWithValue = false;
  screen.list(list, placesBand(device).height, fui::LayoutAnchor::Top);
}

// --- Search results ------------------------------------------------------

void buildResults(toybox::Screen& screen, const ResultsModel& model) {
  placeChrome(screen, model.query, nullptr);

  if (model.count <= 0) {
    if (model.searched) {
      emptyState(screen, "NO MATCH",
                 "Nothing by that name. Try the local spelling, or a larger town nearby.");
    } else {
      emptyState(screen, "SEARCHING", "Asking Open-Meteo which places have this name.");
    }
    return;
  }

  fui::ListProps list;
  list.items = model.items;
  list.count = static_cast<uint16_t>(model.count);
  list.topIndex = 0;
  list.selectedIndex = -1;
  list.action = ActionSearchResult;
  list.rowHeight = placesRowHeight(screen.target(), screen.theme());
  list.labelText = screen.theme().bodyText;
  list.labelText.maxLines = 1;
  list.subtitleText = rowSubtitleStyle(screen.theme());
  list.balanceWrappedLabelWithValue = false;
  // The whole body, because this screen has no footer: choosing a place IS the
  // action, and a bar under it would only hold a second way to leave.
  screen.list(list, static_cast<int16_t>(screen.device().height - kBodyTop - toybox::kMargin),
              fui::LayoutAnchor::Top);
}

// --- NOW -----------------------------------------------------------------

void buildNow(toybox::Screen& screen, const NowModel& model) {
  placeChrome(screen, model.place, nullptr, true);
  viewBar(screen, View::Now);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  int16_t y = static_cast<int16_t>(kBodyTop);

  // The headline: the number, then what it feels like, then the day's range.
  // Three lines, in the order somebody asks the questions.
  const int16_t bigH = screen.target().lineHeight(toybox::kDisplayFont);
  screen.target().text(fui::makeRect(toybox::kMargin, y, width, bigH), model.temperature,
                       plain(toybox::kDisplayFont, fui::TextAlign::Left));
  // The high/low rides the same line, right-aligned, because it is the same
  // kind of fact at a smaller weight and a second line for it would push the
  // table down by a row for no gain.
  if (model.highLow[0] != '\0') {
    screen.target().text(fui::makeRect(toybox::kMargin, y, width, bigH), model.highLow,
                         plain(toybox::kUiFont, fui::TextAlign::Right, fui::Color::Black));
  }
  y = static_cast<int16_t>(y + bigH);

  const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
  if (model.headline[0] != '\0') {
    screen.target().text(fui::makeRect(toybox::kMargin, y, width, lineH), model.headline,
                         plain(toybox::kUiFont, fui::TextAlign::Left));
    y = static_cast<int16_t>(y + lineH);
  }

  const int16_t smallH = screen.target().lineHeight(toybox::kTileFont);
  if (model.feelsLike[0] != '\0') {
    screen.target().text(fui::makeRect(toybox::kMargin, y, width, smallH), model.feelsLike,
                         plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray));
    y = static_cast<int16_t>(y + smallH);
  }
  if (model.observed[0] != '\0' || model.staleness[0] != '\0') {
    // When the reading was taken, and how old that makes it. Both, because
    // they answer different questions: the first is the forecast's own
    // timestamp in the PLACE's timezone, the second is how long ago this
    // device heard it. A screen showing only the first looks live forever.
    std::string when = model.observed;
    if (model.staleness[0] != '\0') {
      if (!when.empty()) when += "  .  ";
      when += model.staleness;
    }
    screen.target().text(fui::makeRect(toybox::kMargin, y, width, smallH), when.c_str(),
                         plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray));
    y = static_cast<int16_t>(y + smallH);
  }

  y = static_cast<int16_t>(y + toybox::kGutter);
  screen.target().fill(fui::makeRect(toybox::kMargin, y, width, toybox::kRule), fui::Paint::solid(fui::Color::Black));
  y = static_cast<int16_t>(y + toybox::kRule + toybox::kGutter);

  // The table: label, the plain-words note, then the number. Three columns on
  // ONE line, which is the whole reason this screen can carry eleven fields --
  // stacking the note under the value made each row 61px and only seven fit,
  // so a panel with room for humidity, wind, pressure, cloud, visibility, UV,
  // dew point, rain, sun times and elevation showed the first seven of them.
  //
  // The note is the middle column rather than an afterthought beside the
  // number, because it is what makes this descriptive: "1012 hPa" is a
  // readout and "1012 hPa, Normal" is a forecast.
  const int16_t rowH = static_cast<int16_t>(lineH + 2);
  const int16_t bottom = static_cast<int16_t>(device.height - kFooterReserve);
  // Three columns that do not overlap, and the arithmetic says so rather than
  // the eye: the value was right-aligned across the FULL width, so anything
  // wider than the value column ran back underneath the note and drew
  // "Gentle breeze14 km/h SSW". Each column now gets a rect of its own and
  // each string is fitted to it.
  // The columns are MEASURED, not chosen. Fixed fractions were tried twice and
  // both splits cut something this screen exists to carry: an even three-way
  // elided "14 km/h SSW" and "Dew point", and widening the value column to fix
  // that elided "Gentle breeze" and "Mostly cloudy" -- the plain-words notes,
  // which are the whole difference between a readout and a description.
  //
  // So the labels and the values are given exactly the width their widest
  // member needs and the note column takes what is left. The strings come from
  // a fixed vocabulary but the cut they are set in does not, so this is asked
  // of the target rather than assumed.
  fui::TextStyle labelStyle = plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::DarkGray);
  fui::TextStyle noteStyle = plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray);
  fui::TextStyle valueStyle = plain(toybox::kUiFont, fui::TextAlign::Right);
  const int16_t gap = static_cast<int16_t>(toybox::kGutter / 2);
  int16_t widestLabel = 0;
  int16_t widestValue = 0;
  for (int i = 0; i < model.detailCount; ++i) {
    const int16_t label = screen.target().measureText(labelStyle.font, model.details[i].label, labelStyle).width;
    const int16_t value = screen.target().measureText(valueStyle.font, model.details[i].value, valueStyle).width;
    if (label > widestLabel) widestLabel = label;
    if (value > widestValue) widestValue = value;
  }
  // Ceilings, so one pathological string cannot starve the other two columns.
  const int16_t labelCap = static_cast<int16_t>(width * 38 / 100);
  const int16_t valueCap = static_cast<int16_t>(width * 44 / 100);
  int16_t labelW = static_cast<int16_t>(widestLabel + gap);
  if (labelW > labelCap) labelW = labelCap;
  int16_t valueW = static_cast<int16_t>(widestValue + gap);
  if (valueW > valueCap) valueW = valueCap;
  const int16_t noteW = static_cast<int16_t>(width - labelW - valueW);
  const int16_t noteX = static_cast<int16_t>(toybox::kMargin + labelW);
  const int16_t valueX = static_cast<int16_t>(noteX + noteW);
  for (int i = 0; i < model.detailCount; ++i) {
    if (y + rowH > bottom) break;  // the bar is taken first; this never overruns it
    const Detail& detail = model.details[i];
    screen.target().text(fui::makeRect(toybox::kMargin, y, labelW, lineH),
                         toybox::fitLines(screen.target(), detail.label, labelW, 1, labelStyle).c_str(), labelStyle);
    if (detail.note[0] != '\0') {
      // Fitted, because these are words from a fixed vocabulary but the cut
      // they are set in is not: "Mostly cloudy" at the reading face is wider
      // than the column on a narrower panel.
      screen.target().text(fui::makeRect(noteX, y, noteW, lineH),
                           toybox::fitLines(screen.target(), detail.note, noteW, 1, noteStyle).c_str(), noteStyle);
    }
    screen.target().text(fui::makeRect(valueX, y, valueW, lineH),
                         toybox::fitLines(screen.target(), detail.value, valueW, 1, valueStyle).c_str(), valueStyle);
    y = static_cast<int16_t>(y + rowH);
  }
  (void)smallH;
}

// --- HOURS and WEEK ------------------------------------------------------

fui::Rect listBand(const fui::DeviceContext& device) {
  return fui::makeRect(toybox::kMargin, kBodyTop, static_cast<int16_t>(device.width - 2 * toybox::kMargin),
                       static_cast<int16_t>(device.height - kFooterReserve - kBodyTop));
}

int16_t hourRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& tokens) {
  (void)tokens;
  return static_cast<int16_t>(target.lineHeight(toybox::kUiFont) + toybox::kGutter / 2);
}

int16_t dayRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& tokens) {
  (void)tokens;
  return static_cast<int16_t>(target.lineHeight(toybox::kUiFont) + target.lineHeight(toybox::kTileFont) +
                              toybox::kGutter / 2);
}

void buildHours(toybox::Screen& screen, const HoursModel& model) {
  placeChrome(screen, model.place, model.pageLabel, true);
  viewBar(screen, View::Hours);

  if (model.count <= 0) {
    emptyState(screen, "NO HOURS", "This forecast carried no hourly detail. Tap NOW, then refresh.");
    return;
  }

  const fui::DeviceContext& device = screen.device();
  const fui::Rect band = listBand(device);
  const int16_t rowH = hourRowHeight(screen.target(), screen.theme());
  const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
  const int16_t headH = screen.target().lineHeight(toybox::kTileFont);

  fui::TextStyle cell = plain(toybox::kUiFont, fui::TextAlign::Left);
  fui::TextStyle right = plain(toybox::kUiFont, fui::TextAlign::Right);
  fui::TextStyle head = plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray);
  fui::TextStyle headRight = plain(toybox::kTileFont, fui::TextAlign::Right, fui::Color::DarkGray);

  // Measured, not apportioned. At the reading cut "00:00" is wider than a
  // sixth of this panel, so a fixed split drew "00:0027 C" -- the time running
  // straight into the temperature. Each column is asked what its widest member
  // needs and CONDITIONS takes the remainder, because it is the only column
  // whose vocabulary is open-ended and the only one that elides gracefully.
  // The HEADING is measured too, and that is not a detail: the labels are set
  // in the small cut and the data in the reading cut, and a column sized only
  // by its numbers drew "TEMPCONDITIONS" and "RAINWIND". Whichever of the two
  // is wider decides the column.
  static constexpr const char* kHeadTime = "TIME";
  static constexpr const char* kHeadTemp = "TEMP";
  static constexpr const char* kHeadCond = "CONDITIONS";
  static constexpr const char* kHeadRain = "RAIN";
  // "WIND", not "WIND KM/H". Spelling the unit is the honest thing and it was
  // tried: it is 35px wider than the numbers under it, every one of those
  // pixels comes out of CONDITIONS, and CONDITIONS then elided "Partly
  // cloudy" to "Partly...". Between naming a unit and naming the weather, the
  // weather wins -- this column is the only open-ended one on the screen and
  // the reason anyone opens the app. The unit is on NOW in full ("14 km/h
  // SSW"), one tap away and on the screen that opens first.
  static constexpr const char* kHeadWind = "WIND";

  int16_t timeW = screen.target().measureText(head.font, kHeadTime, head).width;
  int16_t tempW = screen.target().measureText(head.font, kHeadTemp, head).width;
  int16_t rainW = screen.target().measureText(head.font, kHeadRain, head).width;
  int16_t windW = screen.target().measureText(head.font, kHeadWind, head).width;
  for (int i = 0; i < model.count; ++i) {
    const HourRow& row = model.rows[i];
    const int16_t t = screen.target().measureText(cell.font, row.time, cell).width;
    const int16_t d = screen.target().measureText(cell.font, row.temperature, cell).width;
    const int16_t r = screen.target().measureText(cell.font, row.precip, cell).width;
    const int16_t w = screen.target().measureText(cell.font, row.wind, cell).width;
    if (t > timeW) timeW = t;
    if (d > tempW) tempW = d;
    if (r > rainW) rainW = r;
    if (w > windW) windW = w;
  }
  const int16_t gap = static_cast<int16_t>(toybox::kGutter / 2);
  timeW = static_cast<int16_t>(timeW + gap);
  tempW = static_cast<int16_t>(tempW + gap);
  rainW = static_cast<int16_t>(rainW + gap);
  windW = static_cast<int16_t>(windW + gap);
  int16_t condW = static_cast<int16_t>(band.width - timeW - tempW - rainW - windW);
  // A panel too narrow for five columns drops the wind rather than overlapping
  // them; the same fact is on NOW and on WEEK.
  bool showWind = condW >= band.width / 5;
  if (!showWind) {
    condW = static_cast<int16_t>(condW + windW);
    windW = 0;
  }

  // One header line, in the small cut, so the bare numbers underneath can stay
  // bare. Without it a column of "18" is a number with no unit; with it the
  // row carries four facts in the room three would otherwise need.
  int16_t y = band.y;
  int16_t x = band.x;
  screen.target().text(fui::makeRect(x, y, timeW, headH), kHeadTime, head);
  x = static_cast<int16_t>(x + timeW);
  screen.target().text(fui::makeRect(x, y, tempW, headH), kHeadTemp, head);
  x = static_cast<int16_t>(x + tempW);
  screen.target().text(fui::makeRect(x, y, condW, headH), kHeadCond, head);
  x = static_cast<int16_t>(x + condW);
  screen.target().text(fui::makeRect(x, y, rainW, headH), kHeadRain, headRight);
  if (showWind) {
    x = static_cast<int16_t>(x + rainW);
    screen.target().text(fui::makeRect(x, y, windW, headH), kHeadWind, headRight);
  }
  y = static_cast<int16_t>(y + headH + toybox::kGutter / 2);
  screen.target().fill(fui::makeRect(band.x, y, band.width, 1), fui::Paint::solid(fui::Color::Black));
  y = static_cast<int16_t>(y + 1 + toybox::kGutter / 2);

  for (int i = 0; i < model.count; ++i) {
    if (y + rowH > band.bottom()) break;
    const HourRow& row = model.rows[i];
    x = band.x;
    screen.target().text(fui::makeRect(x, y, timeW, lineH), row.time, cell);
    x = static_cast<int16_t>(x + timeW);
    screen.target().text(fui::makeRect(x, y, tempW, lineH), row.temperature, cell);
    x = static_cast<int16_t>(x + tempW);
    screen.target().text(fui::makeRect(x, y, condW, lineH),
                         toybox::fitLines(screen.target(), row.conditions, condW, 1, cell).c_str(), cell);
    x = static_cast<int16_t>(x + condW);
    screen.target().text(fui::makeRect(x, y, rainW, lineH), row.precip, right);
    if (showWind) {
      x = static_cast<int16_t>(x + rainW);
      screen.target().text(fui::makeRect(x, y, windW, lineH), row.wind, right);
    }
    y = static_cast<int16_t>(y + rowH);
  }
}

void buildWeek(toybox::Screen& screen, const WeekModel& model) {
  placeChrome(screen, model.place, nullptr, true);
  viewBar(screen, View::Week);

  if (model.count <= 0) {
    emptyState(screen, "NO WEEK", "This forecast carried no daily detail. Tap NOW, then refresh.");
    return;
  }

  const fui::DeviceContext& device = screen.device();
  const fui::Rect band = listBand(device);
  const int16_t rowH = dayRowHeight(screen.target(), screen.theme());
  const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
  const int16_t smallH = screen.target().lineHeight(toybox::kTileFont);

  const int16_t dayW = static_cast<int16_t>(band.width * 38 / 100);
  const int16_t tempW = static_cast<int16_t>(band.width - dayW);

  int16_t y = band.y;
  for (int i = 0; i < model.count; ++i) {
    if (y + rowH > band.bottom()) break;
    const DayRow& row = model.rows[i];
    screen.target().text(fui::makeRect(band.x, y, dayW, lineH), row.day,
                         plain(toybox::kUiFont, fui::TextAlign::Left));
    screen.target().text(fui::makeRect(static_cast<int16_t>(band.x + dayW), y, tempW, lineH), row.highLow,
                         plain(toybox::kUiFont, fui::TextAlign::Right));
    // The second line carries what the first cannot: the conditions in words,
    // the rain chance, and the sun times. A week of temperatures alone is a
    // chart; this is a forecast.
    const int16_t noteY = static_cast<int16_t>(y + lineH);
    std::string left = row.conditions;
    if (row.precip[0] != '\0') left += row.precip;
    screen.target().text(fui::makeRect(band.x, noteY, dayW + tempW / 2, smallH), left.c_str(),
                         plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray));
    screen.target().text(fui::makeRect(band.x, noteY, band.width, smallH), row.sun,
                         plain(toybox::kTileFont, fui::TextAlign::Right, fui::Color::DarkGray));
    y = static_cast<int16_t>(y + rowH);
  }
}

// --- Notices and the remove confirm --------------------------------------

void buildNotice(toybox::Screen& screen, const NoticeModel& model) {
  chrome(screen, "WEATHER", nullptr);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);
  const int16_t headlineH = screen.target().lineHeight(toybox::kDisplayFont);
  const int16_t bodyH = screen.target().lineHeight(toybox::kUiFont);

  screen.target().text(fui::makeRect(toybox::kMargin, top, width, headlineH), model.headline,
                       plain(toybox::kDisplayFont, fui::TextAlign::Center));
  // Six lines, because these say what to DO -- a certificate file to copy, a
  // radio to check -- and a sentence cut off after two is advice nobody can
  // follow.
  screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(top + headlineH + toybox::kGutter), width,
                                     static_cast<int16_t>(bodyH * 6)),
                       model.message, plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::Black, 6));

  if (model.actionLabel == nullptr) return;
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  fui::ButtonProps action;
  action.label = model.actionLabel;
  action.action = ActionNotice;
  screen.button(action, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));
}

void buildRemoveConfirm(toybox::Screen& screen, const ConfirmModel& model) {
  chrome(screen, "REMOVE?", nullptr);

  const fui::DeviceContext& device = screen.device();
  const int16_t width = static_cast<int16_t>(device.width - 2 * toybox::kMargin);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);
  const int16_t headlineH = screen.target().lineHeight(toybox::kDisplayFont);
  const int16_t bodyH = screen.target().lineHeight(toybox::kUiFont);

  fui::TextStyle name = screen.theme().bodyText;
  name.align = fui::TextAlign::Center;
  name.color = fui::Color::Black;
  name.maxLines = 2;
  const std::string shown = toybox::fitLines(screen.target(), model.place, width, 2, name);
  screen.target().text(fui::makeRect(toybox::kMargin, top, width, static_cast<int16_t>(headlineH * 2)), shown.c_str(),
                       name);
  screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(top + headlineH * 2 + toybox::kGutter),
                                     width, static_cast<int16_t>(bodyH * 3)),
                       model.detail, plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::DarkGray, 3));

  // KEEP IT takes the primary-action band a thumb expects; REMOVE is a smaller
  // outlined control set apart from it, on pixels no control on the screen
  // before occupied. Same rule as Instapaper's disconnect and the Notes
  // delete: a stray or remembered tap keeps the place.
  const int16_t footerY = static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
  fui::ButtonProps keep;
  keep.label = "KEEP IT";
  keep.action = ActionRemoveCancel;
  screen.button(keep, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));

  const int16_t removeWidth = static_cast<int16_t>(width / 2);
  fui::ButtonProps remove;
  remove.label = "REMOVE";
  remove.action = ActionRemoveConfirm;
  remove.styles = toybox::rowStyles();
  screen.button(remove, fui::makeRect(static_cast<int16_t>(toybox::kMargin + (width - removeWidth) / 2),
                                      static_cast<int16_t>(footerY - kFooterHeight - toybox::kMargin * 2), removeWidth,
                                      kFooterHeight));
}

}  // namespace weatherui
