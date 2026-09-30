#pragma once

// Stocks: a watchlist from NASDAQ, NYSE, NSE and BSE, what the holdings are
// worth, and each stock's day or last ten days as a chart.
//
// ---------------------------------------------------------------------------
// Three decisions worth knowing.
//
// 1. IT OPENS ON THE CACHE. Every stock's last series is on the card, so the
//    list draws at once and with no radio, and every price carries the
//    exchange time it was true at, marked "saved" until this visit refreshes
//    it. The network is touched when asked, or when there is nothing at all
//    to show.
//
// 2. A REFRESH IS ONE STOCK PER LOOP PASS, with the busy screen repainted
//    between them. A ten-stock refresh is ten TLS handshakes; done in one pass
//    the panel would sit on one frame for twenty seconds, and Back could not
//    stop it.
//
// 3. A FAILED STOCK KEEPS ITS OLD PRICE. One delisted symbol or one timeout
//    does not blank the list: the row keeps its saved series and says why it
//    is old. Only a refresh where nothing at all arrived is a notice.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../activities/Activity.h"
#include "../ui/ToyboxScreen.h"
#include "StocksCore.h"
#include "StocksScreens.h"
#include "StocksStore.h"

class StocksActivity final : public Activity {
 public:
  StocksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Stocks", renderer, mappedInput) {}
  ~StocksActivity() override = default;

  static std::unique_ptr<Activity> create(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase : uint8_t { List, Detail, Busy, Notice, StockMenu };

  void loadSpan(stocks::Span span);
  void setSpan(stocks::Span span);
  void startRefresh(int only);
  void needNetwork();
  void onWifiChosen(bool connected);
  void refreshStep();
  bool refreshBatch();
  void finishRefresh();
  void showNotice(const char* headline, std::string message);
  void back();
  void openStock(int index);
  void moveStock(int delta);
  void turnPage(int delta);
  void askToAdd();
  void askForShares();
  void removeOpen();
  void reloadAfterEdit();
  bool anyValid() const;

  void buildListModel(stocksui::ListModel& model, const freeink::ui::DrawTarget& target,
                      const freeink::ui::DeviceContext& device);
  void buildDetailModel(stocksui::DetailModel& model);

  stocks::Store store_;
  // Index-aligned with store_.holdings(), for the span on screen.
  std::vector<stocks::Series> series_;
  std::vector<uint8_t> fresh_;         // fetched this visit rather than read from the card
  std::vector<std::string> failures_;  // why the last fetch of this stock failed

  Phase phase_ = Phase::List;
  Phase returnPhase_ = Phase::List;
  bool editing_ = false;
  int openIndex_ = -1;
  int listTop_ = 0;
  int listVisible_ = 1;

  // The refresh in progress: stocks [cursor_, end_) are still to fetch.
  bool refreshing_ = false;
  int cursor_ = 0;
  int end_ = 0;
  int fetched_ = 0;
  int done_ = 0;
  int total_ = 0;
  // The one-request fetch has been tried for this refresh; after it, the
  // cursor walks missing_ (the stocks it did not bring back) when useMissing_.
  bool batchTried_ = false;
  bool useMissing_ = false;
  std::vector<int> missing_;
  char busyText_[48] = "";

  std::string noticeHeadline_;
  std::string noticeMessage_;

  // Owned strings, because every model holds pointers rather than copies.
  std::vector<std::string> text_;
  std::vector<stocksui::StockRow> rows_;
  std::vector<stocksui::WorthLine> worth_;
  std::vector<stocksui::Stat> stats_;
  char pageLabel_[24] = "";
  char position_[24] = "";

  toybox::Interactions interactions_;
  bool interactionsReady_ = false;
  // A tap within this window of a screen appearing is answering the screen
  // before it. See InstapaperActivity, which established the number.
  static constexpr uint32_t kSettleMs = 600;
  Phase lastShownPhase_ = Phase::List;
  uint32_t phaseShownAtMs_ = 0;
  bool everShown_ = false;
};
