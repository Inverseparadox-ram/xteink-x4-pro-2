#include "StocksFetch.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(FREEINK_NET_WOLFSSL)
#include <Arduino.h>
#include <HalStorage.h>
#include <SecureHttpClient.h>
#include <esp_heap_caps.h>

#include "../study/StudySyncRoots.h"
#include "StocksRoots.h"
#else
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#endif

namespace stocks {
namespace {

constexpr const char* kTag = "STOCKS";

// A day of five-minute steps is about 12KB a stock from Yahoo, 8KB from Twelve
// Data, and a batch is up to eight of those; it lands in PSRAM. The ceiling exists because a captive portal or a
// changed API can answer with anything, and running out of heap mid-parse is a reboot, not a message.
constexpr size_t kMaxBody = 256 * 1024;

// Yahoo answers a client that does not look like a browser with 429 whatever
// the rate. This is the string a current desktop Safari sends.
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 "
    "Safari/605.1.15";

std::string yahooBase() {
#if !defined(FREEINK_NET_WOLFSSL)
  // The simulator points at a local fixture server, which is how these screens
  // get rendered in an environment with no route to either service. The
  // device build has no such door.
  if (const char* env = std::getenv("CROSSPLAY_STOCKS_BASE")) return env;
#endif
  return "https://query1.finance.yahoo.com";
}

std::string twelveBase() {
#if !defined(FREEINK_NET_WOLFSSL)
  if (const char* env = std::getenv("CROSSPLAY_TWELVEDATA_BASE")) return env;
#endif
  return "https://api.twelvedata.com";
}

std::string twelveKey;

// Seconds on a clock that only goes forward, for the quiet periods below.
uint32_t nowSeconds() {
#if defined(FREEINK_NET_WOLFSSL)
  return millis() / 1000;
#else
  return static_cast<uint32_t>(std::time(nullptr));
#endif
}

// After a 429 a source is left alone until this time: asking again for each
// remaining stock only extends the refusal.
uint32_t yahooQuietUntil = 0;
uint32_t twelveQuietUntil = 0;
bool quiet(const uint32_t until) { return until != 0 && static_cast<int32_t>(until - nowSeconds()) > 0; }

// Symbols Twelve Data answered with an error of its own (not on the plan, not
// known), so a refresh does not spend credits asking again until the app is
// reopened with a new key.
std::vector<std::string> twelveRefused;
bool refusedByTwelve(const Holding& holding) {
  const std::string symbol = twelveSymbol(holding);
  for (const std::string& s : twelveRefused) {
    if (s == symbol) return true;
  }
  return false;
}

constexpr const char* kYahooRefused =
    "Yahoo is turning requests away. Try again in a few minutes, or put a Twelve Data key in "
    "/Stocks/twelvedata.txt.";
constexpr const char* kYahooRefusedWithKey = "Yahoo is turning requests away. Try again in a few minutes.";
const char* yahooRefused() { return twelveKey.empty() ? kYahooRefused : kYahooRefusedWithKey; }

enum class Source : uint8_t { Yahoo, Twelve };
const char* sourceName(const Source source) { return source == Source::Twelve ? "Twelve Data" : "Yahoo Finance"; }

#if defined(FREEINK_NET_WOLFSSL)

bool insufficientHeap(std::string& message) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxBlock = ESP.getMaxAllocHeap();
  // Internal RAM only (getFreeHeap and getMaxAllocHeap both ask for it). TLS
  // and the parse put their large buffers in PSRAM, so what has to be free
  // here is the Wi-Fi driver's and the handshake's small working set. 70KB was
  // Weather's number and it refused on an X4 Pro with Wi-Fi up and the
  // watchlist loaded, which is the normal state of this app.
  if (freeHeap < 40000 || maxBlock < 16000) {
    LOG_ERR(kTag, "heap too low: free=%u block=%u", static_cast<unsigned>(freeHeap), static_cast<unsigned>(maxBlock));
    message = "Not enough memory free to fetch prices. Leave the app and open it again.";
    return true;
  }
  return false;
}

// An SD override wins over the baked roots, so a CA rotation at either
// service is a file copy rather than a reflash.
const std::string& sdRoots() {
  static std::string roots;
  static bool probed = false;
  if (!probed) {
    probed = true;
    HalFile file;
    if (Storage.openFileForRead(kTag, "/.crosspoint/stocks/roots.pem", file)) {
      const size_t size = file.size();
      if (size > 512 && size < 65536) {
        roots.resize(size);
        if (file.read(reinterpret_cast<uint8_t*>(roots.data()), size) == static_cast<int>(size) &&
            roots.find("-----BEGIN CERTIFICATE-----") != std::string::npos) {
          LOG_INF(kTag, "using SD root bundle (%u bytes)", static_cast<unsigned>(size));
        } else {
          roots.clear();
          LOG_ERR(kTag, "SD root bundle unreadable; using baked roots");
        }
      } else {
        roots.clear();
        LOG_ERR(kTag, "SD root bundle size %u rejected; using baked roots", static_cast<unsigned>(size));
      }
    }
  }
  return roots;
}

const char* caRoots(const Source source) {
  if (!sdRoots().empty()) return sdRoots().c_str();
  if (source == Source::Yahoo) return kYahooCaRoots;
  // Joined once, on the heap (PSRAM at this size), so no root is stored twice
  // in flash.
  static std::string joined;
  if (joined.empty()) joined = std::string(kYahooCaRoots) + study::kBridgeCaRoots + kExtraCaRoots;
  return joined.c_str();
}

int httpGet(const Source source, const std::string& url, std::string& body, std::string& message) {
  if (insufficientHeap(message)) return 0;
  freeink::SecureHttpClient http;
  http.setCACert(caRoots(source));
  http.setTimeout(30000);
  http.setFollowRedirects(2);
  if (!http.begin(url)) {
    message = "That price address did not make sense. Update the firmware.";
    return 0;
  }
  http.setUserAgent(kUserAgent);
  http.addHeader("Accept", "application/json");
  const int status = http.sendRequest("GET", std::string());
  if (status <= 0) {
    LOG_ERR(kTag, "%s GET failed: %d", sourceName(source), status);
    message = std::string("Could not reach ") + sourceName(source) +
              ". Check Wi-Fi. If Wi-Fi is fine, its certificate may have changed: put a current root bundle at "
              "/.crosspoint/stocks/roots.pem on the card.";
    http.end();
    return 0;
  }
  body = http.getString();
  http.end();
  return status;
}

#else  // simulator: curl, the same door Weather uses for the same reason.

int httpGet(const Source source, const std::string& url, std::string& body, std::string& message) {
  char outPath[] = "/tmp/stockshttp-XXXXXX";
  const int fd = mkstemp(outPath);
  if (fd < 0) {
    message = "sim: mkstemp failed";
    return 0;
  }
  close(fd);
  const std::string cmd = "curl -sS -m 60 -o '" + std::string(outPath) + "' -w '%{http_code}' -A '" + kUserAgent +
                          "' -H 'Accept: application/json' '" + url + "'";
  FILE* pipe = popen(cmd.c_str(), "r");
  char statusBuf[8] = {};
  if (pipe) {
    if (fgets(statusBuf, sizeof(statusBuf), pipe) == nullptr) statusBuf[0] = '\0';
    pclose(pipe);
  }
  FILE* out = fopen(outPath, "rb");
  if (out) {
    char chunk[1024];
    size_t n = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), out)) > 0) body.append(chunk, n);
    fclose(out);
  }
  unlink(outPath);
  const int status = std::atoi(statusBuf);
  if (status <= 0) message = std::string("Could not reach ") + sourceName(source) + ".";
  return status;
}

