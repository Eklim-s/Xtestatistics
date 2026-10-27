"""Run the real HAL/TrustedTime boot path with simulated RTC and system clocks."""
from pathlib import Path
import subprocess
import tempfile

from test_trusted_time import STUBS as TIME_STUBS

ROOT = Path(__file__).resolve().parents[1]
STUBS = dict(TIME_STUBS)
STUBS["Arduino.h"] = STUBS["Arduino.h"].replace(
    'const char* tz, const char*', 'const char* tz, const char*, const char* = nullptr'
) + '\n#include <cstdio>\n#include <cstdint>\n'
STUBS["Logging.h"] += '#define LOG_INF(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\n'
STUBS["WiFi.h"] = '''#pragma once
constexpr int WL_CONNECTED = 1;
struct WifiStub { int status() { return 0; } };
inline WifiStub WiFi;
'''
STUBS["Preferences.h"] = '''#pragma once
#include <cstdint>
inline int64_t savedFloor = 0;
struct Preferences {
  bool begin(const char*, bool) { return true; }
  int64_t getLong64(const char*, int64_t) { return savedFloor; }
  void putLong64(const char*, int64_t v) { savedFloor = v; }
  void end() {}
};
'''
CHECK = r'''
#include <HalClock.h>
#include <TrustedTime.h>
#include <Preferences.h>
#include <cassert>
#include <cstring>
#include <sys/time.h>
time_t systemEpoch = 0;
bool present = true, readable = true;
Rtc::DateTime rtc{2026, 10, 24, 23, 30, 0, 6};
bool Rtc::begin() { return ::present; }
bool Rtc::now(DateTime& out) { out = rtc; return readable; }
bool Rtc::set(const DateTime&) { return true; }
extern "C" time_t __wrap_time(time_t* out) {
  if (out) *out = systemEpoch;
  return systemEpoch;
}
extern "C" int __wrap_settimeofday(const timeval* tv, const void*) {
  systemEpoch = tv->tv_sec;
  return 0;
}
int main() {
  halClock.setTimezone("MSK-3");
  halClock.begin();
  assert(systemEpoch == 1792884600);
  trustedtime::init();
  assert(trustedtime::trustedNow() == systemEpoch);
  char date[11], clock[9];
  assert(halClock.formatDate(date, sizeof(date)) && strcmp(date, "2026-10-25") == 0);
  assert(halClock.formatTime(clock, sizeof(clock)) && strcmp(clock, "02:30") == 0);
  assert(!halClock.formatDate(date, 10));
  // A later offline boot advances past yesterday's saved floor.
  savedFloor = systemEpoch;
  systemEpoch = 0;
  rtc.day = 25;
  halClock.begin();
  trustedtime::init();
  assert(systemEpoch == savedFloor + 86400);
  // The persisted floor still prevents time moving backwards.
  savedFloor = systemEpoch + 3600;
  halClock.begin();
  trustedtime::init();
  assert(systemEpoch == savedFloor);
  for (auto bad : {Rtc::DateTime{2026, 2, 29}, Rtc::DateTime{2026, 0, 1},
                   Rtc::DateTime{2026, 13, 1}, Rtc::DateTime{2026, 4, 31},
                   Rtc::DateTime{2024, 1, 1}, Rtc::DateTime{2100, 1, 1},
                   Rtc::DateTime{2026, 1, 1, 24}, Rtc::DateTime{2026, 1, 1, 0, 60}}) {
    rtc = bad;
    systemEpoch = 0;
    halClock.begin();
    assert(systemEpoch == 0 && !halClock.formatDate(date, sizeof(date)));
  }
  rtc = {2028, 2, 29};
  halClock.begin();
  assert(systemEpoch > 0);
  readable = false;
  systemEpoch = 0;
  halClock.begin();
  assert(systemEpoch == 0 && !halClock.formatDate(date, sizeof(date)));
  present = false;
  halClock.begin();
  assert(!halClock.isAvailable() && systemEpoch == 0);
}
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="hal-clock-test-") as directory:
        work = Path(directory)
        for name, content in STUBS.items():
            (work / name).write_text(content)
        (work / "check.cpp").write_text('#include <initializer_list>\n' + CHECK)
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            f"-I{work}", f"-I{ROOT / 'lib/hal'}", f"-I{ROOT / 'lib/TrustedTime'}",
            f"-I{ROOT / 'freeink-sdk/libs/hardware/Rtc/include'}",
            str(ROOT / "lib/hal/HalClock.cpp"), str(ROOT / "lib/TrustedTime/TrustedTime.cpp"),
            str(work / "check.cpp"), "-Wl,--wrap=time,--wrap=settimeofday", "-o", str(work / "check"),
        ], check=True)
        subprocess.run([str(work / "check")], check=True)
    print("HAL RTC boot and date checks passed")
