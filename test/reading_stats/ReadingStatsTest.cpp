#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <string>

#include "src/reading_stats/ReadingStats.h"

namespace {
constexpr const char* SESSIONS = "/ReadingStats/sessions.jsonl";
constexpr const char* SUMMARY = "/ReadingStats/summary.json";
constexpr const char* PENDING = "/ReadingStats/pending.jsonl";

int64_t epoch(int year, int month, int day, int hour = 12, int minute = 0, int second = 0) {
  std::tm date{};
  date.tm_year = year - 1900;
  date.tm_mon = month - 1;
  date.tm_mday = day;
  date.tm_hour = hour;
  date.tm_min = minute;
  date.tm_sec = second;
  date.tm_isdst = -1;
  return std::mktime(&date);
}

class ReadingStatsTest : public testing::Test {
 protected:
  void SetUp() override {
    statsTest::reset();
    setenv("TZ", "UTC0", 1);
    tzset();
  }
  bool append(int64_t start, uint32_t duration = 60, int64_t end = 0) {
    return ReadingStats::appendSession("/Books/test.epub", "", start, end ? end : start + duration, duration, 0, 10);
  }
};

TEST_F(ReadingStatsTest, AccumulatesSecondsWithoutBootstrapRounding) {
  const auto now = epoch(2026, 10, 1);
  statsTest::files[SESSIONS] = "{\"s\":" + std::to_string(now) + ",\"d\":59}\n";
  ASSERT_TRUE(append(now + 120, 1));
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

TEST_F(ReadingStatsTest, ReadFailuresAreUnavailableRatherThanEmptyOrPartialTotals) {
  const auto now = epoch(2026, 10, 1);
  ASSERT_TRUE(append(now));
  ASSERT_TRUE(append(now + 86400));
  statsTest::failRead = SESSIONS;
  EXPECT_FALSE(ReadingStats::analyze(1, now).available);
  statsTest::failRead = SUMMARY;
  EXPECT_FALSE(ReadingStats::analyze(1, now).available);
  statsTest::failRead.clear();
  statsTest::failOpen = SUMMARY;
  EXPECT_FALSE(ReadingStats::analyze(1, now).available);
  statsTest::failOpen.clear();
  const auto recovered = ReadingStats::analyze(1, now);
  EXPECT_TRUE(recovered.available);
  EXPECT_EQ(recovered.totalMinutes, 2);
  EXPECT_EQ(recovered.totalDays, 2);
}

TEST_F(ReadingStatsTest, EmptyHistoryIsAvailable) {
  EXPECT_TRUE(ReadingStats::analyze(366, epoch(2026, 10, 1)).available);
}

TEST_F(ReadingStatsTest, MidnightEndpointDoesNotCountUnreadDay) {
  const auto midnight = epoch(2026, 10, 2, 0);
  ASSERT_TRUE(append(midnight - 60, 60, midnight));
  const auto result = ReadingStats::analyze(0, midnight);
  EXPECT_EQ(result.totalDays, 1);
  EXPECT_EQ(result.todayMinutes, 0);
  EXPECT_EQ(result.streakDays, 1);
}

TEST_F(ReadingStatsTest, IgnoresZeroSecondDaySegments) {
  const auto midnight = epoch(2026, 10, 2, 0);
  ASSERT_TRUE(append(midnight - 1, 1, midnight + 99));
  const auto result = ReadingStats::analyze(0, midnight);
  ASSERT_EQ(result.daily.size(), 1u);
  EXPECT_EQ(result.daily.front().day, 2);
  EXPECT_EQ(result.totalDays, 1);
}

TEST_F(ReadingStatsTest, CurrentStreakExpiresButRecordRemains) {
  ASSERT_TRUE(append(epoch(2026, 10, 1)));
  ASSERT_TRUE(append(epoch(2026, 10, 2)));
  EXPECT_EQ(ReadingStats::analyze(0, epoch(2026, 10, 3)).streakDays, 2);
  const auto result = ReadingStats::analyze(0, epoch(2026, 10, 10));
  EXPECT_EQ(result.streakDays, 0);
  EXPECT_EQ(result.recordStreakDays, 2);
}

TEST_F(ReadingStatsTest, RevisitingEarlierKnownDateDoesNotAddADay) {
  ASSERT_TRUE(append(epoch(2026, 10, 1)));
  ASSERT_TRUE(append(epoch(2026, 10, 2)));
  ASSERT_TRUE(append(epoch(2026, 10, 1, 13)));
  const auto result = ReadingStats::analyze(0, epoch(2026, 10, 2));
  EXPECT_EQ(result.totalDays, 2);
  EXPECT_EQ(result.streakDays, 2);
}

TEST_F(ReadingStatsTest, FallDstUsesNextLocalMidnight) {
  setenv("TZ", "Europe/Oslo", 1);
  tzset();
  const auto start = epoch(2026, 10, 25, 0);
  const auto end = epoch(2026, 10, 26, 0);
  ASSERT_EQ(end - start, 25 * 3600);
  ASSERT_TRUE(append(start, 3600, end));
  const auto result = ReadingStats::analyze(0, end);
  ASSERT_EQ(result.daily.size(), 1u);
  EXPECT_EQ(result.daily.front().day, 25);
  EXPECT_EQ(result.daily.front().minutes, 60);
  EXPECT_EQ(result.totalDays, 1);
}

TEST_F(ReadingStatsTest, FailedSummaryRenameRecoversWithoutDuplicates) {
  const auto now = epoch(2026, 10, 1);
  ASSERT_TRUE(append(now));
  statsTest::failRename = SUMMARY;
  ASSERT_TRUE(append(now + 120));
  ASSERT_TRUE(statsTest::files.count(PENDING));
  ASSERT_FALSE(statsTest::files.count(SUMMARY));
  ASSERT_TRUE(append(now + 120));
  const auto result = ReadingStats::analyze(0, now);
  EXPECT_EQ(result.totalMinutes, 2);
  EXPECT_EQ(std::count(statsTest::files[SESSIONS].begin(), statsTest::files[SESSIONS].end(), '\n'), 2);
}

TEST_F(ReadingStatsTest, FailedLogCloseRecoversJournal) {
  const auto now = epoch(2026, 10, 1);
  statsTest::failClose = SESSIONS;
  ASSERT_TRUE(append(now));
  ASSERT_TRUE(ReadingStats::recoverPending());
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
  ASSERT_TRUE(append(now));
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

TEST_F(ReadingStatsTest, FailedStagingNeverChangesCommittedSummary) {
  const auto now = epoch(2026, 10, 1);
  ASSERT_TRUE(append(now));
  const auto summary = statsTest::files[SUMMARY];
  statsTest::failWrite = "/ReadingStats/pending.jsonl.tmp";
  ASSERT_FALSE(append(now + 120));
  EXPECT_EQ(statsTest::files[SUMMARY], summary);
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

TEST_F(ReadingStatsTest, JournalSurvivesLogOpenFailureAndAnotherAppend) {
  const auto now = epoch(2026, 10, 1);
  statsTest::failWrite = SESSIONS;
  ASSERT_TRUE(append(now));
  ASSERT_TRUE(statsTest::files.count(PENDING));
  ASSERT_TRUE(append(now + 120));
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 2);
}

TEST_F(ReadingStatsTest, TrimRenameFailurePreservesReadyCopy) {
  const auto now = epoch(2026, 10, 1);
  ASSERT_TRUE(append(now));
  auto& file = statsTest::files[SESSIONS];
  file.insert(0, 1024 * 1024 - file.size(), '\n');
  statsTest::failRename = SESSIONS;
  ASSERT_TRUE(append(now + 120));
  EXPECT_FALSE(statsTest::files.count(SESSIONS));
  EXPECT_TRUE(statsTest::files.count("/ReadingStats/sessions.jsonl.ready"));
  ASSERT_TRUE(ReadingStats::recoverPending());
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 2);
  EXPECT_LT(statsTest::files[SESSIONS].size(), 1024u * 1024u);
}

TEST_F(ReadingStatsTest, SummaryStillWorksWithoutDetailedLog) {
  const auto now = epoch(2026, 10, 1);
  ASSERT_TRUE(append(now));
  statsTest::files.erase(SESSIONS);
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

TEST_F(ReadingStatsTest, InvalidSummaryIsNotOverwritten) {
  statsTest::files[SUMMARY] = "{\"seconds\":-1,\"days\":1,\"record\":1,\"current\":1,\"last\":20261001}\n";
  EXPECT_FALSE(append(epoch(2026, 10, 2)));
}

TEST_F(ReadingStatsTest, LongAndBlankLinesDoNotHideLaterSessions) {
  const auto now = epoch(2026, 10, 1);
  statsTest::files[SESSIONS] = std::string(800, 'x') + "\n\n{\"s\":" + std::to_string(now) + ",\"d\":60}\n";
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

TEST_F(ReadingStatsTest, JournalReplaysPartialAppendAfterRestart) {
  const auto now = epoch(2026, 10, 1);
  ASSERT_TRUE(append(now));
  const auto original = statsTest::files[SESSIONS];
  statsTest::failWrite = SESSIONS;
  ASSERT_TRUE(append(now + 120));
  statsTest::files[SESSIONS] = original + "{\"s\":123";
  ASSERT_TRUE(ReadingStats::recoverPending());
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 2);
  EXPECT_EQ(std::count(statsTest::files[SESSIONS].begin(), statsTest::files[SESSIONS].end(), '\n'), 2);
}

TEST_F(ReadingStatsTest, PendingCannotBeOverwrittenByNextFailedRecovery) {
  const auto now = epoch(2026, 10, 1);
  statsTest::failWrite = SESSIONS;
  ASSERT_TRUE(append(now));
  const auto pending = statsTest::files[PENDING];
  statsTest::failWrite = SESSIONS;
  ASSERT_FALSE(append(now + 120));
  EXPECT_EQ(statsTest::files[PENDING], pending);
  ASSERT_TRUE(append(now + 120));
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 2);
}

TEST_F(ReadingStatsTest, InterruptedSummaryWriteKeepsJournalAuthoritative) {
  const auto now = epoch(2026, 10, 1);
  statsTest::failWrite = "/ReadingStats/summary.json.tmp";
  ASSERT_TRUE(append(now));
  ASSERT_TRUE(statsTest::files.count(PENDING));
  ASSERT_TRUE(ReadingStats::recoverPending());
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

TEST_F(ReadingStatsTest, JournalRemovalFailureIsIdempotent) {
  const auto now = epoch(2026, 10, 1);
  statsTest::failRemove = PENDING;
  ASSERT_TRUE(append(now));
  ASSERT_TRUE(append(now));
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

TEST_F(ReadingStatsTest, DeferredExitSurvivesCallerAndRetriesOnce) {
  const auto now = epoch(2026, 10, 1);
  ASSERT_TRUE(ReadingStats::deferSession("/Books/test.epub", "", now, now + 60, 60, 0, 10));
  ReadingStats::retryDeferred();
  ReadingStats::retryDeferred();
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

TEST_F(ReadingStatsTest, DateIndexPreventsDuplicateArchivedDays) {
  const auto day1 = epoch(2026, 10, 1), day2 = epoch(2026, 10, 2);
  ASSERT_TRUE(append(day1));
  ASSERT_TRUE(append(day2));
  // The first day has already left the detailed history after compaction.
  const auto log = statsTest::files[SESSIONS];
  statsTest::files[SESSIONS] = log.substr(log.find('\n') + 1);
  ASSERT_TRUE(append(day1 + 120));
  const auto result = ReadingStats::analyze(0, day2);
  EXPECT_EQ(result.totalDays, 2);
  EXPECT_EQ(result.totalMinutes, 3);
  EXPECT_EQ(result.streakDays, 2);
}

TEST_F(ReadingStatsTest, BridgingOlderGapUpdatesRecordAndCurrentStreak) {
  ASSERT_TRUE(append(epoch(2026, 10, 1)));
  ASSERT_TRUE(append(epoch(2026, 10, 3)));
  ASSERT_TRUE(append(epoch(2026, 10, 2)));
  const auto result = ReadingStats::analyze(0, epoch(2026, 10, 3));
  EXPECT_EQ(result.totalDays, 3);
  EXPECT_EQ(result.recordStreakDays, 3);
  EXPECT_EQ(result.streakDays, 3);
}

TEST_F(ReadingStatsTest, SummaryAccessorRecoversBeforeReturningTotals) {
  const auto now = epoch(2026, 10, 1);
  statsTest::failRename = SUMMARY;
  ASSERT_TRUE(append(now));
  char summary[192];
  ASSERT_TRUE(ReadingStats::readSummaryJson(summary, sizeof(summary)));
  EXPECT_NE(std::string(summary).find("\"seconds\":60"), std::string::npos);
  EXPECT_FALSE(statsTest::files.count(PENDING));
}

TEST_F(ReadingStatsTest, UnterminatedLegacyRecordGetsASeparator) {
  const auto now = epoch(2026, 10, 1);
  statsTest::files[SESSIONS] = "{\"s\":" + std::to_string(now) + ",\"d\":60}";
  ASSERT_TRUE(append(now + 120));
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 2);
  EXPECT_EQ(std::count(statsTest::files[SESSIONS].begin(), statsTest::files[SESSIONS].end(), '\n'), 2);
}

TEST_F(ReadingStatsTest, InvalidDeferredEntryCannotBlockValidSession) {
  const auto now = epoch(2026, 10, 1);
  const std::string oversized(450, 'x');
  EXPECT_FALSE(ReadingStats::deferSession(oversized.c_str(), "", now, now + 60, 60, 0, 10));
  const std::string escaped(100, '\x01');
  EXPECT_FALSE(ReadingStats::deferSession(escaped.c_str(), "", now, now + 60, 60, 0, 10));
  EXPECT_FALSE(ReadingStats::deferSession("/ok.epub", "", now, now + 90000, 90000, 0, 10));
  ASSERT_TRUE(ReadingStats::deferSession("/ok.epub", "", now, now + 60, 60, 0, 10));
  ReadingStats::retryDeferred();
  EXPECT_EQ(ReadingStats::analyze(0, now).totalMinutes, 1);
}

int completions = 0;
void complete(const char*, const char*, int64_t, int64_t, uint32_t, uint16_t, uint16_t) { ++completions; }

TEST_F(ReadingStatsTest, DeferredCompletionRunsOnceAfterDurableAcceptance) {
  const auto now = epoch(2026, 10, 1);
  completions = 0;
  ASSERT_TRUE(ReadingStats::deferSession("/ok.epub", "", now, now + 60, 60, 0, 10, complete));
  statsTest::failWrite = "/ReadingStats/pending.jsonl.tmp";
  ReadingStats::retryDeferred();
  EXPECT_EQ(completions, 0);
  ReadingStats::retryDeferred();
  ReadingStats::retryDeferred();
  EXPECT_EQ(completions, 1);
}

TEST_F(ReadingStatsTest, MigratesManyDatesWithFixedCalendarScratch) {
  std::string log;
  const auto first = epoch(2000, 1, 1);
  for (int day = 0; day < 10000; ++day) {
    log += "{\"s\":" + std::to_string(first + day * 86400LL) + ",\"d\":60}\n";
  }
  ASSERT_LT(log.size(), 1024u * 1024u);
  statsTest::files[SESSIONS] = std::move(log);
  ASSERT_TRUE(append(first + 10000 * 86400LL));
  char summary[192];
  ASSERT_TRUE(ReadingStats::readSummaryJson(summary, sizeof(summary), first + 10000 * 86400LL));
  EXPECT_NE(std::string(summary).find("\"days\":10001"), std::string::npos);
  EXPECT_NE(std::string(summary).find("\"current\":10001"), std::string::npos);
  const auto result = ReadingStats::analyze(366, first + 10000 * 86400LL);
  EXPECT_EQ(result.daily.size(), 366u);
  EXPECT_EQ(result.totalMinutes, 10001);
}

TEST_F(ReadingStatsTest, ApiCalendarAuthorityIsDeviceLocal) {
  const auto now = epoch(2026, 10, 2, 0);
  ASSERT_TRUE(append(now));
  char summary[192];
  ASSERT_TRUE(ReadingStats::readSummaryJson(summary, sizeof(summary), now));
  EXPECT_NE(std::string(summary).find("\"today\":20261002"), std::string::npos);
  EXPECT_NE(std::string(summary).find("\"yesterday\":20261001"), std::string::npos);
}

TEST_F(ReadingStatsTest, SparseLegacyYearsSkipEmptyCalendarWindows) {
  const auto first = epoch(1970, 1, 1), last = epoch(9999, 1, 1);
  statsTest::files[SESSIONS] = std::string(32000, '\n') + "{\"s\":" + std::to_string(first) +
                               ",\"d\":60}\n{\"s\":" + std::to_string(last) + ",\"d\":60}\n";
  const auto originalSize = statsTest::files[SESSIONS].size();
  ASSERT_TRUE(append(epoch(2026, 10, 1)));
  EXPECT_LE(statsTest::readBytes[SESSIONS], originalSize * 4 + 1024);
}
}  // namespace