#endif

float takeFloat(JsonVariantConst value) {
  if (value.isNull() || !value.is<float>()) return 0.0f;
  const float v = value.as<float>();
  return std::isfinite(v) ? v : 0.0f;
}

double takeDouble(JsonVariantConst value) {
  if (value.isNull() || !value.is<double>()) return 0.0;
  const double v = value.as<double>();
  return std::isfinite(v) ? v : 0.0;
}

// The service reports its own failures as {"chart":{"result":null,"error":
// {"code":"Not Found","description":"No data found, symbol may be delisted"}}}.
bool takeServiceError(JsonVariantConst chart, std::string& message) {
  JsonVariantConst error = chart["error"];
  if (error.isNull()) return false;
  const char* description = error["description"].is<const char*>() ? error["description"].as<const char*>() : nullptr;
  message =
      description != nullptr ? "Yahoo says: " + cleanName(description, 120) : std::string("Yahoo refused the request.");
  return true;
}

}  // namespace

namespace {

// Reads one chart result -- the object the chart endpoint wraps in
// chart.result[0] and the spark endpoint in spark.result[i].response[0] --
// into a Series. The two endpoints share the shape, so they share this.
bool readResult(JsonVariantConst result, const Span span, Series& out, std::string& message) {
  Series series;
  series.span = span;
  JsonVariantConst m = result["meta"];
  series.price = takeDouble(m["regularMarketPrice"]);
  series.marketTime = m["regularMarketTime"].is<int64_t>() ? m["regularMarketTime"].as<int64_t>() : 0;
  series.gmtOffset = m["gmtoffset"].is<int32_t>() ? m["gmtoffset"].as<int32_t>() : 0;
  series.currency = cleanName(m["currency"].is<const char*>() ? m["currency"].as<const char*>() : nullptr, 8);
  const char* name = m["longName"].is<const char*>()    ? m["longName"].as<const char*>()
                     : m["shortName"].is<const char*>() ? m["shortName"].as<const char*>()
                                                        : nullptr;
  series.name = cleanName(name);
  // For one day the service's `previousClose` is yesterday's close, which is
  // what today's move is measured from. For a month it is absent and
  // chartPreviousClose is the close before the month, which keepLast() then
  // moves to the day before the window.
  series.previousClose = takeDouble(m["previousClose"]);
  if (series.previousClose <= 0 || span == Span::Days) series.previousClose = takeDouble(m["chartPreviousClose"]);
  series.dayHigh = takeFloat(m["regularMarketDayHigh"]);
  series.dayLow = takeFloat(m["regularMarketDayLow"]);

  JsonArrayConst times = result["timestamp"];
  JsonVariantConst q = result["indicators"]["quote"][0];
  JsonArrayConst opens = q["open"];
  JsonArrayConst highs = q["high"];
  JsonArrayConst lows = q["low"];
  JsonArrayConst closes = q["close"];
  const size_t count = times.isNull() ? 0 : times.size();
  series.points.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    Point p;
    p.time = static_cast<uint32_t>(times[i].as<int64_t>());
    p.close = takeFloat(closes[i]);
    series.points.push_back(p);
    // The session's open, high and low are kept once rather than per point;
    // the spark endpoint sends closes only, and the meta's day range stands.
    const float o = takeFloat(opens[i]);
    const float h = takeFloat(highs[i]);
    const float l = takeFloat(lows[i]);
    if (series.dayOpen <= 0 && o > 0) series.dayOpen = o;
    if (h > series.dayHigh) series.dayHigh = h;
    if (l > 0 && (series.dayLow <= 0 || l < series.dayLow)) series.dayLow = l;
  }
  dropGaps(series);
  if (span == Span::Days) keepLast(series, kDaysShown);
  // A price with no session yet (a symbol that has not opened today) still has
  // a last price; one point at it keeps the screens honest and non-empty.
  if (series.points.empty() && series.price > 0) {
    Point p;
    p.time = static_cast<uint32_t>(series.marketTime);
    p.close = static_cast<float>(series.price);
    series.points.push_back(p);
  }
  if (series.price <= 0 && !series.points.empty()) series.price = series.points.back().close;
  if (series.marketTime <= 0 && !series.points.empty()) series.marketTime = series.points.back().time;
  if (!series.valid()) {
    message = "Yahoo sent a chart with no price in it.";
    return false;
  }
  out = std::move(series);
  return true;
}

