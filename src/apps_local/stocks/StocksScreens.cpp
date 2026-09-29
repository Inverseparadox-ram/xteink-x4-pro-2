#include "StocksScreens.h"

#include <FreeInkUIIcon.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "../ui/ToyboxIcons.h"
#include "../ui/ToyboxText.h"

namespace stocksui {
namespace {

constexpr int kBodyTop = toybox::kBodyTop;
constexpr int kFooterHeight = toybox::kPillHeight;
constexpr int kFooterReserve = toybox::kMargin + kFooterHeight + toybox::kGutter;
// The right-hand column of a chart, where its price labels sit.
constexpr int16_t kAxisWidth = 84;

fui::TextStyle plain(const fui::FontId font, const fui::TextAlign align = fui::TextAlign::Left,
                     const fui::Color color = fui::Color::Black, const uint8_t maxLines = 1, const bool bold = false) {
  fui::TextStyle style;
  style.font = font;
  style.align = align;
  style.color = color;
  style.maxLines = maxLines;
  style.bold = bold;
  return style;
}

int16_t footerTop(const fui::DeviceContext& device) {
  return static_cast<int16_t>(device.height - toybox::kMargin - kFooterHeight);
}

int16_t usableWidth(const fui::DeviceContext& device) {
  return static_cast<int16_t>(device.width - 2 * toybox::kMargin);
}

void chrome(toybox::Screen& screen, const char* title, const char* rightLabel, const bool refreshable) {
  fui::TextStyle bandTitle = screen.theme().bodyText;
  bandTitle.color = fui::Color::White;
  bandTitle.maxLines = 1;
  fui::HeaderProps header;
  header.title = title;
  header.rightLabel = rightLabel;
  header.borderEdges = fui::EdgesNone;
  header.titleText = bandTitle;
  if (rightLabel != nullptr) {
    // Paper, not ink: the band is solid black.
    header.subtitleText = screen.theme().smallText;
    header.subtitleText.color = fui::Color::White;
    header.subtitleText.align = fui::TextAlign::Right;
  }
  if (refreshable) {
    header.trailingIcon = fui::bitmapFromIcon(icon_weather_refresh_32);
    header.trailingAction = ActionRefresh;
    // Styled for the band; unset, it is a black glyph on a black fill.
    header.trailingStyles = toybox::bandOutlineStyles();
  }
  toybox::absoluteChrome(screen);
  toybox::headerBand(screen, header);
  screen.insetContent(fui::Insets{toybox::kGutter * 3, toybox::kMargin, toybox::kMargin, toybox::kMargin});
}

// A filled triangle pointing the way the price went, or a short bar when it
// went nowhere. The fonts have no arrows, and a sign alone is the smallest
// mark on the row for the one fact most people open the app for.
void trendMark(fui::DrawTarget& target, const int16_t x, const int16_t centreY, const int16_t size,
               const int8_t trend) {
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const int16_t half = static_cast<int16_t>(size / 2);
  if (trend > 0) {
    target.triangle(fui::Point{static_cast<int16_t>(x + half), static_cast<int16_t>(centreY - half)},
                    fui::Point{x, static_cast<int16_t>(centreY + half)},
                    fui::Point{static_cast<int16_t>(x + size), static_cast<int16_t>(centreY + half)}, ink);
  } else if (trend < 0) {
    target.triangle(fui::Point{x, static_cast<int16_t>(centreY - half)},
                    fui::Point{static_cast<int16_t>(x + size), static_cast<int16_t>(centreY - half)},
                    fui::Point{static_cast<int16_t>(x + half), static_cast<int16_t>(centreY + half)}, ink);
  } else {
    target.fill(fui::makeRect(x, static_cast<int16_t>(centreY - 1), size, 3), ink);
  }
}

// Right-aligned text with its trend mark just to the left of it, measured so
// the two never overlap whatever the number's width.
void trendText(fui::DrawTarget& target, const fui::Rect& rect, const char* text, const int8_t trend,
               const fui::TextStyle& style) {
  const int16_t width = target.measureText(style.font, text, style).width;
  const int16_t size = static_cast<int16_t>(rect.height * 2 / 5 < 14 ? 14 : rect.height * 2 / 5);
  const int16_t markX = static_cast<int16_t>(rect.right() - width - size - 8);
  if (markX >= rect.x) trendMark(target, markX, static_cast<int16_t>(rect.y + rect.height / 2), size, trend);
  target.text(rect, text, style);
}

void dashed(fui::DrawTarget& target, const int16_t x0, const int16_t x1, const int16_t y) {
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  for (int16_t x = x0; x < x1; x = static_cast<int16_t>(x + 8)) {
    const int16_t len = static_cast<int16_t>(x1 - x < 4 ? x1 - x : 4);
    target.fill(fui::makeRect(x, y, len, 1), ink);
  }
}

void emptyState(toybox::Screen& screen, const int16_t top, const char* headline, const char* sentence) {
  const fui::DeviceContext& device = screen.device();
  const int16_t width = usableWidth(device);
  const int16_t headlineH = screen.target().lineHeight(toybox::kDisplayFont);
  const int16_t bodyH = screen.target().lineHeight(toybox::kUiFont);
  screen.target().text(fui::makeRect(toybox::kMargin, top, width, headlineH), headline,
                       plain(toybox::kDisplayFont, fui::TextAlign::Center));
  screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(top + headlineH + toybox::kGutter), width,
                                     static_cast<int16_t>(bodyH * 4)),
                       sentence, plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::DarkGray, 4));
}

