#pragma once

// The Stocks screens. Freestanding builders in the Weather mould: a model in,
// a drawn frame out, no renderer and no Activity, so host-tests/ui/ can assert
// what they drew and what they made tappable.
//
// ---------------------------------------------------------------------------
// The layout decision.
//
// The WATCHLIST is rows of three columns: what it is, what it did, what it
// costs. The middle column is a line drawn from the same series the detail
// screen charts, because on a 1-bit panel a shape is read faster than a signed
// number, and the number is right beside it for anyone who wants it.
//
// The toggle is one segmented bar, in the same pixels on the list and on a
// stock: TODAY or 10 DAYS. Flipping it changes every number on screen at once,
// the holdings total included, so there is never a screen showing one span's
// price beside another span's change.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "../ui/ToyboxScreen.h"
#include "StocksCore.h"

namespace stocksui {

namespace fui = freeink::ui;

// Weather 360s, Remote 380s, Clock and xkcd the 400s. Stocks takes the 440s.
enum : fui::ActionId {
  ActionOpenStock = 440,
  ActionRefresh = 441,
  ActionSpanToday = 442,
  ActionSpanDays = 443,
  ActionEdit = 444,
  ActionAdd = 445,
  ActionEditStock = 446,
  ActionChangeShares = 447,
  ActionRemoveStock = 448,
  ActionKeepStock = 449,
  ActionNotice = 450,
  ActionPrevStock = 451,
  ActionNextStock = 452,
};

// -1 down, 0 flat, +1 up.
int8_t trendOf(double delta);

// --- The watchlist -----------------------------------------------------------

struct StockRow {
  const char* symbol = "";
  const char* detail = "";  // "NASDAQ . 10 held", or why there is no price
  const char* price = "";   // "$227.79", "--" when never fetched
  const char* change = "";  // "+1.32%"
  int8_t trend = 0;
  const stocks::Series* series = nullptr;  // null: nothing to draw
};

struct WorthLine {
  const char* value = "";   // "$2,277.90"
  const char* change = "";  // "+29.50 (+1.31%)"
  int8_t trend = 0;
};

struct ListModel {
  const StockRow* rows = nullptr;  // one page
  int count = 0;
  int total = 0;  // the whole watchlist, for the empty state
  const WorthLine* worth = nullptr;
  int worthCount = 0;
  stocks::Span span = stocks::Span::Today;
  const char* pageLabel = nullptr;  // "1 / 3", or null on one page
  const char* problem = "";         // the first watchlist.txt problem, if any
  bool editing = false;
};

void buildList(toybox::Screen& screen, const ListModel& model);

// How many rows fit on a page with this much above them. The activity pages
// with the same function the screen draws with, so a page turn never skips a
// stock.
int listRowsThatFit(const fui::DrawTarget& target, const fui::DeviceContext& device, int worthCount, bool problem);

// --- One stock -----------------------------------------------------------------

struct Stat {
  const char* label = "";
  const char* value = "";
};

struct DetailModel {
  const char* symbol = "";
  const char* name = "";   // "Apple Inc. . NASDAQ"
  const char* price = "";  // "$227.79"
  const char* move = "";   // "+2.95 (+1.31%)"
  int8_t trend = 0;
  const char* asOf = "";      // "Tue 29, 3:59 PM exchange time"
  const char* position = "";  // "3 / 8"
  const stocks::Series* series = nullptr;
  stocks::Exchange exchange = stocks::Exchange::Nasdaq;
  const Stat* stats = nullptr;
  int statCount = 0;
  const char* holding = "";      // "10 SHARES  $2,277.90", empty when watched
  const char* holdingMove = "";  // "+29.50 over the day"
  int8_t holdingTrend = 0;
  const char* problem = "";  // why there is no series, when there is none
};

void buildDetail(toybox::Screen& screen, const DetailModel& model);

// --- Editing -------------------------------------------------------------------

struct StockMenuModel {
  const char* symbol = "";
  const char* detail = "";  // "NASDAQ . 10 shares"
};

void buildStockMenu(toybox::Screen& screen, const StockMenuModel& model);

// --- Notices ---------------------------------------------------------------------

struct NoticeModel {
  const char* headline = "";
  const char* message = "";
  const char* actionLabel = nullptr;
};

void buildNotice(toybox::Screen& screen, const NoticeModel& model);

}  // namespace stocksui
