#pragma once

// The one file that knows where prices come from. Two sources, in order:
//
// Twelve Data, when /Stocks/twelvedata.txt holds a key. An official API with
// published limits: a free key covers NASDAQ and NYSE, 800 requests a day and
// 8 a minute; NSE and BSE need a paid plan.
//   GET https://api.twelvedata.com/time_series?symbol=AAPL,RELIANCE:NSE
//       &interval=5min&outputsize=90&timezone=UTC     TODAY (and yesterday's close)
//       &interval=1day&outputsize=12&timezone=UTC     DAYS
//
// Yahoo, for everything Twelve Data did not answer: no key, both countries,
// but unofficial, and it turns clients away with 429 when it likes.
//   GET https://query1.finance.yahoo.com/v8/finance/spark?symbols=...   the list
//   GET https://query1.finance.yahoo.com/v8/finance/chart/<SYMBOL>      one stock
//
// A source that answers 429 is left alone for a while (a minute for Twelve
// Data, whose limit is per minute; five for Yahoo) rather than asked again
// for every remaining stock, which is what keeps a refusal from becoming a
// longer one.

#include <cstdint>
#include <string>
#include <vector>

#include "StocksCore.h"

namespace stocks {

// The key from /Stocks/twelvedata.txt, "" for none. Set by Store::load().
void setTwelveDataKey(const std::string& key);
bool haveTwelveDataKey();

// Turns a Yahoo chart response into a Series for `span`. False with `message`
// set when the body is not a chart, is the service's own error, or has no
// price.
bool parseChart(const std::string& body, Span span, Series& out, std::string& message);

// One stock: Twelve Data if it can, else Yahoo. `message` says what to do
// when both failed.
bool fetchSeries(const Holding& holding, Span span, Series& out, std::string& message);

// The whole list in as few requests as the sources allow. `got[i]` says which
// holdings came back; false (with `message`) only when nothing did, and then
// the caller falls back to fetchSeries one stock at a time.
bool fetchBatch(const std::vector<Holding>& holdings, Span span, std::vector<Series>& out, std::vector<uint8_t>& got,
                std::string& message);

// Yahoo's spark answer read into `out`, lined up with `symbols` (Yahoo's own
// spelling, yahooSymbol()). Returns how many were found. Public so the
// simulator fixture and the tests can check the shape.
size_t parseSpark(const std::string& body, Span span, const std::vector<std::string>& symbols, std::vector<Series>& out,
                  std::vector<uint8_t>& got);

// A Twelve Data time_series answer for `holdings` (one symbol or a batch),
// lined up the same way. `refused[i]` marks a symbol Twelve Data answered with
// an error of its own (not on the plan, not known), which Yahoo then gets.
// `message` is set when the whole answer was an error: a bad key, the limit.
// Returns the service's error code for that case (401, 429...), else 0.
int parseTwelve(const std::string& body, Span span, const std::vector<Holding>& holdings, std::vector<Series>& out,
                std::vector<uint8_t>& got, std::vector<uint8_t>& refused, std::string& message);

}  // namespace stocks
