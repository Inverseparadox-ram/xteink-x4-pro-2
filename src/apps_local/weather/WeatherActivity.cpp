#include "WeatherActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>
#include <ctime>

#include "../../DevMode.h"
#include "../../SilentRestart.h"
#include "../../activities/network/WifiSelectionActivity.h"
#include "../../activities/util/KeyboardEntryActivity.h"
#include "../../components/UITheme.h"
#include "../Shelf.h"
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxText.h"
#include "../ui/ToyboxTheme.h"

namespace fui = freeink::ui;

namespace {
// Below this the clock has never been set. 2020-01-01, the same floor the rest
// of the fork uses.
constexpr int64_t kClockFloor = 1577836800;
}  // namespace

std::unique_ptr<Activity> WeatherActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return makeUniqueNoThrow<WeatherActivity>(renderer, mappedInput);
}

uint32_t WeatherActivity::nowOrZero() {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  return now > kClockFloor ? static_cast<uint32_t>(now) : 0u;
}

// --- Lifecycle -----------------------------------------------------------

void WeatherActivity::onEnter() {
  Activity::onEnter();
  toybox::ensureFonts(renderer);
  store_.load();
  phase_ = Phase::Places;
  listTop_ = 0;
  LOG_INF("WEATHER", "opened: %d places", static_cast<int>(store_.places().size()));
  requestUpdate();
}

void WeatherActivity::onExit() {
  Activity::onExit();
  // The radio comes down with the app, unless Developer Mode brought it up --
  // silentRestart() reboots, and doing that whenever this app closes while dev
  // mode is on is indistinguishable from a crash. Same rule as Instapaper.
  if (WiFi.getMode() != WIFI_MODE_NULL && !devmode::holdsRadio()) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

// --- Navigation ----------------------------------------------------------

void WeatherActivity::showPlaces() {
  RenderLock lock(*this);
  phase_ = Phase::Places;
  openId_ = 0;
  placesEditing_ = false;
  results_.clear();
  reading_ = weather::Reading{};
}

bool WeatherActivity::loadCached(const uint32_t id) {
  const std::string body = store_.cachedBody(id);
  if (body.empty()) return false;
  std::string ignored;
  if (!weather::parseForecast(body, reading_, ignored)) {
    LOG_ERR("WEATHER", "cached forecast for %lu did not parse", static_cast<unsigned long>(id));
    return false;
  }
  reading_.placeId = id;
  return true;
}

void WeatherActivity::openPlace(const uint32_t id) {
  const weather::Place* place = store_.find(id);
  if (place == nullptr) return;
  bool cached = false;
  {
    RenderLock lock(*this);
    openId_ = id;
    view_ = weatherui::View::Now;
    hourTop_ = 0;
    reading_ = weather::Reading{};
    // Decision 1: the cache first, so the app says something the instant it is
    // opened. The fetch is what the refresh control is for.
    cached = loadCached(id);
    if (cached) phase_ = Phase::Forecast;
  }
  if (cached) {
    requestUpdate();
    return;
  }
  needNetwork(Step::Refresh, "FETCHING THE FORECAST");
}

// --- Scheduling the slow parts -------------------------------------------

void WeatherActivity::request(const Step next, const char* busyMessage) {
  {
    RenderLock lock(*this);
    phase_ = Phase::Busy;
    busyMessage_ = busyMessage;
    step_ = next;
  }
  // Paint the busy screen now; the work happens on the next loop pass, so the
  // panel is never blank while the radio works.
  requestUpdate();
}

void WeatherActivity::needNetwork(const Step next, const char* busyMessage) {
  if (WiFi.status() == WL_CONNECTED) {
    request(next, busyMessage);
    return;
  }
  // The picker is a child activity and its result decides whether the step
  // happens at all. Requesting first and connecting after would run it against
  // a radio that is not up.
  step_ = next;
  busyMessage_ = busyMessage;
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiChosen(!result.isCancelled); });
}

