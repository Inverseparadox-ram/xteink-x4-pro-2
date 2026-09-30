#include "StocksActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>

#include "../../DevMode.h"
#include "../../SilentRestart.h"
#include "../../activities/network/WifiSelectionActivity.h"
#include "../../activities/util/KeyboardEntryActivity.h"
#include "../../components/UITheme.h"
#include "../Shelf.h"
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxText.h"
#include "../ui/ToyboxTheme.h"
#include "StocksFetch.h"

namespace fui = freeink::ui;

namespace {

constexpr const char* kTag = "STOCKS";

// Keeps a string alive for as long as the model that points at it. `text` is
// reserved up front with room for every string a paint makes, so a push never
// reallocates and never moves a string a model already points into; a paint
// that somehow needs more gets an empty field rather than a dangling pointer.
const char* keep(std::vector<std::string>& text, std::string value) {
  if (text.size() >= text.capacity()) {
    LOG_ERR(kTag, "text pool full");
    return "";
  }
  text.push_back(std::move(value));
  return text.back().c_str();
}

std::string sharesText(const double shares) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.6g", shares);
  return text;
}

}  // namespace

std::unique_ptr<Activity> StocksActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return makeUniqueNoThrow<StocksActivity>(renderer, mappedInput);
}

// --- Lifecycle -----------------------------------------------------------

void StocksActivity::onEnter() {
  Activity::onEnter();
  toybox::ensureFonts(renderer);
  store_.load();
  phase_ = Phase::List;
  listTop_ = 0;
  loadSpan(store_.span());
  // Decision 1: a card with nothing saved at all is the one case where the
  // app goes to the network by itself, because otherwise it opens on a list
  // of dashes.
  if (!store_.holdings().empty() && !anyValid()) {
    startRefresh(-1);
    return;
  }
  requestUpdate();
}

void StocksActivity::onExit() {
  Activity::onExit();
  // The radio comes down with the app unless Developer Mode brought it up;
  // same rule, and same reason, as Weather.
  if (WiFi.getMode() != WIFI_MODE_NULL && !devmode::holdsRadio()) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

// --- The span ------------------------------------------------------------

void StocksActivity::loadSpan(const stocks::Span span) {
  const std::vector<stocks::Holding>& holdings = store_.holdings();
  series_.assign(holdings.size(), stocks::Series{});
  fresh_.assign(holdings.size(), 0);
  failures_.assign(holdings.size(), std::string());
  for (size_t i = 0; i < holdings.size(); ++i) {
    if (!store_.cached(holdings[i], span, series_[i])) series_[i] = stocks::Series{};
    series_[i].span = span;
  }
}

bool StocksActivity::anyValid() const {
  for (const stocks::Series& s : series_) {
    if (s.valid()) return true;
  }
  return false;
}

void StocksActivity::setSpan(const stocks::Span span) {
  if (span == store_.span()) {
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    store_.setSpan(span);
    loadSpan(span);
  }
  // Any stock this span has never fetched: fetch them all rather than show a
  // list of dashes, or a total that quietly leaves those stocks out.
  bool missing = false;
  for (const stocks::Series& s : series_) missing = missing || !s.valid();
  if (missing && !store_.holdings().empty()) {
    startRefresh(-1);
    return;
  }
  requestUpdate();
}

// --- Refreshing ----------------------------------------------------------

void StocksActivity::startRefresh(const int only) {
  const int count = static_cast<int>(store_.holdings().size());
  if (count == 0) return;
  {
    RenderLock lock(*this);
    returnPhase_ = (phase_ == Phase::Detail || phase_ == Phase::List) ? phase_ : Phase::List;
    cursor_ = only >= 0 && only < count ? only : 0;
    end_ = only >= 0 && only < count ? only + 1 : count;
    fetched_ = 0;
    done_ = 0;
    total_ = end_ - cursor_;
    // A whole list goes as one request; a single stock is its own request.
    batchTried_ = total_ == 1;
  }
  needNetwork();
}

void StocksActivity::needNetwork() {
  if (WiFi.status() == WL_CONNECTED) {
    RenderLock lock(*this);
    refreshing_ = true;
    phase_ = Phase::Busy;
    if (total_ == 1) {
      std::snprintf(busyText_, sizeof(busyText_), "FETCHING %s",
                    store_.holdings()[static_cast<size_t>(cursor_)].symbol.c_str());
    } else {
      std::snprintf(busyText_, sizeof(busyText_), "FETCHING %d STOCKS", total_);
    }
    requestUpdate();
    return;
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiChosen(!result.isCancelled); });
}

