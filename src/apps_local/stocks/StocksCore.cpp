#include "StocksCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace stocks {
namespace {

constexpr size_t kMaxSymbolChars = 20;
// A five-minute session is 75 to 78 points and a month of days about 23; the
// cap only exists so a corrupt cache cannot ask for a million.
constexpr size_t kMaxPoints = 400;

char upper(const char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string upperCopy(const std::string& text) {
  std::string out = text;
  for (char& c : out) c = upper(c);
  return out;
}

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Exchange symbols are letters and digits plus the three marks the two
// exchanges actually use: M&M and BAJAJ-AUTO on the NSE, BRK.B in New York.
bool symbolCharOk(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '&' || c == '-' || c == '.';
}

bool parseExchange(const std::string& word, Exchange& out) {
  const std::string w = upperCopy(word);
  if (w == "NASDAQ") {
    out = Exchange::Nasdaq;
  } else if (w == "NYSE") {
    out = Exchange::Nyse;
  } else if (w == "NSE") {
    out = Exchange::Nse;
  } else if (w == "BSE") {
    out = Exchange::Bse;
  } else {
    return false;
  }
  return true;
}

bool parseShares(const std::string& word, double& out) {
  if (word.empty()) return false;
  char* end = nullptr;
  const double value = std::strtod(word.c_str(), &end);
  if (end == nullptr || *end != '\0') return false;
  if (!std::isfinite(value) || value < 0) return false;
  out = value;
  return true;
}

std::vector<std::string> words(const std::string& line) {
  std::vector<std::string> out;
  out.reserve(4);
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && isSpace(line[i])) ++i;
    const size_t start = i;
    while (i < line.size() && !isSpace(line[i])) ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
  return out;
}

void problem(std::vector<WatchlistProblem>& problems, const int line, const char* what, const std::string& detail) {
  WatchlistProblem p;
  p.line = line;
  p.text = what;
  if (!detail.empty()) {
    p.text += ": ";
    p.text += detail.substr(0, 24);
  }
  problems.push_back(std::move(p));
}

// Days since 1970-01-01 to a civil date. Howard Hinnant's algorithm, which is
// exact for every date this app will meet and needs no timezone database.
void civilFromDays(int64_t days, int& year, int& month, int& day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

int64_t floorDiv(const int64_t a, const int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

std::string formatNumber(const double value, const int digits) {
  char text[48];
  std::snprintf(text, sizeof(text), "%.*g", digits, value);
  return text;
}

bool takeDouble(const char* text, double& out) {
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || !std::isfinite(value)) return false;
  out = value;
  return true;
}

bool takeInt(const char* text, int64_t& out) {
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (end == text) return false;
  out = value;
  return true;
}

}  // namespace

// --- The watchlist ---------------------------------------------------------

const char* exchangeName(const Exchange exchange) {
  switch (exchange) {
    case Exchange::Nasdaq:
      return "NASDAQ";
    case Exchange::Nyse:
      return "NYSE";
    case Exchange::Nse:
      return "NSE";
    case Exchange::Bse:
      return "BSE";
  }
  return "?";
}

const char* currencyFor(const Exchange exchange) {
  return (exchange == Exchange::Nse || exchange == Exchange::Bse) ? "INR" : "USD";
}

bool parseWatchlist(const std::string& text, std::vector<Holding>& out, std::vector<WatchlistProblem>& problems) {
  out.clear();
  problems.clear();
  out.reserve(kMaxHoldings);
  int lineNumber = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    ++lineNumber;
    std::string line = text.substr(start, end - start);
    start = end + 1;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    const std::vector<std::string> parts = words(line);
    if (parts.empty()) {
      if (end == text.size()) break;
      continue;
    }

    if (parts.size() < 2 || parts.size() > 3) {
      problem(problems, lineNumber, "Expected SYMBOL EXCHANGE [SHARES]", parts[0]);
    } else {
      Holding holding;
      holding.symbol = upperCopy(parts[0]);
      bool symbolOk = !holding.symbol.empty() && holding.symbol.size() <= kMaxSymbolChars;
      for (const char c : holding.symbol) symbolOk = symbolOk && symbolCharOk(c);
      if (!symbolOk) {
        problem(problems, lineNumber, "Not a symbol", parts[0]);
      } else if (!parseExchange(parts[1], holding.exchange)) {
        problem(problems, lineNumber, "Exchange must be NASDAQ, NYSE, NSE or BSE", parts[1]);
      } else if (parts.size() == 3 && !parseShares(parts[2], holding.shares)) {
        problem(problems, lineNumber, "Shares must be a number", parts[2]);
      } else {
        bool duplicate = false;
        for (const Holding& seen : out) {
          duplicate = duplicate || (seen.symbol == holding.symbol && seen.exchange == holding.exchange);
        }
        if (duplicate) {
          problem(problems, lineNumber, "Listed twice", holding.symbol);
        } else if (out.size() >= kMaxHoldings) {
          problem(problems, lineNumber, "Watchlist is full", holding.symbol);
        } else {
          out.push_back(std::move(holding));
        }
      }
    }
    if (end == text.size()) break;
  }
  return problems.empty();
}

