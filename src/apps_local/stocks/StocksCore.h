#pragma once

// The stocks app's model: the watchlist, what a price series means, what the
// holdings are worth, and how any of it is written down.
//
// Freestanding C++17 -- no ArduinoJson, no HalStorage, no network -- so
// host-tests/stocks builds it with nothing but a compiler. Turning Yahoo's JSON
// into a Series happens in StocksFetch, which is the one file that knows the
// service exists.
//
// ---------------------------------------------------------------------------
// Two decisions the screens rest on.
//
// 1. ONE SPAN AT A TIME. The toggle is TODAY or DAYS, and a refresh fetches
//    only the span on screen: one request per stock rather than two. On this
//    device every HTTPS request is a TLS handshake of a second or two, and a
//    ten-stock watchlist fetched both ways would be half a minute of waiting
//    for a view nobody asked for.
//
// 2. NO PRICE IS SHOWN WITHOUT ITS TIME. Free prices are delayed, markets
//    close, and a reader can be opened offline on a week-old cache. Every
//    number on screen comes with the exchange time it was true at.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace stocks {

// --- The watchlist ---------------------------------------------------------

// NYSE and BSE ride along because a portfolio that is "NASDAQ and NSE" in
// spirit nearly always holds one of each that is not, and turning them away
// would make the worth wrong by exactly that holding.
enum class Exchange : uint8_t { Nasdaq, Nyse, Nse, Bse };

const char* exchangeName(Exchange exchange);
const char* currencyFor(Exchange exchange);  // "USD" or "INR"

// A watchlist longer than this is cut, with a problem naming the first line
// left out: every stock is one TLS request on a refresh.
inline constexpr size_t kMaxHoldings = 24;

struct Holding {
  std::string symbol;  // as the exchange lists it: AAPL, RELIANCE
  Exchange exchange = Exchange::Nasdaq;
  double shares = 0;  // 0 = watched, not held; fractions allowed
};

struct WatchlistProblem {
  int line = 0;
  std::string text;
};

// watchlist.txt: one stock per line, `SYMBOL EXCHANGE [SHARES]`. Blank lines
// and anything after '#' are ignored; the exchange is case-insensitive. A line
// that cannot be read is reported with its number and skipped, never guessed
// at -- a holding silently read as zero shares is a worth that is quietly
// wrong.
bool parseWatchlist(const std::string& text, std::vector<Holding>& out, std::vector<WatchlistProblem>& problems);

// What a fresh card gets: the format, explained in its own comments, with two
// example lines to overwrite.
std::string sampleWatchlist();

// The length of a regular session: 6h30 in New York, 6h15 in Mumbai. The
// intraday chart is drawn against it, so a morning's line stops part way
// across instead of being stretched to look like a whole day.
int32_t sessionSeconds(Exchange exchange);

// The symbol Yahoo's chart service knows it by: AAPL, RELIANCE.NS, SBIN.BO.
std::string yahooSymbol(const Holding& holding);

// A company name as the service spelled it, made safe to draw and to cache:
// printable ASCII only (the reader's UI cuts have nothing else), one line,
// at most `cap` characters.
std::string cleanName(const char* text, size_t cap = 48);

// --- Prices over time ------------------------------------------------------

enum class Span : uint8_t {
  Today,  // today's session, in five-minute steps
  Days,   // the last kDaysShown trading days, one close each
};

inline constexpr size_t kDaysShown = 10;

const char* spanName(Span span);  // "TODAY" / "10 DAYS"

// Eight bytes: a time and a close. A watchlist refreshed on the device holds
// a few hundred of these in the chip's internal RAM, the scarce kind, and the
// open, high and low of every five-minute step were 16 bytes a point that only
// ever fed three numbers on one screen -- kept once per series instead.
struct Point {
  uint32_t time = 0;  // UTC seconds
  float close = 0;
};