void StocksActivity::onWifiChosen(const bool connected) {
  if (!connected) {
    // They backed out of the picker: whatever was on screen before stays.
    RenderLock lock(*this);
    refreshing_ = false;
    phase_ = returnPhase_;
    requestUpdate();
    return;
  }
  needNetwork();
}

// The whole watchlist in one request, and only what that request did not
// bring back one stock at a time (decision 2's per-stock passes, repainting
// the busy screen between them).
bool StocksActivity::refreshBatch() {
  const std::vector<stocks::Holding>& holdings = store_.holdings();
  batchTried_ = true;
  const size_t from = static_cast<size_t>(cursor_);
  const size_t to = static_cast<size_t>(end_) < holdings.size() ? static_cast<size_t>(end_) : holdings.size();
  const std::vector<stocks::Holding> chunk(holdings.begin() + static_cast<std::ptrdiff_t>(from),
                                           holdings.begin() + static_cast<std::ptrdiff_t>(to));
  std::vector<stocks::Series> results;
  std::vector<uint8_t> got;
  std::string message;
  const stocks::Span span = store_.span();
  if (!stocks::fetchBatch(chunk, span, results, got, message)) {
    LOG_ERR(kTag, "batch failed, fetching one at a time: %s", message.c_str());
    return false;
  }
  missing_.clear();
  for (size_t k = 0; k < chunk.size(); ++k) {
    const size_t i = from + k;
    if (!got[k]) {
      missing_.push_back(static_cast<int>(i));
      continue;
    }
    store_.writeCache(holdings[i], results[k]);
    RenderLock lock(*this);
    series_[i] = std::move(results[k]);
    fresh_[i] = 1;
    failures_[i].clear();
    ++fetched_;
  }
  RenderLock lock(*this);
  // Only the stocks the batch did not carry go through the one-at-a-time
  // path below; the cursor walks that list instead of the whole range.
  cursor_ = 0;
  end_ = static_cast<int>(missing_.size());
  done_ = 0;
  total_ = end_;
  if (end_ > 0) {
    std::snprintf(busyText_, sizeof(busyText_), "FETCHING %s",
                  holdings[static_cast<size_t>(missing_[0])].symbol.c_str());
  }
  requestUpdate();
  return true;
}

void StocksActivity::refreshStep() {
  const std::vector<stocks::Holding>& holdings = store_.holdings();
  if (!batchTried_) {
    if (refreshBatch()) {
      useMissing_ = true;
      return;
    }
  }
  if (cursor_ >= end_) {
    finishRefresh();
    return;
  }
  const int index = useMissing_ ? missing_[static_cast<size_t>(cursor_)] : cursor_;
  if (index >= static_cast<int>(holdings.size())) {
    finishRefresh();
    return;
  }
  const size_t i = static_cast<size_t>(index);
  stocks::Series fresh;
  std::string message;
  const stocks::Span span = store_.span();
  if (stocks::fetchSeries(holdings[i], span, fresh, message)) {
    store_.writeCache(holdings[i], fresh);
    RenderLock lock(*this);
    series_[i] = std::move(fresh);
    fresh_[i] = 1;
    failures_[i].clear();
    ++fetched_;
  } else {
    LOG_ERR(kTag, "%s: %s", holdings[i].symbol.c_str(), message.c_str());
    RenderLock lock(*this);
    failures_[i] = message;
  }
  RenderLock lock(*this);
  ++cursor_;
  ++done_;
  if (cursor_ < end_) {
    const int next = useMissing_ ? missing_[static_cast<size_t>(cursor_)] : cursor_;
    std::snprintf(busyText_, sizeof(busyText_), "FETCHING %s", holdings[static_cast<size_t>(next)].symbol.c_str());
  }
  requestUpdate();
}

void StocksActivity::finishRefresh() {
  refreshing_ = false;
  useMissing_ = false;
  if (fetched_ == 0) {
    // Decision 3: only a refresh where nothing arrived is a notice.
    std::string reason;
    for (size_t i = failures_.size(); i > 0 && reason.empty(); --i) reason = failures_[i - 1];
    showNotice("NO PRICES", reason.empty() ? std::string("Nothing came back. Try again in a moment.") : reason);
    requestUpdate();
    return;
  }
  RenderLock lock(*this);
  phase_ = returnPhase_;
  requestUpdate();
}

