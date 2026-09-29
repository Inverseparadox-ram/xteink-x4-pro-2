#include "StocksFetch.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(FREEINK_NET_WOLFSSL)
#include <Arduino.h>
#include <HalStorage.h>
#include <SecureHttpClient.h>

#include "StocksRoots.h"
#else
#include <unistd.h>

#include <cstdlib>
#endif

namespace stocks {
namespace {

constexpr const char* kTag = "STOCKS";

// A day of five-minute steps is about 12KB; a month of days about 4KB. The
// ceiling exists because a captive portal or a changed API can answer with
// anything, and running out of heap mid-parse is a reboot, not a message.
constexpr size_t kMaxBody = 128 * 1024;

// Yahoo answers a client that does not look like a browser with 429 whatever
// the rate. This is the string a current desktop Safari sends.
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 "
    "Safari/605.1.15";

std::string baseUrl() {
#if !defined(FREEINK_NET_WOLFSSL)
  // The simulator points at a local fixture server, which is how these screens
  // get rendered in an environment with no route to Yahoo. The device build
  // has no such door.
  if (const char* env = std::getenv("CROSSPLAY_STOCKS_BASE")) return env;
#endif
  return "https://query1.finance.yahoo.com";
}

#if defined(FREEINK_NET_WOLFSSL)

bool insufficientHeap(std::string& message) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxBlock = ESP.getMaxAllocHeap();
  if (freeHeap < 70000 || maxBlock < 20000) {
    LOG_ERR(kTag, "heap too low: free=%u block=%u", static_cast<unsigned>(freeHeap), static_cast<unsigned>(maxBlock));
    message = "Not enough memory free to fetch prices. Leave the app and open it again.";
    return true;
  }
  return false;
}

// An SD override wins over the baked DigiCert roots, so a CA rotation at
// Yahoo is a file copy rather than a reflash.
const char* caRoots() {
  static std::string sdRoots;
  static bool probed = false;
  if (!probed) {
    probed = true;
    HalFile file;
    if (Storage.openFileForRead(kTag, "/.crosspoint/stocks/roots.pem", file)) {
      const size_t size = file.size();
      if (size > 512 && size < 65536) {
        sdRoots.resize(size);
        if (file.read(reinterpret_cast<uint8_t*>(sdRoots.data()), size) == static_cast<int>(size) &&
            sdRoots.find("-----BEGIN CERTIFICATE-----") != std::string::npos) {
          LOG_INF(kTag, "using SD root bundle (%u bytes)", static_cast<unsigned>(size));
        } else {
          sdRoots.clear();
          LOG_ERR(kTag, "SD root bundle unreadable; using baked roots");
        }
      } else {
        sdRoots.clear();
        LOG_ERR(kTag, "SD root bundle size %u rejected; using baked roots", static_cast<unsigned>(size));
      }
    }
  }
  return sdRoots.empty() ? kYahooCaRoots : sdRoots.c_str();
}

int httpGet(const std::string& url, std::string& body, std::string& message) {
  if (insufficientHeap(message)) return 0;
  freeink::SecureHttpClient http;
  http.setCACert(caRoots());
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
    LOG_ERR(kTag, "GET failed: %d", status);
    message =
        "Could not reach Yahoo Finance. Check Wi-Fi. If Wi-Fi is fine, Yahoo may have changed certificate: put a "
        "current root bundle at /.crosspoint/stocks/roots.pem on the card.";
    http.end();
    return 0;
  }
  body = http.getString();
  http.end();
  return status;
}

#else  // simulator: curl, the same door Weather uses for the same reason.

int httpGet(const std::string& url, std::string& body, std::string& message) {
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
  if (status <= 0) message = "Could not reach Yahoo Finance.";
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

bool parseChart(const std::string& body, const Span span, Series& out, std::string& message) {
  // Only what the screens use. The meta block alone carries trading periods
  // and valid ranges this app never reads, and a filter keeps them out of RAM.
  JsonDocument filter;
  JsonObject meta = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  meta["currency"] = true;
  meta["regularMarketPrice"] = true;
  meta["regularMarketTime"] = true;
  meta["chartPreviousClose"] = true;
  meta["previousClose"] = true;
  meta["gmtoffset"] = true;
  meta["longName"] = true;
  meta["shortName"] = true;
  filter["chart"]["result"][0]["timestamp"] = true;
  JsonObject quote = filter["chart"]["result"][0]["indicators"]["quote"][0].to<JsonObject>();
  quote["open"] = true;
  quote["high"] = true;
  quote["low"] = true;
  quote["close"] = true;
  filter["chart"]["error"] = true;

  JsonDocument doc;
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
    p.time = times[i].as<int64_t>();
    p.close = takeFloat(closes[i]);
    p.open = takeFloat(opens[i]);
    p.high = takeFloat(highs[i]);
    p.low = takeFloat(lows[i]);
    series.points.push_back(p);
  }
  dropGaps(series);
  if (span == Span::Days) keepLast(series, kDaysShown);
  // A price with no session yet (a symbol that has not opened today) still has
  // a last price; one point at it keeps the screens honest and non-empty.
  if (series.points.empty() && series.price > 0) {
    Point p;
    p.time = series.marketTime;
    p.open = p.high = p.low = p.close = static_cast<float>(series.price);
    series.points.push_back(p);
  }
  if (!series.valid()) {
    message = "Yahoo sent a chart with no price in it.";
    return false;
  }
  out = std::move(series);
  return true;
}

bool fetchSeries(const Holding& holding, const Span span, Series& out, std::string& message) {
  const std::string url = baseUrl() + "/v8/finance/chart/" + yahooSymbol(holding) +
                          (span == Span::Today ? "?range=1d&interval=5m" : "?range=1mo&interval=1d");
  std::string body;
  const int status = httpGet(url, body, message);
  if (status <= 0) return false;
  if (body.size() > kMaxBody) {
    LOG_ERR(kTag, "response too large: %u bytes", static_cast<unsigned>(body.size()));
    message = "Yahoo sent far more than a price chart. Nothing was changed.";
    return false;
  }
  if (status == 429) {
    message = "Yahoo is turning requests away for now. Try again in a few minutes.";
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
  LOG_INF(kTag, "%s %s: %d points at %.2f", yahooSymbol(holding).c_str(), spanName(span),
          static_cast<int>(out.points.size()), out.price);
  return true;
}

}  // namespace stocks