// TODAY | 10 DAYS, then whatever the screen adds after them. The current span
// still registers its action: a tap on it is a repaint, never a dead control.
void spanBar(toybox::Screen& screen, const stocks::Span span, const char* extraLabel, const fui::ActionId extraAction) {
  const fui::DeviceContext& device = screen.device();
  const int16_t y = footerTop(device);
  const int16_t usable = usableWidth(device);
  const int parts = extraLabel != nullptr ? 3 : 2;
  const int16_t segment = static_cast<int16_t>((usable - (parts - 1) * toybox::kGutter) / parts);

  struct Segment {
    const char* label;
    fui::ActionId action;
    bool current;
  };
  const Segment segments[3] = {
      {"TODAY", ActionSpanToday, span == stocks::Span::Today},
      {"10 DAYS", ActionSpanDays, span == stocks::Span::Days},
      {extraLabel, extraAction, false},
  };
  for (int i = 0; i < parts; ++i) {
    fui::ButtonProps button;
    button.label = segments[i].label;
    button.action = segments[i].action;
    if (!segments[i].current) button.styles = toybox::rowStyles();
    const int16_t x = static_cast<int16_t>(toybox::kMargin + i * (segment + toybox::kGutter));
    const int16_t width =
        i == parts - 1 ? static_cast<int16_t>(usable - (parts - 1) * (segment + toybox::kGutter)) : segment;
    screen.button(button, fui::makeRect(x, y, width, kFooterHeight));
  }
}

// --- Charts ------------------------------------------------------------------

struct DataSpan {
  double low = 0;
  double high = 0;
};

// The extremes of the closes alone: what the right-hand labels name. The
// chart's own range is padded (rangeOf) and a padded number is not a price
// anything traded at.
DataSpan closesOf(const stocks::Series& series, const bool withHighLow) {
  DataSpan d;
  bool any = false;
  for (const stocks::Point& p : series.points) {
    const double lo = withHighLow ? p.low : p.close;
    const double hi = withHighLow ? p.high : p.close;
    if (!any) {
      d.low = lo;
      d.high = hi;
      any = true;
    }
    if (lo < d.low) d.low = lo;
    if (hi > d.high) d.high = hi;
  }
  return d;
}

// The labels in the axis column: the highest price, the lowest, and the
// previous close the chart is read against, each at its own height and
// dropped when it would sit on top of another.
void axisLabels(fui::DrawTarget& target, const fui::Rect& plot, const stocks::Series& series,
                const stocks::Range& range, const bool withHighLow) {
  const int16_t smallH = target.lineHeight(toybox::kTileFont);
  const fui::TextStyle style = plain(toybox::kTileFont, fui::TextAlign::Right, fui::Color::Black);
  const fui::TextStyle prevStyle = plain(toybox::kTileFont, fui::TextAlign::Right, fui::Color::DarkGray);
  const int16_t x = static_cast<int16_t>(plot.right() + 4);
  const int16_t w = static_cast<int16_t>(kAxisWidth - 4);
  const DataSpan data = closesOf(series, withHighLow);

  const int16_t highY = stocks::yFor(data.high, range, plot.y, plot.height);
  const int16_t lowY = stocks::yFor(data.low, range, plot.y, plot.height);
  const int16_t prevY = stocks::yFor(series.previousClose, range, plot.y, plot.height);
  auto place = [&](const int16_t y) {
    int16_t top = static_cast<int16_t>(y - smallH / 2);
    if (top < plot.y - smallH / 2) top = static_cast<int16_t>(plot.y - smallH / 2);
    return top;
  };
  const std::string high = stocks::formatAmount(data.high, series.currency);
  const std::string low = stocks::formatAmount(data.low, series.currency);
  target.text(fui::makeRect(x, place(highY), w, smallH), high.c_str(), style);
  if (lowY - highY >= smallH) target.text(fui::makeRect(x, place(lowY), w, smallH), low.c_str(), style);
  if (series.previousClose > 0 && std::abs(prevY - highY) >= smallH && std::abs(prevY - lowY) >= smallH) {
    const std::string prev = stocks::formatAmount(series.previousClose, series.currency);
    target.text(fui::makeRect(x, place(prevY), w, smallH), prev.c_str(), prevStyle);
  }
}

