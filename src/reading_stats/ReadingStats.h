#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Key-value pair for daily aggregates returned by analyze().
struct DailyEntry {
  int year;
  int month;  // 1–12
  int day;    // 1–31
  int minutes;
};

struct ReadingStatsSummary {
  bool available = false;    // false on recovery, aggregate, or history read failure
  int streakDays = 0;        // current consecutive days through today or yesterday
  int recordStreakDays = 0;  // all-time best streak
  int todayMinutes = 0;
  int totalMinutes = 0;
  int averageMinutes = 0;         // avg per reading day
  int totalDays = 0;              // distinct days with reading
  std::vector<DailyEntry> daily;  // recent days, ascending
};

namespace ReadingStats {

// Whether the stats directory and file exist.
bool exists();

// Finish any journaled append before exposing either API view. False means retry later.
bool recoverPending();
bool readSummaryJson(char* buffer, size_t capacity, int64_t todayEpoch = 0);

// Bounded RAM fallback for failed reader exits; not retained across reboot or deep sleep.
using SessionCompletion = void (*)(const char*, const char*, int64_t, int64_t, uint32_t, uint16_t, uint16_t);
// Completion must not depend on an activity instance; it runs after ownership becomes durable.
bool deferSession(const char* bookPath, const char* document, int64_t startEpoch, int64_t endEpoch,
                  uint32_t durationSeconds, uint16_t startProgressBp, uint16_t endProgressBp,
                  SessionCompletion onSaved = nullptr);
void retryDeferred();

// True transfers ownership to a durable journal, even if applying it is deferred.
// False leaves ownership with the caller. Exact retries of the last record are harmless.
// Clamps the file at ~1 MB by dropping the oldest half of lines.
bool appendSession(const char* bookPath, const char* document, int64_t startEpoch, int64_t endEpoch,
                   uint32_t durationSeconds, uint16_t startProgressBp, uint16_t endProgressBp);

// Stream the sessions file and aggregate per-day minutes.
// maxDays: maximum number of distinct days to keep in `daily` (0 = all).
// todayEpoch: current UNIX timestamp used to compute the streak cutoff.
ReadingStatsSummary analyze(int maxDays, int64_t todayEpoch);

}  // namespace ReadingStats