void filterResult(JsonObject result) {
  JsonObject meta = result["meta"].to<JsonObject>();
  for (const char* key : {"currency", "regularMarketPrice", "regularMarketTime", "chartPreviousClose", "previousClose",
                          "gmtoffset", "longName", "shortName", "regularMarketDayHigh", "regularMarketDayLow"}) {
    meta[key] = true;
  }
  result["timestamp"] = true;
  JsonObject quote = result["indicators"]["quote"][0].to<JsonObject>();
  for (const char* key : {"open", "high", "low", "close"}) quote[key] = true;
}

#if defined(BOARD_HAS_PSRAM) && defined(FREEINK_NET_WOLFSSL)
// The parsed document lives in PSRAM. ArduinoJson's pools are allocated in
// blocks small enough that the heap would otherwise put them in internal RAM,
// which is the memory a TLS connection and the Wi-Fi driver are short of --
// and running it low is what made refreshes refuse to start.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void* pointer) override { heap_caps_free(pointer); }
  void* reallocate(void* pointer, size_t size) override {
    return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};
ArduinoJson::Allocator* jsonAllocator() {
  static PsramAllocator allocator;
  return &allocator;
}
#else
ArduinoJson::Allocator* jsonAllocator() { return ArduinoJson::detail::DefaultAllocator::instance(); }
#endif

