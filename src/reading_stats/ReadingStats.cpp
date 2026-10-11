#include "ReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

constexpr const char* STATS_FILE = "/ReadingStats/sessions.jsonl";
constexpr const char* STATS_DIR = "/ReadingStats";

// Keep the file from growing forever: once it reaches 1 MB, drop the
// oldest half so new sessions continue to be recorded without an SD-open
// cost that includes a full rewrite every write.
constexpr size_t MAX_FILE_BYTES = 1024 * 1024;

// Maximum bytes in a single JSONL line (start+end+duration+book+doc+bp).
constexpr size_t MAX_LINE_BYTES = 512;

// Buffer size for streaming reads.
constexpr size_t READ_BUF_SIZE = 2048;

// Given a unix timestamp, pack into year/month/day using UTC-ish arithmetic.
// Simple portable version that avoids <time.h> overhead on the read path.
void epochToYmd(int64_t epoch, int& y, int& m, int& d) {
  // Days since 1970-01-01
  int days = static_cast<int>(epoch / 86400);
  y = 1970;
  // Approximate year by subtracting 365/366-day chunks.
  while (true) {
    int daysInYear = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    if (days < daysInYear) break;
    days -= daysInYear;
    y++;
  }
  // Month lengths (non-leap; February adjusted below)
  static const int monthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  for (m = 1; m <= 12; m++) {
    int md = monthDays[m - 1];
    if (m == 2 && leap) md = 29;
    if (days < md) break;
    days -= md;
  }
  d = days + 1;
}

// Pack y/m/d into a single int for map keying: y*10000 + m*100 + d
int ymdKey(int y, int m, int d) { return y * 10000 + m * 100 + d; }

// Read one JSONL line from `file` starting at current position into `buf`.
// Returns bytes read (excluding newline), or 0 on EOF, or -1 on error.
int readLine(HalFile& file, char* buf, size_t bufSize) {
  if (!file || !file.isOpen()) return -1;
  size_t pos = 0;
  while (pos < bufSize - 1) {
    int c = file.read();
    if (c < 0) {
      if (pos == 0) return 0;  // EOF at start
      break;                   // EOF after some data: return what we have
    }
    if (c == '\n') break;
    if (c == '\r') continue;  // skip standalone \r or \r before \n
    buf[pos++] = static_cast<char>(c);
  }
  buf[pos] = '\0';
  return static_cast<int>(pos);
}

// Minimal JSON integer parser: reads the value of key "K" from an object.
// Returns true and sets *val; returns false if key not found or malformed.
bool jsonGetInt(const char* json, const char* key, int64_t& val) {
  char search[64];
  snprintf(search, sizeof(search), "\"%s\":", key);
  const char* pos = strstr(json, search);
  if (!pos) return false;
  pos += strlen(search);
  // Skip whitespace
  while (*pos == ' ' || *pos == '\t') pos++;
  bool neg = false;
  if (*pos == '-') { neg = true; pos++; }
  int64_t v = 0;
  while (*pos >= '0' && *pos <= '9') {
    v = v * 10 + (*pos - '0');
    pos++;
  }
  val = neg ? -v : v;
  return true;
}

}  // namespace

namespace ReadingStats {

bool exists() { return Storage.exists(STATS_FILE); }

bool appendSession(const char* bookPath, const char* document,
                   int64_t startEpoch, int64_t endEpoch,
                   uint32_t durationSeconds,
                   uint16_t startProgressBp, uint16_t endProgressBp) {
  // Ensure directory exists.
  if (!Storage.exists(STATS_DIR)) {
    if (!Storage.mkdir(STATS_DIR, true)) {
      LOG_ERR("RDSTATS", "Cannot create %s", STATS_DIR);
      return false;
    }
  }

  // Build the JSONL line.
  char line[MAX_LINE_BYTES];
  int len = snprintf(line, sizeof(line),
                     "{\"s\":%lld,\"e\":%lld,\"d\":%u,\"b\":\"%s\",\"doc\":\"%s\",\"sb\":%u,\"eb\":%u}\n",
                     static_cast<long long>(startEpoch),
                     static_cast<long long>(endEpoch),
                     static_cast<unsigned>(durationSeconds),
                     bookPath, document,
                     static_cast<unsigned>(startProgressBp),
                     static_cast<unsigned>(endProgressBp));
  if (len < 0 || static_cast<size_t>(len) >= sizeof(line)) {
    LOG_ERR("RDSTATS", "Line too large");
    return false;
  }

  HalFile file = Storage.open(STATS_FILE, O_WRONLY | O_CREAT | O_APPEND);
  if (!file || !file.isOpen()) {
    LOG_ERR("RDSTATS", "Cannot open %s for append", STATS_FILE);
    return false;
  }

  // If over the size cap, drop oldest half.
  if (file.fileSize64() > MAX_FILE_BYTES) {
    file.close();
    // Read entire file, keep second half, rewrite.
    HalFile rfile = Storage.open(STATS_FILE, O_RDONLY);
    if (!rfile || !rfile.isOpen()) {
      LOG_ERR("RDSTATS", "Cannot reopen for trim");
      return false;
    }
    const size_t totalSize = static_cast<size_t>(rfile.fileSize64());
    const size_t keepStart = totalSize / 2;
    // Find the first newline at or after keepStart to avoid a truncated line.
    rfile.seek(keepStart);
    while (rfile.position() < rfile.fileSize64()) {
      int c = rfile.read();
      if (c < 0) break;
      if (c == '\n') break;
    }
    const size_t actualStart = static_cast<size_t>(rfile.position());
    const size_t keepSize = totalSize - actualStart;

    // Read the tail into a heap buffer.
    auto* buf = new (std::nothrow) char[keepSize + 1];
    if (!buf) {
      rfile.close();
      // OOM: just overwrite — better than losing future sessions.
      file = Storage.open(STATS_FILE, O_WRONLY | O_CREAT | O_TRUNC);
      if (file && file.isOpen()) {
        file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(len));
        file.flush();
      }
      return true;
    }
    rfile.seek(actualStart);
    size_t readBytes = rfile.read(reinterpret_cast<uint8_t*>(buf), keepSize);
    rfile.close();

    // Rewrite with the tail + new line.
    HalFile wfile = Storage.open(STATS_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (wfile && wfile.isOpen()) {
      if (readBytes > 0) {
        wfile.write(reinterpret_cast<const uint8_t*>(buf), readBytes);
      }
      wfile.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(len));
      wfile.flush();
    }
    delete[] buf;
    return true;
  }