std::string sampleWatchlist() {
  return "# CrossPlay Stocks watchlist.\n"
         "#\n"
         "# One stock per line:  SYMBOL  EXCHANGE  [SHARES]\n"
         "#   EXCHANGE is NASDAQ, NYSE, NSE or BSE.\n"
         "#   SHARES is how many you hold; leave it out to just watch.\n"
         "#   Anything after a # is ignored.\n"
         "#\n"
         "# Edit this file on a computer, put the card back, and open Stocks.\n"
         "\n"
         "AAPL      NASDAQ  10\n"
         "MSFT      NASDAQ\n"
         "RELIANCE  NSE     25\n"
         "INFY      NSE\n";
}

int32_t sessionSeconds(const Exchange exchange) {
  return (exchange == Exchange::Nse || exchange == Exchange::Bse) ? (6 * 3600 + 15 * 60) : (6 * 3600 + 30 * 60);
}

std::string yahooSymbol(const Holding& holding) {
  switch (holding.exchange) {
    case Exchange::Nse:
      return holding.symbol + ".NS";
    case Exchange::Bse:
      return holding.symbol + ".BO";
    case Exchange::Nasdaq:
    case Exchange::Nyse:
      break;
  }
  // Yahoo writes class shares with a dash: BRK.B is BRK-B.
  std::string out = holding.symbol;
  for (char& c : out) {
    if (c == '.') c = '-';
  }
  return out;
}

std::string twelveSymbol(const Holding& holding) {
  switch (holding.exchange) {
    case Exchange::Nse:
      return holding.symbol + ":NSE";
    case Exchange::Bse:
      return holding.symbol + ":BSE";
    case Exchange::Nasdaq:
    case Exchange::Nyse:
      break;
  }
  return holding.symbol;
}

namespace {

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's).
int64_t daysFromCivil(int64_t y, const unsigned m, const unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// The day of the month of the nth Sunday (1-based) of month m in year y.
unsigned nthSunday(const int64_t y, const unsigned m, const unsigned n) {
  const int64_t first = daysFromCivil(y, m, 1);
  const unsigned weekday = static_cast<unsigned>(((first % 7) + 11) % 7);  // 0 = Sunday; 1970-01-01 was a Thursday
  return 1 + (7 - weekday) % 7 + 7 * (n - 1);
}

}  // namespace

int32_t exchangeOffset(const Exchange exchange, const int64_t utc) {
  if (exchange == Exchange::Nse || exchange == Exchange::Bse) return 19800;
  // Which year it is in New York barely matters at the edges: both switches
  // happen at 2am local, hours away from any New Year.
  const int64_t days = (utc - 5 * 3600) / 86400 - ((utc - 5 * 3600) % 86400 < 0 ? 1 : 0);
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = static_cast<int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
  // 2am EST is 07:00 UTC; 2am EDT is 06:00 UTC.
  const int64_t start = daysFromCivil(year, 3, nthSunday(year, 3, 2)) * 86400 + 7 * 3600;
  const int64_t end = daysFromCivil(year, 11, nthSunday(year, 11, 1)) * 86400 + 6 * 3600;
  return utc >= start && utc < end ? -4 * 3600 : -5 * 3600;
}

bool parseUtcTime(const char* text, int64_t& out) {
  if (text == nullptr) return false;
  const auto digits = [&](const size_t at, const size_t count, unsigned& value) {
    value = 0;
    for (size_t i = at; i < at + count; ++i) {
      if (text[i] < '0' || text[i] > '9') return false;
      value = value * 10 + static_cast<unsigned>(text[i] - '0');
    }
    return true;
  };
  unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (!digits(0, 4, y) || text[4] != '-' || !digits(5, 2, mo) || text[7] != '-' || !digits(8, 2, d)) return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
  if (text[10] != '\0') {
    if (text[10] != ' ' || !digits(11, 2, h) || text[13] != ':' || !digits(14, 2, mi)) return false;
    if (text[16] == ':' && !digits(17, 2, s)) return false;
    if (h > 23 || mi > 59 || s > 60) return false;
  }
  out = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
  return true;
}

std::string parseKeyFile(const std::string& text) {
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    size_t first = 0;
    while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
    size_t last = line.size();
    while (last > first && (line[last - 1] == ' ' || line[last - 1] == '\t' || line[last - 1] == '\r')) --last;
    if (last == first) continue;
    const std::string key = line.substr(first, last - first);
    bool plausible = key.size() >= 16 && key.size() <= 64;
    for (const char c : key) {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) plausible = false;
    }
    if (plausible) return key;
  }
  return std::string();
}