// Percent-encoding for a symbol in a query: M&M is a real NSE symbol.
std::string encodeSymbol(const std::string& symbol) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(symbol.size() * 3);
  for (const char c : symbol) {
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '.' ||
        u == '_') {
      out += static_cast<char>(u);
    } else {
      out += '%';
      out += kHex[u >> 4];
      out += kHex[u & 0x0F];
    }
  }
  return out;
}

const char* rangeQuery(const Span span) {
  return span == Span::Today ? "range=1d&interval=5m" : "range=1mo&interval=1d";
}

}  // namespace

bool parseChart(const std::string& body, const Span span, Series& out, std::string& message) {
  // Only what the screens use. The meta block alone carries trading periods
  // and valid ranges this app never reads, and a filter keeps them out of RAM.
  JsonDocument filter;
  filterResult(filter["chart"]["result"][0].to<JsonObject>());
  filter["chart"]["error"] = true;

  JsonDocument doc(jsonAllocator());
  const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err != DeserializationError::Ok) {
    LOG_ERR(kTag, "chart parse failed: %s", err.c_str());
    message = "The prices did not arrive in one piece. Try again.";
    return false;
  }
  JsonVariantConst chart = doc["chart"];
  if (chart.isNull()) {
    message = "Yahoo answered with something that is not a price chart.";
    return false;
  }
  JsonVariantConst result = chart["result"][0];
  if (result.isNull()) {
    if (!takeServiceError(chart, message)) message = "Yahoo had no prices for that symbol.";
    return false;
  }
  return readResult(result, span, out, message);
}

size_t parseSpark(const std::string& body, const Span span, const std::vector<std::string>& symbols,
                  std::vector<Series>& out, std::vector<uint8_t>& got) {
  got.assign(symbols.size(), 0);
  out.resize(symbols.size());
  JsonDocument filter;
  JsonObject entry = filter["spark"]["result"][0].to<JsonObject>();
  entry["symbol"] = true;
  filterResult(entry["response"][0].to<JsonObject>());

  JsonDocument doc(jsonAllocator());
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return 0;
  JsonArrayConst results = doc["spark"]["result"];
  if (results.isNull()) return 0;
  size_t found = 0;
  for (JsonVariantConst item : results) {
    const char* symbol = item["symbol"].is<const char*>() ? item["symbol"].as<const char*>() : nullptr;
    if (symbol == nullptr) continue;
    for (size_t i = 0; i < symbols.size(); ++i) {
      if (got[i] || symbols[i] != symbol) continue;
      std::string ignored;
      if (readResult(item["response"][0], span, out[i], ignored)) {
        got[i] = 1;
        ++found;
      }
      break;
    }
  }
  return found;
}

