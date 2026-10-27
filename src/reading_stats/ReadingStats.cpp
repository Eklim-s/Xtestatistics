#include "ReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
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
constexpr const char* SUMMARY_FILE = "/ReadingStats/summary.json";
constexpr const char* SUMMARY_TMP_FILE = "/ReadingStats/summary.json.tmp";
constexpr const char* PENDING_FILE = "/ReadingStats/pending.jsonl";
constexpr const char* PENDING_TMP_FILE = "/ReadingStats/pending.jsonl.tmp";
constexpr const char* STATS_READY_FILE = "/ReadingStats/sessions.jsonl.ready";
constexpr const char* MIGRATION_FILE = "/ReadingStats/dates.migration.tmp";

struct AggregateSummary {
  int64_t totalSeconds = 0;
  int totalDays = 0;
  int recordStreak = 0;
  int currentStreak = 0;
  int lastDay = 0;
  bool hasDates = false;
};

struct Transaction {
  AggregateSummary aggregate;
  uint64_t originalSize = 0;
  uint64_t keepStart = 0;
  uint64_t targetSize = 0;
  bool separator = false;
};

// Four failed exits survive activity destruction without fallible allocation (~2.3 KB).
struct DeferredSession {
  char book[512] = {};
  char document[33] = {};
  int64_t start = 0;
  int64_t end = 0;
  uint32_t duration = 0;
  uint16_t startBp = 0;
  uint16_t endBp = 0;
  ReadingStats::SessionCompletion onSaved = nullptr;
};
DeferredSession deferred[4];

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