void previousCloseLine(fui::DrawTarget& target, const fui::Rect& plot, const stocks::Series& series,
                       const stocks::Range& range) {
  if (series.previousClose <= 0) return;
  dashed(target, plot.x, plot.right(), stocks::yFor(series.previousClose, range, plot.y, plot.height));
}

void baseline(fui::DrawTarget& target, const fui::Rect& plot) {
  target.fill(fui::makeRect(plot.x, plot.bottom(), plot.width, 1), fui::Paint::solid(fui::Color::Black));
}

// The session so far, placed against the whole session's width.
void chartToday(fui::DrawTarget& target, const fui::Rect& chart, const stocks::Series& series,
                const stocks::Exchange exchange) {
  const int16_t smallH = target.lineHeight(toybox::kTileFont);
  const fui::Rect plot = fui::makeRect(chart.x, chart.y, static_cast<int16_t>(chart.width - kAxisWidth),
                                       static_cast<int16_t>(chart.height - smallH - 4));
  const stocks::Range range = stocks::rangeOf(series, false);
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  baseline(target, plot);
  previousCloseLine(target, plot, series, range);

  const int64_t start = series.points.front().time;
  const int64_t session = stocks::sessionSeconds(exchange);
  auto xFor = [&](const int64_t t) {
    int64_t offset = t - start;
    if (offset < 0) offset = 0;
    if (offset > session) offset = session;
    return static_cast<int16_t>(plot.x + offset * (plot.width - 1) / session);
  };
  fui::Point last{xFor(start), stocks::yFor(series.points.front().close, range, plot.y, plot.height)};
  for (const stocks::Point& p : series.points) {
    const fui::Point here{xFor(p.time), stocks::yFor(p.close, range, plot.y, plot.height)};
    target.line(last, here, 3, ink);
    last = here;
  }
  target.fill(fui::makeRect(static_cast<int16_t>(last.x - 4), static_cast<int16_t>(last.y - 4), 9, 9), ink, 4);
  axisLabels(target, plot, series, range, false);

  const std::string open = stocks::formatExchangeTime(start, series.gmtOffset);
  const std::string close = stocks::formatExchangeTime(start + session, series.gmtOffset);
  const int16_t labelY = static_cast<int16_t>(plot.bottom() + 4);
  const fui::TextStyle left = plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray);
  const fui::TextStyle right = plain(toybox::kTileFont, fui::TextAlign::Right, fui::Color::DarkGray);
  target.text(fui::makeRect(plot.x, labelY, static_cast<int16_t>(plot.width / 2), smallH), open.c_str(), left);
  target.text(fui::makeRect(static_cast<int16_t>(plot.x + plot.width / 2), labelY, static_cast<int16_t>(plot.width / 2),
                            smallH),
              close.c_str(), right);
}

// The day number under each point of a daily chart: "29", because ten
// "Tue 29"s do not fit under ten points on a 480px panel.
void dayNumbers(fui::DrawTarget& target, const fui::Rect& plot, const stocks::Series& series) {
  const int16_t smallH = target.lineHeight(toybox::kTileFont);
  const int n = static_cast<int>(series.points.size());
  const int16_t slot = static_cast<int16_t>(plot.width / (n > 0 ? n : 1));
  const fui::TextStyle style = plain(toybox::kTileFont, fui::TextAlign::Center, fui::Color::DarkGray);
  for (int i = 0; i < n; ++i) {
    const std::string day = stocks::formatExchangeDay(series.points[static_cast<size_t>(i)].time, series.gmtOffset);
    const size_t space = day.find(' ');
    const std::string number = space == std::string::npos ? day : day.substr(space + 1);
    target.text(
        fui::makeRect(static_cast<int16_t>(plot.x + i * slot), static_cast<int16_t>(plot.bottom() + 4), slot, smallH),
        number.c_str(), style);
  }
}