void WeatherActivity::onWifiChosen(const bool connected) {
  if (!connected) {
    // They backed out of the picker, so they did not want the network. Where
    // that leaves them depends on whether there is anything to look at: a
    // place with a cached forecast still shows it.
    const Step wanted = step_;
    step_ = Step::None;
    if (wanted == Step::Refresh && openId_ != 0 && reading_.valid()) {
      RenderLock lock(*this);
      phase_ = Phase::Forecast;
    } else {
      showPlaces();
    }
    requestUpdate();
    return;
  }
  request(step_, busyMessage_);
}

// --- The two network steps -----------------------------------------------

void WeatherActivity::doSearch() {
  std::string message;
  std::vector<weather::Place> found;
  if (!weather::searchPlaces(query_, found, message)) {
    showNotice("NO LUCK", message);
    return;
  }
  RenderLock lock(*this);
  results_ = std::move(found);
  phase_ = Phase::Results;
}

void WeatherActivity::doRefresh() {
  const weather::Place* place = store_.find(openId_);
  if (place == nullptr) {
    showPlaces();
    return;
  }
  weather::Reading fresh;
  std::string body;
  std::string message;
  if (!weather::fetchForecast(*place, fresh, message, &body)) {
    // A failed refresh does not throw away a forecast that is merely old: if
    // there is something on screen it stays, and the notice offers the way
    // back to it.
    showNotice("NO FORECAST", message);
    return;
  }
  fresh.fetchedAt = nowOrZero();
  {
    RenderLock lock(*this);
    reading_ = std::move(fresh);
    phase_ = Phase::Forecast;
  }
  store_.writeCache(openId_, body);
  if (weather::Place* saved = store_.find(openId_)) {
    // The forecast knows the place's timezone and elevation better than the
    // geocoder did, so the saved place learns them from the reading.
    if (!reading_.timezone.empty()) saved->timezone = reading_.timezone;
    if (reading_.elevation.has) saved->elevation = reading_.elevation;
    store_.writeExport(*saved, reading_);
    store_.save();
  }
}

// --- Typing --------------------------------------------------------------

void WeatherActivity::askForPlaceName() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, "PLACE NAME", std::string(),
                                                           weather::kMaxNameChars, InputType::Text);
  if (!keyboard) {
    LOG_ERR("WEATHER", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const auto& typed = std::get<KeyboardResult>(result.data);
    query_ = weather::sanitize(typed.text, weather::kMaxNameChars);
    if (query_.empty()) {
      requestUpdate();
      return;
    }
    needNetwork(Step::Search, "SEARCHING FOR THAT PLACE");
  });
}

// --- Notices and removal -------------------------------------------------

void WeatherActivity::showNotice(const char* headline, std::string message) {
  RenderLock lock(*this);
  noticeHeadline_ = headline;
  noticeMessage_ = std::move(message);
  phase_ = Phase::Notice;
}

void WeatherActivity::askRemove() {
  const weather::Place* place = store_.find(openId_);
  if (place == nullptr) {
    showPlaces();
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    placeLabel_ = weather::describePlace(*place);
    confirmDetail_ = "The saved forecast for this place is deleted from the card. Nothing else is touched.";
    phase_ = Phase::Confirm;
  }
  requestUpdate();
}

void WeatherActivity::performRemove() {
  const uint32_t id = openId_;
  store_.remove(id);
  store_.save();
  showPlaces();
  requestUpdate();
}

void WeatherActivity::setView(const weatherui::View view) {
  RenderLock lock(*this);
  view_ = view;
  if (view == weatherui::View::Hours) hourTop_ = 0;
  requestUpdate();
}

void WeatherActivity::turnPage(const int delta) {
  if (view_ != weatherui::View::Hours || hourVisible_ <= 0) return;
  const int count = static_cast<int>(reading_.hours.size());
  if (count <= hourVisible_) return;
  const int pages = (count + hourVisible_ - 1) / hourVisible_;
  const int page = hourTop_ / hourVisible_;
  // Wraps: a page key that stops working at the last page reads as broken.
  RenderLock lock(*this);
  hourTop_ = ((page + (delta > 0 ? 1 : pages - 1)) % pages) * hourVisible_;
  requestUpdate();
}

