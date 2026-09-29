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

#include <string>

#include "StocksCore.h"

namespace stocks {

// Turns a chart response into a Series for `span`. False with `message` set
// when the body is not a chart, is the service's own error, or has no price.
bool parseChart(const std::string& body, Span span, Series& out, std::string& message);

// One request. `message` says what to do when it fails.
bool fetchSeries(const Holding& holding, Span span, Series& out, std::string& message);

}  // namespace stocks