const char* sampleKeyFile() {
  return "# Stocks: a Twelve Data API key, on its own line below this comment.\n"
         "#\n"
         "# A free key (twelvedata.com, no card needed) covers NASDAQ and NYSE:\n"
         "# 800 requests a day, 8 a minute, and each stock is one request. NSE\n"
         "# and BSE need a paid plan; without one they keep coming from Yahoo.\n"
         "#\n"
         "# With no key, every price comes from Yahoo, which sometimes refuses.\n"
         "\n";
}

std::string cleanName(const char* text, const size_t cap) {
  std::string out;
  if (text == nullptr) return out;
  out.reserve(cap);
  bool lastSpace = true;
  for (const char* p = text; *p != '\0' && out.size() < cap; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (space) {
      if (!lastSpace) out += ' ';
      lastSpace = true;
    } else if (c >= 0x21 && c < 0x7F) {
      out += static_cast<char>(c);
      lastSpace = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// --- Prices over time ------------------------------------------------------

const char* spanName(const Span span) { return span == Span::Today ? "TODAY" : "10 DAYS"; }

void keepLast(Series& series, const size_t count) {
  if (series.points.size() <= count) return;
  const size_t drop = series.points.size() - count;
  series.previousClose = series.points[drop - 1].close;
  series.points.erase(series.points.begin(), series.points.begin() + static_cast<std::ptrdiff_t>(drop));
}

void dropGaps(Series& series) {
  std::vector<Point>& points = series.points;
  size_t kept = 0;
  for (size_t i = 0; i < points.size(); ++i) {
    if (!(points[i].close > 0) || !std::isfinite(points[i].close)) continue;
    points[kept++] = points[i];
  }
  points.resize(kept);
}

Move moveOf(const Series& series) {
  Move move;
  move.to = series.price;
  if (series.previousClose > 0) {
    move.from = series.previousClose;
  } else if (series.dayOpen > 0) {
    move.from = series.dayOpen;
  } else {
    move.from = series.points.empty() ? series.price : series.points.front().close;
  }
  move.delta = move.to - move.from;
  move.percent = move.from > 0 ? move.delta / move.from * 100.0 : 0.0;
  return move;
}

Extremes extremesOf(const Series& series) {
  Extremes e;
  if (series.points.empty()) return e;
  e.open = series.dayOpen > 0 ? series.dayOpen : series.points.front().close;
  e.high = series.points.front().close;
  e.low = series.points.front().close;
  for (const Point& p : series.points) {
    if (p.close > e.high) e.high = p.close;
    if (p.close < e.low) e.low = p.close;
  }
  // The service's own high and low see between the five-minute closes.
  if (series.dayHigh > e.high) e.high = series.dayHigh;
  if (series.dayLow > 0 && series.dayLow < e.low) e.low = series.dayLow;
  return e;
}

// --- What the holdings are worth -------------------------------------------

std::vector<Worth> worthByCurrency(const std::vector<Holding>& holdings, const std::vector<const Series*>& series) {
  std::vector<Worth> out;
  out.reserve(2);
  for (size_t i = 0; i < holdings.size() && i < series.size(); ++i) {
    const Holding& holding = holdings[i];
    const Series* s = series[i];
    if (holding.shares <= 0 || s == nullptr || !s->valid()) continue;
    const std::string currency = s->currency.empty() ? currencyFor(holding.exchange) : s->currency;
    Worth* worth = nullptr;
    for (Worth& w : out) {
      if (w.currency == currency) worth = &w;
    }
    if (worth == nullptr) {
      out.push_back(Worth{currency, 0, 0});
      worth = &out.back();
    }
    const Move move = moveOf(*s);
    worth->value += holding.shares * s->price;
    worth->change += holding.shares * move.delta;
  }
  return out;
}

// --- Writing numbers down ----------------------------------------------------

const char* currencyPrefix(const std::string& currency) {
  if (currency == "USD") return "$";
  if (currency == "INR") return "Rs ";
  return "";
}

std::string formatAmount(const double value, const std::string& currency) {
  const double magnitude = std::fabs(value);
  const long long cents = std::llround(magnitude * 100.0);
  const long long whole = cents / 100;
  const int fraction = static_cast<int>(cents % 100);

  char digits[32];
  std::snprintf(digits, sizeof(digits), "%lld", whole);
  const std::string plain = digits;
  const bool indian = currency == "INR";

  // Grouped from the right: threes throughout, or three then twos (lakh,
  // crore) for rupees.
  std::string grouped;
  grouped.reserve(plain.size() + plain.size() / 2 + 1);
  const int n = static_cast<int>(plain.size());
  for (int i = 0; i < n; ++i) {
    const int fromRight = n - i;
    grouped += plain[static_cast<size_t>(i)];
    const int left = fromRight - 1;
    if (left <= 0) continue;
    const bool comma = indian ? (left == 3 || (left > 3 && (left - 3) % 2 == 0)) : (left % 3 == 0);
    if (comma) grouped += ',';
  }

  char tail[16];
  std::snprintf(tail, sizeof(tail), ".%02d", fraction);
  std::string out;
  if (value < 0 && cents != 0) out += '-';
  out += grouped;
  out += tail;
  return out;
}

std::string formatMoney(const double value, const std::string& currency) {
  const char* prefix = currencyPrefix(currency);
  const std::string amount = formatAmount(value, currency);
  std::string out;
  const bool negative = !amount.empty() && amount[0] == '-';
  if (negative) out += '-';
  if (prefix[0] != '\0') {
    out += prefix;
  } else if (!currency.empty()) {
    out += currency;
    out += ' ';
  }
  out += negative ? amount.substr(1) : amount;
  return out;
}

std::string formatPercent(const double percent) {
  const long long hundredths = std::llround(percent * 100.0);
  char text[48];
  if (hundredths == 0) return "0.00%";
  const long long magnitude = hundredths < 0 ? -hundredths : hundredths;
  std::snprintf(text, sizeof(text), "%c%lld.%02lld%%", hundredths < 0 ? '-' : '+', magnitude / 100, magnitude % 100);
  return text;
}

std::string formatMove(const Move& move, const std::string& currency) {
  const long long cents = std::llround(move.delta * 100.0);
  std::string out;
  if (cents > 0) out += '+';
  if (cents < 0) out += '-';
  out += formatAmount(std::fabs(move.delta), currency);
  out += " (";
  out += formatPercent(move.percent);
  out += ')';
  return out;
}

std::string formatExchangeTime(const int64_t utcSeconds, const int32_t gmtOffset) {
  const int64_t local = utcSeconds + gmtOffset;
  const int64_t ofDay = local - floorDiv(local, 86400) * 86400;
  const int hour = static_cast<int>(ofDay / 3600);
  const int minute = static_cast<int>((ofDay % 3600) / 60);
  const int twelve = hour % 12 == 0 ? 12 : hour % 12;
  char text[16];
  std::snprintf(text, sizeof(text), "%d:%02d %s", twelve, minute, hour < 12 ? "AM" : "PM");
  return text;
}

std::string formatExchangeDay(const int64_t utcSeconds, const int32_t gmtOffset) {
  static constexpr const char* kDays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const int64_t days = floorDiv(utcSeconds + gmtOffset, 86400);
  // 1970-01-01 was a Thursday.
  const int64_t weekday = ((days % 7) + 7 + 4) % 7;
  int year = 0;
  int month = 0;
  int day = 0;
  civilFromDays(days, year, month, day);
  char text[16];
  std::snprintf(text, sizeof(text), "%s %d", kDays[weekday], day);
  return text;
}

// --- Drawing a series ----------------------------------------------------------

Range rangeOf(const Series& series) {
  double low = 0;
  double high = 0;
  bool any = false;
  auto take = [&](const double v) {
    if (!(v > 0) || !std::isfinite(v)) return;
    if (!any) {
      low = high = v;
      any = true;
      return;
    }
    if (v < low) low = v;
    if (v > high) high = v;
  };
  for (const Point& p : series.points) {
    take(p.close);
  }
  take(series.previousClose);
  take(series.price);
  if (!any) return Range{0, 1};

  double span = high - low;
  const double mid = (high + low) / 2;
  const double floor = std::fabs(mid) * 0.002 > 0.01 ? std::fabs(mid) * 0.002 : 0.01;
  if (span < floor) {
    low = mid - floor / 2;
    high = mid + floor / 2;
    span = floor;
  }
  const double pad = span * 0.08;
  return Range{low - pad, high + pad};
}

int16_t yFor(const double value, const Range& range, const int16_t top, const int16_t height) {
  const double span = range.high - range.low;
  if (!(span > 0) || height <= 1) return top;
  double frac = (value - range.low) / span;
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  return static_cast<int16_t>(top + std::lround((1.0 - frac) * (height - 1)));
}

// --- The cache -----------------------------------------------------------------

std::string serializeSeries(const Series& series) {
  std::string out;
  out.reserve(160 + series.points.size() * 24);
  out += "crossplay-stocks-series 2\n";
  out += "span ";
  out += series.span == Span::Today ? "today" : "days";
  out += "\nprice " + formatNumber(series.price, 12);
  out += "\nprevious " + formatNumber(series.previousClose, 12);
  out += "\ncurrency " + cleanName(series.currency.c_str(), 8);
  out += "\nname " + cleanName(series.name.c_str());
  out += "\nmarket " + std::to_string(series.marketTime);
  out += "\noffset " + std::to_string(series.gmtOffset);
  char day[96];
  std::snprintf(day, sizeof(day), "\nday %.9g %.9g %.9g", static_cast<double>(series.dayOpen),
                static_cast<double>(series.dayHigh), static_cast<double>(series.dayLow));
  out += day;
  out += "\npoints " + std::to_string(series.points.size()) + "\n";
  for (const Point& p : series.points) {
    char line[48];
    std::snprintf(line, sizeof(line), "%lu %.9g\n", static_cast<unsigned long>(p.time), static_cast<double>(p.close));
    out += line;
  }
  return out;
}

bool parseSeries(const std::string& text, Series& out) {
  Series s;
  size_t pos = 0;
  auto nextLine = [&](std::string& line) {
    if (pos >= text.size()) return false;
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    line = text.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
  };
  // Each header line is `key value`; the value is everything after the first
  // space, so a company name keeps its own spaces.
  auto field = [&](const char* key, std::string& value) {
    std::string line;
    if (!nextLine(line)) return false;
    const size_t keyLen = std::strlen(key);
    if (line.compare(0, keyLen, key) != 0) return false;
    if (line.size() == keyLen) {
      value.clear();
      return true;
    }
    if (line[keyLen] != ' ') return false;
    value = line.substr(keyLen + 1);
    return true;
  };

  std::string value;
  std::string line;
  // Version 1 carried an open, high and low per point. It is refused rather
  // than converted: the next refresh rewrites it, and a cache is only a cache.
  if (!nextLine(line) || line != "crossplay-stocks-series 2") return false;
  if (!field("span", value)) return false;
  if (value == "today") {
    s.span = Span::Today;
  } else if (value == "days") {
    s.span = Span::Days;
  } else {
    return false;
  }
  if (!field("price", value) || !takeDouble(value.c_str(), s.price)) return false;
  if (!field("previous", value) || !takeDouble(value.c_str(), s.previousClose)) return false;
  if (!field("currency", s.currency)) return false;
  if (!field("name", s.name)) return false;
  int64_t number = 0;
  if (!field("market", value) || !takeInt(value.c_str(), s.marketTime)) return false;
  if (!field("offset", value) || !takeInt(value.c_str(), number)) return false;
  if (number < -86400 || number > 86400) return false;
  s.gmtOffset = static_cast<int32_t>(number);
  if (!field("day", value)) return false;
  double o = 0, h = 0, l = 0;
  if (std::sscanf(value.c_str(), "%lf %lf %lf", &o, &h, &l) != 3) return false;
  s.dayOpen = static_cast<float>(o);
  s.dayHigh = static_cast<float>(h);
  s.dayLow = static_cast<float>(l);
  if (!field("points", value) || !takeInt(value.c_str(), number)) return false;
  if (number < 0 || static_cast<size_t>(number) > kMaxPoints) return false;
  s.points.reserve(static_cast<size_t>(number));
  for (int64_t i = 0; i < number; ++i) {
    if (!nextLine(line)) return false;
    unsigned long t = 0;
    double c = 0;
    if (std::sscanf(line.c_str(), "%lu %lf", &t, &c) != 2) return false;
    Point p;
    p.time = static_cast<uint32_t>(t);
    p.close = static_cast<float>(c);
    s.points.push_back(p);
  }
  if (!s.valid()) return false;
  out = std::move(s);
  return true;
}

}  // namespace stocks
