#include "ReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
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

constexpr size_t COPY_BUF_SIZE = 256;
constexpr const char* STATS_TMP_FILE = "/ReadingStats/sessions.jsonl.tmp";

bool appendJsonEscaped(char* out, size_t capacity, size_t& pos, const char* value) {
  if (!value) value = "";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
    const char* escaped = nullptr;
    switch (*p) {
      case '\\':
        escaped = "\\\\";
        break;
      case '"':
        escaped = "\\\"";
        break;
      case '\n':
        escaped = "\\n";
        break;
      case '\r':
        escaped = "\\r";
        break;
      case '\t':
        escaped = "\\t";
        break;
      case '\b':
        escaped = "\\b";
        break;
      case '\f':
        escaped = "\\f";
        break;
      default:
        break;
    }
    if (escaped) {
      const size_t length = strlen(escaped);
      if (pos + length >= capacity) return false;
      memcpy(out + pos, escaped, length);
      pos += length;
    } else if (*p < 0x20) {
      if (pos + 6 >= capacity) return false;
      snprintf(out + pos, capacity - pos, "\\u%04x", static_cast<unsigned>(*p));
      pos += 6;
    } else {
      if (pos + 1 >= capacity) return false;
      out[pos++] = static_cast<char>(*p);
    }
  }
  return true;
}

bool appendLiteral(char* out, size_t capacity, size_t& pos, const char* value) {
  const size_t length = value ? strlen(value) : 0;
  if (pos + length >= capacity) return false;
  if (length > 0) memcpy(out + pos, value, length);
  pos += length;
  return true;
}

bool writeAll(HalFile& file, const uint8_t* data, size_t length) {
  while (length > 0) {
    const size_t written = file.write(data, length);
    if (written == 0 || written > length) return false;
    data += written;
    length -= written;
  }
  return true;
}

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

int daysInMonth(int year, int month) {
  static const int monthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
  return monthDays[month - 1];
}

int nextDayKey(int key) {
  int day = key % 100;
  int month = (key / 100) % 100;
  int year = key / 10000;
  if (++day > daysInMonth(year, month)) {
    day = 1;
    if (++month > 12) {
      month = 1;
      ++year;
    }
  }
  return ymdKey(year, month, day);
}

// Read one JSONL line from `file` starting at current position into `buf`.
// Returns bytes read (excluding newline), 0 on EOF, -1 on error, or -2 when
// the complete line was discarded because it did not fit in the buffer.
int readLine(HalFile& file, char* buf, size_t bufSize) {
  if (!file || !file.isOpen() || !buf || bufSize == 0) return -1;
  size_t pos = 0;
  bool tooLong = false;
  while (true) {
    int c = file.read();
    if (c < 0) {
      if (pos == 0 && !tooLong) return 0;  // EOF at start
      break;
    }
    if (c == '\n') break;
    if (c == '\r') continue;  // skip standalone \r or \r before \n
    if (pos + 1 < bufSize && !tooLong) {
      buf[pos++] = static_cast<char>(c);
    } else {
      tooLong = true;
    }
  }
  buf[pos] = '\0';
  if (tooLong) return -2;
  return static_cast<int>(pos);
}

// Minimal JSON integer parser: reads the value of key "K" from an object.
// Returns true and sets *val; returns false if key not found or malformed.
bool jsonGetInt(const char* json, const char* key, int64_t& val) {
  if (!json || !key) return false;
  char search[64];
  const int searchLength = snprintf(search, sizeof(search), "\"%s\"", key);
  if (searchLength < 0 || static_cast<size_t>(searchLength) >= sizeof(search)) return false;
  const char* pos = json;
  while ((pos = strstr(pos, search)) != nullptr) {
    const char* afterKey = pos + searchLength;
    while (*afterKey == ' ' || *afterKey == '\t' || *afterKey == '\r' || *afterKey == '\n') afterKey++;
    if (*afterKey == ':') {
      pos = afterKey + 1;
      break;
    }
    pos = afterKey;
  }
  if (!pos) return false;
  while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
  bool neg = false;
  if (*pos == '-') {
    neg = true;
    pos++;
  }
  if (*pos < '0' || *pos > '9') return false;
  const uint64_t limit = neg ? (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U)
                             : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  uint64_t magnitude = 0;
  while (*pos >= '0' && *pos <= '9') {
    const uint64_t digit = static_cast<uint64_t>(*pos - '0');
    if (magnitude > (limit - digit) / 10U) return false;
    magnitude = magnitude * 10U + digit;
    pos++;
  }
  if (*pos != '\0' && *pos != ',' && *pos != '}' && *pos != ' ' && *pos != '\t' && *pos != '\r' && *pos != '\n') {
    return false;
  }
  if (neg) {
    if (magnitude == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U) {
      val = std::numeric_limits<int64_t>::min();
    } else {
      val = -static_cast<int64_t>(magnitude);
    }
  } else {
    val = static_cast<int64_t>(magnitude);
  }
  return true;
}

}  // namespace