std::string WeatherActivity::stalenessLabel() const {
  const uint32_t now = nowOrZero();
  // Without a clock there is no honest age to state, and "0 MIN AGO" would be
  // a lie about a forecast that could be days old.
  if (reading_.fetchedAt == 0 || now == 0 || now < reading_.fetchedAt) return "SAVED COPY";
  const uint32_t age = now - reading_.fetchedAt;
  char label[32];
  if (age < 90) {
    std::snprintf(label, sizeof(label), "JUST NOW");
  } else if (age < 3600) {
    std::snprintf(label, sizeof(label), "%u MIN AGO", static_cast<unsigned>(age / 60));
  } else if (age < 86400) {
    std::snprintf(label, sizeof(label), "%u HR AGO", static_cast<unsigned>(age / 3600));
  } else {
    std::snprintf(label, sizeof(label), "%u DAYS AGO", static_cast<unsigned>(age / 86400));
  }
  return label;
}

// --- Input ---------------------------------------------------------------

void WeatherActivity::loop() {
  // The deferred step, one pass after the screen that announced it.
  if (step_ != Step::None && phase_ == Phase::Busy) {
    const Step what = step_;
    step_ = Step::None;
    switch (what) {
      case Step::Search:
        doSearch();
        break;
      case Step::Refresh:
        doRefresh();
        break;
      case Step::None:
        break;
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    switch (phase_) {
      case Phase::Places:
        // An app never names where Back goes; the shelf puts it back in
        // whichever folder opened it. On the X4 Pro this is the only way out.
        shelf::leave(renderer, mappedInput);
        return;
      case Phase::Forecast:
      case Phase::Results:
      case Phase::Busy:
        showPlaces();
        break;
      case Phase::Notice:
        // Back out of a failure to whatever there was before it: a place with
        // a forecast still has one.
        if (openId_ != 0 && reading_.valid()) {
          RenderLock lock(*this);
          phase_ = Phase::Forecast;
        } else {
          showPlaces();
        }
        break;
      case Phase::Confirm:
        // Back is the safe answer to "remove?", exactly like KEEP IT. It
        // returns where the question was asked from: the places list when the
        // mode asked it, the forecast when the forecast did.
        {
          RenderLock lock(*this);
          phase_ = placesEditing_ ? Phase::Places : Phase::Forecast;
          if (placesEditing_) openId_ = 0;
        }
        break;
    }
    requestUpdate();
    return;
  }

  // The two side keys page whatever is paged: the places list, and the hours.
  const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
  if (next || prev) {
    if (phase_ == Phase::Places && listVisible_ > 0) {
      const int count = static_cast<int>(rowIds_.size());
      if (count > listVisible_) {
        const int pages = (count + listVisible_ - 1) / listVisible_;
        const int page = listTop_ / listVisible_;
        listTop_ = ((page + (next ? 1 : pages - 1)) % pages) * listVisible_;
        requestUpdate();
      }
      return;
    }
    if (phase_ == Phase::Forecast) turnPage(next ? 1 : -1);
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (!mappedInput.wasScreenTapped(tapX, tapY) || !interactionsReady_) return;

  // A tap within kSettleMs of this screen appearing is answering the previous
  // one. Dropped rather than queued.
  if (everShown_ && millis() - phaseShownAtMs_ < kSettleMs) return;

  fui::InputSnapshot input;
  input.touchReleased = true;
  input.touchX = static_cast<int16_t>(tapX);
  input.touchY = static_cast<int16_t>(tapY);
  const fui::ActionEvent event = interactions_.route(input);

  switch (event.action) {
    case weatherui::ActionOpenPlace:
      if (event.value >= 0 && event.value < static_cast<int>(rowIds_.size())) {
        openPlace(rowIds_[static_cast<size_t>(event.value)]);
      }
      break;
    case weatherui::ActionAddPlace:
      askForPlaceName();
      break;
    case weatherui::ActionSearchResult: {
      if (event.value < 0 || event.value >= static_cast<int>(results_.size())) break;
      const weather::Place& chosen = results_[static_cast<size_t>(event.value)];
      if (store_.alreadySaved(chosen)) {
        // Already on the list. Open the one that is there rather than making a
        // second row that will always agree with the first.
        for (const weather::Place& saved : store_.places()) {
          if (store_.alreadySaved(chosen) && weather::describePlace(saved) == weather::describePlace(chosen)) {
            openPlace(saved.id);
            return;
          }
        }
      }
      if (store_.places().size() >= weather::kMaxPlaces) {
        showNotice("THAT IS ENOUGH PLACES",
                   "Remove one before adding another. Open a place and use the header button.");
        requestUpdate();
        break;
      }
      const uint32_t id = store_.add(chosen).id;
      store_.save();
      openPlace(id);
      return;
    }
    case weatherui::ActionRefresh:
      if (openId_ != 0) needNetwork(Step::Refresh, "FETCHING THE FORECAST");
      break;
    case weatherui::ActionViewNow:
      setView(weatherui::View::Now);
      break;
    case weatherui::ActionViewHours:
      setView(weatherui::View::Hours);
      break;
    case weatherui::ActionViewWeek:
      setView(weatherui::View::Week);
      break;
    case weatherui::ActionEditPlaces: {
      RenderLock lock(*this);
      placesEditing_ = !placesEditing_;
      requestUpdate();
      break;
    }
    case weatherui::ActionRemovePlace:
      // Only reachable from the places list in remove mode, where the row
      // carries which place. The confirm is what actually destroys anything.
      if (event.value >= 0 && event.value < static_cast<int>(rowIds_.size())) {
        openId_ = rowIds_[static_cast<size_t>(event.value)];
        askRemove();
      }
      break;
    case weatherui::ActionRemoveConfirm:
      performRemove();
      break;
    case weatherui::ActionRemoveCancel: {
      RenderLock lock(*this);
      phase_ = placesEditing_ ? Phase::Places : Phase::Forecast;
      if (placesEditing_) openId_ = 0;
      requestUpdate();
      break;
    }
    case weatherui::ActionNotice:
      if (openId_ != 0 && reading_.valid()) {
        RenderLock lock(*this);
        phase_ = Phase::Forecast;
      } else {
        showPlaces();
      }
      requestUpdate();
      break;
    case weatherui::ActionPagePrev:
      turnPage(-1);
      break;
    case weatherui::ActionPageNext:
      turnPage(1);
      break;
    default:
      break;
  }
}

// --- Building the rows ---------------------------------------------------

void WeatherActivity::buildPlaceRows(const fui::DrawTarget& target, const fui::DeviceContext& device,
                                     const fui::ThemeTokens& tokens) {
  const std::vector<weather::Place>& all = store_.places();
  rowIds_.clear();
  rowLabels_.clear();
  rowSubtitles_.clear();
  rowValues_.clear();
  rows_.clear();

  const int16_t rowHeight = weatherui::placesRowHeight(target, tokens);
  listVisible_ = fui::listVisibleRows(weatherui::placesBand(device), rowHeight, tokens.listRowGap);
  if (listVisible_ <= 0) listVisible_ = 1;
  const int count = static_cast<int>(all.size());
  if (listTop_ >= count) listTop_ = 0;
  const int shown = count - listTop_ < listVisible_ ? count - listTop_ : listVisible_;
  if (shown <= 0) return;

  fui::TextStyle titleStyle = tokens.bodyText;
  titleStyle.maxLines = 1;
  const int16_t titleWidth = static_cast<int16_t>(weatherui::placesBand(device).width - 2 * tokens.listSidePadding);

  rowIds_.reserve(static_cast<size_t>(shown));
  rowLabels_.reserve(static_cast<size_t>(shown));
  rowSubtitles_.reserve(static_cast<size_t>(shown));
  for (int i = 0; i < shown; ++i) {
    const weather::Place& place = all[static_cast<size_t>(listTop_ + i)];
    rowIds_.push_back(place.id);
    rowLabels_.push_back(toybox::fitLines(target, place.name.c_str(), titleWidth, 1, titleStyle));
    // The subtitle names where the place IS, which is the whole reason a
    // geocoder gives several answers for one name.
    std::string where = place.region;
    if (!place.country.empty() && place.country != place.region) {
      if (!where.empty()) where += ", ";
      where += place.country;
    }
    if (where.empty()) {
      char coords[48];
      std::snprintf(coords, sizeof(coords), "%.2f, %.2f", static_cast<double>(place.latitude),
                    static_cast<double>(place.longitude));
      where = coords;
    }
    rowSubtitles_.push_back(where);
  }
  // A second pass, because a push_back can reallocate and ListItem holds
  // pointers rather than copies.
  rows_.reserve(rowLabels_.size());
  for (size_t i = 0; i < rowLabels_.size(); ++i) {
    fui::ListItem row;
    row.label = rowLabels_[i].c_str();
    row.subtitle = rowSubtitles_[i].c_str();
    row.actionValue = static_cast<int16_t>(i);
    rows_.push_back(row);
  }
}

void WeatherActivity::buildResultRows() {
  rowLabels_.clear();
  rowSubtitles_.clear();
  rows_.clear();
  rowLabels_.reserve(results_.size());
  rowSubtitles_.reserve(results_.size());
  for (const weather::Place& place : results_) {
    rowLabels_.push_back(place.name);
    std::string where = place.region;
    if (!place.country.empty() && place.country != place.region) {
      if (!where.empty()) where += ", ";
      where += place.country;
    }
    // The coordinates are on the row on purpose: two towns with one name and
    // one country are told apart by nothing else.
    char coords[48];
    std::snprintf(coords, sizeof(coords), "  (%.2f, %.2f)", static_cast<double>(place.latitude),
                  static_cast<double>(place.longitude));
    where += coords;
    rowSubtitles_.push_back(where);
  }
  rows_.reserve(rowLabels_.size());
  for (size_t i = 0; i < rowLabels_.size(); ++i) {
    fui::ListItem row;
    row.label = rowLabels_[i].c_str();
    row.subtitle = rowSubtitles_[i].c_str();
    row.actionValue = static_cast<int16_t>(i);
    rows_.push_back(row);
  }
}

namespace {
// A detail row, or nothing at all when the value is absent. Returning early on
// absence is what keeps an unreported field off the screen instead of drawing
// it as a zero.
void addDetail(std::vector<std::string>& text, std::vector<weatherui::Detail>& out, const char* label,
               const std::string& value, const std::string& note = std::string()) {
  if (value.empty()) return;
  text.push_back(value);
  text.push_back(note);
  weatherui::Detail detail;
  detail.label = label;
  out.push_back(detail);
}
}  // namespace

void WeatherActivity::buildDetails() {
  detailText_.clear();
  details_.clear();
  // Two strings per row, appended in lockstep, then bound in a second pass --
  // the vector reallocates as it grows and Detail holds pointers.
  detailText_.reserve(24);
  details_.reserve(12);

  char buf[64];
  const weather::Reading& r = reading_;

  if (r.humidity.has) {
    std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(r.humidity.v));
    addDetail(detailText_, details_, "Humidity", buf, weather::describeHumidity(r.humidity.v));
  }
  if (r.windSpeed.has) {
    if (r.windDirection.has) {
      std::snprintf(buf, sizeof(buf), "%.0f km/h %s", static_cast<double>(r.windSpeed.v),
                    weather::compassPoint(r.windDirection.v));
    } else {
      std::snprintf(buf, sizeof(buf), "%.0f km/h", static_cast<double>(r.windSpeed.v));
    }
    addDetail(detailText_, details_, "Wind", buf, weather::describeWind(r.windSpeed.v));
  }
  if (r.windGust.has) {
    std::snprintf(buf, sizeof(buf), "%.0f km/h", static_cast<double>(r.windGust.v));
    addDetail(detailText_, details_, "Gusts", buf, weather::describeWind(r.windGust.v));
  }
  if (r.pressure.has) {
    std::snprintf(buf, sizeof(buf), "%.0f hPa", static_cast<double>(r.pressure.v));
    addDetail(detailText_, details_, "Pressure", buf, weather::describePressure(r.pressure.v));
  }
  if (r.cloudCover.has) {
    std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(r.cloudCover.v));
    addDetail(detailText_, details_, "Cloud", buf, weather::describeCloudCover(r.cloudCover.v));
  }
  if (r.visibility.has) {
    std::snprintf(buf, sizeof(buf), "%.0f km", static_cast<double>(r.visibility.v) / 1000.0);
    addDetail(detailText_, details_, "Visibility", buf, weather::describeVisibility(r.visibility.v));
  }
  if (r.uvIndex.has) {
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(r.uvIndex.v));
    addDetail(detailText_, details_, "UV", buf, weather::describeUv(r.uvIndex.v));
  }
  if (r.dewPoint.has) {
    std::snprintf(buf, sizeof(buf), "%.1f C", static_cast<double>(r.dewPoint.v));
    addDetail(detailText_, details_, "Dew", buf, "");
  }
  if (r.precipitation.has) {
    std::snprintf(buf, sizeof(buf), "%.1f mm", static_cast<double>(r.precipitation.v));
    addDetail(detailText_, details_, "Rain", buf, r.precipitation.v > 0.0f ? "Falling" : "None");
  }
  if (!r.days.empty()) {
    const weather::Day& today = r.days.front();
    if (!today.sunrise.empty() || !today.sunset.empty()) {
      std::snprintf(buf, sizeof(buf), "%s / %s", weather::clockOf(today.sunrise).c_str(),
                    weather::clockOf(today.sunset).c_str());
      addDetail(detailText_, details_, "Sun", buf, weather::durationOf(today.daylightSeconds.or_(0.0f)));
    }
  }
  if (r.elevation.has) {
    std::snprintf(buf, sizeof(buf), "%.0f m", static_cast<double>(r.elevation.v));
    addDetail(detailText_, details_, "Elevation", buf, "");
  }

  for (size_t i = 0; i < details_.size(); ++i) {
    details_[i].value = detailText_[i * 2].c_str();
    details_[i].note = detailText_[i * 2 + 1].c_str();
  }

  // The headline block.
  headlineText_ = weather::describeCode(r.code);
  temperatureText_ = r.temperature.has ? [&] {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f C", static_cast<double>(r.temperature.v));
    return std::string(text);
  }()
                                       : std::string("--");
  feelsText_.clear();
  if (r.apparent.has) {
    char text[48];
    std::snprintf(text, sizeof(text), "FEELS LIKE %.0f C", static_cast<double>(r.apparent.v));
    feelsText_ = text;
  }
  highLowText_.clear();
  if (!r.days.empty() && r.days.front().high.has && r.days.front().low.has) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f / %.0f", static_cast<double>(r.days.front().high.v),
                  static_cast<double>(r.days.front().low.v));
    highLowText_ = text;
  }
  observedText_.clear();
  if (!r.observedAt.empty()) {
    observedText_ = weather::clockOf(r.observedAt);
    if (!r.timezoneAbbr.empty()) observedText_ += " " + r.timezoneAbbr;
  }
  stalenessText_ = stalenessLabel();
}

