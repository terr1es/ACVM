//  time.c — the clock reads that only the operating system can provide.
//
//  Built into time.dll / libtime.so and reached from Axiom through stdlib/time.axd.
//
//  ── why so little is here ─────────────────────────────────────────────────
//
//  Everything a program normally wants from "time" -- breaking a timestamp into
//  year/month/day, formatting it, parsing it back, adding a week, working out the
//  weekday -- is arithmetic. Calendar conversions are pure integer maths (the
//  algorithms are in stdlib/time.ax), so they live in Axiom where they cost no DLL
//  and are trivially testable.
//
//  What genuinely cannot be done in Axiom is *reading the clock*. That is four
//  functions. Keeping the boundary this narrow means a program that formats a
//  timestamp it already has needs no library at all.
//
//  ── two clocks, and why the difference matters ────────────────────────────
//
//  `timeNowMs` reads the wall clock: the time a human would agree with. It is
//  what you print, log, or store.
//
//  `timeMonotonicNs` reads a clock that only ever moves forward. Measured
//  durations must use this one, because the wall clock **can jump backwards** --
//  an NTP correction, a laptop waking from sleep, or a user editing the date will
//  all do it. Timing an operation with the wall clock and getting a negative
//  duration is a real failure mode, not a theoretical one.
//
//  Rule of thumb: if you are going to *display* the number, use the wall clock.
//  If you are going to *subtract* two of them, use the monotonic clock.

#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/// Milliseconds since the Unix epoch, UTC.
///
/// The epoch is fixed at 1970-01-01T00:00:00Z, so the value is independent of the
/// machine's timezone. Converting to local time happens in Axiom, where the offset
/// is an explicit argument rather than a hidden global.
int64_t timeNowMs(void) {
#ifdef _WIN32
    // FILETIME counts 100-nanosecond intervals since 1601-01-01. Shifting the
    // epoch by the 11644473600 seconds between then and 1970 is the standard move.
    FILETIME fileTime;
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER ticks;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return (int64_t)(ticks.QuadPart / 10000ull) - 11644473600000ll;
#else
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);
    return (int64_t)spec.tv_sec * 1000ll + (int64_t)(spec.tv_nsec / 1000000l);
#endif
}

/// Nanoseconds from an arbitrary fixed origin, guaranteed non-decreasing.
///
/// Only differences are meaningful: the origin differs per boot and per platform,
/// so the absolute value means nothing. Divide by 1000000 for milliseconds.
int64_t timeMonotonicNs(void) {
#ifdef _WIN32
    // QueryPerformanceCounter is the documented monotonic source; its frequency
    // is not 1 GHz on every machine, so it has to be divided out.
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart == 0) {
        return 0;
    }
    const int64_t ticksPerSecond = (int64_t)frequency.QuadPart;
    const int64_t whole = counter.QuadPart / ticksPerSecond;
    const int64_t remainder = counter.QuadPart % ticksPerSecond;
    // Split the division so the nanosecond scaling cannot overflow on a counter
    // that has been running for a long time.
    return whole * 1000000000ll + (remainder * 1000000000ll) / ticksPerSecond;
#else
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return (int64_t)spec.tv_sec * 1000000000ll + (int64_t)spec.tv_nsec;
#endif
}

/// Sleeps for roughly `milliseconds`. A non-positive or absurd request returns
/// immediately rather than blocking forever.
///
/// "Roughly" is honest: the OS may round up to its timer granularity, and the
/// thread may oversleep if the scheduler is busy. Do not use this as a precision
/// delay; it is for backing off, polling politely, and pacing output.
void timeSleepMs(int64_t milliseconds) {
    if (milliseconds <= 0) {
        return;
    }
    // An hour is far beyond any sensible sleep request and keeps the internal
    // conversion well inside its type.
    if (milliseconds > 3600000ll) {
        milliseconds = 3600000ll;
    }
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec request;
    request.tv_sec = (time_t)(milliseconds / 1000ll);
    request.tv_nsec = (long)((milliseconds % 1000ll) * 1000000ll);
    // Resume after a signal rather than returning early, so the caller gets the
    // full duration it asked for.
    while (nanosleep(&request, &request) == -1) {
        // EINTR leaves the remaining time in `request`; anything else stops.
    }
#endif
}

/// The local timezone offset **at the given instant**, in minutes east of UTC.
///
/// Takes the instant rather than being a constant because daylight saving makes
/// the offset depend on *when* you ask: Europe/London is +0 in January and +60 in
/// July. A function returning one fixed number would be wrong for half the year.
///
/// Returns 0 when the offset cannot be determined.
int64_t timeLocalOffsetMin(int64_t epochMs) {
#ifdef _WIN32
    // Convert to a FILETIME, ask Windows what the offset was at that moment.
    // The two-call pattern is required: the first fills the structures.
    const int64_t fileTimeTicks = (epochMs + 11644473600000ll) * 10000ll;
    FILETIME utcFileTime;
    ULARGE_INTEGER ticks;
    ticks.QuadPart = (ULONGLONG)fileTimeTicks;
    utcFileTime.dwLowDateTime = ticks.LowPart;
    utcFileTime.dwHighDateTime = ticks.HighPart;

    FILETIME localFileTime;
    SYSTEMTIME utcSystemTime;
    SYSTEMTIME localSystemTime;
    if (!FileTimeToSystemTime(&utcFileTime, &utcSystemTime)) {
        return 0;
    }
    if (!SystemTimeToFileTime(&utcSystemTime, &utcFileTime)) {
        return 0;
    }
    if (!FileTimeToLocalFileTime(&utcFileTime, &localFileTime)) {
        return 0;
    }
    if (!FileTimeToSystemTime(&localFileTime, &localSystemTime)) {
        return 0;
    }
    // Difference in whole minutes. Comparing the two SYSTEMTIME values avoids
    // depending on how the local FILETIME was derived.
    SYSTEMTIME utcAgain;
    if (!FileTimeToSystemTime(&utcFileTime, &utcAgain)) {
        return 0;
    }
    const int64_t utcMinutes =
        ((int64_t)utcAgain.wDay * 24ll + utcAgain.wHour) * 60ll + utcAgain.wMinute;
    const int64_t localMinutes =
        ((int64_t)localSystemTime.wDay * 24ll + localSystemTime.wHour) * 60ll +
        localSystemTime.wMinute;
    int64_t difference = localMinutes - utcMinutes;
    // A month boundary makes the day numbers wrap; the real offsets are all well
    // inside a day, so bring the difference back into range.
    if (difference > 1440ll) {
        difference -= 1440ll * 7ll;
    } else if (difference < -1440ll) {
        difference += 1440ll * 7ll;
    }
    return difference;
#else
    const time_t seconds = (time_t)(epochMs / 1000ll);
    struct tm local;
    struct tm utc;
    if (localtime_r(&seconds, &local) == NULL) {
        return 0;
    }
    if (gmtime_r(&seconds, &utc) == NULL) {
        return 0;
    }
    // Both `struct tm` values describe the same instant, so their calendar fields
    // differ only by the offset. `tm_gmtoff` would be simpler where it exists, but
    // it is a BSD/GNU extension rather than standard C.
    const int64_t localMinutes = (int64_t)local.tm_hour * 60ll + local.tm_min;
    const int64_t utcMinutes = (int64_t)utc.tm_hour * 60ll + utc.tm_min;
    int64_t difference = localMinutes - utcMinutes;
    if (difference > 1440ll) {
        difference -= 1440ll * 7ll;
    } else if (difference < -1440ll) {
        difference += 1440ll * 7ll;
    }
    return difference;
#endif
}