void StocksActivity::showNotice(const char* headline, std::string message) {
  RenderLock lock(*this);
  noticeHeadline_ = headline;
  noticeMessage_ = std::move(message);
  phase_ = Phase::Notice;
}

// --- Navigation ----------------------------------------------------------

void StocksActivity::back() {
  switch (phase_) {
    case Phase::List:
      if (editing_) {
        RenderLock lock(*this);
        editing_ = false;
        break;
      }
      // The shelf puts Back where the app was opened from; on the X4 Pro this
      // is the only way out.
      shelf::leave(renderer, mappedInput);
      return;
    case Phase::Detail:
    case Phase::StockMenu:
    case Phase::Notice: {
      RenderLock lock(*this);
      phase_ = Phase::List;
      break;
    }
    case Phase::Busy: {
      // Stops the refresh after the stock in flight; what arrived is kept.
      RenderLock lock(*this);
      refreshing_ = false;
      phase_ = returnPhase_;
      break;
    }
  }
  requestUpdate();
}

void StocksActivity::openStock(const int index) {
  if (index < 0 || index >= static_cast<int>(store_.holdings().size())) return;
  RenderLock lock(*this);
  openIndex_ = index;
  phase_ = editing_ ? Phase::StockMenu : Phase::Detail;
  requestUpdate();
}

void StocksActivity::moveStock(const int delta) {
  const int count = static_cast<int>(store_.holdings().size());
  if (count <= 1 || phase_ != Phase::Detail) return;
  // Wraps: a key that stops working at the end of the list reads as broken.
  RenderLock lock(*this);
  openIndex_ = (openIndex_ + delta + count) % count;
  requestUpdate();
}

void StocksActivity::turnPage(const int delta) {
  const int count = static_cast<int>(store_.holdings().size());
  if (listVisible_ <= 0 || count <= listVisible_) return;
  const int pages = (count + listVisible_ - 1) / listVisible_;
  const int page = listTop_ / listVisible_;
  RenderLock lock(*this);
  listTop_ = ((page + (delta > 0 ? 1 : pages - 1)) % pages) * listVisible_;
  requestUpdate();
}

// --- Editing -------------------------------------------------------------

void StocksActivity::reloadAfterEdit() {
  RenderLock lock(*this);
  loadSpan(store_.span());
  const int count = static_cast<int>(store_.holdings().size());
  if (listTop_ >= count) listTop_ = 0;
  if (openIndex_ >= count) openIndex_ = count - 1;
}

void StocksActivity::askToAdd() {
  // One line in the watchlist's own format, so what is typed here and what is
  // written in the file on a computer are the same thing.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, "SYMBOL EXCHANGE SHARES",
                                                           std::string(), 40, InputType::Text);
  if (!keyboard) {
    LOG_ERR(kTag, "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const auto& typed = std::get<KeyboardResult>(result.data);
    std::vector<stocks::Holding> parsed;
    std::vector<stocks::WatchlistProblem> problems;
    stocks::parseWatchlist(typed.text, parsed, problems);
    if (parsed.size() != 1) {
      showNotice("NOT ADDED", problems.empty() ? std::string("Type one line, like: TCS NSE 5")
                                               : problems.front().text + ". Type one line, like: TCS NSE 5");
      requestUpdate();
      return;
    }
    if (store_.holdings().size() >= stocks::kMaxHoldings) {
      showNotice("WATCHLIST IS FULL", "Remove a stock before adding another.");
      requestUpdate();
      return;
    }
    if (!store_.put(parsed.front())) {
      showNotice("NOT ADDED", "The card could not be written. Check it is in and not locked.");
      requestUpdate();
      return;
    }
    reloadAfterEdit();
    int added = -1;
    for (size_t i = 0; i < store_.holdings().size(); ++i) {
      const stocks::Holding& h = store_.holdings()[i];
      if (h.symbol == parsed.front().symbol && h.exchange == parsed.front().exchange) added = static_cast<int>(i);
    }
    {
      RenderLock lock(*this);
      editing_ = false;
      phase_ = Phase::List;
    }
    // Only the stock just added is fetched, which is also the check that the
    // symbol exists: a typo comes back as Yahoo's own "No data found".
    if (added >= 0) {
      startRefresh(added);
    } else {
      requestUpdate();
    }
  });
}