namespace {

// Twelve Data sends numbers as strings ("148.85001"); accept either.
float takeNumber(JsonVariantConst value) {
  if (value.is<const char*>()) {
    const char* text = value.as<const char*>();
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    return end != text && std::isfinite(v) ? static_cast<float>(v) : 0.0f;
  }
  return takeFloat(value);
}

std::string twelveError(JsonVariantConst item) {
  const char* said = item["message"].is<const char*>() ? item["message"].as<const char*>() : nullptr;
  return said != nullptr ? "Twelve Data says: " + cleanName(said, 120) : std::string("Twelve Data refused it.");
}

struct Bar {
  int64_t time = 0;  // UTC
  float open = 0, high = 0, low = 0, close = 0;
};

// One symbol's time_series object into a Series. Values arrive newest first,
// in UTC because the request asked for timezone=UTC.
bool readTwelve(JsonVariantConst item, const Holding& holding, const Span span, Series& out, std::string& message) {
  JsonArrayConst values = item["values"];
  if (values.isNull() || values.size() == 0) {
    message = "Twelve Data had no prices for that symbol.";
    return false;
  }
  std::vector<Bar> bars;  // newest first, as sent
  bars.reserve(values.size());
  for (JsonVariantConst v : values) {
    Bar bar;
    if (!parseUtcTime(v["datetime"].is<const char*>() ? v["datetime"].as<const char*>() : nullptr, bar.time)) continue;
    bar.close = takeNumber(v["close"]);
    if (bar.close <= 0) continue;
    bar.open = takeNumber(v["open"]);
    bar.high = takeNumber(v["high"]);
    bar.low = takeNumber(v["low"]);
    bars.push_back(bar);
  }
  if (bars.empty()) {
    message = "Twelve Data sent prices this app could not read.";
    return false;
  }
  Series series;
  series.span = span;
  const int32_t offset = exchangeOffset(holding.exchange, bars.front().time);
  series.gmtOffset = offset;
  const char* currency = item["meta"]["currency"].is<const char*>() ? item["meta"]["currency"].as<const char*>()
                                                                    : currencyFor(holding.exchange);
  series.currency = cleanName(currency, 8);

  size_t shown = 0;  // how many of the newest bars are on the chart
  if (span == Span::Today) {
    // The newest session is the bars on the newest exchange-local date; the
    // bar before it is yesterday's close, which today's move is measured from.
    const auto localDay = [&](const int64_t t) {
      const int64_t local = t + offset;
      return local / 86400 - (local % 86400 < 0 ? 1 : 0);
    };
    const int64_t today = localDay(bars.front().time);
    while (shown < bars.size() && localDay(bars[shown].time) == today) ++shown;
  } else {
    shown = bars.size() < kDaysShown ? bars.size() : kDaysShown;
  }
  if (shown < bars.size()) series.previousClose = bars[shown].close;

  series.points.reserve(shown);
  for (size_t k = shown; k > 0; --k) {
    const Bar& bar = bars[k - 1];
    Point p;
    // A daily bar is a DATE. Placed at local noon it lands on the right day
    // on every screen that reads it through gmtOffset.
    p.time = static_cast<uint32_t>(span == Span::Today ? bar.time : bar.time + 12 * 3600 - offset);
    p.close = bar.close;
    series.points.push_back(p);
    if (series.dayOpen <= 0 && bar.open > 0) series.dayOpen = bar.open;
    if (bar.high > series.dayHigh) series.dayHigh = bar.high;
    if (bar.low > 0 && (series.dayLow <= 0 || bar.low < series.dayLow)) series.dayLow = bar.low;
  }
  series.price = bars.front().close;
  // A five-minute bar is stamped with its start; its price is as of its end.
  series.marketTime = span == Span::Today ? bars.front().time + 300 : series.points.back().time;
  if (!series.valid()) {
    message = "Twelve Data sent a series with no price in it.";
    return false;
  }
  out = std::move(series);
  return true;
}

void filterTwelve(JsonObject item) {
  item["status"] = true;
  item["code"] = true;
  item["message"] = true;
  item["meta"]["currency"] = true;
  JsonObject value = item["values"][0].to<JsonObject>();
  for (const char* key : {"datetime", "open", "high", "low", "close"}) value[key] = true;
}

std::string twelveUrl(const std::vector<Holding>& holdings, const Span span) {
  std::string list;
  for (const Holding& h : holdings) {
    if (!list.empty()) list += ',';
    list += encodeSymbol(twelveSymbol(h));
  }
  // TODAY asks for 90 five-minute bars: a whole session is 75 (NSE) or 78
  // (New York), so the answer always reaches back into the session before,
  // whose last bar is the previous close.
  const char* shape = span == Span::Today ? "interval=5min&outputsize=90" : "interval=1day&outputsize=12";
  return twelveBase() + "/time_series?symbol=" + list + "&" + shape + "&timezone=UTC&apikey=" + twelveKey;
}

// Twelve Data for `holdings`, filling what it answered. Marks what it refused.
// Returns false (with `message`) when the request itself failed.
bool twelveBatch(const std::vector<Holding>& holdings, const Span span, std::vector<Series>& out,
                 std::vector<uint8_t>& got, std::string& message) {
  std::string body;
  const int status = httpGet(Source::Twelve, twelveUrl(holdings, span), body, message);
  if (status <= 0) return false;
  if (body.size() > kMaxBody) {
    message = "Twelve Data sent far more than prices. Nothing was changed.";
    return false;
  }
  std::vector<uint8_t> refused;
  const int code = parseTwelve(body, span, holdings, out, got, refused, message);
  if (status == 429 || code == 429) {
    twelveQuietUntil = nowSeconds() + 60;
    LOG_ERR(kTag, "Twelve Data: over the per-minute limit; Yahoo for the rest");
    if (message.empty()) message = "Twelve Data's per-minute limit was reached.";
    return false;
  }
  if (code == 401 || code == 403 || status == 401) {
    // A bad key fails every request the same way: stop asking this session.
    twelveQuietUntil = nowSeconds() + 3600;
    LOG_ERR(kTag, "Twelve Data refused the key: %s", message.c_str());
    message = "Twelve Data refused the key in /Stocks/twelvedata.txt. " + message;
    return false;
  }
  if (code != 0 || status >= 400) {
    if (message.empty()) {
      char text[64];
      std::snprintf(text, sizeof(text), "Twelve Data answered %d.", status);
      message = text;
    }
    return false;
  }
  size_t found = 0;
  for (size_t i = 0; i < holdings.size(); ++i) {
    if (got[i]) {
      ++found;
    } else if (refused[i]) {
      twelveRefused.push_back(twelveSymbol(holdings[i]));
      LOG_INF(kTag, "Twelve Data will not serve %s; Yahoo gets it", twelveSymbol(holdings[i]).c_str());
    }
  }
  LOG_INF(kTag, "Twelve Data %s: %u of %u in one request", spanName(span), static_cast<unsigned>(found),
          static_cast<unsigned>(holdings.size()));
  return true;
}

bool yahooChart(const Holding& holding, const Span span, Series& out, std::string& message) {
  if (quiet(yahooQuietUntil)) {
    message = yahooRefused();
    return false;
  }
  const std::string url =
      yahooBase() + "/v8/finance/chart/" + encodeSymbol(yahooSymbol(holding)) + "?" + rangeQuery(span);
  std::string body;
  const int status = httpGet(Source::Yahoo, url, body, message);
  if (status <= 0) return false;
  if (body.size() > kMaxBody) {
    LOG_ERR(kTag, "response too large: %u bytes", static_cast<unsigned>(body.size()));
    message = "Yahoo sent far more than a price chart. Nothing was changed.";
    return false;
  }
  if (status == 429) {
    yahooQuietUntil = nowSeconds() + 300;
    message = yahooRefused();
    return false;
  }
  if (status >= 400) {
    // A 404 carries the service's own words ("symbol may be delisted"), which
    // is what tells a typo in watchlist.txt apart from an outage.
    Series ignored;
    std::string said;
    parseChart(body, span, ignored, said);
    if (said.rfind("Yahoo says", 0) == 0) {
      message = said;
    } else {
      char text[80];
      std::snprintf(text, sizeof(text), "Yahoo answered %d. Try again in a moment.", status);
      message = text;
    }
    return false;
  }
  if (!parseChart(body, span, out, message)) return false;
  LOG_INF(kTag, "%s %s: %d points at %.2f (Yahoo)", yahooSymbol(holding).c_str(), spanName(span),
          static_cast<int>(out.points.size()), out.price);
  return true;
}

// Yahoo's spark for the holdings `want` marks, filling `out`/`got` in place.
void yahooSpark(const std::vector<Holding>& holdings, const std::vector<uint8_t>& want, const Span span,
                std::vector<Series>& out, std::vector<uint8_t>& got, std::string& message) {
  if (quiet(yahooQuietUntil)) {
    message = yahooRefused();
    return;
  }
  std::vector<std::string> symbols;
  std::vector<size_t> index;
  std::string list;
  for (size_t i = 0; i < holdings.size(); ++i) {
    if (!want[i]) continue;
    symbols.push_back(yahooSymbol(holdings[i]));
    index.push_back(i);
    if (!list.empty()) list += ',';
    list += encodeSymbol(symbols.back());
  }
  if (symbols.empty()) return;
  const std::string url = yahooBase() + "/v8/finance/spark?symbols=" + list + "&" + rangeQuery(span);
  std::string body;
  const int status = httpGet(Source::Yahoo, url, body, message);
  if (status <= 0) return;
  if (status == 429) {
    yahooQuietUntil = nowSeconds() + 300;
    message = yahooRefused();
    LOG_ERR(kTag, "Yahoo spark: 429; leaving Yahoo alone for five minutes");
    return;
  }
  if (status >= 400 || body.size() > kMaxBody) {
    char text[80];
    std::snprintf(text, sizeof(text), "Yahoo answered %d to the watchlist request.", status);
    message = text;
    LOG_ERR(kTag, "spark: status %d, %u bytes", status, static_cast<unsigned>(body.size()));
    return;
  }
  std::vector<Series> sparkOut;
  std::vector<uint8_t> sparkGot;
  const size_t found = parseSpark(body, span, symbols, sparkOut, sparkGot);
  for (size_t k = 0; k < index.size(); ++k) {
    if (!sparkGot[k]) continue;
    out[index[k]] = std::move(sparkOut[k]);
    got[index[k]] = 1;
  }
  LOG_INF(kTag, "spark %s: %u of %u symbols in one request", spanName(span), static_cast<unsigned>(found),
          static_cast<unsigned>(symbols.size()));
  if (found == 0) message = "Yahoo's watchlist answer was not in a shape this app knows.";
}

bool twelveUsable(const Holding& holding) {
  return !twelveKey.empty() && !quiet(twelveQuietUntil) && !refusedByTwelve(holding);
}

}  // namespace