namespace ReadingStats {

bool exists() { return Storage.exists(STATS_FILE); }

bool appendSession(const char* bookPath, const char* document, int64_t startEpoch, int64_t endEpoch,
                   uint32_t durationSeconds, uint16_t startProgressBp, uint16_t endProgressBp) {
  // Ensure directory exists.
  if (!Storage.exists(STATS_DIR)) {
    if (!Storage.mkdir(STATS_DIR, true)) {
      LOG_ERR("RDSTATS", "Cannot create %s", STATS_DIR);
      return false;
    }
  }

  // Build the JSONL line.
  auto lineBuffer = makeUniqueNoThrow<char[]>(MAX_LINE_BYTES);
  if (!lineBuffer) {
    LOG_ERR("RDSTATS", "Cannot allocate statistics line");
    return false;
  }
  char* line = lineBuffer.get();
  int len =
      snprintf(line, MAX_LINE_BYTES, "{\"s\":%lld,\"e\":%lld,\"d\":%u,\"b\":\"", static_cast<long long>(startEpoch),
               static_cast<long long>(endEpoch), static_cast<unsigned>(durationSeconds));
  if (len < 0 || static_cast<size_t>(len) >= MAX_LINE_BYTES) {
    LOG_ERR("RDSTATS", "Line too large");
    return false;
  }
  size_t linePos = static_cast<size_t>(len);
  if (!appendJsonEscaped(line, MAX_LINE_BYTES, linePos, bookPath) ||
      !appendLiteral(line, MAX_LINE_BYTES, linePos, "\",\"doc\":\"") ||
      !appendJsonEscaped(line, MAX_LINE_BYTES, linePos, document)) {
    LOG_ERR("RDSTATS", "Line too large");
    return false;
  }
  const int suffixLength = snprintf(line + linePos, MAX_LINE_BYTES - linePos, "\",\"sb\":%u,\"eb\":%u}\n",
                                    static_cast<unsigned>(startProgressBp), static_cast<unsigned>(endProgressBp));
  if (suffixLength < 0 || static_cast<size_t>(suffixLength) >= MAX_LINE_BYTES - linePos) {
    LOG_ERR("RDSTATS", "Line too large");
    return false;
  }
  linePos += static_cast<size_t>(suffixLength);

  HalFile file = Storage.open(STATS_FILE, O_WRONLY | O_CREAT | O_APPEND);
  if (!file || !file.isOpen()) {
    LOG_ERR("RDSTATS", "Cannot open %s for append", STATS_FILE);
    return false;
  }

  // Trim before append so the new record does not push the file over the cap.
  const uint64_t existingSize = file.fileSize64();
  if (existingSize > MAX_FILE_BYTES || linePos > MAX_FILE_BYTES - existingSize) {
    if (!file.close()) {
      LOG_ERR("RDSTATS", "Cannot close statistics file before trim");
      return false;
    }
    HalFile rfile = Storage.open(STATS_FILE, O_RDONLY);
    if (!rfile || !rfile.isOpen()) {
      LOG_ERR("RDSTATS", "Cannot reopen for trim");
      return false;
    }
    const uint64_t totalSize = rfile.fileSize64();
    const uint64_t keepStart = totalSize / 2;
    // Find the first newline at or after keepStart to avoid a truncated line.
    if (!rfile.seek64(keepStart)) {
      rfile.close();
      return false;
    }
    bool scanOk = true;
    while (rfile.position() < rfile.fileSize64()) {
      int c = rfile.read();
      if (c < 0) {
        scanOk = false;
        break;
      }
      if (c == '\n') break;
    }
    if (!scanOk) {
      rfile.close();
      LOG_ERR("RDSTATS", "Cannot find trim boundary");
      return false;
    }

    auto copyBuffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUF_SIZE);
    if (!copyBuffer) {
      rfile.close();
      LOG_ERR("RDSTATS", "Cannot allocate trim buffer");
      return false;
    }

    HalFile wfile = Storage.open(STATS_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (!wfile || !wfile.isOpen()) {
      rfile.close();
      Storage.remove(STATS_TMP_FILE);
      LOG_ERR("RDSTATS", "Cannot open trim temporary file");
      return false;
    }
    bool ok = true;
    while (ok) {
      const int readBytes = rfile.read(copyBuffer.get(), COPY_BUF_SIZE);
      if (readBytes < 0) {
        ok = false;
        break;
      }
      if (readBytes == 0) break;
      ok = writeAll(wfile, copyBuffer.get(), static_cast<size_t>(readBytes));
    }
    if (ok) ok = writeAll(wfile, reinterpret_cast<const uint8_t*>(line), linePos);
    wfile.flush();
    if (!wfile.close()) ok = false;
    if (!rfile.close()) ok = false;
    if (!ok || !Storage.replaceFile(STATS_TMP_FILE, STATS_FILE)) {
      Storage.remove(STATS_TMP_FILE);
      LOG_ERR("RDSTATS", "Cannot complete statistics trim");
      return false;
    }
    return true;
  }

  // Normal append.
  const uint64_t originalSize = existingSize;
  const bool written = writeAll(file, reinterpret_cast<const uint8_t*>(line), linePos);
  file.flush();
  if (!written) {
    const bool restored = file.truncate(originalSize);
    file.flush();
    const bool closed = file.close();
    if (!restored || !closed) {
      LOG_ERR("RDSTATS", "Cannot restore statistics after short write");
      return false;
    }
    LOG_ERR("RDSTATS", "Cannot append statistics");
    return false;
  }
  if (!file.close()) {
    LOG_ERR("RDSTATS", "Cannot append statistics");
    return false;
  }
  return true;
}

ReadingStatsSummary analyze(int maxDays, int64_t todayEpoch) {
  ReadingStatsSummary sum;
  HalFile file = Storage.open(STATS_FILE, O_RDONLY);
  if (!file || !file.isOpen() || file.isDirectory()) return sum;

  // Per-day minutes: ymdKey → total seconds → minutes at the end.
  std::map<int, int64_t> daySeconds;

  auto lineBuffer = makeUniqueNoThrow<char[]>(MAX_LINE_BYTES);
  if (!lineBuffer) return sum;
  char* buf = lineBuffer.get();
  while (true) {
    int lineLen = readLine(file, buf, sizeof(buf));
    if (lineLen == -2) continue;
    if (lineLen <= 0) break;

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
    int mins = static_cast<int>(it->second / 60);
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
  const int64_t totalSeconds =
      std::accumulate(daySeconds.begin(), daySeconds.end(), int64_t{0},
                      [](const int64_t total, const auto& entry) { return total + entry.second; });
  sum.totalMinutes = static_cast<int>(totalSeconds / 60);
  sum.totalDays = static_cast<int>(daySeconds.size());
  if (sum.totalDays > 0) {
    sum.averageMinutes = sum.totalMinutes / sum.totalDays;
  }

  // Streak computation: count consecutive days ending yesterday.
  // yesterdayKey = todayKey - 1
  int yesterdayKey;
  {
    int yd = todayD - 1;
    int ym = todayM;
    int yy = todayY;
    if (yd < 1) {
      ym--;
      if (ym < 1) {
        ym = 12;
        yy--;
      }
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
      if (cm < 1) {
        cm = 12;
        cy--;
      }
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
    } else if (k == nextDayKey(prev)) {
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
