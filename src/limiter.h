// FFXI limiter clock and waits. The native timer uses whole milliseconds, imposing a 17 ms
// minimum frame at 60 FPS (loop RVA 0x012b05).
#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <intrin.h>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "profile.h"

namespace truefps {

// Wildcards (??) cover every absolute address.
inline constexpr const char* kLoopPattern =
    "8B 2D ?? ?? ?? ?? 89 5E 28 88 5C 24 17 8B 4E 1C 8B F9 89 4C 24 18 8B 17 FF 52 30 8B 07 8B CF D9 5C 24 18 FF 50 24 "
    "D8 4C 24 18 D9 1D ?? ?? ?? ?? D9 05 ?? ?? ?? ?? D8 35 ?? ?? ?? ?? D9 56 2C D9 56 28 DB 46 30";
// The divisor setter; imm32 at +12 = the app object pointer.
inline constexpr const char* kAppPattern = "81 EC 00 01 00 00 3B C1 74 21 8B 0D";
inline constexpr const char* kResetPattern =
    "56 8B F1 FF 15 ?? ?? ?? ?? 8B 56 0C 8B C8 2B CA 8B 56 1C 2B CA 8B 56 14 89 46 0C 03 D1 33 C0 89 56 14 89 46 18 89 46 1C 5E C3";
inline constexpr const char* kRatePattern =
    "83 EC 08 56 8B F1 FF 15 ?? ?? ?? ?? 8B 56 1C 8B 4E 0C 2B C2 2B C1 75 05 B8 01 00 00 00 89 44 24 04 C7 44 24 08 00 00 00 00 "
    "DF 6C 24 04 D8 3D ?? ?? ?? ?? D9 56 04 5E 83 C4 08 C3";
inline constexpr const char* kScalePattern = "D9 41 08 C3";
// The game step accessor: max(app->step, 1.0), ticks of 1/60 s. Two copies, RVA 0x014cf0 and 0x014d20.
inline constexpr const char* kStepPattern =
    "8B 0D ?? ?? ?? ?? D9 41 28 D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 07 D9 05 ?? ?? ?? ?? C3 D9 41 28 C3";
inline constexpr size_t kStepImmOffset = 2;   // imm32 = address of the app object pointer
inline constexpr size_t kAppStep = 0x28;
// Local-player movement, RVA 0x0a6475: `mov eax,[app]; mov edx,[esi]; mov ecx,[eax+0x2c]` reads app+0x2C, the frame's
// real ticks capped at 20, and is its only direct reader; imm32 = the app pointer.
inline constexpr const char* kMovePattern = "A1 ?? ?? ?? ?? 8B 16 8B 48 2C C7 44 24 18 00 00 00 00 89 4C 24 64";
inline constexpr size_t kMoveImmOffset = 1;
inline constexpr size_t kLoopImmOffset = 2;   // imm32 of `mov ebp, [Sleep IAT slot]`
inline constexpr size_t kAppImmOffset = 12;
// The menu manager's focused window pointer (manager + 0x54), in the menu UI tick at RVA 0x15fde2:
// `mov ecx,[eax+0C]; test ecx,ecx; je; mov edx,[ecx+08]; test edx,edx; je; cmp eax,[focus]`. imm32 at +16.
inline constexpr const char* kMenuFocusPattern = "8B 48 0C 85 C9 74 ?? 8B 51 08 85 D2 74 ?? 3B 05 ?? ?? ?? ??";
inline constexpr size_t kMenuFocusImmOffset = 16;
inline constexpr size_t kVtReset = 8, kVtRate = 9, kVtScale = 12, kVtCopy = 16;
// Timer object: +0x04 float rate, +0x08 float scale, +0x0C uint32 last reset (timeGetTime), +0x1C int32 paused ms.
// App object: +0x1C timer*, +0x30 int32 divisor.
inline constexpr size_t kTimerRate = 0x04, kTimerLast = 0x0C, kTimerPaused = 0x1C, kAppTimer = 0x1C, kAppDivisor = 0x30;

struct Module { uintptr_t base = 0; size_t size = 0; bool contains(uintptr_t a, size_t n = 1) const { return a >= base && a + n <= base + size; } };

inline std::vector<int> parsePattern(const char* text) {
    std::vector<int> out;
    for (const char* p = text; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') { out.push_back(-1); p += 2; continue; }
        char hex[3] = {p[0], p[1], 0};
        out.push_back(int(strtol(hex, nullptr, 16)));
        p += 2;
    }
    return out;
}

inline bool matchesAt(const uint8_t* data, size_t size, size_t at, const std::vector<int>& pat) {
    if (at > size || size - at < pat.size()) return false;
    for (size_t i = 0; i < pat.size(); i++)
        if (pat[i] >= 0 && data[at + i] != uint8_t(pat[i])) return false;
    return true;
}

inline std::vector<size_t> findAll(const uint8_t* data, size_t size, const std::vector<int>& pat, size_t limit = 2) {
    std::vector<size_t> hits;
    if (pat.empty() || size < pat.size()) return hits;
    for (size_t i = 0; i + pat.size() <= size && hits.size() < limit; i++)
        if (matchesAt(data, size, i, pat)) hits.push_back(i);
    return hits;
}

// Every pattern must start with a fixed byte.
inline std::vector<std::vector<size_t>> findAllMany(const uint8_t* data, size_t size, const std::vector<std::vector<int>>& pats, size_t limit = 2) {
    std::vector<std::vector<size_t>> hits(pats.size());
    std::vector<size_t> byFirst[256];
    for (size_t i = 0; i < pats.size(); i++)
        if (!pats[i].empty() && pats[i][0] >= 0) byFirst[pats[i][0]].push_back(i);
    for (size_t at = 0; at < size; at++)
        for (const size_t i : byFirst[data[at]])
            if (hits[i].size() < limit && matchesAt(data, size, at, pats[i])) hits[i].push_back(at);
    return hits;
}

inline bool readRaw(uintptr_t address, void* dest, size_t size) {
    if (!address || uint64_t(address) + size > 0x100000000ull) return false;
    __try { std::memcpy(dest, reinterpret_cast<void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool writeRaw(uintptr_t address, const void* src, size_t size) {
    if (!address || uint64_t(address) + size > 0x100000000ull) return false;
    __try { std::memcpy(reinterpret_cast<void*>(address), src, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <typename T> bool readValue(uintptr_t address, T& out) { return readRaw(address, &out, sizeof(T)); }

inline bool functionMatches(uintptr_t fn, const Module& m, const char* pattern) {
    const auto pat = parsePattern(pattern);
    if (!m.contains(fn, pat.size())) return false;
    std::vector<uint8_t> bytes(pat.size());
    return readRaw(fn, bytes.data(), bytes.size()) && matchesAt(bytes.data(), bytes.size(), 0, pat);
}

using ResetFn = void(__fastcall*)(void* self, void* edx);

// Smooth-mode clock terms: s = visual step (<= 20 ticks, times the cutscene speed); w = whole ticks, rest carried (phiBefore = prior carry);
// n = ceil(s / 1.05), sig = s / n; F*, SIGMA, Q075, P025 = per-iteration factors (1 - (1 - k)^sig; Q075/P025 = 0.75^(s/2)).
inline constexpr uint32_t kBits025 = 0x3E800000, kBits0125 = 0x3E000000, kBits005 = 0x3D4CCCCD, kBits05 = 0x3F000000,
                          kBits075 = 0x3F400000, kBits1 = 0x3F800000, kBitsM1 = 0xBF800000, kBits001 = 0x3C23D70A, kBits00001 = 0x38D1B717,
                          kBits04 = 0x3ECCCCCD, kBits004 = 0x3D23D70A, kBitsSixDeg = 0x3DD67750;
inline float floatFromBits(uint32_t bits) { float f = 0.0f; std::memcpy(&f, &bits, 4); return f; }
// Native 30: the game's default 30 fps, N ticks a frame.
inline constexpr double kNativeStep = 2.0;
inline constexpr double kNativeFramesPerSecond = 60.0 / kNativeStep;
inline constexpr double kSafetyPollsPerSecond = 30.0;   // zoning safety and the shutdown wait
// Per-frame easing `min(k * step, 1)` on native's clock: g with s * g = 1 - (1 - N*k)^(s/N).
inline float nativeEase(uint32_t kBits, double s) {
    const float literal = floatFromBits(kBits);
    if (s == kNativeStep) return literal;
    const double r = 1.0 - kNativeStep * double(literal);
    if (!(s > 0.0)) return float(-std::log(r) / kNativeStep);
    return float(-std::expm1((s / kNativeStep) * std::log(r)) / s);
}
inline uint32_t bitsOf(float f) { uint32_t b = 0; std::memcpy(&b, &f, 4); return b; }
// A "fraction of the way per tick" factor k applied once per iteration instead of once per tick: 1 - (1 - k)^sig.
// The camera factor calls push k at the call, so the value found in the client's code is the one converted here.
inline float perIterationK(uint32_t kBits, bool one, double sig) {
    const double k = double(floatFromBits(kBits));
    return one ? float(k) : float(1.0 - std::pow(1.0 - k, sig));
}

struct FrameValues {
    double s = 1.0, sig = 1.0;
    float phiBefore = 0.0f;
    int32_t w = 1, n = 1;
    bool one = true;
    float f025 = 0.25f, f0125 = 0.125f, f05 = 0.5f, sigma = 1.0f, q075 = 0.75f, p025 = 0.25f;
// The movement deadband, 0.01 units per frame scaled to the step (0.01 * walkScale, never above the client's own 0.01).
    float f001 = floatFromBits(kBits001);
    float f00001 = floatFromBits(kBits00001);   // the 0.0001 displacement gates
// The camera's gates: 0.01 for the collision pass, 0.0001 for a tiny net move; both 0 above 30 fps.
    float fc001 = floatFromBits(kBits001), fc00001 = floatFromBits(kBits00001);
// The zoom return: a quarter of the way to the default a call, snapping within 4.
    float fz025 = 0.25f, fzLow = -1.0f, fzHigh = 1.0f;
// Actor light blend (0.4 * step) and shadow direction filters (0.04 * step), on nativeEase.
    float fl04 = floatFromBits(kBits04), fl004 = floatFromBits(kBits004);
// The sound-start grace (0x3650a): 3.0 down by 1 an update.
    float fsnd = 1.0f;
    float move = 1.0f;  // real ticks, without the cutscene speed
};

inline FrameValues originalFrame() { return FrameValues{}; }

inline FrameValues nextFrame(double& carry, float realTicks, float speed) {
    const double native = kNativeStep;
    FrameValues v;
    const double real = realTicks > 0.0f ? (realTicks < 20.0f ? double(realTicks) : 20.0) : 0.0;
    v.s = real * (speed > 1.0f ? double(speed) : 1.0);
    v.phiBefore = float(carry);
    carry += v.s;
    const double whole = std::floor(carry + 1e-9);   // 0.7 + 0.3 is one tick, not 0.99999
    v.w = int32_t(whole);
    carry = carry - whole > 0.0 ? carry - whole : 0.0;
    v.n = v.s > 0.0 ? int32_t(std::ceil(v.s / 1.05 - 1e-9)) : 0;
    v.sig = v.n ? v.s / v.n : 0.0;
    v.one = v.sig == 1.0;
    if (!v.one) {
        v.f025 = float(1.0 - std::pow(0.75, v.sig));
        v.f0125 = float(1.0 - std::pow(0.875, v.sig));
        v.f05 = float(1.0 - std::pow(0.5, v.sig));
        v.sigma = float(v.sig);
    }
    if (v.s < native) {   // shorter than a native frame
        v.fc001 = 0.0f;
        v.fc00001 = 0.0f;
    }
    v.fl04 = nativeEase(kBits04, v.s);
    v.fl004 = nativeEase(kBits004, v.s);
    if (v.s != native) {
        const double quarter = 1.0 - std::pow(0.75, v.s / native);
        v.fz025 = float(quarter);
        v.fzHigh = float(quarter * 4.0);
        v.fzLow = -v.fzHigh;
    }
// The walk-animation rate filter (0xc8b36/0xc8b3e): rate*0.75 + sample*0.25 a CALL.
    if (v.s != native) {
        v.q075 = float(std::pow(0.75, v.s / native));
        v.p025 = float(1.0 - std::pow(0.75, v.s / native));
    }
    const double walkScale = v.s < native ? v.s / native : 1.0;
    if (walkScale != 1.0) {
        v.f001 = float(floatFromBits(kBits001) * walkScale);
        v.f00001 = float(floatFromBits(kBits00001) * walkScale);
    }
    v.fsnd = float((std::min)(1.0, real / kNativeStep));
    v.move = float(real);
    return v;
}

// Caller-address policy: S = fractional step, N = iteration count, W = whole ticks.
// Sorted entries apply only while their group is enabled.
enum : uint8_t { kPolicyW = 0, kPolicyS = 1, kPolicyN = 2, kPolicyReset = 3, kPolicyEntArrive = 4, kPolicyEntDec = 5, kPolicyEntRot = 6, kPolicyTrail = 7,
                 kPolicyEventMove = 8, kPolicyNativeReal = 9, kPolicyEventTimer = 10 };
inline float __cdecl smoothPolicyValue(uint8_t kind, uintptr_t esi);   // smooth.h; esi = the caller's ESI
struct PolicyEntry { uintptr_t ret = 0; uint8_t kind = kPolicyW; uint8_t group = 0; };
inline constexpr size_t kMaxPolicy = 80, kMaxGroups = 48;
inline PolicyEntry g_truefpsPolicy[kMaxPolicy];
inline size_t g_truefpsPolicyCount = 0;
inline uint8_t g_truefpsGroupOn[kMaxGroups] = {};        // single-byte writes: readers see 0 or 1
inline uint8_t g_truefpsGroupOffThread[kMaxGroups] = {}; // set when a group's step call runs off the game thread

inline const PolicyEntry* findPolicy(uintptr_t ret) {
    size_t lo = 0, hi = g_truefpsPolicyCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_truefpsPolicy[mid].ret < ret) lo = mid + 1;
        else hi = mid;
    }
    return (lo < g_truefpsPolicyCount && g_truefpsPolicy[lo].ret == ret) ? &g_truefpsPolicy[lo] : nullptr;
}
// Call only while the step accessors are not patched. False on overflow or a duplicate.
inline bool setPolicyTable(const PolicyEntry* entries, size_t count) {
    if (count > kMaxPolicy) return false;
    PolicyEntry sorted[kMaxPolicy];
    for (size_t i = 0; i < count; i++) {
        size_t j = i;
        for (; j > 0 && sorted[j - 1].ret > entries[i].ret; j--) sorted[j] = sorted[j - 1];
        sorted[j] = entries[i];
    }
    for (size_t i = 1; i < count; i++) if (sorted[i].ret == sorted[i - 1].ret) return false;
    g_truefpsPolicyCount = 0;
    for (size_t i = 0; i < count; i++) g_truefpsPolicy[i] = sorted[i];
    g_truefpsPolicyCount = count;
    return true;
}

// The mean of the last 4 frames' real ticks, each capped at 20, as the client's own step.
struct TickSpreader {
    float hist[4] = {};
    int next = 0, count = 0;
    void reset() { next = 0; count = 0; }
    float push(float ticks) {
        hist[next] = ticks < 20.0f ? ticks : 20.0f;
        next = (next + 1) % 4;
        if (count < 4) ++count;
        double sum = 0.0;
        for (int i = 0; i < count; i++) sum += hist[i];
        return float(sum / count);
    }
};

struct State {
    int64_t frequency = 0;
    int64_t lastReset = 0;        // QPC at the last reset
    uintptr_t appSlot = 0;        // address of the app object pointer
    uintptr_t timer = 0;          // the timer whose vtable is swapped (0 = none)
    uintptr_t originalVtable = 0;
    uintptr_t vtableCopy[kVtCopy] = {};
    ResetFn originalReset = nullptr;
    uintptr_t sleepTarget = 0;    // the loop's `mov ebp, [imm32]` now reads this slot
    bool waiting = true;          // Release clears it: preciseSleep becomes Sleep
    float lastFrameTicks = 0.0f;  // ticks the last frame took, at the timer reset
    TickSpreader spreader;
    bool smoothStep = false;      // the step on smooth mode's clock (set at Present)
    bool frameSmooth = false;
    bool moveReal = false;        // movement on real ticks, else on w
    double carry = 0.0;           // the tick fraction owed to the whole-tick consumers
    FrameValues frame;
    float speed = 1.0f;           // cutscene speed-up multiplier, 1 = off
    DWORD stepThread = 0;         // the game thread; calls from others are counted
    volatile long offThreadSteps = 0;
    volatile uintptr_t lastOffThreadRet = 0;
    volatile long lastOffThreadTid = 0;
    // stats (game thread only)
    uint64_t waits = 0, spins = 0, spinCounts = 0;
    uint64_t waitCounts = 0;      // QPC counts waited in the limiter or the pacing
    uint64_t lateCounts = 0;      // QPC counts a wait woke past its deadline
    uint64_t limiterWaitCounts = 0;   // the part in the precise limiter's wait
};
inline State g;

// What the player-movement stub reads (kMovePattern). Plain globals for the naked stubs.
inline uintptr_t g_truefpsAppSlot = 0;
inline volatile long g_truefpsSmoothMove = 0; // 1: movement reads g_truefpsMoveTicks, not app+0x2C
inline float g_truefpsMoveTicks = 0.0f;       // real or whole ticks (State::moveReal)

// The slots the patched client code and the stubs read: per-iteration easing factors, scaled thresholds and per-native-frame pacing.
inline uint8_t g_truefpsOne = 1;                      // sig == 1: every factor stub takes its original instruction
// No stub reads these three: the patched client code reads F025 and F05, and the cells below follow F0125.
inline float g_truefpsF025 = 0.25f, g_truefpsF0125 = 0.125f, g_truefpsF05 = 0.5f;
// An operand another tool can save and write back after truefps has unloaded must stay valid, so the two camera push
// multipliers and the three actor render positions are swapped to cells in a page that outlives the module
// (smooth.h), not to the globals here. A cell is not tied to any one constant: allocatePersistentCells points each
// one at the slot its rows would otherwise have been swapped to, so a site on another constant needs no more than its
// row. Rows that name one client constant in one routine share a cell, as they share that constant in the client: a
// tool that saves one of those operands and writes it back into all of them (xicamera does, for the two push
// multipliers) then writes truefps's own cell into each. A cell follows its slot while its routine is on and holds
// the client's constant otherwise, which is what that routine's whole ticks read. The class is left open: every other
// SwapImm site still swaps in a global here, because no tool is known to save one of those operands. Giving one the
// same rule is a table edit (SiteSpec::cell) and larger arrays below.
inline constexpr size_t kSiteCellCount = 2;
inline float* g_truefpsSiteCells[kSiteCellCount] = {};
inline const float* g_truefpsSiteCellSource[kSiteCellCount] = {};   // the paced value each cell follows while its routine is on
inline float g_truefpsSiteCellRest[kSiteCellCount] = {};            // the client's constant, which it holds otherwise
inline uint8_t g_truefpsSiteCellGroup[kSiteCellCount] = {};         // its routine: smooth.h's group index
inline void syncSiteCells() {
    for (size_t i = 0; i < kSiteCellCount; i++)
        if (g_truefpsSiteCells[i] && g_truefpsSiteCellSource[i])
            *g_truefpsSiteCells[i] = g_truefpsGroupOn[g_truefpsSiteCellGroup[i]] ? *g_truefpsSiteCellSource[i] : g_truefpsSiteCellRest[i];
}
// The five camera factor calls push their multiplier as an immediate at the call. The value found there at resolve is
// kept per stub, so a factor another tool retuned is the one the stub's gate matches and the one converted for the
// frame, instead of being passed through and then applied once per iteration.
inline uint32_t g_truefpsLookAtKBits = kBits025, g_truefpsRecenterKBits = kBits005, g_truefpsResetKBits = kBits0125, g_truefpsFirstPersonKBits = kBits0125;
inline float g_truefpsLookAtK = 0.25f, g_truefpsRecenterK = floatFromBits(kBits005), g_truefpsResetK = 0.125f, g_truefpsFirstPersonK = 0.125f;
inline float g_truefpsSigma = 1.0f, g_truefpsQ075 = 0.75f, g_truefpsP025 = 0.25f;
inline float g_truefpsF001 = floatFromBits(kBits001);
inline float g_truefpsF00001 = floatFromBits(kBits00001);
inline float g_truefpsFC001 = floatFromBits(kBits001), g_truefpsFC00001 = floatFromBits(kBits00001);
inline float g_truefpsFZ025 = 0.25f, g_truefpsFZLow = -1.0f, g_truefpsFZHigh = 1.0f;
inline float g_truefpsFL04 = floatFromBits(kBits04), g_truefpsFL004 = floatFromBits(kBits004);
inline float g_truefpsFSound = 1.0f;
inline double g_truefpsSig = 1.0;
inline float g_truefpsResetF = 0.125f;                // the reset key's per-iteration factor
// The target name pulse (smooth.h, glowCounterStub): sin(16c degrees) on a frame counter, 22.5 a cycle.
inline uint8_t g_truefpsGlowSmooth = 0;
inline int32_t g_truefpsGlowCounter = 0;
inline double g_truefpsGlowTicks = 0.0;   // in native frames
// Light visibility re-check (smooth.h, visCounterStub): a light re-checks when the frame counter mod 4 = its phase.
inline uint8_t g_truefpsVisSmooth = 0;      // 1: the stub answers
inline uint8_t g_truefpsVisAdvanced = 0;    // the counter is fresh this frame
inline int32_t g_truefpsVisCounter = 0;
inline double g_truefpsVisTicks = 0.0;   // in native frames
inline constexpr size_t kGroupVisibilityIndex = 15;        // smooth.h's group enum
inline constexpr size_t kGroupNetIconIndex = 20;
inline constexpr size_t kGroupHistoryIndex = 21;
inline constexpr size_t kGroupObstructionIndex = 22;
inline constexpr size_t kGroupTrailIndex = 29;
inline constexpr size_t kGroupCountersIndex = 37;
inline constexpr size_t kGroupWindGustsIndex = 39;
// The mount load wait and Trust emote weapon hide (smooth.h, mountCounterStub/emoteCounterStub).
inline uint8_t g_truefpsCounterSmooth = 0;
// Cloth wind gusts (smooth.h, windGustStub).
inline uint8_t g_truefpsGustSmooth = 0;
// Weapon trails (smooth.h, trailStub).
inline uint8_t g_truefpsTrailSmooth = 0;
// The camera's collision recovery history (smooth.h, historyStub; the client writes one of four entries per camera call).
inline uint8_t g_truefpsHistorySmooth = 0;
// The camera's obstruction test (smooth.h, obstructionStub): reaches past the eye on a short frame.
inline uint8_t g_truefpsObstructionSmooth = 0;
inline constexpr float kObstructionReach = 0.55f;   // the snap's 0.5 clearance plus a margin
// Pure: the point `reach` beyond `eye` along start->eye. False when they coincide.
inline bool obstructionEnd(const float* start, const float* eye, float reach, float* out) {
    const float dx = eye[0] - start[0], dy = eye[1] - start[1], dz = eye[2] - start[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(len > 1e-4f)) return false;
    const float k = reach / len;
    out[0] = eye[0] + dx * k;
    out[1] = eye[1] + dy * k;
    out[2] = eye[2] + dz * k;
    return true;
}
// Whole native frames elapsed, fractions carried, none skipped: the net icon (one phase per 11 - quality/10 calls), history, trail and event clocks count on it.
inline uint64_t g_truefpsUiTickTotal = 0;
inline double g_truefpsUiTickCarry = 0.0;
// Cutscene moves (smooth.h: MoveTo 0xb2ea0, MoveMode2 0xb31f0, SMove 0xb5630): step * speed / 60 a call.
inline float g_truefpsEventStep = 1.0f;
inline volatile long g_truefpsPlayerIndex = -1;   // the local player's entity index; -1 unknown
// The held mouse-button repeat (smooth.h, GroupMouseRepeat): 24 ticks to the first repeat, then 12.
inline float g_truefpsNativeRealStep = 1.0f;
// The preview camera's turn (smooth.h, GroupPreviewCamera): 0.10472 radians, 6 degrees a call.
inline float g_truefpsSixDegreeNative = floatFromBits(kBitsSixDeg);
struct EventPace {
    double carry = 0.0;   // ticks at cutscene speed, not yet given to a move
    double real = 0.0;    // real ticks since the last native frame boundary
    double avg = 0.0;     // average real ticks per frame; 0 = no frame yet
    bool slow = false;    // frames average a native frame or longer: every frame moves
};
inline EventPace g_truefpsEventPace;
inline constexpr double kEventSlowFrames = 2.03;    // switch to moving every frame (29.6 fps)
inline constexpr double kEventNativeFrames = 2.015; // and back to native frame boundaries (29.8 fps)
inline constexpr double kEventAverageWeight = 1.0 / 16.0;
// Each boundary moves this share of its distance toward the middle of its frame.
inline constexpr double kEventCentring = 0.03;
// Pure: the move step from `s` (ticks at cutscene speed) and `move` (real ticks); 0 or a native frame's worth.
inline float eventMoveStep(EventPace& p, double s, double move) {
    if (!(s > 0.0)) s = 0.0;
    if (!(move > 0.0)) { p.carry += s; return 0.0f; }
    const double speed = s / move;
    const double pulse = std::floor(kNativeStep * speed + 1e-9);
    p.avg = p.avg > 0.0 ? p.avg + (move - p.avg) * kEventAverageWeight : move;
    if (p.avg >= (p.slow ? kEventNativeFrames : kEventSlowFrames)) {
        p.slow = true;
        const double owed = p.carry + s;
        double whole = std::floor(owed + 1e-9);
        if (whole < pulse) whole = owed + 1e-9 >= pulse / 2.0 ? pulse : 0.0;   // owed back by the next frames
        p.carry = owed - whole;
        return float(whole);
    }
    if (p.slow) {
        p.slow = false;
        p.real = kNativeStep - move / 2.0;
        p.carry = speed * p.real + (std::max)(0.0, p.carry);
    }
    const double end = p.real + move;
    if (end + 1e-9 < kNativeStep) {
        p.real = end;
        p.carry += s;
        return 0.0f;
    }
    const double crossed = std::floor(end / kNativeStep + 1e-9);
    const double after = (std::max)(0.0, end - crossed * kNativeStep);   // real ticks past the last boundary
    const double share = (std::min)(1.0, (std::max)(0.0, (move - after) / move));
    const double owed = p.carry + s * share;
    p.carry = s - s * share;
    const double shift = kEventCentring * (0.5 - (std::min)(1.0, after / move)) * move;
    p.real = after + shift;
    p.carry += speed * shift;
    const double whole = std::floor(owed + 1e-9);
    p.carry += (std::max)(0.0, owed - whole);
    return float(whole);
}
inline uint8_t g_truefpsNetIconSmooth = 0;
// Pure: at most one sample per native frame (`frame` = their whole count).
struct HistorySlots {
    const void* obj[4] = {};
    uint64_t frame[4] = {};
    int next = 0;
};
inline bool historyDue(HistorySlots& h, const void* obj, uint64_t frame) {
    for (int i = 0; i < 4; i++) {
        if (h.obj[i] != obj) continue;
        if (h.frame[i] == frame) return false;
        h.frame[i] = frame;
        return true;
    }
    h.obj[h.next] = obj;
    h.frame[h.next] = frame;
    h.next = (h.next + 1) % 4;
    return true;
}
struct NetIconState { const void* obj = nullptr; uint64_t lastTicks = 0; };
// Pure: each multiple of `period` crossed in `ticks` (native frames) advances the phase once.
inline int32_t netIconPhase(NetIconState& st, const void* obj, int32_t phase, int32_t quality, uint64_t ticks) {
    int32_t period = 11 - quality / 10;
    if (period < 1) period = 1;
    if (phase < 0 || phase > 3) phase = 0;
    if (st.obj == obj && ticks >= st.lastTicks) {
        const uint64_t crossed = ticks / uint64_t(period) - st.lastTicks / uint64_t(period);
        phase = int32_t((uint64_t(phase) + crossed) % 4);
    }
    st.obj = obj;
    st.lastTicks = ticks;
    return phase;
}
// True when a native frame boundary passed; the counter then moves by exactly 1.
inline bool visAdvance(double& ticks, double s, int32_t& counter) {
    const double before = std::floor(ticks);
    ticks += s;
    if (ticks > 1e9) ticks -= 1e9;   // keep the floor exact
    if (std::floor(ticks) == before) return false;
    ++counter;
    return true;
}
inline float g_truefpsCallStep = 1.0f;                // once-per-call counters, in native calls
inline float g_truefpsWEB0 = 1.0f;                    // the effects engine's whole-tick cached step
inline uintptr_t g_truefpsEb0Ret = 0;                 // the return address of the call that fills it
inline uint32_t g_truefpsEntFrame = 0;                // frame number for the entity clocks (smooth.h)

inline void publishFrame(const FrameValues& v, bool smooth) {
    ++g_truefpsEntFrame;
    g.frame = v;
    g_truefpsOne = v.one ? 1 : 0;
    g_truefpsF025 = v.f025; g_truefpsF0125 = v.f0125; g_truefpsF05 = v.f05;
    g_truefpsSigma = v.sigma; g_truefpsQ075 = v.q075; g_truefpsP025 = v.p025; g_truefpsF001 = v.f001; g_truefpsF00001 = v.f00001; g_truefpsFC001 = v.fc001; g_truefpsFC00001 = v.fc00001;
    g_truefpsFZ025 = v.fz025; g_truefpsFZLow = v.fzLow; g_truefpsFZHigh = v.fzHigh;
    g_truefpsFL04 = v.fl04; g_truefpsFL004 = v.fl004; g_truefpsFSound = v.fsnd;
    g_truefpsSig = v.one ? 1.0 : v.sig;
    g_truefpsLookAtK = perIterationK(g_truefpsLookAtKBits, v.one, v.sig);
    g_truefpsRecenterK = perIterationK(g_truefpsRecenterKBits, v.one, v.sig);
    g_truefpsResetK = perIterationK(g_truefpsResetKBits, v.one, v.sig);
    g_truefpsFirstPersonK = perIterationK(g_truefpsFirstPersonKBits, v.one, v.sig);
    g_truefpsResetF = g_truefpsResetK;
    syncSiteCells();
    g_truefpsCallStep = smooth ? float(v.s / kNativeStep) : 1.0f;
    // Outside smooth mode, the client's own 6 degrees.
    if (!smooth) g_truefpsSixDegreeNative = floatFromBits(kBitsSixDeg);
}

inline int64_t qpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

inline int32_t divisorNow() {
    uintptr_t app = 0;
    int32_t divisor = 0;
    if (!g.appSlot || !readValue(g.appSlot, app) || !app || !readValue(app + kAppDivisor, divisor)) return 0;
    return divisor;
}

// The [smooth] fps key of older settings files: 0 off, -1 max, 61-1000 fps; anything else off.
inline constexpr int kSmoothMinFps = 61, kSmoothMaxFps = 1000;
inline int normalizeSmoothFps(int fps) {
    if (fps == -1 || fps == 0) return fps;
    if (fps < kSmoothMinFps) return 0;
    return fps > kSmoothMaxFps ? kSmoothMaxFps : fps;
}

// [gui] scale in the settings file: 1.0 to 2.0 (NaN is outside).
inline bool guiScaleValid(float scale) { return scale >= 1.0f && scale <= 2.0f; }

// Append a save-failure note before the result's final period.
inline std::string lineWithNote(std::string line, const char* note) {
    if (!note || !*note) return line;
    if (!line.empty() && line.back() == '.') line.pop_back();
    return line + " (" + note + ").";
}

// The fps counter's position: inside the game window when its size is known, else 0 to kOverlayPosMax.
inline constexpr int kOverlayPosMax = 16384;
inline int clampOverlayPos(int value, int limit) {
    const int top = (limit > 0 && limit < kOverlayPosMax) ? limit : kOverlayPosMax;
    if (value < 0) return 0;
    return value > top ? top : value;
}

// A value is taken only when the whole token is a number in range.
inline bool parseIntArg(const std::string& s, int lo, int hi, int& out) {
    if (s.empty() || s.size() > 11 || std::isspace(static_cast<unsigned char>(s[0]))) return false;
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size() || v < lo || v > hi) return false;
    out = int(v);
    return true;
}

// A counter whose 16c degrees (mod 360) is the pulse phase of `frames`: m = floor(2 frames) mod 45, c = 23 m mod 45.
inline int32_t glowCounterFor(double frames) {
    double t = std::fmod(frames, 22.5);
    if (t < 0.0) t += 22.5;
    const int32_t m = int32_t(std::floor(t * 2.0)) % 45;
    return (23 * m) % 45;
}

// The frame's clock values, computed once at the timer reset.
inline void beginFrame() {
    const bool smooth = g.smoothStep && g.lastFrameTicks > 0.0f;
    if (smooth && !g.frameSmooth) {
        g.carry = 0.0;   // a new run owes no fraction
        g.spreader.reset();
        uintptr_t app = 0;
        int32_t counter = 0;
        g_truefpsEventPace = EventPace{};
        if (g.appSlot && readValue(g.appSlot, app) && app && readValue(app + 0x34, counter)) {
            g_truefpsGlowTicks = double(((16LL * counter) % 360 + 360) % 360) / 16.0;
            g_truefpsVisCounter = counter;   // carry on with the client's phase
        }
    }
    g.frameSmooth = smooth;
    const float ticks = smooth ? g.spreader.push(g.lastFrameTicks) : g.lastFrameTicks;
    const double native = kNativeStep;
    const FrameValues v = smooth ? nextFrame(g.carry, ticks, g.speed) : originalFrame();
    publishFrame(v, smooth);
    if (smooth) {
        g_truefpsGlowTicks = std::fmod(g_truefpsGlowTicks + double(v.move) / native, 22.5);   // native frames from real ticks
        g_truefpsGlowCounter = glowCounterFor(g_truefpsGlowTicks);
        g_truefpsVisAdvanced = visAdvance(g_truefpsVisTicks, double(v.move) / native, g_truefpsVisCounter) ? 1 : 0;   // once a native frame
        g_truefpsUiTickCarry += double(v.move) / native;
        g_truefpsEventStep = eventMoveStep(g_truefpsEventPace, v.s, v.move);
        const double whole = std::floor(g_truefpsUiTickCarry + 1e-9);
        g_truefpsUiTickTotal += uint64_t(whole);
        g_truefpsNativeRealStep = float((std::min)(whole, 10.0) * kNativeStep);   // at most 10 native frames
        g_truefpsSixDegreeNative = floatFromBits(kBitsSixDeg) * g_truefpsNativeRealStep / float(kNativeStep);   // 6 degrees a native frame
        g_truefpsUiTickCarry = g_truefpsUiTickCarry - whole > 0.0 ? g_truefpsUiTickCarry - whole : 0.0;
    }
    g_truefpsGlowSmooth = smooth ? 1 : 0;
    g_truefpsVisSmooth = (smooth && g_truefpsGroupOn[kGroupVisibilityIndex]) ? 1 : 0;   // only while its group is on
    g_truefpsNetIconSmooth = (smooth && g_truefpsGroupOn[kGroupNetIconIndex]) ? 1 : 0;
    g_truefpsHistorySmooth = (smooth && g_truefpsGroupOn[kGroupHistoryIndex]) ? 1 : 0;
    g_truefpsObstructionSmooth = (smooth && g_truefpsGroupOn[kGroupObstructionIndex] && v.s < native) ? 1 : 0;   // only on a frame shorter than a native one
    g_truefpsTrailSmooth = (smooth && g_truefpsGroupOn[kGroupTrailIndex]) ? 1 : 0;
    g_truefpsCounterSmooth = (smooth && g_truefpsGroupOn[kGroupCountersIndex]) ? 1 : 0;
    g_truefpsGustSmooth = (smooth && g_truefpsGroupOn[kGroupWindGustsIndex]) ? 1 : 0;
    g_truefpsMoveTicks = g.moveReal ? v.move : float(v.w);
}

// Ends smooth mode's clock: the original step, the client's constants, every policy off.
inline void smoothClockOff() {
    g.smoothStep = false;
    g.frameSmooth = false;
    g.moveReal = false;
    g_truefpsSmoothMove = 0;
    g_truefpsGlowSmooth = 0;
    g_truefpsVisSmooth = 0;
    g_truefpsNetIconSmooth = 0;
    g_truefpsHistorySmooth = 0;
    g_truefpsObstructionSmooth = 0;
    g_truefpsTrailSmooth = 0;
    g_truefpsCounterSmooth = 0;
    g_truefpsGustSmooth = 0;
    for (auto& on : g_truefpsGroupOn) on = 0;
    publishFrame(originalFrame(), false);
}

// Calls inside TrueFPS code that call out and return, any thread. Unload needs 0 under its freeze.
inline volatile long g_truefpsStepInflight = 0;
struct InflightScope {
    InflightScope() { _InterlockedIncrement(&g_truefpsStepInflight); }
    ~InflightScope() { _InterlockedDecrement(&g_truefpsStepInflight); }
    InflightScope(const InflightScope&) = delete;
    InflightScope& operator=(const InflightScope&) = delete;
};

// vtable[+0x24]: the original's 1000 / elapsed ms (stored at +4), fractional.
inline float __fastcall preciseRate(void* self, void*) {
    const InflightScope inflight;
    const int64_t now = qpcNow();
    int32_t paused = 0;
    std::memcpy(&paused, static_cast<char*>(self) + kTimerPaused, 4);
    double ms = double(now - g.lastReset) * 1000.0 / double(g.frequency) - double(paused);
    if (!(ms > 0.0)) ms = 1.0;  // the original turns an elapsed 0 ms into 1 ms
    const float rate = float(1000.0 / ms);
    std::memcpy(static_cast<char*>(self) + kTimerRate, &rate, 4);
    return rate;
}

// vtable[+0x20]: the original reset (it keeps the whole-millisecond fields), plus the QPC time.
inline void __fastcall preciseReset(void* self, void* edx) {
    const InflightScope inflight;
    const int64_t now = qpcNow();
    int32_t paused = 0;
    std::memcpy(&paused, static_cast<char*>(self) + kTimerPaused, 4);
    const double ms = double(now - g.lastReset) * 1000.0 / double(g.frequency) - double(paused);
    g.lastFrameTicks = ms > 0.0 ? float(ms * 0.06) : 0.0f;
    beginFrame();
    g.originalReset(self, edx);
    g.lastReset = now;
}

// Replaces the limiter's Sleep; only 1 ms waits with a positive divisor. Sleeps to 0.5 ms out, then spins.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
inline bool g_hrTimerRetired = false;   // later waits use Sleep(1) with the 3 ms margin
inline bool g_hrTimerMade = false;
inline HANDLE g_hrTimerHandle = nullptr;
inline HANDLE hrTimer() {
    if (!g_hrTimerMade) {
        g_hrTimerMade = true;
        g_hrTimerHandle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    return g_hrTimerRetired ? nullptr : g_hrTimerHandle;
}
inline void closeHrTimer() {
    g_hrTimerRetired = true;
    if (g_hrTimerHandle) CloseHandle(g_hrTimerHandle);
    g_hrTimerHandle = nullptr;
}
inline bool hrTimerAvailable() { return hrTimer() != nullptr; }
// A new load makes its own timer.
inline void resetHrTimer() { closeHrTimer(); g_hrTimerRetired = false; g_hrTimerMade = false; }
// Wine hands back a timer without its high resolution: at load, five 1.3 ms waits must best 0.5 ms.
inline constexpr int kTimerCheckWaits = 5;
inline constexpr double kTimerCheckMs = 1.3, kTimerCheckLateMs = 0.5;
inline bool timerPrecise(double bestLateMs) { return bestLateMs >= 0.0 && bestLateMs <= kTimerCheckLateMs; }
inline double g_hrTimerCheckLateMs = -1.0;   // the load check's best lateness in ms; -1 = not checked
inline double checkHrTimer() {
    HANDLE h = hrTimer();
    if (!h || g.frequency <= 0) return -1.0;
    double best = 1e9;
    for (int i = 0; i < kTimerCheckWaits; i++) {
        LARGE_INTEGER due;
        due.QuadPart = -LONGLONG(kTimerCheckMs * 10000.0);   // relative, 100 ns units
        const int64_t start = qpcNow();
        if (!SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE)) { g_hrTimerRetired = true; return -1.0; }
        WaitForSingleObject(h, 50);
        best = (std::min)(best, double(qpcNow() - start) * 1000.0 / double(g.frequency) - kTimerCheckMs);
    }
    g_hrTimerCheckLateMs = best;
    if (!timerPrecise(best)) g_hrTimerRetired = true;
    return best;
}
// How close to the deadline a wait still sleeps: 1 ms with the timer, 3 ms with Sleep(1).
inline int64_t sleepThreshold() { return hrTimerAvailable() ? g.frequency / 1000 : g.frequency * 3 / 1000; }
// Bound waits in case an invalid client field produces a distant deadline.
inline constexpr int64_t kLongestWaitSeconds = 1;
// Sleeps to about 0.5 ms before `target`. False when there is no high-resolution timer.
inline bool timerSleepUntil(int64_t target, int64_t now) {
    HANDLE h = hrTimer();
    if (!h) return false;
    int64_t remain = target - now - g.frequency / 2000;
    if (remain <= 0) return true;
    if (remain > kLongestWaitSeconds * g.frequency) remain = kLongestWaitSeconds * g.frequency;   // the 100 ns conversion below overflows int64 past about a day
    LARGE_INTEGER due;
    due.QuadPart = -(remain * 10000000LL / g.frequency);   // relative, 100 ns units
    if (!SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE)) { g_hrTimerRetired = true; return false; }
    WaitForSingleObject(h, DWORD(remain * 1000 / g.frequency) + 20);
    return true;
}
// One step toward `target`: the timer if it arms, else Sleep(1) over 3 ms out. False: spin.
inline bool sleepToward(int64_t target, int64_t now) {
    if (target - now <= sleepThreshold()) return false;
    if (timerSleepUntil(target, now)) return true;
    if (target - now > g.frequency * 3 / 1000) { Sleep(1); return true; }
    return false;
}

inline void __stdcall preciseSleep(DWORD ms) {
    const InflightScope inflight;
    if (ms != 0 && g.waiting && g.timer) {
        const int32_t divisor = divisorNow();
        const int64_t now = qpcNow();
        // A reset within the last second: the limiter is timing with the swapped timer.
        if (divisor > 0 && divisor <= 60 && now >= g.lastReset && now - g.lastReset < g.frequency) {
            int32_t paused = 0;
            readValue(g.timer + kTimerPaused, paused);
            // The loop breaks at elapsed ms >= divisor * 1000 / 60; 20 us absorbs float rounding.
            const int64_t deadline = g.lastReset + int64_t(double(g.frequency) * (double(divisor) / 60.0 + double(paused) / 1000.0 + 0.00002));
            // Recheck the client's pause field at least once per second to avoid an indefinite wait.
            const bool capped = deadline - now > kLongestWaitSeconds * g.frequency;
            const int64_t target = capped ? now + kLongestWaitSeconds * g.frequency : deadline;
            ++g.waits;
            if (sleepToward(target, now)) {
                const int64_t after = qpcNow();
                g.waitCounts += uint64_t(after - now);
                g.limiterWaitCounts += uint64_t(after - now);
                if (!capped && after > target) g.lateCounts += uint64_t(after - target);   // overslept past the deadline
                return;
            }
            int64_t t = now;
            while (t < target) {
                if (target - t > g.frequency / 2000) SwitchToThread(); else YieldProcessor();
                t = qpcNow();
            }
            ++g.spins;
            g.spinCounts += uint64_t(t - now);
            g.waitCounts += uint64_t(t - now);
            g.limiterWaitCounts += uint64_t(t - now);
            if (!capped && now < target && t > target) g.lateCounts += uint64_t(t - target);   // a wait that began late did not oversleep
            return;
        }
    }
    Sleep(ms);
}

// The step the game should use. `base` is app->step; the original returns max(base, 1), times the speed.
inline float stepValue(float base, float speed) {
    float step = base < 1.0f ? 1.0f : base;   // NaN falls through to base, like the original's fcomp/jp
    if (speed > 1.0f) step *= speed;
    return step;
}

// Case-insensitive, whole-name match.
inline bool nameExcluded(const std::string& name, const std::vector<std::string>& excluded) {
    std::string lower = name;
    for (auto& ch : lower) ch = char(tolower(static_cast<unsigned char>(ch)));
    for (const auto& e : excluded) if (lower == e) return true;
    return false;
}

// The [cutscene] exclude key: '|'-separated names, lowercased; a missing key keeps `names` (\x7f sentinel).
inline void readNameList(const char* iniFile, const char* key, std::vector<std::string>& names) {
    char buf[2048] = {};
    GetPrivateProfileStringA("cutscene", key, "\x7f", buf, sizeof buf, iniFile);
    if (std::strcmp(buf, "\x7f") == 0) return;
    names.clear();
    const std::string all = buf;
    for (size_t start = 0; start <= all.size();) {
        const size_t bar = all.find('|', start);
        std::string item = all.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        for (auto& ch : item) ch = char(tolower(static_cast<unsigned char>(ch)));
        if (!item.empty()) names.push_back(item);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
}
inline void readExcludeList(const char* iniFile, std::vector<std::string>& names) { readNameList(iniFile, "exclude", names); }

// Client menu names are 16 bytes, "menu    " plus the space-padded name; returns it lowercased.
inline std::string menuShortName(const char* raw16) {
    if (std::memcmp(raw16, "menu    ", 8) != 0) return std::string();
    std::string name;
    for (int i = 8; i < 16 && raw16[i] != ' ' && raw16[i] != '\0'; i++) {
        const unsigned char ch = static_cast<unsigned char>(raw16[i]);
        if (ch < 0x21 || ch > 0x7E) return std::string();
        name.push_back(char(tolower(ch)));
    }
    return name;
}
// The cutscene menus that hold the speed-up at 1x while focused.
inline const std::vector<std::string> kCutsceneMenusDefault = {"query", "evitem", "evitem01", "comyn"};
// [cutscene] speed: 1 (normal) to 6; anything else is the default.
inline constexpr int kCutsceneSpeedDefault = 1;
inline int readCutsceneSpeed(const char* ini) {
    const int speed = int(GetPrivateProfileIntA("cutscene", "speed", kCutsceneSpeedDefault, ini));
    return (speed >= 1 && speed <= 6) ? speed : kCutsceneSpeedDefault;
}

// Frame limit in fps -> divisor (60 / fps); 0 uncapped, -1 when the divisor cannot express it.
inline int divisorForLimit(int fps) {
    if (fps == 0) return 0;
    if (fps < 1 || fps > 60 || 60 % fps != 0) return -1;
    return 60 / fps;
}

// /truefps limit: kRateMax = the monitor's refresh rate, kRateUncapped = no limit, anything else a rate in fps.
inline constexpr int kRateMax = -1, kRateUncapped = 0, kRateMinFps = 5, kRateMaxFps = 1000;
// The background cap takes the same range as the frame rate.
inline constexpr int kBackgroundMinFps = kRateMinFps, kBackgroundMaxFps = kRateMaxFps;
// The background cap: Off leaves the rate alone, Uncapped removes the limit behind another window.
inline constexpr int kBackgroundOff = 0, kBackgroundUncapped = -2;
inline bool rateValid(int rate) { return rate == kRateMax || rate == kRateUncapped || (rate >= kRateMinFps && rate <= kRateMaxFps); }
inline bool backgroundValid(int cap) { return cap == kRateMax || cap == kBackgroundOff || cap == kBackgroundUncapped || (cap >= kBackgroundMinFps && cap <= kBackgroundMaxFps); }
// High above the upper threshold; middle includes both thresholds; low below the lower one.
inline uint32_t bandColour(double fps, int high, int low, uint32_t highColour, uint32_t midColour, uint32_t lowColour) {
    if (fps > double(high)) return highColour;
    return fps >= double(low) ? midColour : lowColour;
}
// A colour band's threshold is a frame rate, so it takes the frame rate's range.
inline int clampBandFps(int fps) { return fps < kRateMinFps ? kRateMinFps : (fps > kRateMaxFps ? kRateMaxFps : fps); }
// Clamp both thresholds; the edited value pushes the other to preserve their order.
// On load, the upper threshold wins.
inline void orderBands(int& high, int& low, bool highMoved) {
    high = clampBandFps(high);
    low = clampBandFps(low);
    if (low <= high) return;
    if (highMoved) low = high;
    else high = low;
}
// A setting in fps (0: no cap), given the monitor's refresh rate for kRateMax.
inline int rateInFps(int rate, int refreshHz) { return rate == kRateMax ? (refreshHz > kRateMaxFps ? kRateMaxFps : refreshHz) : rate; }
// The rate held this frame: the setting, lowered by the background cap. 0: no cap.
inline int heldFps(int rateFps, int backgroundFps, bool background) {
    if (!background || backgroundFps == kBackgroundOff) return rateFps;
    if (backgroundFps == kBackgroundUncapped) return kRateUncapped;
    if (backgroundFps < 0) return rateFps;
    return (rateFps <= 0 || backgroundFps < rateFps) ? backgroundFps : rateFps;
}
// Hz-precise rateInFps/heldFps for Max; heldHz cannot express kBackgroundUncapped, so callers check it.
inline double rateInHz(int rate, double refreshHz) { return rate == kRateMax ? (refreshHz > double(kRateMaxFps) ? double(kRateMaxFps) : refreshHz) : double(rate); }
inline double heldHz(double rate, double background, bool inBackground) { return (inBackground && background > 0.0 && (rate <= 0.0 || background < rate)) ? background : rate; }
// The pacing period in `frequency` counts for `hz` (0 or less: no pacing).
inline int64_t pacePeriod(int64_t frequency, double hz) { return hz > 0.0 ? int64_t(double(frequency) / hz + 0.5) : 0; }

// Connection jobs: 12 records of 0x2C bytes; callback +0x10, state +0x24, retry count +0x28.
// Pace retryable errors (-2047..-1, up to 50 attempts) at 30 Hz.
struct RetryClock { uintptr_t rec; int64_t last; };
inline constexpr size_t kRetrySlots = 16;             // the client's table holds 12
inline constexpr double kRetryHz = 30.0;              // native 30's visit rate
inline constexpr uintptr_t kRetryStateOffset = 0x24, kRetryCountOffset = 0x28;
inline constexpr int32_t kRetryStartState = 2;        // state 2: call the start callback
inline RetryClock g_truefpsRetryClocks[kRetrySlots] = {};
inline volatile long g_truefpsRetryPaced = 0;         // attempts refused since the last log line
inline void resetRetryClocks() { for (auto& c : g_truefpsRetryClocks) c = RetryClock{}; }
// Pace retrying records with 5% timer tolerance; pass other states immediately. Fail open.
inline bool retryDue(RetryClock* slots, size_t n, uintptr_t rec, int32_t state, int32_t count, int64_t now, int64_t period) {
    if (!rec || !slots) return true;
    RetryClock* slot = nullptr;
    RetryClock* spare = nullptr;
    for (size_t i = 0; i < n && !slot; i++) {
        if (slots[i].rec == rec) slot = &slots[i];
        else if (!spare && !slots[i].rec) spare = &slots[i];
    }
    const bool retrying = state == kRetryStartState && count > 0;
    if (retrying && slot && period > 0 && now >= slot->last && now - slot->last < period - period / 20) return false;
    if (!slot) slot = spare;
    if (slot) { slot->rec = rec; slot->last = now; }
    return true;
}
// 1 to make this visit, 0 to skip it. No allocation, no locks.
inline long __cdecl retryGateAt(uintptr_t rec, int64_t now) {
    if (!rec) return 1;
    int32_t state = 0, count = 0;
    std::memcpy(&state, reinterpret_cast<const void*>(rec + kRetryStateOffset), 4);
    std::memcpy(&count, reinterpret_cast<const void*>(rec + kRetryCountOffset), 4);
    if (retryDue(g_truefpsRetryClocks, kRetrySlots, rec, state, count, now, pacePeriod(g.frequency, kRetryHz))) return 1;
    _InterlockedIncrement(&g_truefpsRetryPaced);
    return 0;
}
inline long __cdecl retryGate(uintptr_t rec) { return retryGateAt(rec, qpcNow()); }

// Entity motion counter +0x1CC advances per draw (0x08b460).
// Mount phase 0xE hides through 150, unhides at 151; Trust emote phase 0xB hides until 4.
inline constexpr uintptr_t kCounterPhaseOffset = 0x1bc;   // the entity's phase byte
inline constexpr uintptr_t kCounterValueOffset = 0x1cc;   // the counter both sites raise
inline constexpr uint8_t kMountPhase = 0x0e;              // the load wait's phase
inline constexpr int32_t kMountCounterEnd = 0x96;         // 150
inline constexpr int32_t kEmoteCounterEnd = 4;
inline constexpr int32_t kCounterVisitMax = 10;           // beginFrame's cap on a frame's native frames
// The per-actor fraction of a native frame a drawn frame did not spend, carried to its next visit.
struct CounterCarry { uintptr_t actor; float carry; uint32_t seen; };
inline constexpr size_t kCounterCarrySlots = 16;
// A slot untouched for two rounds of the table belongs to a run that ended.
inline constexpr uint32_t kCounterCarryStale = 2 * uint32_t(kCounterCarrySlots);
inline CounterCarry g_truefpsCounterCarries[kCounterCarrySlots] = {};
inline uint32_t g_truefpsCounterStamp = 0;
inline void resetCounterCarries() { for (auto& c : g_truefpsCounterCarries) c = CounterCarry{}; g_truefpsCounterStamp = 0; }
// Pure: the actor's carry, or null when every slot is live; the visit then counts 1.
inline float* counterCarryFor(CounterCarry* slots, size_t n, uintptr_t actor, uint32_t stamp) {
    if (!slots || !actor) return nullptr;
    CounterCarry* spare = nullptr;
    CounterCarry* oldest = nullptr;
    for (size_t i = 0; i < n; i++) {
        if (slots[i].actor == actor) { slots[i].seen = stamp; return &slots[i].carry; }
        if (!slots[i].actor) { if (!spare) spare = &slots[i]; }
        else if (!oldest || uint32_t(stamp - slots[i].seen) > uint32_t(stamp - oldest->seen)) oldest = &slots[i];
    }
    if (!spare && oldest && uint32_t(stamp - oldest->seen) >= kCounterCarryStale) spare = oldest;
    if (!spare) return nullptr;
    spare->actor = actor;
    spare->carry = 0.0f;
    spare->seen = stamp;
    return &spare->carry;
}
inline void releaseCounterCarry(CounterCarry* slots, size_t n, uintptr_t actor) {
    if (!slots) return;
    for (size_t i = 0; i < n; i++) if (slots[i].actor == actor) slots[i] = CounterCarry{};
}
// Pure: the whole native frames this drawn frame gives the counter, the fraction left in `carry`.
inline int32_t counterVisits(float& carry, float step) {
    const float frames = step / float(kNativeStep);
    float total = carry + (frames > 0.0f ? frames : 0.0f);
    if (!(total >= 0.0f)) total = 0.0f;                                     // a step that is not a number gives nothing
    if (total > float(kCounterVisitMax)) total = float(kCounterVisitMax);
    const float whole = std::floor(total);
    carry = total - whole;
    return int32_t(whole);
}
// Pure: the counter's new value. Outside smooth mode, the client's own increment exactly.
inline int32_t counterAdvance(CounterCarry* slots, size_t n, uintptr_t actor, int32_t counter, int32_t end, float step, bool smooth, uint32_t stamp) {
    if (!smooth) return counter < 0x7FFFFFFF ? counter + 1 : counter;
    if (counter >= end) { releaseCounterCarry(slots, n, actor); return counter; }   // the client zeroes the field on a phase change
    float* carry = counterCarryFor(slots, n, actor, stamp);
    int32_t add = carry ? counterVisits(*carry, step) : 1;
    if (add < 0) add = 0;
    if (add > end - counter) add = end - counter;
    const int32_t next = counter + add;
    if (next >= end) releaseCounterCarry(slots, n, actor);
    return next;
}
// What the two stubs call: read the counter, write the new one, return it; the mount's gate checks the phase.
inline int32_t counterAdvanceAt(uintptr_t actor, int32_t end, uint8_t phase) {
    if (!actor) return 0;
    int32_t counter = 0;
    std::memcpy(&counter, reinterpret_cast<const void*>(actor + kCounterValueOffset), 4);
    bool smooth = g_truefpsCounterSmooth != 0;
    if (smooth && phase) {
        uint8_t now = 0;
        std::memcpy(&now, reinterpret_cast<const void*>(actor + kCounterPhaseOffset), 1);
        smooth = now == phase;
    }
    const int32_t next = counterAdvance(g_truefpsCounterCarries, kCounterCarrySlots, actor, counter, end, g_truefpsNativeRealStep, smooth, ++g_truefpsCounterStamp);
    std::memcpy(reinterpret_cast<void*>(actor + kCounterValueOffset), &next, 4);
    return next;
}
inline int32_t __cdecl mountCounter(uintptr_t actor) { return counterAdvanceAt(actor, kMountCounterEnd, kMountPhase); }
inline int32_t __cdecl emoteCounter(uintptr_t actor) { return counterAdvanceAt(actor, kEmoteCounterEnd, 0); }

// Cloth gusts (0x188a50): each call resamples strength +0x134 to (rand()%10+4)*0.0025
// or decays it by 0.01 toward zero.
inline constexpr uint32_t kGustStrengthOffset = 0x134;   // float: the wind strength
inline constexpr uint32_t kGustFlagOffset = 0x140;       // int: non-zero while a gust blows
inline const float g_truefpsGustDecay = floatFromBits(kBits001);   // the client's own 0.01 a call
// Pure: the native frames this drawn frame gives the gusts; 1 outside smooth mode.
inline int32_t gustsCount() {
    if (!g_truefpsGustSmooth) return 1;
    const float step = g_truefpsNativeRealStep;
    return step >= float(kNativeStep) ? int32_t(step / float(kNativeStep)) : 0;
}
// Pure: the strength after n of the client's own decay calls; `s -= 0.01f` matches its `fld; fsub; fst` bit for bit.
inline float gustDecay(float strength, int32_t n) {
    float s = strength;
    for (int32_t i = 0; i < n; i++) {
        s -= g_truefpsGustDecay;
        if (s < 0.0f) s = 0.0f;
    }
    return s;
}
// Return nonzero to run native resampling; zero exits through the original epilogue.
// SSE work must leave the x87 stack untouched.
inline int32_t __cdecl windGustStep(uintptr_t env) {
    const int32_t n = gustsCount();
    if (!env || n <= 0) return 0;
    int32_t blowing = 0;
    std::memcpy(&blowing, reinterpret_cast<const void*>(env + kGustFlagOffset), 4);
    if (blowing) return 1;
    float strength = 0.0f;
    std::memcpy(&strength, reinterpret_cast<const void*>(env + kGustStrengthOffset), 4);
    const float next = gustDecay(strength, n);
    std::memcpy(reinterpret_cast<void*>(env + kGustStrengthOffset), &next, 4);
    return 0;
}

// The per-frame telemetry costing a system call each is read every kSampleEvery-th frame and on longer frames.
inline constexpr uint32_t kSampleEvery = 16;
inline constexpr float kSampleFrameMs = 6.0f;
inline bool sampleThisFrame(uint32_t frameIndex, float ms) { return ms >= kSampleFrameMs || frameIndex % kSampleEvery == 0; }

struct RefreshInfo { double hz = 0.0; std::string device; };
// Cache QueryDisplayConfig; require two polls before accepting a monitor change.
struct RefreshProbe { std::string device; int wholeHz = 0; std::string pending; int pendingPolls = 0; };
inline bool refreshProbe(RefreshProbe& p, const char* device, int wholeHz) {
    if (p.device.empty()) { p.device = device; p.wholeHz = wholeHz; p.pending.clear(); p.pendingPolls = 0; return true; }
    if (p.device != device) {
        if (p.pending == device) p.pendingPolls++;
        else { p.pending = device; p.pendingPolls = 1; }
        if (p.pendingPolls < 2) return false;
        p.device = device; p.wholeHz = wholeHz; p.pending.clear(); p.pendingPolls = 0;
        return true;
    }
    p.pending.clear(); p.pendingPolls = 0;
    if (p.wholeHz == wholeHz) return false;
    p.wholeHz = wholeHz;
    return true;
}
// At or below 60 FPS, use the next native rate that divides 60; pace the difference in TrueFPS.
inline int divisorAtOrAbove(int fps) {
    if (fps <= 0 || fps > 60) return 0;
    for (int f = fps; f <= 60; ++f) if (60 % f == 0) return 60 / f;
    return 1;
}
inline int gameDivisorFor(int fps) { const int d = divisorForLimit(fps); return d >= 0 ? d : divisorAtOrAbove(fps); }
inline int truefpsPaceFor(int fps) { return (fps > 0 && divisorForLimit(fps) < 0) ? fps : 0; }

// What holding the limit writes this frame, or -1. A divisor of 0 the user did not ask for is left alone.
inline int holdCorrection(int current, int wanted) {
    if (wanted < 0 || current == wanted) return -1;
    if (current == 0) return -1;
    return wanted;
}

// Who last wrote the divisor: `applied` once set after load, `smoothOwns` while the 0 is smooth mode's.
struct LimitState { bool applied = false; bool smoothOwns = false; int lastWritten = -1; };

// What the limit code writes this frame, or -1 for nothing; call limitWritten once written.
inline int limitTarget(const LimitState& s, bool smooth, int current, int limitFps) {
    const int wanted = smooth ? 0 : gameDivisorFor(limitFps);
    if (!s.applied || (!smooth && s.smoothOwns)) return wanted;
    return holdCorrection(current, wanted);
}
inline void limitWritten(LimitState& s, bool smooth, int written = -1) {
    s.applied = true;
    s.smoothOwns = smooth;  // smooth mode only ever writes 0
    s.lastWritten = written;
}
// The next limitTarget writes the limit whatever is there.
inline void limitReapply(LimitState& s) { s.applied = false; }
inline bool ownsDivisor(const LimitState& s, int current) { return s.applied && current == s.lastWritten; }

// Smooth mode switched on but unable to run: the game's own limiter holds the rate, never above 60 fps.
inline int smoothFallbackFps(int fps) { return (fps <= 0 || fps > 60) ? 60 : fps; }
// TrueFPS's own pacing while smooth mode is not running: none in its fallback, none over another tool's divisor.
inline int pacedWithoutSmooth(bool smoothOn, int fps, bool ownDivisor) { return (smoothOn || !ownDivisor) ? 0 : truefpsPaceFor(fps); }

inline bool verifyTimerVtable(uintptr_t vtable, const Module& m, std::string& why) {
    uintptr_t entries[kVtCopy] = {};
    if (!m.contains(vtable, sizeof entries)) { why = "the frame timer's function table is outside the client image"; return false; }
    if (!readRaw(vtable, entries, sizeof entries)) { why = "the frame timer's function table could not be read"; return false; }
    if (!functionMatches(entries[kVtReset], m, kResetPattern)) { why = "the frame timer's reset function is not the expected code"; return false; }
    if (!functionMatches(entries[kVtRate], m, kRatePattern)) { why = "the frame timer's rate function is not the expected code"; return false; }
    if (!functionMatches(entries[kVtScale], m, kScalePattern)) { why = "the frame timer's scale function is not the expected code"; return false; }
    return true;
}

// Call on the game thread.
inline bool swapTimer(uintptr_t timer, uintptr_t vtable) {
    if (!readRaw(vtable, g.vtableCopy, sizeof g.vtableCopy)) return false;
    g.originalReset = reinterpret_cast<ResetFn>(g.vtableCopy[kVtReset]);
    g.vtableCopy[kVtReset] = reinterpret_cast<uintptr_t>(&preciseReset);
    g.vtableCopy[kVtRate] = reinterpret_cast<uintptr_t>(&preciseRate);
    // Start the QPC clock where the whole-millisecond one is.
    uint32_t last = 0;
    if (!readValue(timer + kTimerLast, last)) return false;
    const uint32_t elapsedMs = timeGetTime() - last;
    const int64_t now = qpcNow();
    g.lastReset = now - int64_t(double(elapsedMs < 1000 ? elapsedMs : 0) * double(g.frequency) / 1000.0);
    g.timer = timer;
    g.originalVtable = vtable;
    const uintptr_t copy = reinterpret_cast<uintptr_t>(g.vtableCopy);
    if (!writeRaw(timer, &copy, 4)) { g.timer = 0; return false; }
    return true;
}

// Only when the object still carries the copy.
inline bool restoreTimer(uintptr_t currentTimer) {
    if (!g.timer) return true;
    uintptr_t now = 0;
    const uintptr_t copy = reinterpret_cast<uintptr_t>(g.vtableCopy);
    bool ok = true;
    if (currentTimer == g.timer && readValue(g.timer, now) && now == copy)
        ok = writeRaw(g.timer, &g.originalVtable, 4);
    if (ok) g.timer = 0;
    return ok;
}
// At unload: whether the frame timer truefps switched (`switched`) may still call into this module once it is gone.
// Decided on what the app's timer slot names now (`current`) before that object is touched: once the slot has moved
// on, the object may be freed, and heap bookkeeping in its first dword would read as a vtable outside the image.
// `vtable` is the switched object's table, read (`vtableRead`) only while the slot still names it.
// - Nothing switched, or an empty slot: no. The client's own destructor puts its class's table back, frees the timer
//   and empties the slot (RVA 0x109A9 on Sep-10), which is what every game close does before this runs.
// - Another object in the slot: yes. The switched one may still carry the copy, and it is never read.
// - The same object: truefps's copy (restoreTimer takes it out next) or the original table, no; another table inside
//   the client image is the client's own code, no; one outside it is another tool's copy of truefps's table, which
//   may call truefps's entries, yes; a table that could not be read cannot be said to be out, yes.
inline bool timerOrphaned(uintptr_t switched, uintptr_t current, bool vtableRead, uintptr_t vtable, uintptr_t copy, uintptr_t original, const Module& image) {
    if (!switched || !current) return false;
    if (current != switched || !vtableRead) return true;
    if (vtable == copy || vtable == original) return false;
    return !image.contains(vtable, 4);
}

// Both step accessors use caller-address policy in smooth mode, otherwise the original step.
// No calls outside this DLL.
inline float __cdecl stepWorker(uintptr_t ret, uintptr_t esi = 0) {
    const PolicyEntry* entry = findPolicy(ret);
    const DWORD tid = __readfsdword(0x24);   // TEB thread id, no call
    if (tid != g.stepThread) {
        _InterlockedIncrement(&g.offThreadSteps);
        g.lastOffThreadRet = ret;
        g.lastOffThreadTid = long(tid);
        if (entry) g_truefpsGroupOffThread[entry->group] = 1;
    }
    const uintptr_t app = *reinterpret_cast<uintptr_t*>(g.appSlot);   // the original dereferences the same pointer
    const float base = *reinterpret_cast<float*>(app + kAppStep);
    float result = 0.0f, whole = 0.0f;
    if (g.smoothStep && g.frameSmooth) {
        uint8_t kind = (entry && g_truefpsGroupOn[entry->group]) ? entry->kind : kPolicyW;
        // A routine-specific step off the game thread gets whole ticks.
        if (tid != g.stepThread && kind >= kPolicyReset) kind = kPolicyW;
        whole = float(g.frame.w);
        result = kind == kPolicyW ? whole : kind == kPolicyS ? float(g.frame.s) : kind == kPolicyN ? float(g.frame.n) : smoothPolicyValue(kind, esi);
    } else {
        result = whole = stepValue(base, g.speed);
    }
    if (ret && ret == g_truefpsEb0Ret) g_truefpsWEB0 = whole;
    return result;
}

// Replaces `mov eax,[app]` (5 bytes) at the movement site: returns eax = app, so the next `mov ecx,[eax+0x2c]` reads
// the client's real ticks; in smooth mode +0x2C is g_truefpsMoveTicks. ecx/edx untouched, flags free to RVA 0x0a64b3.
__declspec(naked) inline void moveStub() {
    __asm {
        mov eax, dword ptr [g_truefpsAppSlot]
        mov eax, dword ptr [eax]
        cmp dword ptr [g_truefpsSmoothMove], 0
        je done
        lea eax, [g_truefpsMoveTicks]
        sub eax, 2Ch
    done:
        ret
    }
}

__declspec(naked) inline void stepStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        push ecx
        push edx
        push esi                   // the caller's ESI
        push dword ptr [esp + 12]  // the caller's return address
        call stepWorker
        add esp, 8
        pop edx
        pop ecx
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    }
}

// A 5-byte `jmp rel32` from `site` to `target`.
inline void makeJump(uint8_t out[5], uintptr_t site, uintptr_t target) {
    out[0] = 0xE9;
    const int32_t rel = int32_t(target - (site + 5));
    std::memcpy(out + 1, &rel, 4);
}

// Waits until the QPC time `target`; the spin counts with the precise limiter's.
inline void waitUntil(int64_t target) {
    int64_t t = qpcNow(), spinFrom = 0;
    for (; t < target; t = qpcNow()) {
        if (sleepToward(target, t)) continue;
        if (!spinFrom) spinFrom = t;
        if (target - t > g.frequency / 2000) SwitchToThread();
        else YieldProcessor();
    }
    if (spinFrom) { ++g.spins; g.spinCounts += uint64_t(t - spinFrom); }
    if (t > target) g.lateCounts += uint64_t(t - target);
}

// Frame k is recorded at Present entry: truefpsMs covers callback k-1 including pacing;
// presentMs covers Present through timer reset including limiter wait; workMs covers update/draw.
// outsideMs retains the combined span; without a reset, presentMs and workMs are -1. Unknown CPU time is -1.
enum : uint8_t { kFrameBackground = 4, kFrameMenuPaused = 8 };
struct FrameRecord {
    int64_t at = 0;
    float ms = 0.0f, waitMs = 0.0f, lateMs = 0.0f, ticks = 0.0f;
    float workMs = 0.0f, presentMs = 0.0f, outsideMs = 0.0f, truefpsMs = 0.0f, cpuMs = -1.0f, outsideCpuMs = -1.0f;
    float updateMs = -1.0f, drawMs = -1.0f, afterDrawMs = -1.0f;
    float limiterWaitMs = 0.0f;   // the part of waitMs inside presentMs; the rest is in truefpsMs
    uint32_t ioKb = 0;
    uint16_t scenes = 0, smoothFps = 0;
    uint8_t speed = 1, flags = 0;
};
// Split the prior frame at callback end and timer reset. Return false if no reset fell within it.
inline bool splitFrame(FrameRecord& r, int64_t prevAt, int64_t prevPaceEnd, int64_t lastReset, int64_t at, int64_t frequency) {
    const auto toMs = [&](int64_t counts) { return float(double(counts) * 1000.0 / double(frequency)); };
    r.truefpsMs = prevPaceEnd > prevAt ? toMs(prevPaceEnd - prevAt) : 0.0f;
    r.outsideMs = r.ms - r.truefpsMs;
    const bool resetBetween = lastReset > prevPaceEnd && lastReset <= at;
    r.presentMs = resetBetween ? toMs(lastReset - prevPaceEnd) : -1.0f;
    r.workMs = resetBetween ? toMs(at - lastReset) : -1.0f;
    return resetBetween;
}
// Fills r's updateMs, drawMs and afterDrawMs; all three stay -1 unless reset < first BeginScene <= last EndScene <= `at`.
inline bool splitScenes(FrameRecord& r, int64_t lastReset, int64_t firstBegin, int64_t lastEnd, int64_t at, int64_t frequency) {
    r.updateMs = r.drawMs = r.afterDrawMs = -1.0f;
    if (r.workMs < 0.0f || firstBegin <= lastReset || lastEnd < firstBegin || lastEnd > at) return false;
    const auto toMs = [&](int64_t counts) { return float(double(counts) * 1000.0 / double(frequency)); };
    r.updateMs = toMs(firstBegin - lastReset);
    r.drawMs = toMs(lastEnd - firstBegin);
    r.afterDrawMs = toMs(at - lastEnd);
    return true;
}
// Frame part names, as in the Frames tab and /truefps frames.
inline constexpr const char* kPartGame = "Game logic", *kPartRender = "Rendering", *kPartAddons = "Addons & plugins",
    *kPartGpu = "GPU & driver", *kPartTruefps = "TrueFPS", *kPartLimiter = "Frame limiter", *kPartNone = "No single cause",
    *kPartUnknown = "Not measured";
// Present and whatever else ran between TrueFPS's callback and the timer reset, less the limiter's wait.
inline float gpuMs(const FrameRecord& r) { return r.presentMs < 0.0f ? -1.0f : (std::max)(0.0f, r.presentMs - r.limiterWaitMs); }
// TrueFPS's own work in its callback, without the pacing wait it holds there.
inline float truefpsOwnMs(const FrameRecord& r) { return (std::max)(0.0f, r.truefpsMs - (std::max)(0.0f, r.waitMs - r.limiterWaitMs)); }

// Unknown values are left out.
inline std::string framePartsText(const FrameRecord& r) {
    char split[128], io[64] = "", line[256];
    if (r.presentMs >= 0.0f && r.updateMs >= 0.0f) {
        _snprintf_s(split, sizeof split, _TRUNCATE, "game logic %.1f, rendering %.1f, addons & plugins %.1f, GPU & driver %.1f", double(r.updateMs), double(r.drawMs),
                    double(r.afterDrawMs), double(gpuMs(r)));
    } else if (r.presentMs >= 0.0f && r.workMs >= 0.0f) {
        _snprintf_s(split, sizeof split, _TRUNCATE, "game and rendering %.1f, GPU & driver %.1f", double(r.workMs), double(gpuMs(r)));
    } else {
        _snprintf_s(split, sizeof split, _TRUNCATE, "game, rendering and GPU %.1f (not split)", double(r.outsideMs));
    }
    if (r.ioKb) _snprintf_s(io, sizeof io, _TRUNCATE, ", %u kB read from disk since the last sample", r.ioKb);
    _snprintf_s(line, sizeof line, _TRUNCATE, "%s, TrueFPS %.1f; frame limiter %.1f (late %.1f)%s.", split, double(truefpsOwnMs(r)), double(r.waitMs), double(r.lateMs), io);
    return line;
}
// A freeze worth one warning in the log: over 1 s, not zoning, not within 30 s of a load or login, one a minute.
inline bool longFrameWorthLogging(const FrameRecord& r, bool zoning, bool wasBackground, double secondsSinceQuiet, double secondsSinceLast) {
    return r.ms > 1000.0f && !zoning && !wasBackground && !(r.flags & kFrameBackground) && secondsSinceQuiet >= 30.0 && secondsSinceLast >= 60.0;
}
// `ago`: seconds before now.
inline std::string frameDetailText(const FrameRecord& r, double ago) {
    char head[96];
    _snprintf_s(head, sizeof head, _TRUNCATE, "  %.1f ms, %.1f s ago: ", double(r.ms), ago);
    return head + framePartsText(r);
}
// New columns go at the end. The one column taken out, spread (between smooth_fps and cutscene_speed, removed in
// build 6AB28C1F), moved the four after it one place left; a reader that finds columns by the header is not affected.
inline constexpr const char* kFrameCsvHeader =
    "seconds_ago,frame_ms,game_drawing_ms,present_others_ms,truefps_ms,truefps_wait_ms,wait_late_ms,cpu_ms,present_game_drawing_cpu_ms,read_kb,game_ticks,"
    "game_update_ms,drawing_ms,after_drawing_ms,scenes,smooth_fps,cutscene_speed,background,menu_paused,limiter_wait_ms";
// The kFrameCsvHeader columns; unknown values are empty fields.
inline std::string frameCsvRow(const FrameRecord& r, double ago) {
    const auto opt = [](float v) {
        char buf[24] = "";
        if (v >= 0.0f) _snprintf_s(buf, sizeof buf, _TRUNCATE, "%.3f", double(v));
        return std::string(buf);
    };
    char head[64], tail[96];
    _snprintf_s(head, sizeof head, _TRUNCATE, "%.4f,%.3f,", ago, double(r.ms));
    _snprintf_s(tail, sizeof tail, _TRUNCATE, ",%u,%.4f", r.ioKb, double(r.ticks));
    char mid[64];
    _snprintf_s(mid, sizeof mid, _TRUNCATE, ",%.3f,%.3f,%.3f,", double(r.truefpsMs), double(r.waitMs), double(r.lateMs));
    char flags[64];
    _snprintf_s(flags, sizeof flags, _TRUNCATE, ",%u,%u,%u,%d,%d", unsigned(r.scenes), unsigned(r.smoothFps), unsigned(r.speed),
                (r.flags & kFrameBackground) ? 1 : 0, (r.flags & kFrameMenuPaused) ? 1 : 0);
    return head + opt(r.workMs) + "," + opt(r.presentMs) + mid + opt(r.cpuMs) + "," + opt(r.outsideCpuMs) + tail + "," + opt(r.updateMs) + "," + opt(r.drawMs) + "," +
           opt(r.afterDrawMs) + flags + "," + opt(r.limiterWaitMs);
}
// Comment lines, the column header, one row per frame (oldest first).
inline std::string captureText(const CaptureInfo& info, const SYSTEMTIME& t, const std::vector<FrameRecord>& rows, int64_t now, int64_t frequency) {
    std::string text = captureHeaderLines(info, t) + kFrameCsvHeader + "\n";
    text.reserve(text.size() + rows.size() * 170);
    for (const auto& r : rows) text += frameCsvRow(r, double(now - r.at) / double(frequency)) + "\n";
    return text;
}
struct FrameLog {
    static constexpr size_t kSize = 16384;   // over a minute and a half at 175 fps
    FrameRecord rec[kSize];
    size_t next = 0, count = 0;
    void add(const FrameRecord& r) { rec[next] = r; next = (next + 1) % kSize; if (count < kSize) ++count; }
    const FrameRecord& newest(size_t i) const { return rec[(next + kSize - 1 - i) % kSize]; }   // 0 = the latest
};
struct FrameSummary {
    size_t frames = 0, hitches = 0, severe = 0;
    double seconds = 0.0, fps = 0.0, avgMs = 0.0, medianMs = 0.0, worstMs = 0.0, low1Fps = 0.0, ticksPerSecond = 0.0, ticksPerFrame = 0.0;
    // Averaged over the frames where each part is known (-1: none known).
    double avgUpdateMs = -1.0, avgDrawMs = -1.0, avgAfterDrawMs = -1.0;
    double avgGpuMs = -1.0, avgTruefpsOwnMs = 0.0, avgWaitMs = 0.0;   // gpuMs, truefpsOwnMs, the waits
    std::vector<FrameRecord> worst;   // the hitches, worst first (at most 5)
};
// The frames of the last `seconds` before `now`. Hitch: over 1.5x the median; severe: over twice it and 4 ms past.
inline FrameSummary summarizeFrames(const FrameLog& log, int64_t now, int64_t frequency, double seconds) {
    FrameSummary sum;
    const int64_t from = now - int64_t(seconds * double(frequency));
    size_t kept = 0;   // counted first: one allocation of the frames in the window, not a run of growing ones
    while (kept < log.count && log.newest(kept).at >= from) kept++;
    std::vector<FrameRecord> frames;
    frames.reserve(kept);
    for (size_t i = 0; i < kept; i++) frames.push_back(log.newest(i));
    sum.frames = frames.size();
    if (frames.empty()) return sum;
    double totalMs = 0.0, ticks = 0.0;
    std::vector<float> ms;
    ms.reserve(frames.size());
    for (const auto& r : frames) { totalMs += r.ms; ticks += r.ticks; ms.push_back(r.ms); }
    sum.seconds = totalMs / 1000.0;
    sum.fps = totalMs > 0.0 ? double(frames.size()) * 1000.0 / totalMs : 0.0;
    sum.avgMs = totalMs / double(frames.size());
    sum.ticksPerSecond = totalMs > 0.0 ? ticks * 1000.0 / totalMs : 0.0;
    sum.ticksPerFrame = ticks / double(frames.size());
    std::sort(ms.begin(), ms.end());
    sum.medianMs = ms[ms.size() / 2];
    sum.worstMs = ms.back();
    // 1% low: the slowest 1% of frames, at least one.
    const size_t slowest = (std::max)(size_t(1), ms.size() / 100);
    double slowMs = 0.0;
    for (size_t i = ms.size() - slowest; i < ms.size(); i++) slowMs += ms[i];
    sum.low1Fps = slowMs > 0.0 ? 1000.0 * double(slowest) / slowMs : 0.0;
    double update = 0.0, draw = 0.0, after = 0.0, gpu = 0.0, ownWork = 0.0, waited = 0.0;
    size_t split = 0, presentKnown = 0;
    for (const auto& r : frames) {
        ownWork += truefpsOwnMs(r);
        waited += r.waitMs;
        if (r.presentMs >= 0.0f) { gpu += gpuMs(r); ++presentKnown; }
        if (r.updateMs >= 0.0f) { update += r.updateMs; draw += r.drawMs; after += r.afterDrawMs; ++split; }
    }
    sum.avgTruefpsOwnMs = ownWork / double(frames.size());
    sum.avgWaitMs = waited / double(frames.size());
    if (presentKnown) sum.avgGpuMs = gpu / double(presentKnown);
    if (split) { sum.avgUpdateMs = update / double(split); sum.avgDrawMs = draw / double(split); sum.avgAfterDrawMs = after / double(split); }
    const double hitchMs = sum.medianMs * 1.5, severeMs = (std::max)(sum.medianMs * 2.0, sum.medianMs + 4.0);
    for (const auto& r : frames) {
        if (r.ms <= hitchMs) continue;
        ++sum.hitches;
        if (r.ms > severeMs) ++sum.severe;
        sum.worst.push_back(r);
    }
    std::sort(sum.worst.begin(), sum.worst.end(), [](const FrameRecord& a, const FrameRecord& b) { return a.ms > b.ms; });
    if (sum.worst.size() > 5) sum.worst.resize(5);
    return sum;
}

// The named part furthest over its own average, or the limiter's wait when that is larger.
struct SlowCause {
    const char* name = kPartUnknown;
    double ms = -1.0, usualMs = -1.0;   // -1: not shown
};
inline SlowCause slowCause(const FrameRecord& r, const FrameSummary& sum) {
    if (r.updateMs < 0.0f || r.presentMs < 0.0f || sum.avgUpdateMs < 0.0 || sum.avgGpuMs < 0.0) return SlowCause{};
    struct Part { const char* name; double ms, usual; };
    const Part parts[] = {{kPartGame, r.updateMs, sum.avgUpdateMs}, {kPartRender, r.drawMs, sum.avgDrawMs}, {kPartAddons, r.afterDrawMs, sum.avgAfterDrawMs},
                          {kPartGpu, gpuMs(r), sum.avgGpuMs}, {kPartTruefps, truefpsOwnMs(r), sum.avgTruefpsOwnMs}};
    const Part* top = nullptr;
    double topOver = 0.1;   // an overrun under 0.1 ms names nothing
    for (const auto& p : parts)
        if (p.ms - p.usual > topOver) { topOver = p.ms - p.usual; top = &p; }
    const double waitOver = double(r.waitMs) - sum.avgWaitMs;
    if (waitOver > topOver && waitOver > 1.0) return SlowCause{kPartLimiter, r.waitMs, sum.avgWaitMs};
    if (!top) return SlowCause{kPartNone, -1.0, -1.0};
    return SlowCause{top->name, top->ms, top->usual};
}
inline const char* slowCauseHelp(const char* name) {
    if (name == kPartGame) return "The game's own work: loading, many characters or effects.";
    if (name == kPartRender) return "The game preparing and sending the scene to the graphics card. Includes addon drawing; not GPU time.";
    if (name == kPartAddons) return "Ashita, its addons and plugins, after the game draws.";
    if (name == kPartGpu) return "Presenting the frame: the graphics card and its driver, or another program in the way.";
    if (name == kPartTruefps) return "TrueFPS's own work in its frame callback.";
    if (name == kPartLimiter) return "The frame limiter held the frame: a lower cap (the background cap) or a rate change.";
    if (name == kPartNone) return "No single part stands out: every part took a little longer.";
    return "This frame's parts were not measured (the game's timer did not reset between two frames).";
}
// 15% over the slowest of the `count` newest frames, at least 20 ms.
inline float plotTopMs(const FrameLog& log, size_t count) {
    double peak = 0.0;
    for (size_t i = 0; i < count && i < log.count; i++) peak = (std::max)(peak, double(log.newest(i).ms));
    return float((std::max)(20.0, peak * 1.15));
}

}  // namespace truefps