void StocksActivity::askForShares() {
  if (openIndex_ < 0 || openIndex_ >= static_cast<int>(store_.holdings().size())) return;
  const stocks::Holding holding = store_.holdings()[static_cast<size_t>(openIndex_)];
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, "SHARES HELD (0 TO JUST WATCH)",
                                                           holding.shares > 0 ? sharesText(holding.shares) : "0", 16,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR(kTag, "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, holding](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const auto& typed = std::get<KeyboardResult>(result.data);
    // Read through the watchlist parser, so the device accepts exactly what
    // the file does and no second number reader exists to disagree with it.
    std::vector<stocks::Holding> parsed;
    std::vector<stocks::WatchlistProblem> problems;
    const std::string line = holding.symbol + " " + stocks::exchangeName(holding.exchange) + " " + typed.text;
    stocks::parseWatchlist(line, parsed, problems);
    if (parsed.size() != 1) {
      showNotice("NOT CHANGED", "Shares must be a number, like 10 or 2.5.");
      requestUpdate();
      return;
    }
    if (!store_.put(parsed.front())) {
      showNotice("NOT CHANGED", "The card could not be written. Check it is in and not locked.");
      requestUpdate();
      return;
    }
    reloadAfterEdit();
    RenderLock lock(*this);
    phase_ = Phase::List;
    requestUpdate();
  });
}

void StocksActivity::removeOpen() {
  if (openIndex_ < 0 || openIndex_ >= static_cast<int>(store_.holdings().size())) return;
  const stocks::Holding holding = store_.holdings()[static_cast<size_t>(openIndex_)];
  if (!store_.remove(holding)) {
    showNotice("NOT REMOVED", "The card could not be written. Check it is in and not locked.");
    requestUpdate();
    return;
  }
  reloadAfterEdit();
  RenderLock lock(*this);
  phase_ = Phase::List;
  openIndex_ = -1;
  requestUpdate();
}

// --- Input ---------------------------------------------------------------