void WeatherActivity::buildHourRows() {
  hourText_.clear();
  hourRows_.clear();
  const int count = static_cast<int>(reading_.hours.size());
  if (count == 0) return;
  if (hourTop_ >= count) hourTop_ = 0;
  const int shown = count - hourTop_ < hourVisible_ ? count - hourTop_ : hourVisible_;
  if (shown <= 0) return;

  hourText_.reserve(static_cast<size_t>(shown) * 4);
  char buf[48];
  for (int i = 0; i < shown; ++i) {
    const weather::Hour& hour = reading_.hours[static_cast<size_t>(hourTop_ + i)];
    hourText_.push_back(weather::clockOf(hour.time));
    if (hour.temperature.has) {
      // Bare, because buildHours draws a header line naming the column. A unit
      // repeated on twenty-four rows is twenty-four times the width for one
      // fact the heading states once.
      std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(hour.temperature.v));
      hourText_.push_back(buf);
    } else {
      hourText_.push_back("--");
    }
    hourText_.push_back(weather::shortCode(hour.code));
    if (hour.precipProbability.has) {
      std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(hour.precipProbability.v));
      hourText_.push_back(buf);
    } else {
      hourText_.push_back("");
    }
    if (hour.windSpeed.has) {
      std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(hour.windSpeed.v));
      hourText_.push_back(buf);
    } else {
      hourText_.push_back("");
    }
  }
  hourRows_.reserve(static_cast<size_t>(shown));
  for (int i = 0; i < shown; ++i) {
    weatherui::HourRow row;
    row.time = hourText_[static_cast<size_t>(i) * 5 + 0].c_str();
    row.temperature = hourText_[static_cast<size_t>(i) * 5 + 1].c_str();
    row.conditions = hourText_[static_cast<size_t>(i) * 5 + 2].c_str();
    row.precip = hourText_[static_cast<size_t>(i) * 5 + 3].c_str();
    row.wind = hourText_[static_cast<size_t>(i) * 5 + 4].c_str();
    hourRows_.push_back(row);
  }
}

