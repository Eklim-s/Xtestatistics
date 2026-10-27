#pragma once
#include "activities/UiListActivity.h"

// Clock configuration under System settings: timezone, 12/24-hour format,
// home-header display, and manual NTP sync. Only reachable when
// halClock.isAvailable() — SettingsActivity gates the entry.
class ClockSettingsActivity final : public UiListActivity {
 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ITEM_COUNT = 6;

  void onEnter() override;

 private:
  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  // Row values are flash/translation strings assigned directly into rowItems_;
  // Formatted time and date live here so pointers stay valid across the frame.
  char syncTime_[9] = {0};
  char date_[11] = {0};
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
};