  // Normal append.
  file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(len));
  file.flush();
  return true;
}

ReadingStatsSummary analyze(int maxDays, int64_t todayEpoch) {
  ReadingStatsSummary sum;
  HalFile file = Storage.open(STATS_FILE, O_RDONLY);
  if (!file || !file.isOpen() || file.isDirectory()) return sum;

  // Per-day minutes: ymdKey → total seconds → minutes at the end.
  std::map<int, int> daySeconds;

  char buf[MAX_LINE_BYTES];
  while (true) {
    int lineLen = readLine(file, buf, sizeof(buf));
    if (lineLen <= 0) break;
    if (lineLen > static_cast<int>(sizeof(buf)) - 1) continue;

    // Parse start time (epoch seconds) and duration.
    int64_t startEpoch = 0;
    int64_t duration = 0;
    if (!jsonGetInt(buf, "s", startEpoch)) continue;
    if (!jsonGetInt(buf, "d", duration)) continue;
    if (duration <= 0 || duration > 86400) continue;  // sanity: max 24h

    int y, m, d;
    epochToYmd(startEpoch, y, m, d);
    int key = ymdKey(y, m, d);

    daySeconds[key] += static_cast<int>(duration);

    sum.totalMinutes += static_cast<int>(duration) / 60;
  }
  file.close();

  if (daySeconds.empty()) return sum;

  // Convert per-day seconds → minutes, collect into daily vector.
  int todayY, todayM, todayD;
  epochToYmd(todayEpoch, todayY, todayM, todayD);
  int todayKey = ymdKey(todayY, todayM, todayD);

  // Sort by key ascending, keep up to maxDays most recent.
  int count = 0;
  for (auto it = daySeconds.rbegin(); it != daySeconds.rend() && (maxDays == 0 || count < maxDays); ++it, ++count) {
    int key = it->first;
    int mins = it->second / 60;
    DailyEntry de;
    de.year = key / 10000;
    de.month = (key / 100) % 100;
    de.day = key % 100;
    de.minutes = mins;
    // Will be reversed later; store in descending order first.
    sum.daily.push_back(de);

    if (key == todayKey) {
      sum.todayMinutes = mins;
    }
  }
  // Reverse to ascending.
  std::reverse(sum.daily.begin(), sum.daily.end());

  // Compute totals.
  sum.totalDays = static_cast<int>(daySeconds.size());
  if (sum.totalDays > 0) {
    sum.averageMinutes = sum.totalMinutes / sum.totalDays;
  }

  // Streak computation: count consecutive days ending yesterday.
  // yesterdayKey = todayKey - 1
  int yesterdayKey = todayKey;
  {
    int yd = todayD - 1;
    int ym = todayM;
    int yy = todayY;
    if (yd < 1) {
      ym--;
      if (ym < 1) { ym = 12; yy--; }
      // Days in previous month
      static const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      yd = md[ym - 1];
      if (ym == 2 && (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0))) yd = 29;
    }
    yesterdayKey = ymdKey(yy, ym, yd);
  }

  // Current streak: walk backward from yesterday.
  int streak = 0;
  int cursor = yesterdayKey;
  while (daySeconds.find(cursor) != daySeconds.end() && daySeconds[cursor] > 0) {
    streak++;
    // Move cursor back one day.
    int cd = cursor % 100;
    int cm = (cursor / 100) % 100;
    int cy = cursor / 10000;
    cd--;
    if (cd < 1) {
      cm--;
      if (cm < 1) { cm = 12; cy--; }
      static const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      cd = md[cm - 1];
      if (cm == 2 && (cy % 4 == 0 && (cy % 100 != 0 || cy % 400 == 0))) cd = 29;
    }
    cursor = ymdKey(cy, cm, cd);
  }
  sum.streakDays = streak;

  // Record streak: scan all days, find longest consecutive run.
  // Collect all days with reading into sorted vector.
  std::vector<int> allDays;
  for (const auto& kv : daySeconds) {
    if (kv.second > 0) allDays.push_back(kv.first);
  }
  std::sort(allDays.begin(), allDays.end());

  int record = 0;
  int run = 0;
  int prev = 0;
  for (int k : allDays) {
    if (prev == 0 || k == prev) {
      run = 1;
    } else if (k - prev == 1) {
      run++;
    } else {
      if (run > record) record = run;
      run = 1;
    }
    prev = k;
  }
  if (run > record) record = run;
  sum.recordStreakDays = record;

  return sum;
}

}  // namespace ReadingStats