# Reading statistics verification

## Host checks

Use the existing toolchain and host-test cache:

```sh
cmake -S test -B .cache/host-tests
cmake --build .cache/host-tests --parallel 2
ctest --test-dir .cache/host-tests --output-on-failure
node test/reading_stats/viewer.test.cjs
./bin/clang-format-fix -g
pio run -e default
```

The C++ storage double models the SDK's remove-before-rename replacement.
Tests cover uncertain closes, partial appends, journal/summary failures,
compaction recovery, distinct archived dates, bounded migration scans, RAM
deferral and completion callbacks, midnight, DST, and legacy records. Node
checks the two browser views without a live device. These checks do not prove
SD durability, device performance, HTTP transport, or plugin delivery on hardware.

## Required device checks

The human owner must test hardware before opening a PR. Use a spare SD card,
back up the entire `/ReadingStats` directory, and keep any recovery files intact.

1. Read for at least 60 seconds, including a dwell on the last displayed page.
   Close the book, inspect device statistics and both browser views, then
   restart. Time must be counted once, not disappear or double after restart.
2. Repeat sleep/wake and reader exit. With a subscribed test integration,
   verify one accepted session; deferred delivery may occur after `reader.exit`.
3. On the test setup, inject write/rename failures at append, summary replacement,
   and compaction. Restore storage before deep sleep/reboot when testing RAM
   fallback. A journaled session must replay once; failed recovery must return
   HTTP 503 rather than a successful empty history. Four RAM slots are finite
   and are not retained through reboot/deep sleep.
4. With a near-1-MB fixture, trigger compaction. Lifetime totals and distinct-day
   counts must remain unchanged except for the new session; the detail log,
   per-book totals, and charts may cover a shorter interval.
5. Read across local midnight and an Oslo DST transition. A session ending
   exactly at midnight must not create an unread day. After two days without
   reading, current streak must be zero while the record remains.
6. Open the browser in another timezone around midnight. Device streak must
   follow the API's device calendar; labeled browser-local charts follow the
   browser calendar. Changing the device timezone does not reassign stored dates.
7. Inspect statistics in all four orientations. Check week labels, calendar
   placement, and zero-height bars for days without reading.
8. On C3, measure exit/sleep and migration latency with a long history and slow
   SD. Track minimum internal free heap, largest block, and task stack high-water
   marks; aim for more than 50 KB free internal heap. Repeat reader/statistics/
   Wi-Fi transitions and watch for leaks, freezes, resets, or duplicate events.

Migration uses one checked 5,856-byte calendar buffer and streams date keys from
SD. It skips empty date ranges; a full 1970–9999 range requires at most 63 date
passes plus discovery. This bounds memory and read passes, not wall-clock latency.
The four deferred records reserve approximately 2.3 KB static RAM on C3.
Old summaries retain counters, but dates removed before the date index existed
cannot be reconstructed without a backup.