int16_t slotCentre(const fui::Rect& plot, const int i, const int n) {
  return static_cast<int16_t>(plot.x + (2 * i + 1) * plot.width / (2 * (n > 0 ? n : 1)));
}

fui::Rect dailyPlot(const fui::DrawTarget& target, const fui::Rect& chart) {
  const int16_t smallH = target.lineHeight(toybox::kTileFont);
  return fui::makeRect(chart.x, chart.y, static_cast<int16_t>(chart.width - kAxisWidth),
                       static_cast<int16_t>(chart.height - smallH - 4));
}

void chartDaysLine(fui::DrawTarget& target, const fui::Rect& chart, const stocks::Series& series) {
  const fui::Rect plot = dailyPlot(target, chart);
  const stocks::Range range = stocks::rangeOf(series, false);
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  baseline(target, plot);
  previousCloseLine(target, plot, series, range);
  const int n = static_cast<int>(series.points.size());
  fui::Point last{};
  for (int i = 0; i < n; ++i) {
    const fui::Point here{slotCentre(plot, i, n),
                          stocks::yFor(series.points[static_cast<size_t>(i)].close, range, plot.y, plot.height)};
    if (i > 0) target.line(last, here, 3, ink);
    last = here;
  }
  for (int i = 0; i < n; ++i) {
    const int16_t x = slotCentre(plot, i, n);
    const int16_t y = stocks::yFor(series.points[static_cast<size_t>(i)].close, range, plot.y, plot.height);
    target.fill(fui::makeRect(static_cast<int16_t>(x - 4), static_cast<int16_t>(y - 4), 9, 9), ink, 4);
  }
  axisLabels(target, plot, series, range, false);
  dayNumbers(target, plot, series);
}

// Ten closes in two columns of five: the day, the close, that day's change.
int16_t closesTable(fui::DrawTarget& target, const fui::Rect& area, const stocks::Series& series) {
  const int16_t smallH = target.lineHeight(toybox::kTileFont);
  const int16_t rowH = static_cast<int16_t>(smallH + 2);
  const int n = static_cast<int>(series.points.size());
  const int perColumn = (n + 1) / 2;
  const int16_t colW = static_cast<int16_t>((area.width - toybox::kGutter) / 2);
  const fui::TextStyle day = plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray);
  const fui::TextStyle num = plain(toybox::kTileFont, fui::TextAlign::Right, fui::Color::Black);
  // Newest first, which is the order the question is asked in.
  for (int k = 0; k < n; ++k) {
    const int i = n - 1 - k;
    const int column = k / perColumn;
    const int row = k % perColumn;
    const int16_t x = static_cast<int16_t>(area.x + column * (colW + toybox::kGutter));
    const int16_t y = static_cast<int16_t>(area.y + row * rowH);
    if (y + rowH > area.bottom()) continue;
    const stocks::Point& p = series.points[static_cast<size_t>(i)];
    const double prev = i == 0 ? series.previousClose : series.points[static_cast<size_t>(i - 1)].close;
    const std::string when = stocks::formatExchangeDay(p.time, series.gmtOffset);
    const std::string close = stocks::formatAmount(p.close, series.currency);
    const std::string pct = prev > 0 ? stocks::formatPercent((p.close - prev) / prev * 100.0) : std::string();
    const int16_t dayW = static_cast<int16_t>(colW * 30 / 100);
    const int16_t closeW = static_cast<int16_t>(colW * 38 / 100);
    target.text(fui::makeRect(x, y, dayW, smallH), when.c_str(), day);
    target.text(fui::makeRect(static_cast<int16_t>(x + dayW), y, closeW, smallH), close.c_str(), num);
    target.text(
        fui::makeRect(static_cast<int16_t>(x + dayW + closeW), y, static_cast<int16_t>(colW - dayW - closeW), smallH),
        pct.c_str(), num);
  }
  return static_cast<int16_t>(perColumn * rowH);
}

// Label over value, in columns of two: OPEN / HIGH, then LOW / PREV CLOSE.
int16_t statsGrid(fui::DrawTarget& target, const fui::Rect& area, const Stat* stats, const int count) {
  if (count <= 0) return 0;
  const int16_t smallH = target.lineHeight(toybox::kTileFont);
  const int16_t lineH = target.lineHeight(toybox::kUiFont);
  const int16_t cellH = static_cast<int16_t>(smallH + lineH);
  const int columns = 2;
  const int16_t colW = static_cast<int16_t>((area.width - toybox::kGutter) / columns);
  for (int i = 0; i < count; ++i) {
    const int16_t x = static_cast<int16_t>(area.x + (i % columns) * (colW + toybox::kGutter));
    const int16_t y = static_cast<int16_t>(area.y + (i / columns) * cellH);
    target.text(fui::makeRect(x, y, colW, smallH), stats[i].label,
                plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray));
    target.text(fui::makeRect(x, static_cast<int16_t>(y + smallH), colW, lineH), stats[i].value,
                plain(toybox::kUiFont, fui::TextAlign::Left));
  }
  return static_cast<int16_t>(((count + columns - 1) / columns) * cellH);
}