struct Series {
  Span span = Span::Today;
  double price = 0;          // the latest traded price
  double previousClose = 0;  // the close just before the first point shown
  std::string currency;      // "USD", "INR"
  std::string name;          // "Apple Inc.", empty when the service gave none
  int64_t marketTime = 0;    // when `price` was true, UTC seconds
  int32_t gmtOffset = 0;     // the exchange's offset from UTC, seconds
  // The session's (or the window's) first open, highest high and lowest low,
  // 0 when the service did not say. Only the TODAY screen prints them.
  float dayOpen = 0;
  float dayHigh = 0;
  float dayLow = 0;
  std::vector<Point> points;

  bool valid() const { return price > 0 && !points.empty(); }
};

// Keeps the last `count` points and moves previousClose to the close of the
// point just before them, so a ten-day window's change is measured from the
// day before it starts rather than from a month ago.
void keepLast(Series& series, size_t count);

// Drops points with no usable close -- the service writes null for a
// five-minute step nothing traded in -- so nothing downstream sees one.
void dropGaps(Series& series);

struct Move {
  double from = 0;
  double to = 0;
  double delta = 0;
  double percent = 0;
};

// The change across the series: from the close before it to the latest price.
Move moveOf(const Series& series);

// The session's first open and its highest high and lowest low. Zeros for an
// empty series.
struct Extremes {
  double open = 0;
  double high = 0;
  double low = 0;
};
Extremes extremesOf(const Series& series);

// --- What the holdings are worth -------------------------------------------

struct Worth {
  std::string currency;
  double value = 0;
  double change = 0;  // over the span the series cover
};

// One total per currency, never converted: a dollar total and a rupee total
// are two true numbers, and adding them needs an exchange rate this app would
// have to fetch, date and trust. `series` lines up with `holdings`; an entry
// that is null or invalid is left out rather than counted as zero.
std::vector<Worth> worthByCurrency(const std::vector<Holding>& holdings, const std::vector<const Series*>& series);

// --- Writing numbers down ----------------------------------------------------

// "$" or "Rs " -- the reader's fonts have no rupee sign. Empty for anything
// else, which formatMoney() then spells as "EUR 12.00".
const char* currencyPrefix(const std::string& currency);

// Two decimals, grouped: 1,234.56 for dollars, 1,23,456.78 for rupees (the
// Indian lakh grouping, which is how those amounts are read).
std::string formatAmount(double value, const std::string& currency);

// formatAmount with the prefix: "$1,234.56", "Rs 1,23,456.78".
std::string formatMoney(double value, const std::string& currency);

// "+1.32%", "-0.58%", "0.00%".
std::string formatPercent(double percent);

// "+2.95 (+1.32%)".
std::string formatMove(const Move& move, const std::string& currency);

// The exchange's own clock: "3:59 PM".
std::string formatExchangeTime(int64_t utcSeconds, int32_t gmtOffset);

// "Mon 29" -- the day a daily point belongs to, on the exchange's calendar.
std::string formatExchangeDay(int64_t utcSeconds, int32_t gmtOffset);

// --- Drawing a series ----------------------------------------------------------

struct Range {
  double low = 0;
  double high = 0;
};

// The span a chart has to cover: every close and the previous close, which is the line the chart is read
// against. Padded so the extremes do not touch the frame, and widened when the
// series is flat so a still price draws as a line across the middle rather
// than a division by zero.
Range rangeOf(const Series& series);

// Where `value` falls in a box `height` pixels tall starting at `top`: high
// values near the top, as a chart is read.
int16_t yFor(double value, const Range& range, int16_t top, int16_t height);

// --- The cache -----------------------------------------------------------------

// A plain-text form of a Series, so the last fetch shows at once and offline.
// Text rather than binary because it is on a card people open in other
// machines, and a price file anyone can read is one anyone can check.
std::string serializeSeries(const Series& series);
bool parseSeries(const std::string& text, Series& out);

}  // namespace stocks
