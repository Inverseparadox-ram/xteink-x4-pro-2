#pragma once

// The Weather screens. Freestanding builders in the InstapaperScreens mould: a
// model in, a drawn frame out, no renderer and no Activity, so host-tests/ui/
// can assert what they drew and what they made tappable.
//
// ---------------------------------------------------------------------------
// The layout decision, because "as descriptive as possible" is a design
// problem before it is a fetching problem.
//
// NOW is a headline and then a TABLE. The headline is the two numbers a person
// came for -- what it is and what it feels like -- and everything else is
// label/value rows in a fixed order, two per line where they fit. A dashboard
// of tiles was the obvious alternative and it is wrong here twice over: tiles
// spend most of their pixels on borders, and they put the fields in a grid
// whose reading order nobody agrees on. Rows are dense, they scan, and on a
// 1-bit panel they need no chrome at all to be legible.
//
// The three views are siblings, not a hierarchy: NOW, HOURS, WEEK, chosen from
// one segmented bar that is in the same place on all three. A forecast has
// three timescales and no one of them is the parent of another.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "../ui/ToyboxScreen.h"

namespace weatherui {

namespace fui = freeink::ui;

// Chess uses 1-4, the link layer the 200s, Hacker News the 300s, Instapaper
// the 320s, Notes the 340s. Weather takes the 360s.
enum : fui::ActionId {
  ActionOpenPlace = 360,
  ActionAddPlace = 361,
  ActionSearchResult = 362,
  ActionRefresh = 363,
  ActionViewNow = 364,
  ActionViewHours = 365,
  ActionViewWeek = 366,
  ActionRemovePlace = 367,
  ActionRemoveConfirm = 368,
  ActionRemoveCancel = 369,
  ActionNotice = 370,
  ActionPagePrev = 371,
  ActionPageNext = 372,
  ActionEditPlaces = 373,
};

// --- The saved places ----------------------------------------------------

struct PlacesModel {
  const fui::ListItem* items = nullptr;
  int count = 0;
  int topIndex = 0;
  // Removing a place is the one destructive thing this app does, so it lives
  // behind an explicit mode with its own band title -- the same shape the
  // Notes checklist uses, and for the same reason: out of the mode a tap on a
  // row opens a forecast, and a row that sometimes opened and sometimes
  // deleted would be the same-pixel-different-action trap.
  bool editing = false;
};

void buildPlaces(toybox::Screen& screen, const PlacesModel& model);

fui::Rect placesBand(const fui::DeviceContext& device);
int16_t placesRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& tokens);

// --- Search results ------------------------------------------------------

struct ResultsModel {
  const char* query = "";
  const fui::ListItem* items = nullptr;
  int count = 0;
  // The geocoder answered and matched nothing, which is a different screen
  // from a request that failed.
  bool searched = false;
};

void buildResults(toybox::Screen& screen, const ResultsModel& model);

// --- The three forecast views --------------------------------------------

enum class View : uint8_t { Now, Hours, Week };

// One label/value pair on the NOW table. `note` is the plain-words gloss --
// "Comfortable", "Fresh breeze" -- and is what makes the screen descriptive
// rather than merely numeric.
struct Detail {
  const char* label = "";
  const char* value = "";
  const char* note = "";
};

struct NowModel {
  const char* place = "";
  const char* headline = "";     // "Slight rain"
  const char* temperature = "";  // "23.4"
  const char* feelsLike = "";    // "FEELS LIKE 25.1"
  const char* highLow = "";      // "27 / 19"
  const char* observed = "";     // "14:30 . IST"
  const char* staleness = "";    // "UPDATED 12 MIN AGO" or "SAVED COPY"
  const Detail* details = nullptr;
  int detailCount = 0;
};

void buildNow(toybox::Screen& screen, const NowModel& model);

struct HourRow {
  const char* time = "";
  const char* temperature = "";
  const char* conditions = "";
  const char* precip = "";
  const char* wind = "";
};

struct HoursModel {
  const char* place = "";
  const HourRow* rows = nullptr;
  int count = 0;
  const char* pageLabel = nullptr;
};

void buildHours(toybox::Screen& screen, const HoursModel& model);

struct DayRow {
  const char* day = "";
  const char* conditions = "";
  const char* highLow = "";
  const char* precip = "";
  const char* sun = "";
};

struct WeekModel {
  const char* place = "";
  const DayRow* rows = nullptr;
  int count = 0;
};

void buildWeek(toybox::Screen& screen, const WeekModel& model);

// The band the hourly and weekly lists draw into, and their row heights,
// shared with the Activity so its paging arithmetic and the drawn rows come
// from one function rather than two that can only agree by accident.
fui::Rect listBand(const fui::DeviceContext& device);
int16_t hourRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& tokens);
int16_t dayRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& tokens);

// --- Notices and the remove confirm --------------------------------------

struct NoticeModel {
  const char* headline = "";
  const char* message = "";
  const char* actionLabel = nullptr;
};

void buildNotice(toybox::Screen& screen, const NoticeModel& model);

struct ConfirmModel {
  const char* place = "";
  const char* detail = "";
};

void buildRemoveConfirm(toybox::Screen& screen, const ConfirmModel& model);

}  // namespace weatherui