bool formatSession(char* line, size_t& length, const char* book, const char* document, int64_t start, int64_t end,
                   uint32_t duration, uint16_t startBp, uint16_t endBp) {
  if (start <= 0 || start > 253402300799LL || end < start || end > 253402300799LL || duration == 0 ||
      duration > 86400 || startBp > 10000 || endBp > 10000)
    return false;
  const int count =
      snprintf(line, MAX_LINE_BYTES, "{\"s\":%lld,\"e\":%lld,\"d\":%u,\"b\":\"", static_cast<long long>(start),
               static_cast<long long>(end), static_cast<unsigned>(duration));
  if (count < 0 || static_cast<size_t>(count) >= MAX_LINE_BYTES) return false;
  length = static_cast<size_t>(count);
  if (!appendJsonEscaped(line, MAX_LINE_BYTES, length, book) ||
      !appendLiteral(line, MAX_LINE_BYTES, length, "\",\"doc\":\"") ||
      !appendJsonEscaped(line, MAX_LINE_BYTES, length, document))
    return false;
  const int suffix = snprintf(line + length, MAX_LINE_BYTES - length, "\",\"sb\":%u,\"eb\":%u}\n",
                              static_cast<unsigned>(startBp), static_cast<unsigned>(endBp));
  if (suffix < 0 || static_cast<size_t>(suffix) >= MAX_LINE_BYTES - length) return false;
  length += static_cast<size_t>(suffix);
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

// Session dates follow the device's local timezone.
void epochToYmd(int64_t epoch, int& y, int& m, int& d) {
  const time_t timestamp = static_cast<time_t>(epoch);
  struct tm local{};
  if (localtime_r(&timestamp, &local) != nullptr) {
    y = local.tm_year + 1900;
    m = local.tm_mon + 1;
    d = local.tm_mday;
    return;
  }

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

int previousDayKey(int key) {
  int day = key % 100;
  int month = (key / 100) % 100;
  int year = key / 10000;
  if (--day < 1) {
    if (--month < 1) {
      month = 12;
      --year;
    }
    day = daysInMonth(year, month);
  }
  return ymdKey(year, month, day);
}

using DayConsumer = void (*)(void*, int, int64_t);

void distributeSession(void* context, DayConsumer add, int64_t startEpoch, int64_t endEpoch, int64_t duration) {
  if (endEpoch <= startEpoch || endEpoch - startEpoch > 7 * 86400LL || duration > endEpoch - startEpoch) {
    int y, m, d;
    epochToYmd(startEpoch, y, m, d);
    add(context, ymdKey(y, m, d), duration);
    return;
  }

  const int64_t elapsed = endEpoch - startEpoch;
  int64_t cursor = startEpoch;
  int64_t consumed = 0;
  int64_t allocated = 0;
  while (cursor < endEpoch) {
    int y, m, d;
    epochToYmd(cursor, y, m, d);
    const time_t cursorTime = static_cast<time_t>(cursor);
    struct tm nextDay{};
    if (localtime_r(&cursorTime, &nextDay) == nullptr) {
      if (duration > allocated) add(context, ymdKey(y, m, d), duration - allocated);
      return;
    }
    nextDay.tm_hour = 0;
    nextDay.tm_min = 0;
    nextDay.tm_sec = 0;
    nextDay.tm_isdst = -1;
    nextDay.tm_mday++;
    const int64_t boundary = static_cast<int64_t>(mktime(&nextDay));
    const int64_t segmentEnd = boundary > cursor ? std::min(boundary, endEpoch) : endEpoch;
    const int64_t segment = segmentEnd - cursor;
    const int64_t nextAllocated = (duration * (consumed + segment)) / elapsed;
    const int64_t contribution = nextAllocated - allocated;
    if (contribution > 0) add(context, ymdKey(y, m, d), contribution);
    allocated = nextAllocated;
    consumed += segment;
    cursor = segmentEnd;
  }
  if (allocated < duration) {
    int y, m, d;
    epochToYmd(endEpoch - 1, y, m, d);
    add(context, ymdKey(y, m, d), duration - allocated);
  }
}

void addSessionSeconds(std::map<int, int64_t>& days, int64_t start, int64_t end, int64_t duration) {
  distributeSession(
      &days,
      [](void* context, int key, int64_t seconds) { (*static_cast<std::map<int, int64_t>*>(context))[key] += seconds; },
      start, end, duration);
}

// Read one JSONL line from `file` starting at current position into `buf`.
// Returns bytes read (excluding newline), 0 on EOF, -1 on error, -2 when
// the complete line was discarded because it did not fit in the buffer, or -3
// for an empty line.
int readLine(HalFile& file, char* buf, size_t bufSize) {
  if (!file || !file.isOpen() || !buf || bufSize == 0) return -1;
  size_t pos = 0;
  bool tooLong = false;
  while (true) {
    int c = file.read();
    if (c < 0) {
      if (file.position() < file.fileSize64()) return -1;
      if (pos == 0 && !tooLong) return 0;  // EOF at start
      break;
    }
    if (c == '\n') {
      if (pos == 0 && !tooLong) return -3;
      break;
    }
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

bool jsonGetInt(const char* json, const char* key, int64_t& val);

bool parseAggregate(const char* line, AggregateSummary& summary) {
  int64_t days = 0, record = 0, current = 0, last = 0;
  if (!jsonGetInt(line, "seconds", summary.totalSeconds) || !jsonGetInt(line, "days", days) ||
      !jsonGetInt(line, "record", record) || !jsonGetInt(line, "current", current) || !jsonGetInt(line, "last", last))
    return false;
  const int64_t maxInt = std::numeric_limits<int>::max();
  if (summary.totalSeconds < 0 || summary.totalSeconds > maxInt * 60 || days < 0 || days > maxInt || record < 0 ||
      record > days || current < 0 || current > record || last < 0 || last > 99991231)
    return false;
  if (days > 0) {
    const int year = static_cast<int>(last / 10000);
    const int month = static_cast<int>(last / 100 % 100);
    const int day = static_cast<int>(last % 100);
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) return false;
  } else if (last != 0 || summary.totalSeconds != 0) {
    return false;
  }
  summary.totalDays = static_cast<int>(days);
  summary.recordStreak = static_cast<int>(record);
  summary.currentStreak = static_cast<int>(current);
  summary.lastDay = static_cast<int>(last);
  summary.hasDates = strstr(line, "\"dates\"") != nullptr;
  return true;
}

bool loadAggregate(AggregateSummary& summary) {
  HalFile file = Storage.open(SUMMARY_FILE, O_RDONLY);
  if (!file || !file.isOpen()) return false;
  char line[192];
  return readLine(file, line, sizeof(line)) > 0 && parseAggregate(line, summary);
}

bool writeAggregateHeader(HalFile& file, const AggregateSummary& summary) {
  char line[192];
  const int length = snprintf(line, sizeof(line),
                              "{\"seconds\":%lld,\"days\":%d,\"record\":%d,\"current\":%d,\"last\":%d,\"dates\":[\n",
                              static_cast<long long>(summary.totalSeconds), summary.totalDays, summary.recordStreak,
                              summary.currentStreak, summary.lastDay);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(line)) return false;
  return writeAll(file, reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(length));
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

bool matchesTail(HalFile& file, uint64_t size, const char* line, size_t length) {
  if (size < length || file.fileSize64() != size || !file.seek64(size - length)) return false;
  for (size_t i = 0; i < length; ++i) {
    if (file.read() != static_cast<unsigned char>(line[i])) return false;
  }
  return true;
}

bool validDayKey(int key) {
  const int year = key / 10000, month = key / 100 % 100, day = key % 100;
  return year >= 1970 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month);
}

// Dates are streamed from SD; lifetime history does not grow an in-memory container.
class DayMerge {
 public:
  DayMerge(bool stored, const std::map<int, int64_t>& added)
      : file(Storage.open(stored ? SUMMARY_FILE : MIGRATION_FILE, O_RDONLY)),
        addedIt(added.begin()),
        addedEnd(added.end()),
        migration(!stored) {
    if (!file) {
      ok = false;
      return;
    }
    if (stored) {
      int c;
      do {
        c = file.read();
      } while (c >= 0 && c != '\n');
      if (c < 0) {
        ok = false;
        return;
      }
    }
    advanceStored();
  }

  bool next(int& key, bool& isNew) {
    if (!ok) return false;
    const int addedKey = addedIt == addedEnd ? 0 : addedIt->first;
    key = storedKey;
    if (addedKey && (!key || addedKey < key)) key = addedKey;
    if (!key) return false;
    isNew = key != storedKey;
    if (key == storedKey) advanceStored();
    if (key == addedKey) ++addedIt;
    return true;
  }

  bool good() const { return ok; }

 private:
  void advanceStored() {
    const int previous = storedKey;
    storedKey = 0;
    int c;
    do {
      c = file.read();
    } while (c == ',' || c == ' ' || c == '\n' || c == '\r');
    if (c == ']') return;
    while (c >= '0' && c <= '9') {
      if (storedKey > 9999123) {
        ok = false;
        return;
      }
      storedKey = storedKey * 10 + c - '0';
      c = file.read();
    }
    if (migration && storedKey == 0 && c == '\n') return;
    if (c != '\n' || !validDayKey(storedKey) || storedKey <= previous) ok = false;
  }

  HalFile file;
  std::map<int, int64_t>::const_iterator addedIt, addedEnd;
  int storedKey = 0;
  bool ok = true;
  bool migration;
};

bool stageTransaction(const Transaction& transaction, const char* line, size_t length,
                      const std::map<int, int64_t>& added) {
  char header[224];
  const auto& summary = transaction.aggregate;
  const int count =
      snprintf(header, sizeof(header),
               "{\"seconds\":%lld,\"days\":%d,\"record\":%d,\"current\":%d,\"last\":%d,"
               "\"base\":%llu,\"keep\":%llu,\"size\":%llu,\"sep\":%d}\n",
               static_cast<long long>(summary.totalSeconds), summary.totalDays, summary.recordStreak,
               summary.currentStreak, summary.lastDay, static_cast<unsigned long long>(transaction.originalSize),
               static_cast<unsigned long long>(transaction.keepStart),
               static_cast<unsigned long long>(transaction.targetSize), transaction.separator ? 1 : 0);
  if (count < 0 || static_cast<size_t>(count) >= sizeof(header)) return false;
  HalFile pending = Storage.open(PENDING_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!pending || !pending.isOpen()) return false;
  bool written = writeAll(pending, reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(count)) &&
                 writeAll(pending, reinterpret_cast<const uint8_t*>(line), length);
  DayMerge dates(transaction.aggregate.hasDates, added);
  int key = 0;
  bool isNew = false;
  while (written && dates.next(key, isNew)) {
    const int size = snprintf(header, sizeof(header), "%d\n", key);
    written = writeAll(pending, reinterpret_cast<const uint8_t*>(header), size);
  }
  written = written && dates.good() && writeAll(pending, reinterpret_cast<const uint8_t*>("0\n"), 2);
  pending.flush();
  // Close before publishing the journal; failed staging never becomes recoverable.
  if (!pending.close() || !written) return false;
  return Storage.rename(PENDING_TMP_FILE, PENDING_FILE);
}

bool installTrim(const Transaction& transaction, const char* line, size_t length) {
  if (!Storage.exists(STATS_READY_FILE)) {
    HalFile source = Storage.open(STATS_FILE, O_RDONLY);
    if (!source || source.fileSize64() != transaction.originalSize || !source.seek64(transaction.keepStart))
      return false;
    HalFile output = Storage.open(STATS_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (!output || !output.isOpen()) return false;
    // Cold-path copy scratch is bounded to 256 bytes, independent of history size.
    auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUF_SIZE);
    if (!buffer) {
      LOG_ERR("RDSTATS", "Cannot allocate trim buffer");
      return false;
    }
    uint64_t remaining = transaction.originalSize - transaction.keepStart;
    while (remaining > 0) {
      const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, COPY_BUF_SIZE));
      const int read = source.read(buffer.get(), count);
      if (read <= 0 || static_cast<size_t>(read) > count || !writeAll(output, buffer.get(), read)) return false;
      remaining -= static_cast<size_t>(read);
    }
    if ((transaction.separator && !writeAll(output, reinterpret_cast<const uint8_t*>("\n"), 1)) ||
        !writeAll(output, reinterpret_cast<const uint8_t*>(line), length))
      return false;
    output.flush();
    if (!output.close() || !source.close() || !Storage.rename(STATS_TMP_FILE, STATS_READY_FILE)) return false;
  }
  {
    HalFile ready = Storage.open(STATS_READY_FILE, O_RDONLY);
    if (!ready || !matchesTail(ready, transaction.targetSize, line, length)) return false;
  }
  // replaceFile removes the destination first. Keep the ready file on failure.
  return Storage.replaceFile(STATS_READY_FILE, STATS_FILE);
}

