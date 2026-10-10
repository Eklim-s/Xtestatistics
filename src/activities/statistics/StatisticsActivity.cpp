#include "StatisticsActivity.h"

#include <HalDisplay.h>
#include <I18n.h>
#include <ctime>

#include "components/UITheme.h"
#include "fontIds.h"
#include "reading_stats/ReadingStats.h"

void StatisticsActivity::onEnter() {
  Activity::onEnter();
  screenW = renderer.getScreenWidth();
  screenH = renderer.getScreenHeight();
  contentW = screenW - 2 * MARGIN;

  const int64_t now = static_cast<int64_t>(time(nullptr));
  summary = ReadingStats::analyze(366, now);  // up to one year of daily data
  dataLoaded = true;
  requestUpdate();
}

void StatisticsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
}

void StatisticsActivity::render(RenderLock&&) {
  if (!dataLoaded) return;

  renderer.clearScreen();

  int y = MARGIN;
  y = drawHeader(y);
  y = drawStreak(y);
  y = drawKeyStats(y);
  y = drawWeekBars(y);
  drawMonthGrid(y);

  const auto labels = mappedInput.mapLabels("", "", "", tr(STR_BACK));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

int StatisticsActivity::drawHeader(int y) {
  renderer.drawText(UI_12_FONT_ID, MARGIN, y, tr(STR_STATISTICS_TITLE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 2;

  // Thin separator line.
  renderer.drawLine(MARGIN, y, screenW - MARGIN, y);
  y += 4;
  return y;
}

int StatisticsActivity::drawStreak(int y) {
  char buf[64];
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);

  // Streak.
  snprintf(buf, sizeof(buf), "%s: %d %s", tr(STR_STATS_STREAK), summary.streakDays, tr(STR_STATS_DAYS));
  renderer.drawText(SMALL_FONT_ID, MARGIN, y, buf);
  y += lineH + 2;

  // Record.
  snprintf(buf, sizeof(buf), "%s: %d %s", tr(STR_STATS_RECORD), summary.recordStreakDays, tr(STR_STATS_DAYS));
  renderer.drawText(SMALL_FONT_ID, MARGIN, y, buf);
  y += lineH + 6;
  return y;
}

int StatisticsActivity::drawKeyStats(int y) {
  char buf[64];
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);

  // Today.
  snprintf(buf, sizeof(buf), "%s: %d %s", tr(STR_STATS_TODAY), summary.todayMinutes, tr(STR_STATS_MINUTES));
  renderer.drawText(SMALL_FONT_ID, MARGIN, y, buf);
  y += lineH + 2;

  // Total.
  int hours = summary.totalMinutes / 60;
  int mins = summary.totalMinutes % 60;
  snprintf(buf, sizeof(buf), "%s: %dh %dm", tr(STR_STATS_TOTAL), hours, mins);
  renderer.drawText(SMALL_FONT_ID, MARGIN, y, buf);
  y += lineH + 2;

  // Average.
  snprintf(buf, sizeof(buf), "%s: %d %s", tr(STR_STATS_AVERAGE), summary.averageMinutes, tr(STR_STATS_MINUTES));
  renderer.drawText(SMALL_FONT_ID, MARGIN, y, buf);
  y += lineH + 6;
  return y;
}

int StatisticsActivity::drawWeekBars(int y) {
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, MARGIN, y, tr(STR_STATS_LAST_7), true, EpdFontFamily::BOLD);
  y += lineH + 4;

  // Last 7 days from daily list.
  const int barW = std::max(6, (contentW - 28) / 7 - 4);
  const int barH = std::max(20, screenH / 6);
  const int barBaseY = y + barH;

  int maxMins = 1;
  int count = 0;
  for (auto it = summary.daily.rbegin(); it != summary.daily.rend() && count < 7; ++it, ++count) {
    if (it->minutes > maxMins) maxMins = it->minutes;
  }

  count = 0;
  int dayLabels[7] = {0};
  int dayHeights[7] = {0};
  for (auto it = summary.daily.rbegin(); it != summary.daily.rend() && count < 7; ++it, ++count) {
    dayLabels[6 - count] = it->day;
    dayHeights[6 - count] = maxMins > 0 ? (it->minutes * barH / maxMins) : 0;
  }

  for (int i = 0; i < count; i++) {
    int x = MARGIN + i * (barW + 4) + 4;
    int h = std::max(1, dayHeights[i]);
    renderer.fillRect(x, barBaseY - h, barW, h, true);

    // Day label under bar.
    char dayBuf[4];
    snprintf(dayBuf, sizeof(dayBuf), "%d", dayLabels[i]);
    int txW = renderer.getTextWidth(SMALL_FONT_ID, dayBuf);
    renderer.drawText(SMALL_FONT_ID, x + (barW - txW) / 2, barBaseY + 2, dayBuf);
  }

  return barBaseY + renderer.getLineHeight(SMALL_FONT_ID) + 10;
}

