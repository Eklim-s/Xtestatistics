#include "ClockSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <memory>

#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "TimezonePickerActivity.h"
#include "components/UITheme.h"
#include "util/Timezones.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_TIMEZONE = 0,
  ITEM_DST,
  ITEM_FORMAT,
  ITEM_SHOW_ON_HOME,
  ITEM_SYNC,
};

const StrId menuNames[ClockSettingsActivity::ITEM_COUNT] = {
    StrId::STR_TIMEZONE,        StrId::STR_CLOCK_DST,      StrId::STR_CLOCK_FORMAT,
    StrId::STR_CLOCK_IN_HEADER, StrId::STR_CLOCK_SYNC_NOW,
};

const StrId dstNames[CrossPointSettings::CLOCK_DST_MODE_COUNT] = {StrId::STR_CLOCK_DST_AUTO, StrId::STR_STATE_ON,
                                                                  StrId::STR_STATE_OFF};
}  // namespace

ClockSettingsActivity::ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ClockSettings", renderer, mappedInput) {}

void ClockSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* ClockSettingsActivity::headerTitle() const { return tr(STR_CLOCK); }

void ClockSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  app.clearTapFlash();
  switch (index) {
    case ITEM_TIMEZONE:
      if (auto activity = makeUniqueNoThrow<TimezonePickerActivity>(renderer, mappedInput)) {
        startActivityForResult(std::move(activity), nullptr);
      } else {
        LOG_ERR("CLKSET", "OOM: TimezonePickerActivity");
      }
      return;
    case ITEM_DST:
      SETTINGS.clockDst = (SETTINGS.clockDst + 1) % CrossPointSettings::CLOCK_DST_MODE_COUNT;
      timezones::applyToClock();
      break;
    case ITEM_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % 2;
      break;
    case ITEM_SHOW_ON_HOME:
      SETTINGS.clockShowInHeader = (SETTINGS.clockShowInHeader + 1) % 2;
      break;
    case ITEM_SYNC:
      if (auto activity = makeUniqueNoThrow<ClockSyncActivity>(renderer, mappedInput)) {
        startActivityForResult(std::move(activity), nullptr);
      } else {
        LOG_ERR("CLKSET", "OOM: ClockSyncActivity");
      }
      return;
    default:
      return;
  }
  SETTINGS.saveToFile();
  requestUpdate();
}

void ClockSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Every value is a flash/translation string or the member time buffer, so
  // the render pass allocates nothing.
  rowItems_[ITEM_TIMEZONE].value = timezones::table()[timezones::activeIndex()].name;
  const uint8_t dst = SETTINGS.clockDst < CrossPointSettings::CLOCK_DST_MODE_COUNT ? SETTINGS.clockDst : uint8_t{0};
  rowItems_[ITEM_DST].value = I18N.get(dstNames[dst]);
  rowItems_[ITEM_FORMAT].value = SETTINGS.clockFormat == 1 ? tr(STR_CLOCK_FORMAT_12H) : tr(STR_CLOCK_FORMAT_24H);
  GUI.setCheckboxRow(rowItems_[ITEM_SHOW_ON_HOME], SETTINGS.clockShowInHeader);
  // Show the RTC date too, so a correct hour cannot hide an incorrect day.
  struct tm local{};
  char time[9];
  if (halClock.localTime(local) && halClock.formatTime(time, sizeof(time), SETTINGS.clockFormat == 1)) {
    snprintf(syncTime_, sizeof(syncTime_), "%04d-%02d-%02d %s", local.tm_year + 1900, local.tm_mon + 1,
             local.tm_mday, time);
    rowItems_[ITEM_SYNC].value = syncTime_;
  } else {
    rowItems_[ITEM_SYNC].value = tr(STR_NOT_SET);
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