bool recoverTransaction() {
  if (!Storage.exists(PENDING_FILE)) return true;
  // One 512-byte buffer holds both journal header and session, outside the task stack.
  auto buffer = makeUniqueNoThrow<char[]>(MAX_LINE_BYTES);
  if (!buffer) {
    LOG_ERR("RDSTATS", "Cannot allocate recovery line");
    return false;
  }
  Transaction transaction;
  size_t length = 0;
  {
    HalFile pending = Storage.open(PENDING_FILE, O_RDONLY);
    int64_t base = 0, keep = 0, size = 0, separator = 0;
    if (!pending || readLine(pending, buffer.get(), MAX_LINE_BYTES) <= 0 ||
        !parseAggregate(buffer.get(), transaction.aggregate) || !jsonGetInt(buffer.get(), "base", base) ||
        !jsonGetInt(buffer.get(), "keep", keep) || !jsonGetInt(buffer.get(), "size", size) ||
        !jsonGetInt(buffer.get(), "sep", separator) || separator < 0 || separator > 1 || base < 0 || keep < 0 ||
        keep > base || size <= 0 || size > static_cast<int64_t>(MAX_FILE_BYTES))
      return false;
    transaction.originalSize = static_cast<uint64_t>(base);
    transaction.keepStart = static_cast<uint64_t>(keep);
    transaction.targetSize = static_cast<uint64_t>(size);
    transaction.separator = separator != 0;
    const int count = readLine(pending, buffer.get(), MAX_LINE_BYTES);
    if (count <= 0 || static_cast<size_t>(count) + 1 >= MAX_LINE_BYTES) return false;
    length = static_cast<size_t>(count);
    buffer[length++] = '\n';
    if (transaction.targetSize != transaction.originalSize - transaction.keepStart + length + separator) return false;
  }
  bool applied = false;
  {
    HalFile file = Storage.open(STATS_FILE, O_RDONLY);
    applied = file && matchesTail(file, transaction.targetSize, buffer.get(), length);
  }
  if (!applied) {
    if (transaction.keepStart > 0) {
      if (!installTrim(transaction, buffer.get(), length)) return false;
    } else {
      HalFile file = Storage.open(STATS_FILE, O_RDWR | O_CREAT);
      if (!file || file.fileSize64() < transaction.originalSize || file.fileSize64() > transaction.targetSize ||
          !file.truncate(transaction.originalSize) || !file.seek64(transaction.originalSize) ||
          (transaction.separator && !writeAll(file, reinterpret_cast<const uint8_t*>("\n"), 1)) ||
          !writeAll(file, reinterpret_cast<const uint8_t*>(buffer.get()), length))
        return false;
      file.flush();
      if (!file.close()) return false;
    }
  }
  {
    HalFile pending = Storage.open(PENDING_FILE, O_RDONLY);
    if (!pending || readLine(pending, buffer.get(), MAX_LINE_BYTES) <= 0 ||
        readLine(pending, buffer.get(), MAX_LINE_BYTES) <= 0)
      return false;
    HalFile summary = Storage.open(SUMMARY_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (!summary || !writeAggregateHeader(summary, transaction.aggregate)) return false;
    int previous = 0, count = 0;
    while (true) {
      if (readLine(pending, buffer.get(), MAX_LINE_BYTES) <= 0) return false;
      int key = 0;
      const char* p = buffer.get();
      while (*p >= '0' && *p <= '9') {
        if (key > 9999123) return false;
        key = key * 10 + *p++ - '0';
      }
      if (*p) return false;
      if (key == 0) break;
      if (!validDayKey(key) || key <= previous || ++count > transaction.aggregate.totalDays) return false;
      const int size = snprintf(buffer.get(), MAX_LINE_BYTES, "%s%d\n", previous ? "," : "", key);
      if (!writeAll(summary, reinterpret_cast<const uint8_t*>(buffer.get()), size)) return false;
      previous = key;
    }
    if (pending.position() != pending.fileSize64() || !writeAll(summary, reinterpret_cast<const uint8_t*>("]}\n"), 3))
      return false;
    summary.flush();
    if (!summary.close()) return false;
  }
  if (!Storage.replaceFile(SUMMARY_TMP_FILE, SUMMARY_FILE)) return false;
  return Storage.remove(PENDING_FILE);
}

bool readSessionContributions(void* context, DayConsumer add) {
  HalFile file = Storage.open(STATS_FILE, O_RDONLY);
  if (!file) return !Storage.exists(STATS_FILE);
  if (file.isDirectory()) return false;
  auto buffer = makeUniqueNoThrow<char[]>(MAX_LINE_BYTES);
  if (!buffer) {
    LOG_ERR("RDSTATS", "Cannot allocate statistics line");
    return false;
  }
  while (true) {
    const int count = readLine(file, buffer.get(), MAX_LINE_BYTES);
    if (count == -1) return false;
    if (count == 0) return true;
    if (count < 0) continue;
    int64_t start = 0, end = 0, duration = 0;
    if (!jsonGetInt(buffer.get(), "s", start) || !jsonGetInt(buffer.get(), "d", duration) || start <= 0 ||
        start > 253402300799LL || duration <= 0 || duration > 86400)
      continue;
    jsonGetInt(buffer.get(), "e", end);
    if (end < 0 || end > 253402300799LL) continue;
    distributeSession(context, add, start, end, duration);
  }
}

bool readSessionDays(std::map<int, int64_t>& days, size_t limit) {
  struct Window {
    std::map<int, int64_t>& days;
    size_t limit;
  } window{days, limit};
  return readSessionContributions(&window, [](void* context, int key, int64_t seconds) {
    auto& state = *static_cast<Window*>(context);
    if (state.limit && state.days.size() >= state.limit && state.days.find(key) == state.days.end()) {
      if (key < state.days.begin()->first) return;
      state.days.erase(state.days.begin());
    }
    state.days[key] += seconds;
  });
}

bool buildLegacyDates(AggregateSummary* aggregate) {
  constexpr int WINDOW_YEARS = 128;
  constexpr size_t WINDOW_BYTES = WINDOW_YEARS * 366 / 8;
  struct CalendarScan {
    int first = 10000;
    int last = 1970;
    int year = 0;
    int nextYear = 10000;
    int64_t seconds = 0;
    uint8_t* days = nullptr;
  } scan;
  if (!readSessionContributions(&scan, [](void* context, int key, int64_t seconds) {
        auto& state = *static_cast<CalendarScan*>(context);
        state.first = std::min(state.first, key / 10000);
        state.last = std::max(state.last, key / 10000);
        state.seconds += seconds;
      }))
    return false;
  // 5,856 bytes bound date passes to 63 across the supported calendar; empty ranges are skipped.
  auto calendar = makeUniqueNoThrow<uint8_t[]>(WINDOW_BYTES);
  if (!calendar) {
    LOG_ERR("RDSTATS", "Cannot allocate migration calendar");
    return false;
  }
  scan.days = calendar.get();
  HalFile migration = Storage.open(MIGRATION_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!migration) return false;
  int previous = 0, run = 0;
  for (scan.year = scan.first; scan.year <= scan.last; scan.year = scan.nextYear) {
    memset(scan.days, 0, WINDOW_BYTES);
    scan.nextYear = 10000;
    if (!readSessionContributions(&scan, [](void* context, int key, int64_t) {
          auto& state = *static_cast<CalendarScan*>(context);
          const int year = key / 10000;
          if (year >= state.year + WINDOW_YEARS) {
            state.nextYear = std::min(state.nextYear, year);
            return;
          }
          if (year < state.year) return;
          const int month = key / 100 % 100;
          int ordinal = (year - state.year) * 366 + key % 100 - 1;
          for (int m = 1; m < month; ++m) ordinal += daysInMonth(year, m);
          state.days[ordinal / 8] |= 1U << (ordinal % 8);
        }))
      return false;
    for (int year = scan.year; year <= std::min(scan.last, scan.year + WINDOW_YEARS - 1); ++year) {
      int ordinal = (year - scan.year) * 366;
      for (int month = 1; month <= 12; ++month) {
        for (int day = 1; day <= daysInMonth(year, month); ++day, ++ordinal) {
          if (!(scan.days[ordinal / 8] & (1U << (ordinal % 8)))) continue;
          const int key = ymdKey(year, month, day);
          char line[16];
          const int size = snprintf(line, sizeof(line), "%d\n", key);
          if (!writeAll(migration, reinterpret_cast<const uint8_t*>(line), size)) return false;
          if (aggregate) {
            ++aggregate->totalDays;
            run = previous != 0 && key == nextDayKey(previous) ? run + 1 : 1;
            aggregate->recordStreak = std::max(aggregate->recordStreak, run);
            aggregate->currentStreak = run;
            aggregate->lastDay = key;
          }
          previous = key;
        }
      }
    }
  }
  if (!writeAll(migration, reinterpret_cast<const uint8_t*>("0\n"), 2)) return false;
  migration.flush();
  if (!migration.close()) return false;
  if (aggregate) aggregate->totalSeconds = scan.seconds;
  return true;
}

bool prepareAggregate(AggregateSummary& aggregate, std::map<int, int64_t>& contributions, int64_t start, int64_t end,
                      uint32_t duration) {
  const bool haveAggregate = loadAggregate(aggregate);
  if (!haveAggregate && Storage.exists(SUMMARY_FILE)) {
    LOG_ERR("RDSTATS", "Invalid aggregate; preserving history for recovery");
    return false;
  }
  if (!aggregate.hasDates && !buildLegacyDates(haveAggregate ? nullptr : &aggregate)) return false;
  addSessionSeconds(contributions, start, end, duration);
  const int64_t maxSeconds = static_cast<int64_t>(std::numeric_limits<int>::max()) * 60;
  if (aggregate.totalSeconds > maxSeconds - duration) return false;
  aggregate.totalSeconds += duration;
  int previous = 0, run = 0;
  DayMerge dates(aggregate.hasDates, contributions);
  int key = 0;
  bool isNew = false;
  while (dates.next(key, isNew)) {
    if (isNew && key != aggregate.lastDay) ++aggregate.totalDays;
    run = previous != 0 && key == nextDayKey(previous) ? run + 1 : 1;
    if (key == aggregate.lastDay) run = std::max(run, aggregate.currentStreak);
    aggregate.recordStreak = std::max(aggregate.recordStreak, run);
    previous = key;
  }
  if (!dates.good()) return false;
  if (previous >= aggregate.lastDay) {
    aggregate.lastDay = previous;
    aggregate.currentStreak = run;
  }
  return true;
}

}  // namespace