// A shape for a row, from the same series the detail screen charts. No axis
// and no labels: the numbers are right beside it.
void sparkline(fui::DrawTarget& target, const fui::Rect& box, const stocks::Series& series) {
  const size_t n = series.points.size();
  if (n == 0 || box.width < 8) return;
  const stocks::Range range = stocks::rangeOf(series, false);
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  if (series.previousClose > 0)
    dashed(target, box.x, box.right(), stocks::yFor(series.previousClose, range, box.y, box.height));
  if (n == 1) {
    const int16_t y = stocks::yFor(series.points[0].close, range, box.y, box.height);
    target.fill(fui::makeRect(static_cast<int16_t>(box.right() - 5), static_cast<int16_t>(y - 2), 5, 5), ink);
    return;
  }
  fui::Point last{};
  for (size_t i = 0; i < n; ++i) {
    const fui::Point here{static_cast<int16_t>(box.x + static_cast<int>(i) * (box.width - 1) / static_cast<int>(n - 1)),
                          stocks::yFor(series.points[i].close, range, box.y, box.height)};
    if (i > 0) target.line(last, here, 2, ink);
    last = here;
  }
}

struct ListGeometry {
  int16_t worthTop = 0;
  int16_t rowsTop = 0;
  int16_t rowsBottom = 0;
  int16_t problemTop = 0;
  int16_t rowH = 0;
};

ListGeometry listGeometry(const fui::DrawTarget& target, const fui::DeviceContext& device, const int worthCount,
                          const bool problem) {
  ListGeometry g;
  const int16_t lineH = target.lineHeight(toybox::kUiFont);
  const int16_t smallH = target.lineHeight(toybox::kTileFont);
  g.rowH = static_cast<int16_t>(lineH + smallH + toybox::kGutter);
  g.worthTop = static_cast<int16_t>(kBodyTop);
  g.rowsTop = g.worthTop;
  if (worthCount > 0) {
    g.rowsTop = static_cast<int16_t>(g.worthTop + smallH + worthCount * lineH + toybox::kGutter / 2 + toybox::kRule +
                                     toybox::kGutter / 2);
  }
  g.rowsBottom = static_cast<int16_t>(device.height - kFooterReserve);
  if (problem) {
    g.problemTop = static_cast<int16_t>(g.rowsBottom - 2 * smallH);
    g.rowsBottom = static_cast<int16_t>(g.problemTop - toybox::kGutter / 2);
  }
  return g;
}

}  // namespace

int8_t trendOf(const double delta) {
  // Rounded to the cent first, so a move that prints as 0.00 is drawn flat.
  const long long cents = std::llround(delta * 100.0);
  return cents > 0 ? 1 : (cents < 0 ? -1 : 0);
}

// --- The watchlist -----------------------------------------------------------

int listRowsThatFit(const fui::DrawTarget& target, const fui::DeviceContext& device, const int worthCount,
                    const bool problem) {
  const ListGeometry g = listGeometry(target, device, worthCount, problem);
  const int rows = g.rowH > 0 ? (g.rowsBottom - g.rowsTop) / g.rowH : 0;
  return rows > 0 ? rows : 1;
}

