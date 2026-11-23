//  time.axd — the clock reads that only the operating system can provide.
//
//  `library "time"` means these four functions live in time.dll (Windows) /
//  libtime.so. Everything else about time — calendar maths, formatting, parsing,
//  arithmetic — is plain Axiom in stdlib/time.ax and needs no library at all.
//
//      function import (return:(time));
//
//      int64 now = timeNowMs();
//      DateTime when = timeToCivil(now);
//      print(timeFormat(when));           // 2026-11-22 00:07:18
//
//  ── two clocks, do not mix them ───────────────────────────────────────────
//
//  `timeNowMs` is the wall clock: the time a human agrees with. Print it, store
//  it, show it to a user.
//
//  `timeMonotonicNs` only moves forward. Measure durations with it, because the
//  wall clock **can jump backwards** — an NTP correction, waking from sleep, or a
//  user changing the date will do it, and a "negative elapsed time" is a real bug,
//  not a theoretical one.
//
//      int64 start = timeMonotonicNs();
//      DoWork();
//      int64 elapsedMs = (timeMonotonicNs() - start) / 1000000;
//
//  If you will display the number, use the wall clock. If you will subtract two
//  of them, use the monotonic clock.
//
//  ── UTC by default ────────────────────────────────────────────────────────
//
//  `timeNowMs` returns UTC. Local time is opt-in via `timeLocalOffsetMin`, which
//  takes the instant because daylight saving makes the offset depend on when you
//  ask. See `timeToCivilLocal` in stdlib/time.ax for the worked-out version.
//
//  ── no fallback ───────────────────────────────────────────────────────────
//
//  Reading a clock is not something the language can express, so these have no
//  Axiom body. Without time.dll the call fails with `cannot load library 'time'`.
//  Formatting and arithmetic keep working regardless — they only need an `int64`
//  that something else produced.

module "time" version "0.1.0";

library "time";

//  Milliseconds since 1970-01-01T00:00:00Z. Independent of the machine's timezone.
//  Exact to the millisecond; the underlying clock may quantise to ~1–15 ms.
function timeNowMs():int64;

//  Nanoseconds from an arbitrary origin, guaranteed non-decreasing.
//  Only differences are meaningful — the origin differs per boot and per platform.
function timeMonotonicNs():int64;

//  Sleeps for roughly `milliseconds`. Non-positive returns immediately; values
//  above one hour are clamped. "Roughly" is honest: the OS rounds up to its timer
//  granularity, and a busy scheduler makes a thread oversleep.
function timeSleepMs(int64 milliseconds):void;

//  The local timezone offset **at that instant**, in minutes east of UTC.
//  Takes the instant rather than being a constant because DST makes the offset
//  depend on when you ask: Europe/London is +0 in January and +60 in July.
//  Returns 0 when the offset cannot be determined.
function timeLocalOffsetMin(int64 epochMs):int64;