namespace ReadingStats {

bool exists() { return Storage.exists(STATS_FILE) || Storage.exists(SUMMARY_FILE) || Storage.exists(PENDING_FILE); }

void retryDeferred() {
  for (auto& session : deferred) {
    if (session.duration == 0) continue;
    if (!appendSession(session.book, session.document, session.start, session.end, session.duration, session.startBp,
                       session.endBp))
      return;
    if (session.onSaved)
      session.onSaved(session.book, session.document, session.start, session.end, session.duration, session.startBp,
                      session.endBp);
    session.duration = 0;
  }
}

bool deferSession(const char* bookPath, const char* document, int64_t start, int64_t end, uint32_t duration,
                  uint16_t startBp, uint16_t endBp, SessionCompletion onSaved) {
  if (!bookPath || !document || strlen(bookPath) >= sizeof(deferred[0].book) ||
      strlen(document) >= sizeof(deferred[0].document) || duration == 0)
    return false;
  for (auto& session : deferred) {
    if (session.duration != 0) continue;
    size_t length = 0;
    if (!formatSession(session.book, length, bookPath, document, start, end, duration, startBp, endBp)) {
      LOG_ERR("RDSTATS", "Cannot defer an invalid or oversized session");
      return false;
    }
    strcpy(session.book, bookPath);
    strcpy(session.document, document);
    session.start = start;
    session.end = end;
    session.duration = duration;
    session.startBp = startBp;
    session.endBp = endBp;
    session.onSaved = onSaved;
    return true;
  }
  LOG_ERR("RDSTATS", "Deferred session queue is full");
  return false;
}

bool recoverPending() {
  retryDeferred();
  return recoverTransaction();
}

bool readSummaryJson(char* buffer, size_t capacity, int64_t todayEpoch) {
  if (!buffer || capacity == 0 || !recoverPending()) return false;
  AggregateSummary aggregate;
  if (!loadAggregate(aggregate)) {
    if (Storage.exists(SUMMARY_FILE)) return false;
    return snprintf(buffer, capacity, "{}") == 2 && capacity > 2;
  }
  int y, m, d;
  epochToYmd(todayEpoch > 0 ? todayEpoch : static_cast<int64_t>(time(nullptr)), y, m, d);
  const int today = ymdKey(y, m, d);
  const int length =
      snprintf(buffer, capacity,
               "{\"seconds\":%lld,\"days\":%d,\"record\":%d,\"current\":%d,\"last\":%d,"
               "\"today\":%d,\"yesterday\":%d}",
               static_cast<long long>(aggregate.totalSeconds), aggregate.totalDays, aggregate.recordStreak,
               aggregate.currentStreak, aggregate.lastDay, today, previousDayKey(today));
  return length >= 0 && static_cast<size_t>(length) < capacity;
}

bool appendSession(const char* bookPath, const char* document, int64_t startEpoch, int64_t endEpoch,
                   uint32_t durationSeconds, uint16_t startProgressBp, uint16_t endProgressBp) {
  if (!recoverTransaction()) {
    LOG_ERR("RDSTATS", "Previous session is still pending");
    return false;
  }
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
  size_t linePos = 0;
  if (!formatSession(line, linePos, bookPath, document, startEpoch, endEpoch, durationSeconds, startProgressBp,
                     endProgressBp)) {
    LOG_ERR("RDSTATS", "Invalid or oversized reading session");
    return false;
  }

  Transaction transaction;
  {
    HalFile file = Storage.open(STATS_FILE, O_RDONLY);
    if (!file && Storage.exists(STATS_FILE)) return false;
    transaction.originalSize = file ? file.fileSize64() : 0;
    // Exact retries are already durable, including an uncertain close/rename result.
    if (file && matchesTail(file, transaction.originalSize, line, linePos)) return true;
    if (transaction.originalSize > 0) {
      if (!file.seek64(transaction.originalSize - 1)) return false;
      const int last = file.read();
      if (last < 0) return false;
      transaction.separator = last != '\n';
    }
    const size_t appendBytes = linePos + (transaction.separator ? 1 : 0);
    if (transaction.originalSize > MAX_FILE_BYTES || appendBytes > MAX_FILE_BYTES - transaction.originalSize) {
      if (!file.seek64(transaction.originalSize / 2)) return false;
      while (file.position() < transaction.originalSize) {
        const int c = file.read();
        if (c < 0) return false;
        if (c == '\n') break;
      }
      transaction.keepStart = file.position();
      if (transaction.keepStart == transaction.originalSize) transaction.separator = false;
    }
  }
  transaction.targetSize = transaction.originalSize - transaction.keepStart + linePos + (transaction.separator ? 1 : 0);
  std::map<int, int64_t> contributions;
  if (!prepareAggregate(transaction.aggregate, contributions, startEpoch, endEpoch, durationSeconds)) return false;
  if (!stageTransaction(transaction, line, linePos, contributions)) {
    LOG_ERR("RDSTATS", "Cannot stage reading session");
    return false;
  }
  // The published journal owns the session even if applying it must wait for SD recovery.
  if (!recoverTransaction()) LOG_ERR("RDSTATS", "Session journaled; application deferred");
  return true;
}

ReadingStatsSummary analyze(int maxDays, int64_t todayEpoch) {
  ReadingStatsSummary sum;
  if (!recoverPending()) return sum;

  // Per-day minutes: ymdKey → total seconds → minutes at the end.
  std::map<int, int64_t> daySeconds;
  if (!readSessionDays(daySeconds, maxDays > 0 ? static_cast<size_t>(maxDays) : 0)) return sum;
  AggregateSummary aggregate;
  bool haveAggregate = loadAggregate(aggregate);
  if (!haveAggregate && !Storage.exists(SUMMARY_FILE) && Storage.exists(STATS_FILE))
    haveAggregate = buildLegacyDates(&aggregate);
  if (!haveAggregate && (Storage.exists(SUMMARY_FILE) || Storage.exists(STATS_FILE))) return sum;
  sum.available = true;

  if (daySeconds.empty()) {
    if (haveAggregate) {
      sum.totalMinutes = static_cast<int>(aggregate.totalSeconds / 60);
      sum.totalDays = aggregate.totalDays;
      sum.averageMinutes = sum.totalDays > 0 ? sum.totalMinutes / sum.totalDays : 0;
      int y, m, d;
      epochToYmd(todayEpoch, y, m, d);
      const int todayKey = ymdKey(y, m, d);
      sum.streakDays =
          aggregate.lastDay == todayKey || aggregate.lastDay == previousDayKey(todayKey) ? aggregate.currentStreak : 0;
      sum.recordStreakDays = aggregate.recordStreak;
    }
    return sum;
  }

  // Convert per-day seconds → minutes, collect into daily vector.
  int todayY, todayM, todayD;
  epochToYmd(todayEpoch, todayY, todayM, todayD);
  int todayKey = ymdKey(todayY, todayM, todayD);

  // Sort by key ascending, keep up to maxDays most recent.
  sum.daily.reserve(maxDays > 0 ? std::min(daySeconds.size(), static_cast<size_t>(maxDays)) : daySeconds.size());
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

  // Current streak includes today when today has reading; otherwise it ends yesterday.
  int streak = 0;
  const auto todayIt = daySeconds.find(todayKey);
  int cursor = todayIt != daySeconds.end() && todayIt->second > 0 ? todayKey : yesterdayKey;
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

  int record = 0;
  int run = 0;
  int prev = 0;
  for (const auto& day : daySeconds) {
    const int k = day.first;
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

  if (haveAggregate) {
    sum.totalMinutes = static_cast<int>(aggregate.totalSeconds / 60);
    sum.totalDays = aggregate.totalDays;
    sum.averageMinutes = sum.totalDays > 0 ? sum.totalMinutes / sum.totalDays : 0;
    sum.streakDays =
        aggregate.lastDay == todayKey || aggregate.lastDay == previousDayKey(todayKey) ? aggregate.currentStreak : 0;
    sum.recordStreakDays = aggregate.recordStreak;
  }

  return sum;
}

}  // namespace ReadingStats
