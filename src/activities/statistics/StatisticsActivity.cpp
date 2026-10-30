#include "StatisticsActivity.h"

#include <HalDisplay.h>
#include <I18n.h>
#include <TrustedTime.h>

#include <algorithm>
#include <ctime>

#include "components/UITheme.h"
#include "fontIds.h"
#include "reading_stats/ReadingStats.h"

void StatisticsActivity::onEnter() {
  Activity::onEnter();
  screenW = renderer.getScreenWidth();
  screenH = renderer.getScreenHeight();
  const auto area = GUI.getScreenSafeArea(renderer, true);
  contentLeft = area.x + MARGIN;
  contentTop = area.y + MARGIN;
  contentW = area.width - 2 * MARGIN;
  contentBottom = area.y + area.height - MARGIN;

  const int64_t trustedNow = trustedtime::trustedNow();
  const int64_t now = trustedNow > 0 ? trustedNow : static_cast<int64_t>(time(nullptr));
  displayEpoch = static_cast<time_t>(now);
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

  int y = contentTop;
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
  renderer.drawText(UI_12_FONT_ID, contentLeft, y, tr(STR_STATISTICS_TITLE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 2;

  std::tm local{};
  char date[16];
  if (localtime_r(&displayEpoch, &local) && strftime(date, sizeof(date), "%Y-%m-%d", &local) > 0) {
    renderer.drawText(SMALL_FONT_ID, contentLeft, y, date);
    y += renderer.getLineHeight(SMALL_FONT_ID) + 2;
  }

  // Thin separator line.
  renderer.drawLine(contentLeft, y, contentLeft + contentW, y);
  y += 4;
  return y;
}

int StatisticsActivity::drawStreak(int y) {
  char buf[64];
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);

  // Streak.
  snprintf(buf, sizeof(buf), "%s: %d %s", tr(STR_STATS_STREAK), summary.streakDays, tr(STR_STATS_DAYS));
  renderer.drawText(SMALL_FONT_ID, contentLeft, y, buf);
  y += lineH + 2;

  // Record.
  snprintf(buf, sizeof(buf), "%s: %d %s", tr(STR_STATS_RECORD), summary.recordStreakDays, tr(STR_STATS_DAYS));
  renderer.drawText(SMALL_FONT_ID, contentLeft, y, buf);
  y += lineH + 6;
  return y;
}

int StatisticsActivity::drawKeyStats(int y) {
  char buf[64];
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);

  // Today.
  snprintf(buf, sizeof(buf), "%s: %d %s", tr(STR_STATS_TODAY), summary.todayMinutes, tr(STR_STATS_MINUTES));
  renderer.drawText(SMALL_FONT_ID, contentLeft, y, buf);
  y += lineH + 2;

  // Total.
  int hours = summary.totalMinutes / 60;
  int mins = summary.totalMinutes % 60;
  snprintf(buf, sizeof(buf), "%s: %dh %dm", tr(STR_STATS_TOTAL), hours, mins);
  renderer.drawText(SMALL_FONT_ID, contentLeft, y, buf);
  y += lineH + 2;

  // Average.
  snprintf(buf, sizeof(buf), "%s: %d %s", tr(STR_STATS_AVERAGE), summary.averageMinutes, tr(STR_STATS_MINUTES));
  renderer.drawText(SMALL_FONT_ID, contentLeft, y, buf);
  y += lineH + 6;
  return y;
}