void StocksActivity::loop() {
  if (phase_ == Phase::Busy && refreshing_) {
    refreshStep();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    back();
    return;
  }

  // The side keys page the list and step through stocks on a stock.
  const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
  if (next || prev) {
    if (phase_ == Phase::List) turnPage(next ? 1 : -1);
    if (phase_ == Phase::Detail) moveStock(next ? 1 : -1);
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (!mappedInput.wasScreenTapped(tapX, tapY) || !interactionsReady_) return;
  if (everShown_ && millis() - phaseShownAtMs_ < kSettleMs) return;

  fui::InputSnapshot input;
  input.touchReleased = true;
  input.touchX = static_cast<int16_t>(tapX);
  input.touchY = static_cast<int16_t>(tapY);
  const fui::ActionEvent event = interactions_.route(input);

  switch (event.action) {
    case stocksui::ActionOpenStock:
    case stocksui::ActionEditStock:
      openStock(listTop_ + event.value);
      break;
    case stocksui::ActionRefresh:
      startRefresh(phase_ == Phase::Detail ? openIndex_ : -1);
      break;
    case stocksui::ActionSpanToday:
      setSpan(stocks::Span::Today);
      break;
    case stocksui::ActionSpanDays:
      setSpan(stocks::Span::Days);
      break;
    case stocksui::ActionEdit: {
      RenderLock lock(*this);
      editing_ = !editing_;
      requestUpdate();
      break;
    }
    case stocksui::ActionAdd:
      askToAdd();
      break;
    case stocksui::ActionChangeShares:
      askForShares();
      break;
    case stocksui::ActionRemoveStock:
      removeOpen();
      break;
    case stocksui::ActionKeepStock:
    case stocksui::ActionNotice: {
      RenderLock lock(*this);
      phase_ = Phase::List;
      requestUpdate();
      break;
    }
    case stocksui::ActionPrevStock:
      moveStock(-1);
      break;
    case stocksui::ActionNextStock:
      moveStock(1);
      break;
    default:
      break;
  }
}

// --- Models --------------------------------------------------------------

void StocksActivity::buildListModel(stocksui::ListModel& model, const fui::DrawTarget& target,
                                    const fui::DeviceContext& device) {
  const std::vector<stocks::Holding>& holdings = store_.holdings();
  const stocks::Span span = store_.span();

  std::vector<const stocks::Series*> pointers;
  pointers.reserve(series_.size());
  for (const stocks::Series& s : series_) pointers.push_back(&s);
  const std::vector<stocks::Worth> worth =
      editing_ ? std::vector<stocks::Worth>() : stocks::worthByCurrency(holdings, pointers);
  worth_.clear();
  worth_.reserve(worth.size());
  for (const stocks::Worth& w : worth) {
    stocks::Move move;
    move.to = w.value;
    move.from = w.value - w.change;
    move.delta = w.change;
    move.percent = move.from > 0 ? w.change / move.from * 100.0 : 0.0;
    stocksui::WorthLine line;
    line.value = keep(text_, stocks::formatMoney(w.value, w.currency));
    line.change = keep(text_, stocks::formatMove(move, w.currency));
    line.trend = stocksui::trendOf(w.change);
    worth_.push_back(line);
  }

  const std::vector<stocks::WatchlistProblem>& problems = store_.problems();
  std::string problem;
  if (!problems.empty()) {
    char text[160];
    std::snprintf(text, sizeof(text), "watchlist.txt line %d: %s%s", problems.front().line,
                  problems.front().text.c_str(), problems.size() > 1 ? " (and more)" : "");
    problem = text;
  }

  listVisible_ = stocksui::listRowsThatFit(target, device, static_cast<int>(worth_.size()), !problem.empty());
  const int count = static_cast<int>(holdings.size());
  if (listTop_ >= count) listTop_ = 0;
  const int shown = count - listTop_ < listVisible_ ? count - listTop_ : listVisible_;
  rows_.clear();
  rows_.reserve(static_cast<size_t>(shown > 0 ? shown : 0));
  for (int k = 0; k < shown; ++k) {
    const size_t i = static_cast<size_t>(listTop_ + k);
    const stocks::Holding& h = holdings[i];
    const stocks::Series& s = series_[i];
    stocksui::StockRow row;
    row.symbol = h.symbol.c_str();
    std::string detail = stocks::exchangeName(h.exchange);
    if (editing_) {
      detail += h.shares > 0 ? "  .  " + sharesText(h.shares) + " shares" : std::string("  .  watching");
      row.price = "";
    } else if (s.valid()) {
      if (h.shares > 0) detail += "  .  " + sharesText(h.shares) + " held";
      const stocks::Move move = stocks::moveOf(s);
      row.price = keep(text_, stocks::formatMoney(s.price, s.currency));
      row.change = keep(text_, stocks::formatPercent(move.percent));
      row.trend = stocksui::trendOf(move.delta);
      row.series = &s;
    } else {
      detail = failures_[i].empty() ? detail + "  .  no price yet" : detail + "  .  not found";
      row.price = "--";
    }
    row.detail = keep(text_, detail);
    rows_.push_back(row);
  }

  model.rows = rows_.empty() ? nullptr : rows_.data();
  model.count = static_cast<int>(rows_.size());
  model.total = count;
  model.worth = worth_.empty() ? nullptr : worth_.data();
  model.worthCount = static_cast<int>(worth_.size());
  model.span = span;
  model.problem = keep(text_, problem);
  model.editing = editing_;
  if (count > listVisible_) {
    std::snprintf(pageLabel_, sizeof(pageLabel_), "%d / %d", listTop_ / listVisible_ + 1,
                  (count + listVisible_ - 1) / listVisible_);
    model.pageLabel = pageLabel_;
  }
}

void StocksActivity::buildDetailModel(stocksui::DetailModel& model) {
  const stocks::Holding& h = store_.holdings()[static_cast<size_t>(openIndex_)];
  const stocks::Series& s = series_[static_cast<size_t>(openIndex_)];
  const stocks::Span span = store_.span();
  const int count = static_cast<int>(store_.holdings().size());

  model.symbol = h.symbol.c_str();
  model.exchange = h.exchange;
  std::string name = s.name.empty() ? std::string() : s.name + "  .  ";
  name += stocks::exchangeName(h.exchange);
  model.name = keep(text_, name);
  if (count > 1) {
    std::snprintf(position_, sizeof(position_), "%d / %d", openIndex_ + 1, count);
    model.position = position_;
  }
  model.series = &s;
  model.problem = keep(text_, failures_[static_cast<size_t>(openIndex_)]);
  if (!s.valid()) return;

  const stocks::Move move = stocks::moveOf(s);
  model.price = keep(text_, stocks::formatMoney(s.price, s.currency));
  model.move =
      keep(text_, stocks::formatMove(move, s.currency) + (span == stocks::Span::Today ? "  today" : "  in 10 days"));
  model.trend = stocksui::trendOf(move.delta);
  // Decision 1: every price with the exchange time it was true at, and
  // "saved" until this visit has fetched it.
  std::string asOf = stocks::formatExchangeDay(s.marketTime, s.gmtOffset) + ", " +
                     stocks::formatExchangeTime(s.marketTime, s.gmtOffset) + " exchange time";
  if (!fresh_[static_cast<size_t>(openIndex_)]) asOf += "  .  saved";
  model.asOf = keep(text_, asOf);

  stats_.clear();
  stats_.reserve(4);
  const stocks::Extremes e = stocks::extremesOf(s);
  // The day's four numbers. Ten days carry their closes in a table instead.
  if (span == stocks::Span::Today) {
    stats_.push_back(stocksui::Stat{"OPEN", keep(text_, stocks::formatAmount(e.open, s.currency))});
    stats_.push_back(stocksui::Stat{"HIGH", keep(text_, stocks::formatAmount(e.high, s.currency))});
    stats_.push_back(stocksui::Stat{"LOW", keep(text_, stocks::formatAmount(e.low, s.currency))});
    stats_.push_back(stocksui::Stat{"PREV CLOSE", keep(text_, stocks::formatAmount(s.previousClose, s.currency))});
  }
  model.stats = stats_.data();
  model.statCount = static_cast<int>(stats_.size());

  if (h.shares > 0) {
    const std::string held = sharesText(h.shares) + (h.shares == 1 ? " SHARE  " : " SHARES  ") +
                             stocks::formatMoney(h.shares * s.price, s.currency);
    model.holding = keep(text_, held);
    stocks::Move worth;
    worth.from = h.shares * (s.price - move.delta);
    worth.to = h.shares * s.price;
    worth.delta = h.shares * move.delta;
    worth.percent = move.percent;
    model.holdingMove =
        keep(text_, stocks::formatMove(worth, s.currency) + (span == stocks::Span::Today ? " today" : " in 10 days"));
    model.holdingTrend = stocksui::trendOf(worth.delta);
  }
}

// --- Drawing -------------------------------------------------------------

void StocksActivity::render(RenderLock&&) {
  renderer.clearScreen();
  fui::GfxRendererTarget target = toybox::makeTarget(renderer, toybox::readingFaces());
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};
  interactionsReady_ = false;
  toybox::Frame frame(target, device, noInput, interactions_);
  toybox::Screen screen(frame);

  // Room for every string one paint makes: four per row, two per currency,
  // and a handful for the detail screen and its stats.
  text_.clear();
  text_.reserve(static_cast<size_t>(4 * stocks::kMaxHoldings + 32));

  const char* what = "Stocks";
  if (phase_ == Phase::Detail || phase_ == Phase::StockMenu) {
    if (openIndex_ < 0 || openIndex_ >= static_cast<int>(store_.holdings().size())) phase_ = Phase::List;
  }

  switch (phase_) {
    case Phase::List: {
      stocksui::ListModel model;
      buildListModel(model, target, device);
      stocksui::buildList(screen, model);
      what = editing_ ? "Stocks edit" : "Stocks list";
      break;
    }
    case Phase::Detail: {
      stocksui::DetailModel model;
      buildDetailModel(model);
      stocksui::buildDetail(screen, model);
      what = "Stocks detail";
      break;
    }
    case Phase::StockMenu: {
      const stocks::Holding& h = store_.holdings()[static_cast<size_t>(openIndex_)];
      stocksui::StockMenuModel model;
      model.symbol = h.symbol.c_str();
      model.detail = keep(text_, std::string(stocks::exchangeName(h.exchange)) + "  .  " +
                                     (h.shares > 0 ? sharesText(h.shares) + " shares" : std::string("watching")));
      stocksui::buildStockMenu(screen, model);
      what = "Stocks menu";
      break;
    }
    case Phase::Busy: {
      stocksui::NoticeModel model;
      model.headline = busyText_;
      model.message = "Prices come from Yahoo Finance and can be up to fifteen minutes behind the exchange.";
      stocksui::buildNotice(screen, model);
      what = "Stocks busy";
      break;
    }
    case Phase::Notice: {
      stocksui::NoticeModel model;
      model.headline = noticeHeadline_.c_str();
      model.message = noticeMessage_.c_str();
      model.actionLabel = "BACK TO THE LIST";
      stocksui::buildNotice(screen, model);
      what = "Stocks notice";
      break;
    }
  }

  interactionsReady_ = true;
  toybox::reportOverflow(interactions_, what);
  const bool phaseChanged = !everShown_ || phase_ != lastShownPhase_;

  const auto labels = mappedInput.mapLabels("Back", "", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  if (phaseChanged) {
    lastShownPhase_ = phase_;
    phaseShownAtMs_ = millis();
    everShown_ = true;
  }
}