void setTwelveDataKey(const std::string& key) {
  if (key != twelveKey) {
    // A new key may be on a different plan: what the old one refused, ask again.
    twelveRefused.clear();
    twelveQuietUntil = 0;
  }
  twelveKey = key;
}

bool haveTwelveDataKey() { return !twelveKey.empty(); }

int parseTwelve(const std::string& body, const Span span, const std::vector<Holding>& holdings,
                std::vector<Series>& out, std::vector<uint8_t>& got, std::vector<uint8_t>& refused,
                std::string& message) {
  out.assign(holdings.size(), Series{});
  got.assign(holdings.size(), 0);
  refused.assign(holdings.size(), 0);
  if (holdings.empty()) return 0;
  // A batch is an object keyed by the symbols as asked for; one symbol is the
  // series object itself. The filter names every key it may carry.
  JsonDocument filter;
  filterTwelve(filter.to<JsonObject>());
  for (const Holding& h : holdings) filterTwelve(filter[twelveSymbol(h)].to<JsonObject>());

  JsonDocument doc(jsonAllocator());
  const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err != DeserializationError::Ok) {
    LOG_ERR(kTag, "Twelve Data parse failed: %s", err.c_str());
    message = "The prices did not arrive in one piece. Try again.";
    return -1;
  }
  JsonVariantConst root = doc.as<JsonVariantConst>();
  const bool single = !root["status"].isNull();
  if (single && root["status"] == "error") {
    // With one symbol, an error may be that symbol's or the whole request's.
    const int code = root["code"].is<int>() ? root["code"].as<int>() : 0;
    message = twelveError(root);
    if (code == 401 || code == 429) return code;
    if (holdings.size() == 1) {
      refused[0] = 1;
      return 0;
    }
    return code != 0 ? code : -1;
  }
  for (size_t i = 0; i < holdings.size(); ++i) {
    JsonVariantConst item = single ? root : root[twelveSymbol(holdings[i])];
    if (item.isNull()) continue;
    if (item["status"] == "error") {
      refused[i] = 1;
      if (message.empty()) message = twelveError(item);
      continue;
    }
    std::string why;
    if (readTwelve(item, holdings[i], span, out[i], why)) {
      got[i] = 1;
    } else if (message.empty()) {
      message = why;
    }
    if (single) break;
  }
  return 0;
}