void buildList(toybox::Screen& screen, const ListModel& model) {
  chrome(screen, model.editing ? "EDIT WATCHLIST" : "STOCKS", model.pageLabel, !model.editing && model.total > 0);
  const fui::DeviceContext& device = screen.device();
  fui::DrawTarget& target = screen.target();
  const int16_t width = usableWidth(device);
  const int16_t x0 = static_cast<int16_t>(toybox::kMargin);

  if (model.editing) {
    const int16_t y = footerTop(device);
    const int16_t half = static_cast<int16_t>((width - toybox::kGutter) / 2);
    fui::ButtonProps add;
    add.label = "ADD A STOCK";
    add.action = ActionAdd;
    screen.button(add, fui::makeRect(x0, y, half, kFooterHeight));
    fui::ButtonProps done;
    done.label = "DONE";
    done.action = ActionEdit;
    done.styles = toybox::rowStyles();
    screen.button(done, fui::makeRect(static_cast<int16_t>(x0 + half + toybox::kGutter), y,
                                      static_cast<int16_t>(width - half - toybox::kGutter), kFooterHeight));
  } else {
    spanBar(screen, model.span, "EDIT", ActionEdit);
  }

  const bool problem = model.problem != nullptr && model.problem[0] != '\0';
  const int worthCount = model.editing ? 0 : model.worthCount;
  const ListGeometry g = listGeometry(target, device, worthCount, problem);
  const int16_t lineH = target.lineHeight(toybox::kUiFont);
  const int16_t smallH = target.lineHeight(toybox::kTileFont);

  if (problem) {
    target.text(fui::makeRect(x0, g.problemTop, width, static_cast<int16_t>(2 * smallH)), model.problem,
                plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::Black, 2));
  }

  if (model.total <= 0) {
    emptyState(screen, static_cast<int16_t>(kBodyTop + toybox::kMargin * 2), "NO STOCKS",
               model.editing ? "Tap ADD A STOCK and type a line like: TCS NSE 5"
                             : "Tap EDIT to add one, or edit /Stocks/watchlist.txt on a computer.");
    return;
  }

  // What the holdings are worth, one line per currency, measured over the
  // span on screen.
  if (worthCount > 0) {
    int16_t y = g.worthTop;
    const std::string heading = std::string("HOLDINGS  .  ") + stocks::spanName(model.span);
    target.text(fui::makeRect(x0, y, width, smallH), heading.c_str(),
                plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray));
    y = static_cast<int16_t>(y + smallH);
    for (int i = 0; i < worthCount; ++i) {
      const WorthLine& w = model.worth[i];
      // The value takes what it measures and the change the rest, so a rupee
      // total in lakhs does not push its own change off the edge. When the
      // two still do not fit side by side, the change steps down a cut.
      const fui::TextStyle valueStyle = plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::Black, 1, true);
      const int16_t valueW =
          static_cast<int16_t>(target.measureText(valueStyle.font, w.value, valueStyle).width + toybox::kGutter);
      const int16_t changeW = static_cast<int16_t>(width - valueW);
      fui::TextStyle changeStyle = plain(toybox::kUiFont, fui::TextAlign::Right);
      if (target.measureText(changeStyle.font, w.change, changeStyle).width + 26 > changeW) {
        changeStyle.font = toybox::kTileFont;
      }
      target.text(fui::makeRect(x0, y, valueW, lineH), w.value, valueStyle);
      trendText(target, fui::makeRect(static_cast<int16_t>(x0 + valueW), y, changeW, lineH), w.change, w.trend,
                changeStyle);
      y = static_cast<int16_t>(y + lineH);
    }
    y = static_cast<int16_t>(y + toybox::kGutter / 2);
    target.fill(fui::makeRect(x0, y, width, toybox::kRule), fui::Paint::solid(fui::Color::Black));
  }

  const int16_t colA = static_cast<int16_t>(width * 40 / 100);
  const int16_t colB = static_cast<int16_t>(width * 22 / 100);
  const int16_t colC = static_cast<int16_t>(width - colA - colB);
  int16_t y = g.rowsTop;
  for (int i = 0; i < model.count; ++i) {
    if (y + g.rowH > g.rowsBottom) break;
    const StockRow& row = model.rows[i];
    screen.frame().hit(fui::makeRect(x0, y, width, g.rowH), model.editing ? ActionEditStock : ActionOpenStock,
                       static_cast<int16_t>(i));
    const int16_t top = static_cast<int16_t>(y + toybox::kGutter / 2);
    const fui::TextStyle symbol = plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::Black, 1, true);
    const fui::TextStyle detail = plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray);
    target.text(fui::makeRect(x0, top, colA, lineH), toybox::fitLines(target, row.symbol, colA, 1, symbol).c_str(),
                symbol);
    const int16_t detailW = model.editing ? static_cast<int16_t>(colA + colB) : colA;
    target.text(fui::makeRect(x0, static_cast<int16_t>(top + lineH), detailW, smallH),
                toybox::fitLines(target, row.detail, static_cast<int16_t>(detailW - 4), 1, detail).c_str(), detail);
    if (row.series != nullptr && !model.editing) {
      sparkline(target,
                fui::makeRect(static_cast<int16_t>(x0 + colA), static_cast<int16_t>(top + 2),
                              static_cast<int16_t>(colB - toybox::kGutter), static_cast<int16_t>(lineH + smallH - 6)),
                *row.series);
    }
    const int16_t cx = static_cast<int16_t>(x0 + colA + colB);
    target.text(fui::makeRect(cx, top, colC, lineH), row.price, plain(toybox::kUiFont, fui::TextAlign::Right));
    if (row.change[0] != '\0') {
      trendText(target, fui::makeRect(cx, static_cast<int16_t>(top + lineH), colC, smallH), row.change, row.trend,
                plain(toybox::kTileFont, fui::TextAlign::Right));
    }
    y = static_cast<int16_t>(y + g.rowH);
    if (i + 1 < model.count && y + g.rowH <= g.rowsBottom) {
      target.fill(fui::makeRect(x0, static_cast<int16_t>(y - 1), width, 1), fui::Paint::solid(fui::Color::Black));
    }
  }
}