void WeatherActivity::buildDayRows() {
  dayText_.clear();
  dayRows_.clear();
  const size_t count = reading_.days.size();
  if (count == 0) return;
  dayText_.reserve(count * 5);
  char buf[64];
  for (const weather::Day& day : reading_.days) {
    dayText_.push_back(weather::dayLabelOf(day.date));
    dayText_.push_back(weather::shortCode(day.code));
    if (day.high.has || day.low.has) {
      // Either half can be missing on its own, so each is formatted into its
      // own buffer first. Plain locals: the earlier version reached for
      // `static` scratch inside a lambda, which is one shared buffer per call
      // site in a function that runs once per day of the week.
      char high[16] = "--";
      char low[16] = "--";
      if (day.high.has) std::snprintf(high, sizeof(high), "%.0f", static_cast<double>(day.high.v));
      if (day.low.has) std::snprintf(low, sizeof(low), "%.0f", static_cast<double>(day.low.v));
      std::snprintf(buf, sizeof(buf), "%s / %s", high, low);
      dayText_.push_back(buf);
    } else {
      dayText_.push_back("--");
    }
    if (day.precipProbability.has) {
      // ", 70% chance" rather than "rain 70%": the condition beside it is
      // already the word "Rain" often enough that the row read "Rain rain 70%".
      std::snprintf(buf, sizeof(buf), ", %.0f%% chance", static_cast<double>(day.precipProbability.v));
      dayText_.push_back(buf);
    } else {
      dayText_.push_back("");
    }
    if (!day.sunrise.empty()) {
      std::snprintf(buf, sizeof(buf), "%s - %s", weather::clockOf(day.sunrise).c_str(),
                    weather::clockOf(day.sunset).c_str());
      dayText_.push_back(buf);
    } else {
      dayText_.push_back("");
    }
  }
  dayRows_.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    weatherui::DayRow row;
    row.day = dayText_[i * 5 + 0].c_str();
    row.conditions = dayText_[i * 5 + 1].c_str();
    row.highLow = dayText_[i * 5 + 2].c_str();
    row.precip = dayText_[i * 5 + 3].c_str();
    row.sun = dayText_[i * 5 + 4].c_str();
    dayRows_.push_back(row);
  }
}