int StatisticsActivity::drawWeekBars(int y) {
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, contentLeft, y, tr(STR_STATS_LAST_7), true, EpdFontFamily::BOLD);
  y += lineH + 4;

  // Last 7 calendar days, including days without a reading session.
  const int barW = std::max(6, (contentW - 28) / 7 - 4);
  // Reserve calendar headers and six rows before sizing the chart.
  const int calendarReserve = 3 * lineH + 10 + 6 * (lineH + 4);
  const int barH = std::max(0, std::min(screenH / 6, contentBottom - y - lineH - 10 - calendarReserve));
  const int barBaseY = y + barH;

  int maxMins = 1;
  int dayLabels[7] = {0};
  int dayHeights[7] = {0};
  const time_t now = displayEpoch;
  for (int i = 0; i < 7; ++i) {
    std::tm localDay{};
    if (localtime_r(&now, &localDay) == nullptr) continue;
    localDay.tm_mday -= 6 - i;
    localDay.tm_isdst = -1;
    if (mktime(&localDay) == static_cast<time_t>(-1)) continue;
    dayLabels[i] = localDay.tm_mday;
    const int year = localDay.tm_year + 1900;
    const int month = localDay.tm_mon + 1;
    const auto entry = std::find_if(summary.daily.begin(), summary.daily.end(), [&](const DailyEntry& candidate) {
      return candidate.year == year && candidate.month == month && candidate.day == localDay.tm_mday;
    });
    if (entry != summary.daily.end()) {
      maxMins = std::max(maxMins, entry->minutes);
      dayHeights[i] = entry->minutes;
    }
  }

  for (int i = 0; i < 7; i++) {
    int x = contentLeft + i * (barW + 4) + 4;
    if (dayHeights[i] > 0 && barH > 0) {
      const int h = std::max(1, dayHeights[i] * barH / maxMins);
      renderer.fillRect(x, barBaseY - h, barW, h, true);
    }

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
  renderer.drawText(SMALL_FONT_ID, contentLeft, y, tr(STR_STATS_CALENDAR), true, EpdFontFamily::BOLD);
  y += lineH + 4;

  if (summary.daily.empty()) {
    renderer.drawText(SMALL_FONT_ID, contentLeft, y, tr(STR_STATS_NO_DATA));
    return y + lineH + 4;
  }

  // Show the current local month; days without data remain empty.
  const time_t now = displayEpoch;
  std::tm current{};
  if (localtime_r(&now, &current) == nullptr) return y;
  int month = current.tm_mon + 1;
  int year = current.tm_year + 1900;

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

  // Month name header.
  static const char* MONTH_NAMES[] = {"",    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char titleBuf[32];
  snprintf(titleBuf, sizeof(titleBuf), "%s %d", MONTH_NAMES[month], year);
  int tw = renderer.getTextWidth(SMALL_FONT_ID, titleBuf);
  renderer.drawText(SMALL_FONT_ID, contentLeft + (contentW - tw) / 2, y, titleBuf, true, EpdFontFamily::BOLD);
  y += lineH + 1;

  std::tm first = current;
  first.tm_mday = 1;
  first.tm_hour = 12;
  first.tm_isdst = -1;
  if (mktime(&first) == static_cast<time_t>(-1)) return y;
  const int firstDow = (first.tm_wday + 6) % 7 + 1;
  const int totalRows = (firstDow + daysInMonth - 2) / 7 + 1;
  const int cellGap = 1;
  const int gy = y + lineH + 2;
  const int cellSize = std::min((contentW - 8) / 7 - cellGap,
                                (contentBottom - gy) / totalRows - cellGap);
  if (cellSize < lineH + 2) return y;
  const int gridLeft = contentLeft + (contentW - 7 * (cellSize + cellGap)) / 2;
  static const char* DOW = "MTWTFSS";
  for (int col = 0; col < 7; ++col) {
    const char label[2] = {DOW[col], '\0'};
    const int width = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, gridLeft + col * (cellSize + cellGap) + (cellSize - width) / 2, y, label);
  }

  for (int d = 1; d <= daysInMonth; d++) {
    int col = (firstDow + d - 2) % 7;
    int row = (firstDow + d - 2) / 7;
    int cx = gridLeft + col * (cellSize + cellGap);
    int cy = gy + row * (cellSize + cellGap);

    const bool filled = dayMinutes[d] > 0;
    if (filled) {
      renderer.fillRect(cx, cy, cellSize, cellSize, true);
    } else {
      renderer.drawRect(cx, cy, cellSize, cellSize, true);
    }
    char day[4];
    snprintf(day, sizeof(day), "%d", d);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, day);
    renderer.drawText(SMALL_FONT_ID, cx + (cellSize - textW) / 2, cy + (cellSize - lineH) / 2, day, !filled);
  }

  return y + totalRows * (cellSize + cellGap) + 10;
}

