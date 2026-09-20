// Pace the 3600-poll connection timeout independently of FPS.
// RVA 0xEDEA0 resets the counter; 0xEDEB0 increments and tests >= 0xE10 (FFXI-3001).
#pragma once
#include "limiter.h"

namespace truefps {

inline constexpr const char* kWatchdogTickPattern =
    "A1 ?? ?? ?? ?? 33 C9 40 3D ?? ?? ?? ?? A3 ?? ?? ?? ?? 0F 9D C1 8B C1 C3";
inline constexpr const char* kWatchdogResetPattern =
    "C7 05 ?? ?? ?? ?? 00 00 00 00 C3";

// Use a 120-second budget: 3600 polls at 30 Hz.
inline constexpr double kWatchdogPollsPerSecond = kSafetyPollsPerSecond;
// a counter still for this long is no longer a wait
inline constexpr double kWatchdogIdleSeconds = 0.5;

struct WatchdogSite {
    uintptr_t poll = 0;       // never written to
    uintptr_t counter = 0;    // the counter both functions own: read and corrected
    uint32_t limit = 0;       // the client's expiry value, read from its cmp
    bool ok() const { return poll && counter && limit; }
};

inline WatchdogSite resolveWatchdog(const uint8_t* data, size_t size, uintptr_t base) {
    WatchdogSite site;
    const auto polls = findAll(data, size, parsePattern(kWatchdogTickPattern), 4);
    if (polls.size() != 1) return {};
    const size_t at = polls[0];
    uint32_t readSlot = 0, writeSlot = 0, limit = 0;
    std::memcpy(&readSlot, data + at + 1, 4);
    std::memcpy(&limit, data + at + 9, 4);
    std::memcpy(&writeSlot, data + at + 14, 4);
    if (readSlot != writeSlot || !limit || limit > 0x10000) return {};

    // the reset must name the same counter; a common shape, so scan wide
    bool resetAgrees = false;
    for (size_t off : findAll(data, size, parsePattern(kWatchdogResetPattern), 8192)) {
        uint32_t slot = 0;
        std::memcpy(&slot, data + off + 2, 4);
        if (slot == readSlot) { resetAgrees = true; break; }
    }
    if (!resetAgrees) return {};
    if (!base || readSlot < base || uint64_t(readSlot) - base + 4 > size) return {};   // all four bytes inside the image

    site.poll = base + at;
    site.counter = readSlot;
    site.limit = limit;
    return site;
}

// Exit state 0xD runs 0x014A30 once per frame. Each shutdown stage gets
// 900 frames at app+0x24; pace at 30 Hz for the native 30-second timeout.
inline constexpr const char* kShutdownWaitPattern =
    "56 8B F1 0F BF 46 04 83 F8 03 77 1D FF 24 85 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 0B C7 46 24 84 03 00 00 "
    "66 FF 46 04 32 C0 5E C3 8B 46 24 85 C0 8D 48 FF 89 4E 24";
// Its step 3, right after the two waits.
inline constexpr const char* kShutdownLeavePattern = "C7 46 44 0E 00 00 00 C6 46 0A 00 B0 01 5E C3";
inline constexpr uintptr_t kShutdownLeaveReach = 0x100;
// The frame root's state 0xD case.
inline constexpr const char* kShutdownCallPattern = "8B CE C7 02 A0 00 00 00 E8";
// The frame root's call, with the app object.
inline constexpr const char* kFrameRootCallPattern = "8B 0D ?? ?? ?? ?? 83 C4 10 E8";
inline constexpr uintptr_t kFrameRootReach = 0x1000;   // the state 0xD case lies this close after the frame root's start
inline constexpr uint32_t kAppShutdownCount = 0x24, kAppState = 0x44, kAppStateShutdown = 0xD;

// True when the frame root runs this wait with the app object in `appSlot`.
inline bool resolveShutdownWait(const uint8_t* data, size_t size, uintptr_t base, uintptr_t appSlot) {
    if (!base || !appSlot) return false;
    const auto u32 = [&](size_t off) { uint32_t v = 0; std::memcpy(&v, data + off, 4); return v; };
    const auto waits = findAll(data, size, parsePattern(kShutdownWaitPattern), 2);
    if (waits.size() != 1) return false;
    const uintptr_t wait = base + waits[0];
    bool leaves = false;
    for (size_t at : findAll(data, size, parsePattern(kShutdownLeavePattern), 8))
        leaves = leaves || (base + at > wait && base + at - wait < kShutdownLeaveReach);
    if (!leaves) return false;
    uintptr_t caller = 0;
    int calls = 0;
    for (size_t at : findAll(data, size, parsePattern(kShutdownCallPattern), 8)) {
        if (at + 13 > size || uintptr_t(base + at + 13 + int32_t(u32(at + 9))) != wait) continue;
        caller = base + at;
        ++calls;
    }
    if (calls != 1) return false;
    for (size_t at : findAll(data, size, parsePattern(kFrameRootCallPattern), 64)) {
        if (at + 14 > size || u32(at + 2) != appSlot) continue;
        const uintptr_t root = base + at + 14 + int32_t(u32(at + 10));
        if (caller > root && caller - root < kFrameRootReach) return true;
    }
    return false;
}

inline double watchdogTimeoutSeconds(uint32_t limit, double pollsPerSecond) {
    return pollsPerSecond > 0.0 ? double(limit) / pollsPerSecond : 0.0;
}

// Track counter increments, not its absolute value.
struct WatchdogClock {
    bool started = false;       // the first frame only reads
    uint32_t lastSeen = 0;      // the previous frame's, after any correction
    uint32_t base = 0;          // when this wait was first noticed
    double waited = 0.0;        // seconds
    double idle = 0.0;          // seconds without movement
    bool waiting = false;
    uint64_t corrections = 0;   // frames held back
    double longest = 0.0;       // seconds
};

// Lower the counter when it exceeds elapsed time since base. Stop correcting at limit
// to preserve a deadline of at least 120 seconds.
inline bool watchdogCorrection(WatchdogClock& w, uint32_t counter, double frameSeconds, uint32_t limit,
                               uint32_t* write, double pollsPerSecond = kWatchdogPollsPerSecond,
                               double idleSeconds = kWatchdogIdleSeconds) {
    if (!w.started) {                    // the counter holds whatever an earlier login left
        w.started = true;
        w.lastSeen = counter;
        return false;
    }
    if (counter < w.lastSeen) {          // reset to 0, or a new wait started lower
        w.waiting = false;
        w.waited = w.idle = 0.0;
        w.lastSeen = counter;
        return false;
    }
    if (counter == w.lastSeen) {
        if (w.waiting) {
            // wall-clock: still frames count toward the wait
            const double dt = frameSeconds > 0.0 ? frameSeconds : 0.0;
            w.waited += dt;
            if (w.waited > w.longest) w.longest = w.waited;
            w.idle += dt;
            if (w.idle >= idleSeconds) { w.waiting = false; w.waited = w.idle = 0.0; }
        }
        return false;
    }
    if (!w.waiting) {
        w.waiting = true;
        w.base = counter;
        w.waited = w.idle = 0.0;
    }
    w.idle = 0.0;
    w.waited += frameSeconds > 0.0 ? frameSeconds : 0.0;
    if (w.waited > w.longest) w.longest = w.waited;

    const double allowedNow = double(w.base) + w.waited * pollsPerSecond;
    if (allowedNow >= double(limit)) {   // the clock has run out too
        w.lastSeen = counter;
        return false;
    }
    const uint32_t allowed = uint32_t(allowedNow);
    if (counter <= allowed) {            // the client is inside the clock anyway
        w.lastSeen = counter;
        return false;
    }
    *write = allowed;
    w.lastSeen = allowed;
    ++w.corrections;
    return true;
}

} // namespace truefps
