#pragma once

// Weather: what it is doing outside, in as much detail as a 480px panel and a
// public forecast service can carry.
//
// ---------------------------------------------------------------------------
// Three decisions worth knowing.
//
// 1. IT OPENS ON THE CACHE, NOT ON THE RADIO. Tapping a place parses the last
//    body fetched for it and draws that immediately; the network is only
//    touched when something asks for it. A weather app whose first act is to
//    bring up Wi-Fi is a weather app that takes fifteen seconds to tell you
//    anything, on a device that is usually asleep and often has no network at
//    all. The screen says how old the reading is rather than pretending.
//
// 2. Nothing slow happens on the render path. A network step is REQUESTED by
//    setting step_ and asking for a repaint; the loop performs it on the
//    following pass, once the screen announcing it is already on the panel.
//    Same rule, and the same reason, as Instapaper.
//
// 3. The three views share one reading. NOW, HOURS and WEEK are three
//    presentations of one fetch, so switching between them costs a repaint and
//    never a request.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../activities/Activity.h"
#include "../ui/ToyboxFormat.h"
#include "../ui/ToyboxScreen.h"
#include "WeatherFetch.h"
#include "WeatherScreens.h"
#include "WeatherStore.h"

class WeatherActivity final : public Activity {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}
  ~WeatherActivity() override = default;

  static std::unique_ptr<Activity> create(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase : uint8_t {
    Places,    // the saved places
    Results,   // what the geocoder matched
    Forecast,  // one place, in one of three views
    Busy,      // a network step is about to happen or is happening
    Notice,    // a failure, with what to do about it
    Confirm,   // "remove?", before a place is dropped
  };

  enum class Step : uint8_t { None, Search, Refresh };

  void showPlaces();
  void openPlace(uint32_t id);
  void request(Step next, const char* busyMessage);
  void needNetwork(Step next, const char* busyMessage);
  void onWifiChosen(bool connected);
  void askForPlaceName();
  void doSearch();
  void doRefresh();
  void showNotice(const char* headline, std::string message);
  void askRemove();
  void performRemove();
  void setView(weatherui::View view);
  void turnPage(int delta);

  // Parses the cached body for `id` into reading_. False when there is no
  // usable cache, which is what sends a first open to the network.
  bool loadCached(uint32_t id);

  // Rebuilds the owned row strings for whichever screen is about to draw.
  // Done on the render path because it needs a draw target to fit text to the
  // width the component will really give it.
  void buildPlaceRows(const freeink::ui::DrawTarget& target, const freeink::ui::DeviceContext& device,
                      const freeink::ui::ThemeTokens& tokens);
  void buildResultRows();
  void buildDetails();
  void buildHourRows();
  void buildDayRows();

  static uint32_t nowOrZero();
  // "UPDATED 12 MIN AGO", or "SAVED COPY" when the clock cannot date it.
  std::string stalenessLabel() const;

  weather::Store store_;
  weather::Reading reading_;
  std::vector<weather::Place> results_;

  Phase phase_ = Phase::Places;
  Step step_ = Step::None;
  const char* busyMessage_ = "";
  weatherui::View view_ = weatherui::View::Now;
  uint32_t openId_ = 0;
  std::string query_;

  // Owned strings, because every fui model holds pointers rather than copies.
  std::vector<uint32_t> rowIds_;
  std::vector<std::string> rowLabels_;
  std::vector<std::string> rowSubtitles_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rows_;
  int listTop_ = 0;
  int listVisible_ = 0;
  // Places-list remove mode; see PlacesModel::editing.
  bool placesEditing_ = false;

  std::vector<std::string> detailText_;
  std::vector<weatherui::Detail> details_;
  std::string temperatureText_;
  std::string feelsText_;
  std::string highLowText_;
  std::string headlineText_;
  std::string observedText_;
  std::string stalenessText_;
  std::string placeLabel_;

  std::vector<std::string> hourText_;
  std::vector<weatherui::HourRow> hourRows_;
  std::vector<std::string> dayText_;
  std::vector<weatherui::DayRow> dayRows_;
  int hourTop_ = 0;
  int hourVisible_ = 0;
  static constexpr int kPageLabelCap = 2 * toybox::kIntChars + toybox::literalChars(" / ") + 1;
  char pageLabel_[kPageLabelCap] = "";

  std::string noticeHeadline_;
  std::string noticeMessage_;
  std::string confirmDetail_;

  toybox::Interactions interactions_;
  bool interactionsReady_ = false;

  // A tap arriving within this window of a screen becoming VISIBLE is
  // answering the screen before it. This app has a confirm whose REMOVE sits
  // where nothing destructive was a moment earlier, and a refresh control that
  // spends a network round trip. See InstapaperActivity, which established the
  // mechanism and the number.
  static constexpr uint32_t kSettleMs = 600;
  Phase lastShownPhase_ = Phase::Places;
  weatherui::View lastShownView_ = weatherui::View::Now;
  uint32_t phaseShownAtMs_ = 0;
  bool everShown_ = false;
};