bool fetchSeries(const Holding& holding, const Span span, Series& out, std::string& message) {
  if (twelveUsable(holding)) {
    const std::vector<Holding> one{holding};
    std::vector<Series> series;
    std::vector<uint8_t> got;
    std::string said;
    if (twelveBatch(one, span, series, got, said) && got[0]) {
      out = std::move(series[0]);
      return true;
    }
    LOG_INF(kTag, "%s: Twelve Data did not answer (%s); asking Yahoo", holding.symbol.c_str(), said.c_str());
    if (!said.empty()) message = said;
  }
  std::string yahooSaid;
  if (yahooChart(holding, span, out, yahooSaid)) return true;
  // Yahoo's reason, unless Twelve Data's is the one that can be acted on.
  if (message.rfind("Twelve Data refused the key", 0) != 0) message = yahooSaid;
  return false;
}

bool fetchBatch(const std::vector<Holding>& holdings, const Span span, std::vector<Series>& out,
                std::vector<uint8_t>& got, std::string& message) {
  got.assign(holdings.size(), 0);
  out.assign(holdings.size(), Series{});
  if (holdings.empty()) return true;

  // Twelve Data first, eight at a time: the free plan's per-minute limit, so
  // the first eight always fit and a longer list learns its fate in one go.
  static constexpr size_t kTwelveChunk = 8;
  std::vector<Holding> chunk;
  std::vector<size_t> where;
  const auto flush = [&]() {
    if (chunk.empty() || quiet(twelveQuietUntil)) return;
    std::vector<Series> series;
    std::vector<uint8_t> answered;
    std::string said;
    if (twelveBatch(chunk, span, series, answered, said)) {
      for (size_t k = 0; k < chunk.size(); ++k) {
        if (!answered[k]) continue;
        out[where[k]] = std::move(series[k]);
        got[where[k]] = 1;
      }
    } else {
      message = said;
    }
    chunk.clear();
    where.clear();
  };
  for (size_t i = 0; i < holdings.size(); ++i) {
    if (!twelveUsable(holdings[i])) continue;
    chunk.push_back(holdings[i]);
    where.push_back(i);
    if (chunk.size() == kTwelveChunk) flush();
  }
  flush();

  // Yahoo for the rest, in one request.
  std::vector<uint8_t> rest(holdings.size(), 0);
  bool any = false;
  for (size_t i = 0; i < holdings.size(); ++i) {
    rest[i] = got[i] ? 0 : 1;
    any = any || rest[i];
  }
  std::string yahooSaid;
  if (any) yahooSpark(holdings, rest, span, out, got, yahooSaid);

  size_t found = 0;
  for (const uint8_t g : got) found += g;
  if (found > 0) return true;
  if (message.rfind("Twelve Data refused the key", 0) != 0 && !yahooSaid.empty()) message = yahooSaid;
  if (message.empty()) message = "Nothing came back. Try again in a moment.";
  return false;
}

}  // namespace stocks
