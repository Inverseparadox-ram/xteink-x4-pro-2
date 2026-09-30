#include "StocksStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

#include "StocksFetch.h"

namespace stocks {
namespace {

constexpr const char* kTag = "STOCKS";
constexpr const char* kDir = "/.crosspoint/stocks";
constexpr const char* kSpanPath = "/.crosspoint/stocks/span.txt";
constexpr const char* kListDir = "/Stocks";
constexpr const char* kListPath = "/Stocks/watchlist.txt";
constexpr const char* kKeyPath = "/Stocks/twelvedata.txt";
// A watchlist is a few hundred bytes; the cap only stops a wrong file (a
// photo renamed, say) from being read into RAM whole.
constexpr size_t kMaxFile = 32 * 1024;

bool readWholeFile(const char* path, std::string& out) {
  HalFile file;
  if (!Storage.openFileForRead(kTag, path, file)) return false;
  if (file.size() > kMaxFile) {
    LOG_ERR(kTag, "%s is %u bytes; not reading it", path, static_cast<unsigned>(file.size()));
    return false;
  }
  out.clear();
  out.reserve(file.size());
  char buffer[512];
  int n = 0;
  while ((n = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer))) > 0) {
    out.append(buffer, static_cast<size_t>(n));
  }
  return true;
}

bool writeWholeFile(const char* path, const std::string& text) {
  HalFile file;
  if (!Storage.openFileForWrite(kTag, path, file)) return false;
  return file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == static_cast<int>(text.size());
}

// Write beside, then rename: a watchlist half-written by a card pulled mid-save
// is a list somebody has to type again.
bool writeAtomically(const char* path, const std::string& text) {
  const std::string temp = std::string(path) + ".part";
  if (!writeWholeFile(temp.c_str(), text)) {
    LOG_ERR(kTag, "cannot write %s", temp.c_str());
    return false;
  }
  Storage.remove(path);
  if (!Storage.rename(temp.c_str(), path)) {
    LOG_ERR(kTag, "cannot rename %s into place", temp.c_str());
    return false;
  }
  return true;
}

std::string cachePath(const Holding& holding, const Span span) {
  return std::string(kDir) + "/" + yahooSymbol(holding) + (span == Span::Today ? "-today.txt" : "-days.txt");
}

std::string lineFor(const Holding& holding) {
  std::string line = holding.symbol;
  line.resize(line.size() < 10 ? 10 : line.size() + 1, ' ');
  std::string exchange = exchangeName(holding.exchange);
  exchange.resize(8, ' ');
  line += exchange;
  if (holding.shares > 0) {
    char shares[32];
    std::snprintf(shares, sizeof(shares), "%.6g", holding.shares);
    line += shares;
  }
  while (!line.empty() && line.back() == ' ') line.pop_back();
  return line;
}

}  // namespace

const char* Store::watchlistPath() { return kListPath; }

void Store::load() {
  Storage.mkdir(kDir);
  std::string text;
  if (!readWholeFile(kListPath, text)) {
    Storage.mkdir(kListDir);
    text = sampleWatchlist();
    if (!writeAtomically(kListPath, text)) LOG_ERR(kTag, "cannot write the sample watchlist");
  }
  parseWatchlist(text, holdings_, problems_);
  LOG_INF(kTag, "watchlist: %d stocks, %d problems", static_cast<int>(holdings_.size()),
          static_cast<int>(problems_.size()));

  // Read on every open, so a key copied onto the card works without a restart.
  std::string keyText;
  if (!readWholeFile(kKeyPath, keyText)) {
    keyText.clear();
    if (!writeAtomically(kKeyPath, sampleKeyFile())) LOG_ERR(kTag, "cannot write the sample key file");
  }
  const std::string key = parseKeyFile(keyText);
  setTwelveDataKey(key);
  LOG_INF(kTag, "Twelve Data: %s", key.empty() ? "no key, Yahoo only" : "key present");

  std::string span;
  if (readWholeFile(kSpanPath, span) && span.rfind("days", 0) == 0) {
    span_ = Span::Days;
  } else {
    span_ = Span::Today;
  }
}

bool Store::rewrite(const Holding& holding, const std::string* replacement) {
  std::string text;
  if (!readWholeFile(kListPath, text)) text.clear();
  std::string out;
  out.reserve(text.size() + 32);
  bool replaced = false;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    // Each line is judged on its own, so a comment or a line with a problem is
    // copied through untouched and only the stock in question moves.
    std::vector<Holding> one;
    std::vector<WatchlistProblem> ignored;
    parseWatchlist(line, one, ignored);
    const bool match = one.size() == 1 && one[0].symbol == holding.symbol && one[0].exchange == holding.exchange;
    if (match) {
      if (replacement != nullptr && !replaced) {
        out += *replacement;
        out += '\n';
      }
      replaced = true;
      continue;
    }
    out += line;
    out += '\n';
  }
  if (replacement != nullptr && !replaced) {
    out += *replacement;
    out += '\n';
  }
  if (!writeAtomically(kListPath, out)) return false;
  parseWatchlist(out, holdings_, problems_);
  return true;
}

bool Store::put(const Holding& holding) {
  const std::string line = lineFor(holding);
  return rewrite(holding, &line);
}

bool Store::remove(const Holding& holding) {
  Storage.remove(cachePath(holding, Span::Today).c_str());
  Storage.remove(cachePath(holding, Span::Days).c_str());
  return rewrite(holding, nullptr);
}

void Store::setSpan(const Span span) {
  if (span == span_) return;
  span_ = span;
  writeWholeFile(kSpanPath, span == Span::Days ? "days\n" : "today\n");
}

bool Store::cached(const Holding& holding, const Span span, Series& out) const {
  std::string text;
  if (!readWholeFile(cachePath(holding, span).c_str(), text)) return false;
  if (!parseSeries(text, out) || out.span != span) {
    LOG_ERR(kTag, "cache for %s did not parse", holding.symbol.c_str());
    return false;
  }
  return true;
}

void Store::writeCache(const Holding& holding, const Series& series) {
  if (!writeAtomically(cachePath(holding, series.span).c_str(), serializeSeries(series))) {
    LOG_ERR(kTag, "cache for %s not written", holding.symbol.c_str());
  }
}

}  // namespace stocks