// --- One stock -----------------------------------------------------------------

void buildDetail(toybox::Screen& screen, const DetailModel& model) {
  chrome(screen, model.symbol, model.position[0] != '\0' ? model.position : nullptr, true);
  const stocks::Span span = model.series != nullptr ? model.series->span : stocks::Span::Today;
  spanBar(screen, span, nullptr, 0);

  const fui::DeviceContext& device = screen.device();
  fui::DrawTarget& target = screen.target();
  const int16_t width = usableWidth(device);
  const int16_t x0 = static_cast<int16_t>(toybox::kMargin);
  const int16_t lineH = target.lineHeight(toybox::kUiFont);
  const int16_t smallH = target.lineHeight(toybox::kTileFont);
  const int16_t bigH = target.lineHeight(toybox::kDisplayFont);
  int16_t y = static_cast<int16_t>(kBodyTop);

  const fui::TextStyle gray = plain(toybox::kTileFont, fui::TextAlign::Left, fui::Color::DarkGray);
  target.text(fui::makeRect(x0, y, width, smallH), toybox::fitLines(target, model.name, width, 1, gray).c_str(), gray);
  y = static_cast<int16_t>(y + smallH);

  if (model.series == nullptr || !model.series->valid()) {
    emptyState(screen, static_cast<int16_t>(y + toybox::kMargin), "NO PRICE YET",
               model.problem[0] != '\0' ? model.problem : "Tap the refresh button to fetch it.");
    return;
  }
  const stocks::Series& series = *model.series;

  target.text(fui::makeRect(x0, y, width, bigH), model.price, plain(toybox::kDisplayFont, fui::TextAlign::Left));
  y = static_cast<int16_t>(y + bigH);
  const int16_t markSize = 16;
  trendMark(target, x0, static_cast<int16_t>(y + lineH / 2), markSize, model.trend);
  target.text(
      fui::makeRect(static_cast<int16_t>(x0 + markSize + 8), y, static_cast<int16_t>(width - markSize - 8), lineH),
      model.move, plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::Black, 1, true));
  y = static_cast<int16_t>(y + lineH);
  target.text(fui::makeRect(x0, y, width, smallH), model.asOf, gray);
  y = static_cast<int16_t>(y + smallH + toybox::kGutter);

  // What sits under the chart decides how tall the chart can be, so it is
  // measured first and drawn last.
  const bool held = model.holding[0] != '\0';
  const int16_t holdingH =
      held ? static_cast<int16_t>(toybox::kGutter + toybox::kRule + toybox::kGutter / 2 + lineH + smallH) : 0;
  int16_t belowH = 0;
  const int16_t tableRows = static_cast<int16_t>((series.points.size() + 1) / 2);
  const int16_t tableH = static_cast<int16_t>(tableRows * (smallH + 2));
  const int16_t statsH = static_cast<int16_t>(((model.statCount + 1) / 2) * (smallH + lineH));
  if (span == stocks::Span::Today) {
    belowH = static_cast<int16_t>(statsH > 0 ? statsH + toybox::kGutter : 0);
  } else {
    belowH = static_cast<int16_t>(tableH + toybox::kGutter);
  }
  const int16_t bottom = static_cast<int16_t>(device.height - kFooterReserve);
  const int16_t chartH = static_cast<int16_t>(bottom - y - belowH - holdingH);
  const fui::Rect chart = fui::makeRect(x0, y, width, chartH);

  if (span == stocks::Span::Today) {
    chartToday(target, chart, series, model.exchange);
    y = static_cast<int16_t>(chart.bottom() + toybox::kGutter);
    y = static_cast<int16_t>(y + statsGrid(target, fui::makeRect(x0, y, width, statsH), model.stats, model.statCount));
  } else {
    // The days as a line with a dot on each close, and the closes themselves
    // underneath: the chart answers "which way", the table "at what".
    chartDaysLine(target, chart, series);
    y = static_cast<int16_t>(chart.bottom() + toybox::kGutter);
    y = static_cast<int16_t>(y + closesTable(target, fui::makeRect(x0, y, width, tableH), series));
  }

  if (held) {
    y = static_cast<int16_t>(bottom - holdingH + toybox::kGutter);
    target.fill(fui::makeRect(x0, y, width, toybox::kRule), fui::Paint::solid(fui::Color::Black));
    y = static_cast<int16_t>(y + toybox::kRule + toybox::kGutter / 2);
    target.text(fui::makeRect(x0, y, width, lineH), model.holding,
                plain(toybox::kUiFont, fui::TextAlign::Left, fui::Color::Black, 1, true));
    y = static_cast<int16_t>(y + lineH);
    const int16_t size = 12;
    trendMark(target, x0, static_cast<int16_t>(y + smallH / 2), size, model.holdingTrend);
    target.text(fui::makeRect(static_cast<int16_t>(x0 + size + 6), y, static_cast<int16_t>(width - size - 6), smallH),
                model.holdingMove, plain(toybox::kTileFont, fui::TextAlign::Left));
  }
}

