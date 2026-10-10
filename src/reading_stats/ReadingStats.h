#pragma once

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
  int streakDays = 0;       // current consecutive days (ending yesterday)
  int recordStreakDays = 0; // all-time best streak
  int todayMinutes = 0;
  int totalMinutes = 0;
  int averageMinutes = 0;   // avg per reading day
  int totalDays = 0;        // distinct days with reading
  std::vector<DailyEntry> daily;  // recent days, ascending
};

namespace ReadingStats {

// Whether the stats directory and file exist.
bool exists();

// Append one reading session as a JSONL line. Non-blocking; returns false
// only on a write error (full card, etc.). Clamps the file at ~1 MB by
// dropping the oldest half of lines.
bool appendSession(const char* bookPath, const char* document,
                   int64_t startEpoch, int64_t endEpoch,
                   uint32_t durationSeconds,
                   uint16_t startProgressBp, uint16_t endProgressBp);

// Stream the sessions file and aggregate per-day minutes.
// maxDays: maximum number of distinct days to keep in `daily` (0 = all).
// todayEpoch: current UNIX timestamp used to compute the streak cutoff.
ReadingStatsSummary analyze(int maxDays, int64_t todayEpoch);

}  // namespace ReadingStats