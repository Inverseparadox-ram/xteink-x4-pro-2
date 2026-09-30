#pragma once

// The one file that knows Yahoo's chart service exists.
//
// GET https://query1.finance.yahoo.com/v8/finance/chart/<SYMBOL>
//     ?range=1d&interval=5m    TODAY: the session so far, five-minute steps
//     ?range=1mo&interval=1d   DAYS:  a month of closes, cut to kDaysShown
//
// No key and no account, and it covers both New York and Mumbai, which no
// other free source does. It is also unofficial: the day it changes shape
// parseChart() says so in words rather than drawing zeros.

#include <cstdint>
#include <string>
#include <vector>

#include "StocksCore.h"

namespace stocks {

// Turns a chart response into a Series for `span`. False with `message` set
// when the body is not a chart, is the service's own error, or has no price.
bool parseChart(const std::string& body, Span span, Series& out, std::string& message);

// One request. `message` says what to do when it fails.
bool fetchSeries(const Holding& holding, Span span, Series& out, std::string& message);

// The whole list in ONE request, through the spark endpoint:
//   /v8/finance/spark?symbols=AAPL,RELIANCE.NS&range=1d&interval=5m
// One TLS handshake instead of one per stock, which is most of a refresh's
// time on this device. `got[i]` says which holdings came back; false (with
// `message`) only when the request failed outright, and then the caller falls
// back to fetchSeries one stock at a time.
bool fetchBatch(const std::vector<Holding>& holdings, Span span, std::vector<Series>& out, std::vector<uint8_t>& got,
                std::string& message);

// The spark answer read into `out`, lined up with `symbols` (Yahoo's own
// spelling, yahooSymbol()). Returns how many were found. Public so the
// simulator fixture and the tests can check the shape.
size_t parseSpark(const std::string& body, Span span, const std::vector<std::string>& symbols, std::vector<Series>& out,
                  std::vector<uint8_t>& got);

}  // namespace stocks
