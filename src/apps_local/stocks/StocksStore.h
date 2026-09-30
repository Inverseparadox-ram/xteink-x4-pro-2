#pragma once

// The card side of Stocks.
//
//   /Stocks/watchlist.txt                    the stocks, written by a person
//   /Stocks/twelvedata.txt                   optional Twelve Data API key
//   /.crosspoint/stocks/<SYM>-<span>.txt     the last series fetched for each
//   /.crosspoint/stocks/span.txt             TODAY or DAYS, as last left
//   /.crosspoint/stocks/roots.pem            optional CA override (StocksFetch)
//
// The watchlist lives in a visible folder on purpose: it is a file people edit
// on a computer, and a dot-folder is hidden by default on both Macs and
// Windows. Adding and removing on the device edit the same file line by line,
// so comments and ordering a person wrote survive.

#include <string>
#include <vector>

#include "StocksCore.h"

namespace stocks {

class Store {
 public:
  // Reads the watchlist, writing the sample first when the card has none.
  void load();

  const std::vector<Holding>& holdings() const { return holdings_; }
  const std::vector<WatchlistProblem>& problems() const { return problems_; }

  // Adds a stock, or when it is already listed replaces that line, which is
  // how the number of shares is changed on the device. False on a card error.
  bool put(const Holding& holding);
  bool remove(const Holding& holding);

  Span span() const { return span_; }
  void setSpan(Span span);

  bool cached(const Holding& holding, Span span, Series& out) const;
  void writeCache(const Holding& holding, const Series& series);

  static const char* watchlistPath();

 private:
  bool rewrite(const Holding& holding, const std::string* replacement);

  std::vector<Holding> holdings_;
  std::vector<WatchlistProblem> problems_;
  Span span_ = Span::Today;
};

}  // namespace stocks
