// Freestanding tests for StocksCore: the watchlist file, the arithmetic behind
// a move and a worth, and every number the screens print.
//
// The thing under test that matters most is that nothing is GUESSED. A
// watchlist line that cannot be read is reported rather than read as zero
// shares, a missing price is left out of a total rather than counted as zero,
// and a dollar total is never added to a rupee one.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/apps_local/stocks/StocksCore.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                               \
  do {                                                 \
    ++checks;                                          \
    if (!(cond)) {                                     \
      ++failures;                                      \
      std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

using namespace stocks;

static bool near(const double a, const double b) { return std::fabs(a - b) < 1e-6; }

static Point point(const int64_t t, const float close) {
  Point p;
  p.time = t;
  p.open = close;
  p.high = close;
  p.low = close;
  p.close = close;
  return p;
}

static void testWatchlist() {
  std::vector<Holding> list;
  std::vector<WatchlistProblem> problems;
  const std::string text =
      "# comment line\n"
      "aapl nasdaq 10\n"
      "\n"
      "  RELIANCE   NSE   2.5   # fractional, trailing comment\n"
      "M&M nse\n"
      "BRK.B NYSE 1\n"
      "SBIN bse 3\r\n"
      "TSLA LSE 4\n"
      "GOOG NASDAQ lots\n"
      "AAPL NASDAQ 5\n"
      "ONEWORD\n"
      "NVDA NASDAQ -1\n"
      "BAJAJ-AUTO NSE";
  const bool clean = parseWatchlist(text, list, problems);
  CHECK(!clean, "problems reported");
  CHECK(list.size() == 6, "six good lines, got %d", static_cast<int>(list.size()));
  if (list.size() == 6) {
    CHECK(list[0].symbol == "AAPL" && list[0].exchange == Exchange::Nasdaq && near(list[0].shares, 10), "aapl");
    CHECK(list[1].symbol == "RELIANCE" && list[1].exchange == Exchange::Nse && near(list[1].shares, 2.5), "reliance");
    CHECK(list[2].symbol == "M&M" && near(list[2].shares, 0), "m&m watched");
    CHECK(list[3].symbol == "BRK.B" && list[3].exchange == Exchange::Nyse, "brk.b");
    CHECK(list[4].symbol == "SBIN" && list[4].exchange == Exchange::Bse && near(list[4].shares, 3), "sbin crlf");
    CHECK(list[5].symbol == "BAJAJ-AUTO", "last line without a newline");
  }
  CHECK(problems.size() == 5, "five problems, got %d", static_cast<int>(problems.size()));
  if (problems.size() == 5) {
    CHECK(problems[0].line == 8, "LSE on line 8, got %d", problems[0].line);
    CHECK(problems[0].text.find("NASDAQ") != std::string::npos, "exchange message names the choices");
    CHECK(problems[1].line == 9, "bad shares line 9");
    CHECK(problems[2].line == 10 && problems[2].text.find("twice") != std::string::npos, "duplicate");
    CHECK(problems[3].line == 11, "one word");
    CHECK(problems[4].line == 12, "negative shares");
  }
  std::vector<Holding> last;
  parseWatchlist("X NSE\nBAJAJ-AUTO NSE", last, problems);
  CHECK(last.size() == 2 && last[1].symbol == "BAJAJ-AUTO", "last line without newline read");
  CHECK(problems.empty(), "no problems in clean text");

  // The sample is a valid watchlist, or a fresh card opens on an error.
  CHECK(parseWatchlist(sampleWatchlist(), list, problems), "sample parses cleanly");
  CHECK(list.size() == 4, "sample has four stocks");

  // Too many.
  std::string many;
  for (size_t i = 0; i < kMaxHoldings + 2; ++i) many += "S" + std::to_string(i) + " NSE\n";
  parseWatchlist(many, list, problems);
  CHECK(list.size() == kMaxHoldings, "capped at kMaxHoldings");
  CHECK(problems.size() == 2, "two over the cap reported");

  CHECK(parseWatchlist("", list, problems) && list.empty(), "empty file is empty, not an error");
  CHECK(!parseWatchlist("TOOLONGSYMBOLNAMEXXXXXX NSE", list, problems), "overlong symbol");
  CHECK(!parseWatchlist("AB$ NSE", list, problems), "bad char");
  CHECK(!parseWatchlist("AAPL NASDAQ 1 2", list, problems), "four words");
  CHECK(!parseWatchlist("AAPL NASDAQ 1e999", list, problems), "infinite shares");
}

static void testSymbols() {
  Holding h;
  h.symbol = "AAPL";
  h.exchange = Exchange::Nasdaq;
  CHECK(yahooSymbol(h) == "AAPL", "nasdaq bare");
  h.symbol = "RELIANCE";
  h.exchange = Exchange::Nse;
  CHECK(yahooSymbol(h) == "RELIANCE.NS", "nse suffix");
  h.exchange = Exchange::Bse;
  CHECK(yahooSymbol(h) == "RELIANCE.BO", "bse suffix");
  h.symbol = "BRK.B";
  h.exchange = Exchange::Nyse;
  CHECK(yahooSymbol(h) == "BRK-B", "class share dash");
  CHECK(std::string(currencyFor(Exchange::Nse)) == "INR", "nse inr");
  CHECK(std::string(currencyFor(Exchange::Nyse)) == "USD", "nyse usd");
  CHECK(std::string(exchangeName(Exchange::Bse)) == "BSE", "bse name");

  CHECK(cleanName("  Apple\tInc.\n ") == "Apple Inc.", "whitespace folded: [%s]",
        cleanName("  Apple\tInc.\n ").c_str());
  CHECK(cleanName("Soci\xc3\xa9t\xc3\xa9 G") == "Socit G", "non-ascii dropped");
  CHECK(cleanName("abcdef", 3) == "abc", "capped");
  CHECK(cleanName(nullptr).empty(), "null");
}

static void testSeries() {
  Series s;
  s.span = Span::Days;
  s.price = 110;
  s.previousClose = 50;
  for (int i = 0; i < 14; ++i) s.points.push_back(point(i * 86400, static_cast<float>(100 + i)));
  keepLast(s, kDaysShown);
  CHECK(s.points.size() == kDaysShown, "ten kept");
  CHECK(near(s.previousClose, 103), "previous close is the day before the window: %f", s.previousClose);
  CHECK(near(s.points.front().close, 104), "window starts at 104");
  keepLast(s, 20);
  CHECK(s.points.size() == kDaysShown && near(s.previousClose, 103), "keepLast larger is a no-op");

  const Move m = moveOf(s);
  CHECK(near(m.from, 103) && near(m.to, 110) && near(m.delta, 7), "move");
  CHECK(near(m.percent, 7.0 / 103.0 * 100.0), "percent");

  Series gaps;
  gaps.points = {point(1, 10), point(2, 0), point(3, NAN), point(4, 12)};
  gaps.points[3].high = 0;
  dropGaps(gaps);
  CHECK(gaps.points.size() == 2, "two survive");
  CHECK(gaps.points.size() == 2 && near(gaps.points[1].high, 12), "missing high filled from close");

  Series none;
  none.price = 5;
  none.points = {point(1, 4)};
  const Move fromOpen = moveOf(none);
  CHECK(near(fromOpen.from, 4) && near(fromOpen.delta, 1), "no previous close falls back to first open");

  Series day;
  day.points = {point(1, 10), point(2, 12), point(3, 9)};
  day.points[0].open = 9.5f;
  day.points[1].high = 13;
  day.points[2].low = 8.5f;
  const Extremes e = extremesOf(day);
  CHECK(near(e.open, 9.5) && near(e.high, 13) && near(e.low, 8.5), "extremes");
  CHECK(sessionSeconds(Exchange::Nasdaq) == 23400 && sessionSeconds(Exchange::Nse) == 22500, "sessions");

  Series zero;
  const Move z = moveOf(zero);
  CHECK(near(z.percent, 0), "zero series has no percent");
  CHECK(!zero.valid(), "empty series invalid");
}

static void testWorth() {
  std::vector<Holding> holdings(4);
  holdings[0] = Holding{"AAPL", Exchange::Nasdaq, 10};
  holdings[1] = Holding{"MSFT", Exchange::Nasdaq, 0};
  holdings[2] = Holding{"RELIANCE", Exchange::Nse, 2};
  holdings[3] = Holding{"TSLA", Exchange::Nasdaq, 3};

  Series aapl;
  aapl.price = 200;
  aapl.previousClose = 190;
  aapl.currency = "USD";
  aapl.points = {point(1, 200)};
  Series msft = aapl;
  Series rel;
  rel.price = 3000;
  rel.previousClose = 3100;
  rel.currency = "INR";
  rel.points = {point(1, 3000)};
  Series tslaMissing;  // invalid: not counted as zero

  const std::vector<const Series*> series = {&aapl, &msft, &rel, &tslaMissing};
  const std::vector<Worth> worth = worthByCurrency(holdings, series);
  CHECK(worth.size() == 2, "two currencies");
  if (worth.size() == 2) {
    CHECK(worth[0].currency == "USD" && near(worth[0].value, 2000) && near(worth[0].change, 100), "usd");
    CHECK(worth[1].currency == "INR" && near(worth[1].value, 6000) && near(worth[1].change, -200), "inr");
  }
  const std::vector<const Series*> shortList = {&aapl};
  CHECK(worthByCurrency(holdings, shortList).size() == 1, "misaligned lists stop at the shorter");
  const std::vector<const Series*> nulls = {nullptr, nullptr, nullptr, nullptr};
  CHECK(worthByCurrency(holdings, nulls).empty(), "nothing fetched, nothing worth");
}

static void testFormatting() {
  CHECK(formatAmount(1234.5, "USD") == "1,234.50", "usd grouping: %s", formatAmount(1234.5, "USD").c_str());
  CHECK(formatAmount(1234567.891, "USD") == "1,234,567.89", "usd millions");
  CHECK(formatAmount(12345678.9, "INR") == "1,23,45,678.90", "inr crore: %s", formatAmount(12345678.9, "INR").c_str());
  CHECK(formatAmount(123456.78, "INR") == "1,23,456.78", "inr lakh");
  CHECK(formatAmount(999.999, "INR") == "1,000.00", "rounds up across a group: %s",
        formatAmount(999.999, "INR").c_str());
  CHECK(formatAmount(12.3, "USD") == "12.30", "small");
  CHECK(formatAmount(0, "USD") == "0.00", "zero");
  CHECK(formatAmount(-0.001, "USD") == "0.00", "negative zero prints no sign");
  CHECK(formatAmount(-1500, "USD") == "-1,500.00", "negative");
  CHECK(formatMoney(1234.5, "USD") == "$1,234.50", "money usd");
  CHECK(formatMoney(-1234.5, "USD") == "-$1,234.50", "sign before prefix: %s", formatMoney(-1234.5, "USD").c_str());
  CHECK(formatMoney(123456, "INR") == "Rs 1,23,456.00", "money inr");
  CHECK(formatMoney(5, "EUR") == "EUR 5.00", "unknown currency spelled");

  CHECK(formatPercent(1.3249) == "+1.32%", "pct up");
  CHECK(formatPercent(-0.575) == "-0.58%" || formatPercent(-0.575) == "-0.57%", "pct down");
  CHECK(formatPercent(0.004) == "0.00%", "pct zero");
  CHECK(formatPercent(-0.004) == "0.00%", "pct negative zero");
  CHECK(formatPercent(12.5) == "+12.50%", "pct wide");

  Move up{220.0, 222.95, 2.95, 2.95 / 220.0 * 100.0};
  CHECK(formatMove(up, "USD") == "+2.95 (+1.34%)", "move up: %s", formatMove(up, "USD").c_str());
  Move down{3100, 3000, -100, -100.0 / 3100.0 * 100.0};
  CHECK(formatMove(down, "INR") == "-100.00 (-3.23%)", "move down: %s", formatMove(down, "INR").c_str());
  Move flat{10, 10, 0, 0};
  CHECK(formatMove(flat, "USD") == "0.00 (0.00%)", "move flat");
}

static void testTimes() {
  CHECK(formatExchangeTime(1790711940, -14400) == "3:59 PM", "ny close: %s",
        formatExchangeTime(1790711940, -14400).c_str());
  CHECK(formatExchangeTime(1790654400, 19800) == "9:30 AM", "nse open");
  CHECK(formatExchangeTime(1790654400, -14400) == "12:00 AM", "midnight");
  CHECK(formatExchangeTime(1790654400 + 12 * 3600, -14400) == "12:00 PM", "noon");
  CHECK(formatExchangeDay(1790711940, -14400) == "Tue 29", "day: %s", formatExchangeDay(1790711940, -14400).c_str());
  CHECK(formatExchangeDay(1709208000, 0) == "Thu 29", "leap day");
  CHECK(formatExchangeDay(-3600, 0) == "Wed 31", "before the epoch: %s", formatExchangeDay(-3600, 0).c_str());
  CHECK(formatExchangeDay(1790711940, 19800) == "Wed 30", "offset crosses midnight");
}

static void testRange() {
  Series s;
  s.price = 105;
  s.previousClose = 95;
  s.points = {point(1, 100), point(2, 110)};
  s.points[1].high = 120;
  const Range plain = rangeOf(s, false);
  CHECK(plain.low < 95 && plain.high > 110 && plain.high < 120, "closes and previous close, padded");
  const Range withHighLow = rangeOf(s, true);
  CHECK(withHighLow.high > 120, "high included for candles");

  Series flat;
  flat.price = 50;
  flat.previousClose = 50;
  flat.points = {point(1, 50), point(2, 50)};
  const Range r = rangeOf(flat, false);
  CHECK(r.high > r.low, "flat series widened");
  const int16_t mid = yFor(50, r, 100, 101);
  CHECK(mid >= 148 && mid <= 152, "flat draws across the middle: %d", mid);

  CHECK(yFor(r.high, r, 10, 100) == 10, "top");
  CHECK(yFor(r.low, r, 10, 100) == 109, "bottom");
  CHECK(yFor(r.high + 1000, r, 10, 100) == 10, "clamped high");
  CHECK(yFor(-5, r, 10, 100) == 109, "clamped low");

  Series empty;
  const Range e = rangeOf(empty, false);
  CHECK(e.high > e.low, "empty range is usable");
}

static void testCache() {
  Series s;
  s.span = Span::Today;
  s.price = 3012.35;
  s.previousClose = 2999.1;
  s.currency = "INR";
  s.name = "Reliance Industries Limited";
  s.marketTime = 1790654400;
  s.gmtOffset = 19800;
  s.points = {point(1790654400, 3001.5f), point(1790654700, 3012.35f)};
  s.points[0].high = 3004.25f;
  const std::string text = serializeSeries(s);
  Series back;
  CHECK(parseSeries(text, back), "round trip parses");
  CHECK(back.span == Span::Today && near(back.price, 3012.35) && near(back.previousClose, 2999.1), "numbers");
  CHECK(back.currency == "INR" && back.name == "Reliance Industries Limited", "strings: [%s]", back.name.c_str());
  CHECK(back.marketTime == 1790654400 && back.gmtOffset == 19800, "time");
  CHECK(back.points.size() == 2 && back.points[0].high == 3004.25f && back.points[1].close == 3012.35f, "points");

  Series days = s;
  days.span = Span::Days;
  days.name = "";
  Series backDays;
  CHECK(parseSeries(serializeSeries(days), backDays) && backDays.span == Span::Days && backDays.name.empty(),
        "empty name survives");

  Series junk;
  CHECK(!parseSeries("", junk), "empty");
  CHECK(!parseSeries("crossplay-stocks-series 2\n", junk), "future version refused");
  std::string truncated = text.substr(0, text.size() - 10);
  CHECK(!parseSeries(truncated, junk), "truncated refused");
  std::string huge = text;
  const size_t at = huge.find("points 2");
  huge.replace(at, 8, "points 99999");
  CHECK(!parseSeries(huge, junk), "absurd point count refused");
  Series invalid = s;
  invalid.points.clear();
  CHECK(!parseSeries(serializeSeries(invalid), junk), "a series with no points is not a cache");
  // A name carrying a newline cannot break the file apart.
  Series sneaky = s;
  sneaky.name = "Evil\nprice 1";
  Series backSneaky;
  CHECK(parseSeries(serializeSeries(sneaky), backSneaky) && near(backSneaky.price, 3012.35), "newline in name");
}

int main() {
  testWatchlist();
  testSymbols();
  testSeries();
  testWorth();
  testFormatting();
  testTimes();
  testRange();
  testCache();
  // The shape scripts_local/check.sh counts with grep -c "checks, 0 failed".
  std::printf("%s  stocks core: %d checks, %d failed\n", failures ? "FAIL" : "ok  ", checks, failures);
  return failures == 0 ? 0 : 1;
}