// --- Drawing -------------------------------------------------------------

void WeatherActivity::render(RenderLock&&) {
  renderer.clearScreen();
  // The reading cut in the body slot: this app is a table of words and numbers
  // to be read, not a board.
  fui::GfxRendererTarget target = toybox::makeTarget(renderer, toybox::readingFaces());
  const fui::DeviceContext device = target.deviceContext();
  const fui::ThemeTokens& tokens = toybox::themeTokens();
  const fui::InputSnapshot noInput{};
  interactionsReady_ = false;
  toybox::Frame frame(target, device, noInput, interactions_);
  toybox::Screen screen(frame);

  const char* what = "Weather";
  const weather::Place* place = openId_ == 0 ? nullptr : store_.find(openId_);
  placeLabel_ = place != nullptr ? weather::describePlace(*place) : std::string();

  switch (phase_) {
    case Phase::Places: {
      buildPlaceRows(target, device, tokens);
      weatherui::PlacesModel model;
      model.items = rows_.empty() ? nullptr : rows_.data();
      model.count = static_cast<int>(rows_.size());
      model.topIndex = 0;  // the model is handed a PAGE, never the whole list
      model.editing = placesEditing_;
      weatherui::buildPlaces(screen, model);
      what = "Weather places";
      break;
    }

    case Phase::Results: {
      buildResultRows();
      weatherui::ResultsModel model;
      model.query = query_.c_str();
      model.items = rows_.empty() ? nullptr : rows_.data();
      model.count = static_cast<int>(rows_.size());
      model.searched = true;
      weatherui::buildResults(screen, model);
      what = "Weather results";
      break;
    }

    case Phase::Forecast: {
      if (place == nullptr) {
        phase_ = Phase::Places;
        break;
      }
      switch (view_) {
        case weatherui::View::Now: {
          buildDetails();
          weatherui::NowModel model;
          model.place = placeLabel_.c_str();
          model.headline = headlineText_.c_str();
          model.temperature = temperatureText_.c_str();
          model.feelsLike = feelsText_.c_str();
          model.highLow = highLowText_.c_str();
          model.observed = observedText_.c_str();
          model.staleness = stalenessText_.c_str();
          model.details = details_.empty() ? nullptr : details_.data();
          model.detailCount = static_cast<int>(details_.size());
          weatherui::buildNow(screen, model);
          what = "Weather now";
          break;
        }
        case weatherui::View::Hours: {
          // The same row height the screen will draw with, from the same
          // function: computing it twice is how a page turn starts skipping an
          // hour.
          const int16_t rowH = weatherui::hourRowHeight(target, tokens);
          const fui::Rect band = weatherui::listBand(device);
          hourVisible_ = rowH > 0 ? band.height / rowH : 0;
          if (hourVisible_ <= 0) hourVisible_ = 1;
          buildHourRows();
          weatherui::HoursModel model;
          model.place = placeLabel_.c_str();
          model.rows = hourRows_.empty() ? nullptr : hourRows_.data();
          model.count = static_cast<int>(hourRows_.size());
          const int count = static_cast<int>(reading_.hours.size());
          if (count > hourVisible_) {
            std::snprintf(pageLabel_, sizeof(pageLabel_), "%d / %d", hourTop_ / hourVisible_ + 1,
                          (count + hourVisible_ - 1) / hourVisible_);
            model.pageLabel = pageLabel_;
          }
          weatherui::buildHours(screen, model);
          what = "Weather hours";
          break;
        }
        case weatherui::View::Week: {
          buildDayRows();
          weatherui::WeekModel model;
          model.place = placeLabel_.c_str();
          model.rows = dayRows_.empty() ? nullptr : dayRows_.data();
          model.count = static_cast<int>(dayRows_.size());
          weatherui::buildWeek(screen, model);
          what = "Weather week";
          break;
        }
      }
      break;
    }

    case Phase::Busy: {
      weatherui::NoticeModel model;
      model.headline = busyMessage_;
      model.message = "";
      weatherui::buildNotice(screen, model);
      what = "Weather busy";
      break;
    }

    case Phase::Notice: {
      weatherui::NoticeModel model;
      model.headline = noticeHeadline_.c_str();
      model.message = noticeMessage_.c_str();
      model.actionLabel = openId_ != 0 && reading_.valid() ? "BACK TO THE FORECAST" : "BACK TO PLACES";
      weatherui::buildNotice(screen, model);
      what = "Weather notice";
      break;
    }

    case Phase::Confirm: {
      weatherui::ConfirmModel model;
      model.place = placeLabel_.c_str();
      model.detail = confirmDetail_.c_str();
      weatherui::buildRemoveConfirm(screen, model);
      what = "Weather confirm";
      break;
    }
  }

  interactionsReady_ = true;
  toybox::reportOverflow(interactions_, what);
  const bool phaseChanged = !everShown_ || phase_ != lastShownPhase_ || view_ != lastShownView_;

  const auto labels = mappedInput.mapLabels("Back", "", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  // Stamped AFTER the panel has been written, so the settle window measures
  // from when a person could first have seen this screen.
  if (phaseChanged) {
    lastShownPhase_ = phase_;
    lastShownView_ = view_;
    phaseShownAtMs_ = millis();
    everShown_ = true;
  }
}
