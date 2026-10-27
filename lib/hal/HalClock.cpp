#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

HalClock halClock;  // Singleton instance

namespace {
bool validDateTime(const Rtc::DateTime& dt) {
  // RTC drivers encode only two year digits; reject unset dates and bad registers.
  if (dt.year < 2025 || dt.year > 2099 || dt.month < 1 || dt.month > 12 || dt.hour > 23 || dt.minute > 59 ||
      dt.second > 59)
    return false;
  constexpr uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = dt.year % 4 == 0 && (dt.year % 100 != 0 || dt.year % 400 == 0);
  return dt.day >= 1 && dt.day <= days[dt.month - 1] + (dt.month == 2 && leap ? 1 : 0);
}

// UTC calendar date -> Unix epoch, no timezone involvement (newlib has no
// timegm). Days-from-civil per Howard Hinnant's algorithm.
time_t epochFromUtc(const Rtc::DateTime& dt) {
  int y = dt.year;
  const int m = dt.month;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + dt.day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  return static_cast<time_t>(days) * 86400 + dt.hour * 3600L + dt.minute * 60L + dt.second;
}
}  // namespace

void HalClock::begin() {
  _available = _sdkRtc.begin();
  _hasCachedTime = false;
  _lastPollMs = 0;
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
  if (!_available) return;
  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt) || !validDateTime(dt)) {
    LOG_ERR("CLK", "RTC date unavailable or invalid");
    return;
  }
  const timeval tv = {epochFromUtc(dt), 0};
  if (settimeofday(&tv, nullptr) != 0) LOG_ERR("CLK", "Cannot restore system time from RTC");
}

void HalClock::setTimezone(const char* posixTz) {
  setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
  tzset();
  _lastPollMs = 0;  // re-derive local time under the new rule immediately
}

bool HalClock::localTime(struct tm& out) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs == 0 || (now - _lastPollMs) >= CLOCK_POLL_MS) {
    Rtc::DateTime dt;
    if (_sdkRtc.now(dt) && validDateTime(dt)) {
      _cachedUtc = epochFromUtc(dt);
      _hasCachedTime = true;
    } else if (!_hasCachedTime) {
      return false;
    }
    _lastPollMs = now != 0 ? now : 1;  // 0 doubles as the invalidation sentinel
  }
  localtime_r(&_cachedUtc, &out);
  return true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  struct tm local;
  if (!localTime(local)) return false;
  hour = static_cast<uint8_t>(local.tm_hour);
  minute = static_cast<uint8_t>(local.tm_min);
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  struct tm local;
  if (!localTime(local)) return false;

  if (use12Hour) {
    const bool pm = local.tm_hour >= 12;
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, local.tm_min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return true;
}

bool HalClock::formatDate(char* buf, size_t bufSize) const {
  struct tm local;
  if (bufSize < 11 || !localTime(local)) return false;
  return strftime(buf, bufSize, "%Y-%m-%d", &local) != 0;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  // configTzTime overwrites the process TZ with UTC0 for the SNTP exchange;
  // remember the display timezone so it can be restored below.
  const char* tzBefore = getenv("TZ");
  char savedTz[64] = {0};
  if (tzBefore) snprintf(savedTz, sizeof(savedTz), "%s", tzBefore);
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      const bool ok = validDateTime(dt) && _sdkRtc.set(dt);
      if (ok) {
        _cachedUtc = epochFromUtc(dt);
        _hasCachedTime = true;
        _lastPollMs = 0;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
      }
      setTimezone(savedTz);
      return ok;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  setTimezone(savedTz);
  return false;
}
