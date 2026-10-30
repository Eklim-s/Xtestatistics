#pragma once

#include <ctime>
#include <vector>

#include "activities/Activity.h"
#include "reading_stats/ReadingStats.h"

class StatisticsActivity final : public Activity {
 public:
  StatisticsActivity(GfxRenderer& r, MappedInputManager& in) : Activity("Statistics", r, in) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return false; }

 private:
  ReadingStatsSummary summary;
  bool dataLoaded = false;

  // Layout helpers.
  static constexpr int MARGIN = 8;
  static constexpr int ROW_H = 16;
  int screenW = 0;
  int screenH = 0;
  int contentW = 0;
  int contentLeft = 0;
  int contentTop = 0;
  int contentBottom = 0;
  time_t displayEpoch = 0;

  int drawHeader(int y);
  int drawStreak(int y);
  int drawKeyStats(int y);
  int drawWeekBars(int y);
  int drawMonthGrid(int y);
};