int StatisticsActivity::drawMonthGrid(int y) {
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, MARGIN, y, tr(STR_STATS_CALENDAR), true, EpdFontFamily::BOLD);
  y += lineH + 4;

  if (summary.daily.empty()) {
    renderer.drawText(SMALL_FONT_ID, MARGIN, y, tr(STR_STATS_NO_DATA));
    return y + lineH + 4;
  }

  // Use the last month in data.
  const auto& last = summary.daily.back();
  int month = last.month;
  int year = last.year;

  // Days in this month.
  static const int monthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int daysInMonth = monthDays[month - 1];
  if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) daysInMonth = 29;

  // Build lookup: day → minutes.
  int dayMinutes[32] = {0};
  for (const auto& de : summary.daily) {
    if (de.year == year && de.month == month && de.day >= 1 && de.day <= 31) {
      dayMinutes[de.day] = de.minutes;
    }
  }

  // Find max for intensity scaling.
  int maxMin = 1;
  for (int d = 1; d <= daysInMonth; d++) {
    if (dayMinutes[d] > maxMin) maxMin = dayMinutes[d];
  }

  // Month name header.
  static const char* MONTH_NAMES[] = {"",     "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char titleBuf[32];
  snprintf(titleBuf, sizeof(titleBuf), "%s %d", MONTH_NAMES[month], year);
  int tw = renderer.getTextWidth(SMALL_FONT_ID, titleBuf);
  renderer.drawText(SMALL_FONT_ID, MARGIN + (contentW - tw) / 2, y, titleBuf, true, EpdFontFamily::BOLD);
  y += lineH + 1;

  // Day-of-week headers: M T W T F S S (or S M T W T F S)
  static const char* DOW = "M T W T F S S";
  int dowW = renderer.getTextWidth(SMALL_FONT_ID, DOW);
  renderer.drawText(SMALL_FONT_ID, MARGIN + (contentW - dowW) / 2, y, DOW);
  y += lineH;

  // Compute day-of-week of the 1st. We need to know what day of week the 1st is.
  // Simple Zeller-like: 2026-10-01 = Saturday (day 6 if Sunday=0), etc.
  // Use a known anchor: 1970-01-01 = Thursday (4).
  // Days since 1970:
  auto daysSince1970 = [](int y, int m, int d) -> int {
    int days = 0;
    for (int yy = 1970; yy < y; yy++) {
      days += (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365;
    }
    static const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int mm = 1; mm < m; mm++) {
      days += md[mm - 1];
      if (mm == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days++;
    }
    days += d - 1;
    return days;
  };

  // Monday=1..Sunday=7. 1970-01-01 = Thursday = 4.
  int firstDow = ((daysSince1970(year, month, 1) + 3) % 7) + 1;  // 1=Mon

  const int cellSize = std::max(6, (contentW - 12) / 7);
  const int cellGap = 1;
  int gy = y + 2;

  for (int d = 1; d <= daysInMonth; d++) {
    int col = (firstDow + d - 2) % 7;
    int row = (firstDow + d - 2) / 7;
    int cx = MARGIN + col * (cellSize + cellGap) + 4;
    int cy = gy + row * (cellSize + cellGap);

    if (dayMinutes[d] > 0) {
      // Intensity: 25%–100% fill.
      int intensity = 25 + (dayMinutes[d] * 75 / maxMin);
      // Approximate greyscale with dithering via fill percentage.
      if (intensity >= 90) {
        renderer.fillRect(cx, cy, cellSize, cellSize, true);
      } else if (intensity >= 60) {
        renderer.fillRect(cx, cy, cellSize, cellSize, true);
        renderer.fillRect(cx + 1, cy + 1, cellSize - 2, cellSize - 2, false);
      } else if (intensity >= 35) {
        renderer.drawRect(cx, cy, cellSize, cellSize, true);
      } else {
        renderer.drawRect(cx, cy, cellSize, cellSize, true);
        // Thin border only — already done above.
      }
    } else {
      renderer.drawRect(cx, cy, cellSize, cellSize, true);
    }
  }

  int totalRows = (firstDow + daysInMonth - 2) / 7 + 1;
  return y + totalRows * (cellSize + cellGap) + 10;
}