// --- Editing -------------------------------------------------------------------

void buildStockMenu(toybox::Screen& screen, const StockMenuModel& model) {
  chrome(screen, model.symbol, nullptr, false);
  const fui::DeviceContext& device = screen.device();
  const int16_t width = usableWidth(device);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);
  const int16_t lineH = screen.target().lineHeight(toybox::kUiFont);
  screen.target().text(fui::makeRect(toybox::kMargin, top, width, static_cast<int16_t>(lineH * 2)), model.detail,
                       plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::DarkGray, 2));

  fui::ButtonProps shares;
  shares.label = "CHANGE SHARES";
  shares.action = ActionChangeShares;
  screen.button(shares, fui::makeRect(toybox::kMargin, static_cast<int16_t>(top + lineH * 2 + toybox::kMargin * 2),
                                      width, kFooterHeight));

  // KEEP takes the primary band a thumb expects; REMOVE is smaller, outlined
  // and set apart, on pixels nothing destructive occupied a screen earlier.
  const int16_t footerY = footerTop(device);
  fui::ButtonProps keep;
  keep.label = "KEEP IT";
  keep.action = ActionKeepStock;
  screen.button(keep, fui::makeRect(toybox::kMargin, footerY, width, kFooterHeight));
  const int16_t removeWidth = static_cast<int16_t>(width / 2);
  fui::ButtonProps remove;
  remove.label = "REMOVE";
  remove.action = ActionRemoveStock;
  remove.styles = toybox::rowStyles();
  screen.button(remove, fui::makeRect(static_cast<int16_t>(toybox::kMargin + (width - removeWidth) / 2),
                                      static_cast<int16_t>(footerY - kFooterHeight - toybox::kMargin * 2), removeWidth,
                                      kFooterHeight));
}

// --- Notices ---------------------------------------------------------------------

void buildNotice(toybox::Screen& screen, const NoticeModel& model) {
  chrome(screen, "STOCKS", nullptr, false);
  const fui::DeviceContext& device = screen.device();
  const int16_t width = usableWidth(device);
  const int16_t top = static_cast<int16_t>(kBodyTop + toybox::kMargin * 2);
  const int16_t headlineH = screen.target().lineHeight(toybox::kDisplayFont);
  const int16_t bodyH = screen.target().lineHeight(toybox::kUiFont);
  screen.target().text(fui::makeRect(toybox::kMargin, top, width, headlineH), model.headline,
                       plain(toybox::kDisplayFont, fui::TextAlign::Center));
  // Six lines: these say what to DO, and advice cut off after two is advice
  // nobody can follow.
  screen.target().text(fui::makeRect(toybox::kMargin, static_cast<int16_t>(top + headlineH + toybox::kGutter), width,
                                     static_cast<int16_t>(bodyH * 6)),
                       model.message, plain(toybox::kUiFont, fui::TextAlign::Center, fui::Color::Black, 6));
  if (model.actionLabel == nullptr) return;
  fui::ButtonProps action;
  action.label = model.actionLabel;
  action.action = ActionNotice;
  screen.button(action, fui::makeRect(toybox::kMargin, footerTop(device), width, kFooterHeight));
}

}  // namespace stocksui
