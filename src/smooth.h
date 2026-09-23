// Patch step consumers for fractional ticks. Groups install/remove at Present on the game thread;
// failed patches or off-thread calls retire the affected group to whole ticks.
#pragma once
#include "patch.h"

namespace truefps {

// Addresses and constants the stubs use, set by resolve.
inline uintptr_t g_truefpsHelperScale = 0;     // client 0x0272b0: vec *= float arg
inline uintptr_t g_truefpsHelperScaleOut = 0;  // client 0x0272e0: out = vec * float arg
inline uintptr_t g_truefpsStepAccessor = 0;    // the accessor the heading loops call
inline uintptr_t g_truefpsD8CAddr = 0;         // client global 0x456d8c: the auto-rotate hold counter (int)
inline uintptr_t g_truefpsD88Addr = 0;         // client global 0x456d88: the reset-camera frame counter (int)
inline uintptr_t g_truefpsFrameCounter = 0;    // client 0x014d50: the frame counter accessor (app+0x34)
inline uintptr_t g_truefpsNetIcon = 0;         // client 0x2012d0: the network activity icon's phase/sprite function
inline uintptr_t g_truefpsHistoryWriter = 0;   // client 0x1e320: the camera's collision recovery history writer
inline uintptr_t g_truefpsSegmentTest = 0;     // client 0x1815b0: the world segment test (thunk into 0x169140)
inline uintptr_t g_truefpsJobWalker = 0;       // client 0x0f1160: one visit to one asynchronous job record
inline uintptr_t g_truefpsRecoveryHold = 0;    // client global 0x456d74: the camera's recovery hold
inline uintptr_t g_truefpsTrailTail = 0;       // client 0x1c1e8: the trail update's draw-submit tail
inline uintptr_t g_truefpsEffectsGlobal = 0;   // client global 0x47bfa8: the effects engine object (+0xEB0 cached step)
inline float g_truefpsK005 = floatFromBits(kBits005);   // the client's 0.05, 0.012 and -0.125 (fast paths)
inline float g_truefpsK0012 = floatFromBits(0x3C449BA6);
// The eye loop's far-distance threshold (its `fcomp` at 0x1fd03, 100.0 on every build measured): above it the loop
// takes its other branch, which reaches the spring's site by `jmp` (0x1fd2c) carrying its own converted factor.
// resolveSpringFar reads the client value; refreshConstantCopies keeps it current.
inline float g_truefpsSpringFar = 100.0f;
inline float g_truefpsKm0125 = floatFromBits(0xBE000000);

// x87 is empty on entry; no allocation. Callers do not depend on SSE/MMX state.
inline double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

// Auto-rotate (eye loop, 0x1fcc8): per tick the eye moves x*0.05 of the way; over sig ticks 1 - (1 - x*0.05)^sig.
inline double __cdecl rotFactor(double x) {
    return 1.0 - std::pow(clamp01(1.0 - x * double(g_truefpsK005)), g_truefpsSig);
}
// Distance spring 6..100 (the client's `fmul [0.012]` at 0x1fd4c, the patched site at 0x1fd52): e = d - 6,
// eye += D*e*0.012 a tick; over sig ticks the factor on D is (1 - (1 - 0.012*d)^sig) / d. Only this branch:
// beyond 100 the loop scales (d - 100) by its own 0.5 site and jumps to the same tail.
inline double __cdecl springFactor(double e, float d) {
    if (!(d > 0.0f)) return e * double(g_truefpsK0012) * g_truefpsSig;
    return e * (1.0 - std::pow(clamp01(1.0 - double(g_truefpsK0012) * d), g_truefpsSig)) / d;
}
// Push-out below 3 (0x1fda5): x = 3 - d; per tick eye -= D*x*0.125, so x shrinks by (1 - 0.125*d).
inline double __cdecl pushFactor(double x, float d) {
    const double k = -double(g_truefpsKm0125);
    if (d < 1e-6f) return -x * k * g_truefpsSig;
    return -x * (1.0 - std::pow(clamp01(1.0 - k * d), g_truefpsSig)) / d;
}
// Heading loops (0xc6882/0xc6c64): the caller's per-tick k becomes the per-iteration factor for sig.
inline void __cdecl convertK(float* k) {
    *k = float(1.0 - std::pow(1.0 - clamp01(*k), g_truefpsSig));
}

// An int counter the client decrements by 1, run down by a fraction; a client write restarts the shadow.
struct ShadowCounter { int32_t last = 0; double value = 0.0; bool valid = false; };
inline ShadowCounter g_truefpsShadowD8C, g_truefpsShadowD88;
inline int32_t shadowCountdown(ShadowCounter& c, int32_t* p, double by, bool stopAtZero = false) {
    if (!c.valid || *p != c.last) c.value = double(*p);
    c.value -= by;
    if (stopAtZero && c.value < 0.0) c.value = 0.0;
    const int32_t r = int32_t(std::ceil(c.value - 1e-6));  // 4 - 12 * (1/3 as a float) is 0, not 1
    *p = r;
    c.last = r;
    c.valid = true;
    return r;
}
inline int32_t __cdecl countdownSigma(int32_t* p) { return shadowCountdown(g_truefpsShadowD8C, p, double(g_truefpsSigma)); }
// The reset counter counts 8 CALLS down to 0 and must not go negative (0x1e2c0: != 0 means running): 16 ticks, s / 2 a frame.
inline int32_t __cdecl countdownFrame(int32_t* p) { return shadowCountdown(g_truefpsShadowD88, p, double(g_truefpsCallStep), true); }
// The reset key's loop (P4, step call to 0x2014d, factor call 0x2017c): h = min(s, 2R) ticks left; n_h = ceil(h / 1.05)
// iterations of 1 - (1 - k)^(h / n_h), with k the call's factor (normally 0.125).
inline float resetIterations() {
    g_truefpsResetF = g_truefpsResetK;
    const double s = g.frame.s;
    if (!g_truefpsD88Addr) return float(g.frame.n);
    const ShadowCounter& c = g_truefpsShadowD88;
    const int32_t cur = *reinterpret_cast<int32_t*>(g_truefpsD88Addr);
    const double r = kNativeStep * ((c.valid && cur == c.last) ? c.value : double(cur));   // ticks left, 2 per call
    if (!(r < s)) return float(g.frame.n);
    const double h = r > 0.0 ? r : 0.0;
    const int32_t nh = h > 0.0 ? int32_t(std::ceil(h / 1.05 - 1e-9)) : 0;
    if (nh <= 0) return 0.0f;
    const double sig = h / nh;
    const double k = double(floatFromBits(g_truefpsResetKBits));   // the client's factor
    g_truefpsResetF = sig == 1.0 ? float(k) : float(1.0 - std::pow(1.0 - k, sig));
    return float(nh);
}

// Remote interpolation (0x08cd90): remaining +0xF0, stamp +0x162, length +0x164.
// Track fractional R per segment; move min(s,R)/R, arrive when R <= s, write ceil(R-s).
// Invalidate when stamp, length or +0xF0 changes.
struct EntityClock { uintptr_t ent = 0; double remaining = 0.0; int16_t published = 0; uint16_t stamp = 0, length = 0; uint32_t frame = 0; };
inline constexpr size_t kEntityClockBits = 12, kEntityClockProbe = 16;
inline EntityClock g_truefpsEntClocks[size_t(1) << kEntityClockBits];
inline float g_truefpsEntArrive = 0.0f, g_truefpsEntDec = 0.0f, g_truefpsEntRot = 1.0f;
inline void resetEntityClocks() { for (auto& c : g_truefpsEntClocks) c = EntityClock{}; }
inline size_t entityClockSlot(uintptr_t ent) { return size_t((uint32_t(ent) * 2654435761u) >> (32 - kEntityClockBits)); }
// The entity's clock, else a free slot for one; null when the window holds 16 live clocks.
inline EntityClock* entityClockFor(uintptr_t ent) {
    const size_t home = entityClockSlot(ent), mask = (size_t(1) << kEntityClockBits) - 1;
    const uint32_t now = g_truefpsEntFrame;
    EntityClock* claim = nullptr;
    for (size_t i = 0; i < kEntityClockProbe; i++) {
        EntityClock& c = g_truefpsEntClocks[(home + i) & mask];
        if (c.ent == ent) return &c;
        if (!claim && (c.ent == 0 || c.frame + 1 < now)) claim = &c;
    }
    return claim;
}
inline constexpr int32_t kEntStamp = 0x162, kEntLength = 0x164;
// Return path divisors and arrival/decrement/rotation values; outside smooth mode, use n.
inline bool entitiesSmooth();
inline uint8_t entitiesOffThreadFlag();
inline float __cdecl entityStep(uintptr_t ent, int32_t n, int32_t path) {
    // Off the game thread: flag the group, return the original value, touch no shared clock.
    if (g.stepThread && __readfsdword(0x24) != g.stepThread) { entitiesOffThreadFlag(); return float(n); }
    g_truefpsEntArrive = 0.0f;
    g_truefpsEntDec = 0.0f;
    g_truefpsEntRot = 1.0f;
    if (!entitiesSmooth() || n <= 0) return float(n);
    const double s = g.frame.s;
    const uint16_t stamp = *reinterpret_cast<uint16_t*>(ent + kEntStamp), length = *reinterpret_cast<uint16_t*>(ent + kEntLength);
    EntityClock* c = entityClockFor(ent);
    const double r = !c ? double(n) - double(g.frame.phiBefore)
                        : (c->ent == ent && c->published == n && c->stamp == stamp && c->length == length) ? c->remaining : double(n);
    bool arrive = false;
    int32_t published = 0;
    if (!c) {
        // The frame clock's own whole ticks, not a rounding of R - s: the two disagree by one tick at a boundary.
        published = n > g.frame.w ? n - g.frame.w : 0;
        arrive = published == 0;
    } else {
        arrive = r - s <= 1e-6;
        const double left = arrive ? 0.0 : r - s;
        published = arrive ? 0 : int32_t(std::ceil(left - 1e-6));
        c->ent = ent;
        c->remaining = left;
        c->published = int16_t(published);
        c->stamp = stamp;
        c->length = length;
        c->frame = g_truefpsEntFrame;
    }
    const double h = arrive ? r : s;
    g_truefpsEntArrive = arrive ? float(n) : 0.0f;   // the client snaps when n <= this
    g_truefpsEntDec = float(n - published);          // the client subtracts this from +0xF0
    g_truefpsEntRot = h == 1.0 ? 1.0f : float((1.0 - std::pow(0.9375, h)) / 0.0625);   // times the client's 0.0625
    return path == 1 ? float(r) : float(arrive ? s : r);
}


// Stubs. Each counts itself in g_truefpsStepInflight and preserves what the client code it replaced preserved.

// Replace K in push K; push vec; call helper only when it matches the resolved client factor.
// Substitute that factor's per-frame conversion. EAX is free.
__declspec(naked) inline void scaleStub025() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        mov eax, dword ptr [g_truefpsLookAtKBits]
        cmp dword ptr [esp + 8], eax
        jne passThrough
        mov eax, dword ptr [g_truefpsLookAtK]
        mov dword ptr [esp + 8], eax
    passThrough:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsHelperScale]
    }
}
__declspec(naked) inline void scaleStub0125() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        mov eax, dword ptr [g_truefpsFirstPersonKBits]
        cmp dword ptr [esp + 8], eax
        jne passThrough
        mov eax, dword ptr [g_truefpsFirstPersonK]
        mov dword ptr [esp + 8], eax
    passThrough:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsHelperScale]
    }
}
// The reset key's factor call (P4): as scaleStub0125, with the factor resetIterations left for this frame.
__declspec(naked) inline void scaleStubReset() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        mov eax, dword ptr [g_truefpsResetKBits]
        cmp dword ptr [esp + 8], eax
        jne passThrough
        mov eax, dword ptr [g_truefpsResetF]
        mov dword ptr [esp + 8], eax
    passThrough:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsHelperScale]
    }
}
__declspec(naked) inline void scaleStub005() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        mov eax, dword ptr [g_truefpsRecenterKBits]
        cmp dword ptr [esp + 0Ch], eax
        jne passThrough
        mov eax, dword ptr [g_truefpsRecenterK]
        mov dword ptr [esp + 0Ch], eax
    passThrough:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsHelperScaleOut]
    }
}

// Factor stubs (P3): a 6-byte `fmul [K]` -> `call stub; nop`; sig 1 runs that fmul. eax/ecx/edx kept, d at [esp+14h].
// (The spring below keeps the client's fmul and replaces the instructions after it instead.)
__declspec(naked) inline void rotStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsOne], 0
        je slow
        fmul dword ptr [g_truefpsK005]
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    slow:
        push eax
        push ecx
        push edx
        sub esp, 8
        fstp qword ptr [esp]
        call rotFactor
        add esp, 8
        pop edx
        pop ecx
        pop eax
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    }
}
// The 6..100 spring keeps the client's own `fmul [0.012]` (0x1fd4c) and replaces the seven bytes after it,
// `D9 1C 24 8D 44 24 2C` (fstp [esp]; lea eax,[esp+2Ch], 0x1fd52), with `call springStub` + 2 NOPs.
// Entry: ST0 = the branch's delta. d is at client [esp+14h], stub [esp+18h], then [esp+24h] after three pushes.
// Both paths replay the displaced instructions with stack offsets increased by 4 for the return address.
// The loop's beyond-100 branch jumps to this same tail (0x1fd2c, `jmp 0x1fd52`) with (d - 100) already scaled by
// its own site's converted factor, so the stub re-runs the client's own test of d and leaves that branch alone.
// For the 6..100 branch the slow path divides the client's constant back out to recover e.
__declspec(naked) inline void springStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsOne], 0
        je slow
    displaced:
        fstp dword ptr [esp + 4]
        lea eax, [esp + 30h]
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    slow:
        fld dword ptr [esp + 18h]            // d, the distance the client compared at 0x1fd03
        fcomp dword ptr [g_truefpsSpringFar]
        fnstsw ax
        test ah, 41h                         // the client's own `and eax, 4100h`: clear means d is beyond the
        jz displaced                         // threshold, the branch that arrives already converted
        fdiv dword ptr [g_truefpsK0012]
        push eax
        push ecx
        push edx
        push dword ptr [esp + 24h]
        sub esp, 8
        fstp qword ptr [esp]
        call springFactor
        add esp, 0Ch
        pop edx
        pop ecx
        pop eax
        jmp displaced
    }
}
__declspec(naked) inline void pushStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsOne], 0
        je slow
        fmul dword ptr [g_truefpsKm0125]
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    slow:
        push eax
        push ecx
        push edx
        push dword ptr [esp + 24h]
        sub esp, 8
        fstp qword ptr [esp]
        call pushFactor
        add esp, 0Ch
        pop edx
        pop ecx
        pop eax
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    }
}

// Auto-rotate hold (P3, 0x1fb96): `mov eax,[D8C]; dec eax; mov [D8C],eax` -> `call d8cStub` + 6 NOPs; eax = the new
// value with SF set for the client's `jns`.
__declspec(naked) inline void d8cStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        push ecx
        push edx
        push dword ptr [g_truefpsD8CAddr]
        call countdownSigma
        add esp, 4
        pop edx
        pop ecx
        lock dec dword ptr [g_truefpsStepInflight]
        test eax, eax
        ret
    }
}
// Reset-camera frame counter (P4, 0x201a8): `dec dword ptr [D88]` -> `call d88Stub; nop`, down by s / 2 a frame.
__declspec(naked) inline void d88Stub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        push ecx
        push edx
        push dword ptr [g_truefpsD88Addr]
        call countdownFrame
        add esp, 4
        pop edx
        pop ecx
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    }
}

// Heading loops (P8): the loop's step call -> `call headingStub`, converting the caller's k at [esp+14h]. eax free.
__declspec(naked) inline void headingStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsOne], 0
        jne go
        push ecx
        push edx
        lea eax, [esp + 20h]
        push eax
        call convertK
        add esp, 4
        pop edx
        pop ecx
    go:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsStepAccessor]
    }
}

// Path 1, 0x8d340: `movsx ebp,word [esi+F0]; mov [esp+18h],ebp` -> `call entStub1` + 6 NOPs; 0x8d359 `fild` -> `fld`: [esp+18h] = divisor, ebp = n.
// Path 2, 0x8d5c2: `movsx ecx,ax; mov [esp+18h],ecx; fild` -> `call entStub2` + 6 NOPs: ST0 = divisor, ecx = [esp+18h] = n. eax/ecx/edx kept, x87 empty.
__declspec(naked) inline void entStub1() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        push eax
        push ecx
        push edx
        movsx ebp, word ptr [esi + 0F0h]
        push 1
        push ebp
        push esi
        call entityStep
        add esp, 0Ch
        fstp dword ptr [esp + 28h]
        pop edx
        pop ecx
        pop eax
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    }
}
__declspec(naked) inline void entStub2() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        push eax
        push edx
        movsx ecx, ax
        push 2
        push ecx
        push esi
        call entityStep
        add esp, 0Ch
        movsx ecx, word ptr [esi + 0F0h]
        mov dword ptr [esp + 24h], ecx
        pop edx
        pop eax
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    }
}

// Target name pulse (0x83197): the name draw's frame-counter call -> glowCounterStub (g_truefpsGlowCounter); eax only.
__declspec(naked) inline void glowCounterStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsGlowSmooth], 0
        je original
        mov eax, dword ptr [g_truefpsGlowCounter]
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    original:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsFrameCounter]
    }
}

// Network icon (0x20143c): ret 8, this = icon, args = base sprite and quality.
// Pace the 11-quality/10 call period at native FPS; return the full int in EAX.
inline int32_t __fastcall netIconPaced(int32_t* icon, int /*edx*/, int32_t base, int32_t quality) {
    if (!base) return 0;
    static NetIconState state;
    const int32_t phase = netIconPhase(state, icon, icon[0x34 / 4], quality, g_truefpsUiTickTotal);
    icon[0x34 / 4] = phase;
    return phase + base;
}
__declspec(naked) inline void netIconStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsNetIconSmooth], 0
        je original
        push dword ptr [esp + 8]       // quality
        push dword ptr [esp + 8]       // base (the first push moved it to +8)
        call netIconPaced              // ecx = icon, as the caller set it
        lock dec dword ptr [g_truefpsStepInflight]
        ret 8
    original:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsNetIcon]
    }
}

// Camera history (0x1ff3f): call 0x1e320 at native cadence.
// ECX = camera+0xBC, args = look-at and 1, ret 8; preserve ECX for the original.
inline uint32_t __fastcall historyDueNow(void* history) {
    static HistorySlots slots;
    return historyDue(slots, history, g_truefpsUiTickTotal) ? 1u : 0u;
}
__declspec(naked) inline void historyStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsHistorySmooth], 0
        je original
        push ecx
        call historyDueNow               // ecx = the history object
        pop ecx
        test eax, eax
        jz skip
    original:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsHistoryWriter]
    skip:
        lock dec dword ptr [g_truefpsStepInflight]
        ret 8
    }
}

// Obstruction (0x1ff29): this = world, args = look-at/eye/hit, ret 0xC, result in AL.
// Extend eye +0x44 on sub-native frames; restore on a miss. Recovery uses the actual hit eye.
inline constexpr const char* kObstructionHitPattern = "A1 ?? ?? ?? ?? 85 C0 75 22 80 BF F0 00 00 00 04 7C 19";
inline constexpr uintptr_t kObstructionHitOffset = 0x20;   // call + 5, test al,al + 2, jne + 2, + 17h
inline constexpr uintptr_t kCameraEyeOffset = 0x44, kCameraHistoryCount = 0xF0;
inline constexpr uint8_t kRecoveryHistoryFull = 4;
inline bool obstructionWouldRecover(const float* eye) {
    if (!g_truefpsRecoveryHold) return true;   // unverified: never extend
    const int32_t hold = *reinterpret_cast<const int32_t*>(g_truefpsRecoveryHold);
    const uint8_t count = *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(eye) - kCameraEyeOffset + kCameraHistoryCount);
    return hold == 0 && count >= kRecoveryHistoryFull;
}
inline uint32_t __fastcall obstructionTest(void* world, int /*edx*/, float* start, float* eye, float* hit) {
    using Test = uint32_t(__fastcall*)(void*, int, float*, float*, float*);
    const Test original = reinterpret_cast<Test>(g_truefpsSegmentTest);
    if (obstructionWouldRecover(eye)) return original(world, 0, start, eye, hit);
    float reach[3];
    if (!obstructionEnd(start, eye, kObstructionReach, reach)) return original(world, 0, start, eye, hit);
    const float saved[3] = {eye[0], eye[1], eye[2]};
    eye[0] = reach[0]; eye[1] = reach[1]; eye[2] = reach[2];
    const uint32_t result = original(world, 0, start, eye, hit);
    if ((result & 0xFF) == 0) { eye[0] = saved[0]; eye[1] = saved[1]; eye[2] = saved[2]; }
    return result;
}
__declspec(naked) inline void obstructionStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsObstructionSmooth], 0
        je original
        push dword ptr [esp + 12]      // hit out
        push dword ptr [esp + 12]      // eye
        push dword ptr [esp + 12]      // look-at
        call obstructionTest           // ecx = world, as the caller set it
        lock dec dword ptr [g_truefpsStepInflight]
        ret 12
    original:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsSegmentTest]
    }
}

// Weapon trails carry ticks and update/age only on native quanta (2 ticks); otherwise jump
// to draw tail 0x1c1e8. Step call 0x1bc9c; aging helper 0x1c260.
struct TrailClock {
    uintptr_t obj = 0;
    double carry = 0.0;       // ticks owed to this trail
    float phase = 0.0f;       // the phase expected when seen next
    int32_t highWater = 0;    // +14B0 when last seen (it only grows during a trail's life)
    uint32_t frame = 0;       // the drawn frame it was last seen in; ticks are added once a frame
    uint64_t seen = 0;        // the native frame it was last seen in (g_truefpsUiTickTotal)
};
inline constexpr size_t kTrailClockBits = 8, kTrailClockProbe = 16;
inline constexpr uint64_t kTrailStaleNativeFrames = 60;   // a clock unseen this long may be reused
// Trail fields: phase +0x14B8, rate +0x14BC, sample high-water +0x14B0; initializer 0x1B890 resets them.
inline constexpr uintptr_t kTrailPhaseOffset = 0x14B8, kTrailRateOffset = 0x14BC, kTrailHighWaterOffset = 0x14B0;
inline TrailClock g_truefpsTrailClocks[size_t(1) << kTrailClockBits];
inline float g_truefpsTrailQuantum = 2.0f;            // the ticks of the trail update running now
inline uint32_t g_truefpsTrailQuantumFrame = UINT32_MAX;   // the frame that quantum was decided in
inline void resetTrailClocks() { for (auto& c : g_truefpsTrailClocks) c = TrailClock{}; }
// The lifetime step at 0x1c5b4 (kPolicyTrail): this frame's quantum while trails are paced, else whole ticks.
inline float trailLifetimeStep() {
    return (g_truefpsTrailSmooth && g_truefpsTrailQuantumFrame == g_truefpsEntFrame) ? g_truefpsTrailQuantum : float(g.frame.w);
}
enum : int { kTrailSkip = 0, kTrailRun = 1, kTrailClient = 2 };
// The decision for one trail update: kTrailRun sets g_truefpsTrailQuantum, kTrailClient means the client's own call.
inline int __cdecl trailDecide(uintptr_t trail) {
    if (__readfsdword(0x24) != g.stepThread) { g_truefpsGroupOffThread[kGroupTrailIndex] = 1; return kTrailClient; }
    const float phase = *reinterpret_cast<const float*>(trail + kTrailPhaseOffset);
    const float rate = *reinterpret_cast<const float*>(trail + kTrailRateOffset);
    const int32_t highWater = *reinterpret_cast<const int32_t*>(trail + kTrailHighWaterOffset);
    const uint32_t frame = g_truefpsEntFrame;
    const uint64_t now = g_truefpsUiTickTotal;
    const size_t mask = (size_t(1) << kTrailClockBits) - 1;
    const size_t home = size_t((uint32_t(trail) * 2654435761u) >> (32 - kTrailClockBits));
    TrailClock* clock = nullptr;
    TrailClock* empty = nullptr;
    TrailClock* stale = nullptr;
    for (size_t i = 0; i < kTrailClockProbe && !clock; i++) {
        TrailClock& c = g_truefpsTrailClocks[(home + i) & mask];
        if (c.obj == trail) clock = &c;
        else if (c.obj == 0) { if (!empty) empty = &c; }
        else if (!stale && now - c.seen > kTrailStaleNativeFrames) stale = &c;   // a free slot first
    }
    // A new trail, or a reused object whose phase or count went back, starts on max(this frame's ticks, a native frame's).
    const double native = kNativeStep;
    const double firstCarry = (std::max)(native, g.frame.s);
    if (!clock) {
        if (!empty && !stale) {
            g_truefpsTrailQuantum = float(g.frame.w);
            g_truefpsTrailQuantumFrame = frame;
            return kTrailClient;
        }
        clock = empty ? empty : stale;
        *clock = TrailClock{trail, firstCarry, phase, highWater, frame, now};
    } else if (phase < clock->phase - 1e-5f * (std::max)(1.0f, std::fabs(clock->phase)) || highWater < clock->highWater) {
        clock->carry = firstCarry;
        clock->frame = frame;
    }
    if (clock->frame != frame) { clock->frame = frame; clock->carry += g.frame.s; }
    clock->seen = now;
    clock->highWater = highWater;
    clock->phase = phase;
    if (clock->carry + 1e-9 < native) return kTrailSkip;
    // Frames shorter than native's run whole native frames of 2 ticks; a longer one runs its whole ticks.
    const double quantum = g.frame.s + 1e-9 >= native ? std::floor(clock->carry + 1e-9)
                                                       : native * std::floor(clock->carry / native + 1e-9);
    clock->carry = (std::max)(0.0, clock->carry - quantum);
    g_truefpsTrailQuantum = float(quantum);
    g_truefpsTrailQuantumFrame = frame;
    const float after = float(double(phase) + double(g_truefpsTrailQuantum) * double(rate));   // what 0x1bcb4 will store
    if (after > phase) clock->phase = after;
    return kTrailRun;
}
// 0x1bc9c: `call <step accessor>` -> `call trailStub`. ESI = the trail; the x87 stack is empty here and at the tail;
// the skip path drops only the stub's return address. ECX/EDX kept; EAX carries the decision.
__declspec(naked) inline void trailStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsTrailSmooth], 0
        je client
        push ecx
        push edx
        push esi
        call trailDecide
        add esp, 4
        pop edx
        pop ecx
        cmp eax, 1                     // kTrailRun
        je run
        ja client                      // kTrailClient
        lock dec dword ptr [g_truefpsStepInflight]
        add esp, 4                     // kTrailSkip: the frame as after the call, straight to the draw tail
        jmp dword ptr [g_truefpsTrailTail]
    run:
        fld dword ptr [g_truefpsTrailQuantum]
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    client:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsStepAccessor]   // the accessor returns to 0x1bca1
    }
}
inline constexpr const char* kTrailPrologue = "81 EC A4 00 00 00 53 55 56 8B F1 57 8B 86 C0 14 00 00";
inline constexpr const char* kTrailTailBytes = "8B 86 AC 14 00 00 8B 8E B0 14 00 00 3B C1 7E 06 89 86 B0 14 00 00 85 C0 74 4E";
inline constexpr intptr_t kTrailPrologueFromCall = -0x1C, kTrailTailFromCall = 0x54C, kTrailNoSampleJumpFromCall = 0x310;

// Light visibility call 0xCC58A: return -1 unless a native frame boundary was crossed.
__declspec(naked) inline void visCounterStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsVisSmooth], 0
        je original
        mov eax, -1
        cmp byte ptr [g_truefpsVisAdvanced], 0
        je fresh
        mov eax, dword ptr [g_truefpsVisCounter]
    fresh:
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    original:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsFrameCounter]
    }
}

// Sites and groups (Sep-10 RVAs). Each locator must match exactly once.
enum class SiteKind : uint8_t {
    SwapImm,       // `D8 0D/D8 25/D8 1D/D8 15/D9 05 imm32`: imm32 -> a slot. Atomic.
    CallToStub,    // `E8 rel32`: rel32 -> a stub. Atomic.
    ReplaceCall,   // `length` bytes -> `call stub` + NOPs. Game thread, at Present.
    ReplaceFld,    // `D9 8x B0 0E 00 00` -> `D9 05 <slot>`. Game thread, at Present.
    Byte,          // first byte -> `newByte`. Atomic.
    Disp8,         // `7E disp8` (jle): the displacement -> newByte, the original checked against float2Bits. Atomic.
};
enum class CallTarget : uint8_t { None, Scale, ScaleOut, StepAccessor, FrameCounter, NetIcon, HistoryWriter, SegmentTest, TrailStep, JobWalker };
enum class Global : uint8_t { None, D8C, D88, Effects };
// Exact constants identify a site; banded camera factors accept retuning and use the value found.
enum class FloatBand : uint8_t { Exact, UnitPositive, UnitNegative };

struct SiteSpec {
    const char* name;
    uint32_t rva;
    const char* locator;
    int8_t locatorOffset;          // site = match + offset
    const char* context;           // also verified, at site + contextOffset (nullptr: none)
    int8_t contextOffset;
    SiteKind kind;
    uint8_t length;
    int8_t floatAt;                // imm32 at site + floatAt points at a float holding floatBits (0: none; may be negative,
                                   // in an instruction before the site that the patch leaves in)
    uint32_t floatBits;
    int8_t float2At;
    uint32_t float2Bits;
    CallTarget target;
    Global global;                 // an address the stub needs: imm32 at site + globalAt
    int8_t globalAt;
    void (*stub)();                // CallToStub / ReplaceCall
    float* slot;                   // SwapImm / ReplaceFld
    float* constantCopy;           // the fast path's copy of the client constant (floatAt)
    uint8_t newByte;
    // SwapImm only: a value another tool may leave in the operand, pointing outside the client image. A factor of
    // 1 has no per-frame decay, so the site needs no pacing and is left exactly as that tool set it. 0: none.
    uint32_t neutralBits;
    // SwapImm only: 1-based persistent cell, valid if another tool restores this operand after unload.
    // Sites using one constant in one routine share a cell: another tool may restore one saved operand to all.
    // 0: swap to `slot`.
    uint8_t cell;
    // Allowed values at `floatAt`; banded sites keep the resolved value in `constantCopy`.
    FloatBand band;
    // Pushed factor at site + contextFloatAt, within the context. Copied to `contextBits` for the stub's
    // comparison and per-frame conversion. 0: none.
    int8_t contextFloatAt;
    uint32_t* contextBits;
};

enum Group : uint8_t {
    GroupCameraInput, GroupLookAt, GroupRecenter, GroupEye, GroupResetKey, GroupLockOn, GroupFirstPersonCamera, GroupRenderPos,
    GroupHeading, GroupAlpha, GroupAnimRate, GroupMotion, GroupEffects, GroupEntities, GroupGlow, GroupVisibility, GroupPlayer,
    GroupMovementDeadband, GroupMotionFade, GroupCameraGate, GroupNetIcon, GroupHistory, GroupObstruction, GroupZoomReturn,
    GroupLightBlend, GroupShadowDirection, GroupCastBar, GroupActorState, GroupWorldPhase, GroupTrails,
    GroupEventWalk, GroupEventMove, GroupEventTimedMove, GroupSoundGrace, GroupMouseRepeat, GroupHelpDesk, GroupConnectionRetry,
    GroupActorCounters, GroupPreviewCamera, GroupWindGusts, GroupHoldBar, kGroupCount
};
static_assert(kGroupCount <= kMaxGroups, "g_truefpsGroupOn and g_truefpsGroupOffThread hold kMaxGroups groups");
static_assert(GroupVisibility == kGroupVisibilityIndex, "limiter.h's kGroupVisibilityIndex names GroupVisibility");
static_assert(GroupNetIcon == kGroupNetIconIndex, "limiter.h's kGroupNetIconIndex names GroupNetIcon");
static_assert(GroupHistory == kGroupHistoryIndex, "limiter.h's kGroupHistoryIndex names GroupHistory");
static_assert(GroupObstruction == kGroupObstructionIndex, "limiter.h's kGroupObstructionIndex names GroupObstruction");
static_assert(GroupTrails == kGroupTrailIndex, "limiter.h's kGroupTrailIndex names GroupTrails");
static_assert(GroupActorCounters == kGroupCountersIndex, "limiter.h's kGroupCountersIndex names GroupActorCounters");
static_assert(GroupWindGusts == kGroupWindGustsIndex, "limiter.h's kGroupWindGustsIndex names GroupWindGusts");
inline bool entitiesSmooth() { return g.smoothStep && g.frameSmooth && g_truefpsGroupOn[GroupEntities]; }
inline uint8_t entitiesOffThreadFlag() { return g_truefpsGroupOffThread[GroupEntities] = 1; }
// Connection retry call 0x0F1116: skipped visits leave the record unchanged and return 0 (pending).
__declspec(naked) inline void retryPaceStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        cmp byte ptr [g_truefpsGroupOn + GroupConnectionRetry], 0
        je original
        push dword ptr [esp + 4]       // the record, as the caller pushed it
        call retryGate
        add esp, 4
        test eax, eax
        jz skip
    original:
        lock dec dword ptr [g_truefpsStepInflight]
        jmp dword ptr [g_truefpsJobWalker]
    skip:
        xor eax, eax                   // not finished, the walker's own answer while a job retries
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    }
}

// The mount load wait (0x08ba1d): `inc eax; mov [esi+1CCh],eax` (7 bytes) -> `call mountCounterStub` + 2 NOPs;
// mountCounter writes the counter and returns it in EAX. ESI = the entity.
__declspec(naked) inline void mountCounterStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        push ecx
        push edx
        push esi                       // the entity
        call mountCounter
        add esp, 4
        pop edx
        pop ecx
        lock dec dword ptr [g_truefpsStepInflight]
        ret
    }
}
// Trust emote weapon hide (0x08be8f): `inc ecx; test bl,bl; mov [esi+1CCh],ecx` (9 bytes) -> `call emoteCounterStub` + 4 NOPs.
// The client's `je` reads ZF of `test bl,bl` (BL = Trust flag), so the stub runs it last. ECX = the new counter.
__declspec(naked) inline void emoteCounterStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        push esi                       // the entity
        call emoteCounter
        add esp, 4
        mov ecx, eax
        lock dec dword ptr [g_truefpsStepInflight]
        test bl, bl                    // the flags the client's `je` reads
        ret
    }
}

// Cloth wind gusts (0x188aa6): `mov eax,[esi+140h]; test eax,eax; je <decay>` (10 bytes) -> `call windGustStub` + 5 NOPs. ESI = environment node.
// windGustStep 0 = finished, leaving through the routine's own epilogue (`push ecx; push esi` at 0x188a50); non-zero runs the client's resample block after the site.
__declspec(naked) inline void windGustStub() {
    __asm {
        lock inc dword ptr [g_truefpsStepInflight]
        push ecx
        push edx
        push esi                       // the environment node
        call windGustStep
        add esp, 4
        pop edx
        pop ecx
        lock dec dword ptr [g_truefpsStepInflight]
        test eax, eax
        jz done
        ret                            // the client's own resample follows the site
    done:
        add esp, 4                     // the routine is finished: drop the return into it
        pop esi                        // and leave through its own epilogue
        pop ecx
        ret
    }
}

// After 15 unmoved pulses (~0.5 s), raise non-player cutscene movement to 2x, 3x, etc.
// Cap at 20 ticks without reducing the original step. Runner (ESI): entity +2, cursor +0x256,
// event data +0x260; position offset +0x140.
struct EventMoveClock {
    uintptr_t runner = 0;
    uint32_t frame = 0;       // the drawn frame of the last decision; a move call reads the step up to four times
    uint64_t seen = 0;        // the native frame it was last seen in
    uint16_t cursor = 0;
    float pos[3] = {};
    int32_t stalls = 0;       // pulses in a row with the same cursor and position
    float value = 0.0f;
};
inline constexpr size_t kEventClockSlots = 64;
inline constexpr int32_t kEventStallPulses = 15;
inline EventMoveClock g_truefpsEventClocks[kEventClockSlots];
inline volatile long g_truefpsEventBoosts = 0;   // bigger steps given this session
inline void resetEventClocks() { for (auto& c : g_truefpsEventClocks) c = EventMoveClock{}; }
inline float eventMoveValue(uintptr_t runner) {
    const float base = g_truefpsEventStep;
    if (!(base > 0.0f) || !runner) return base;   // between native frames nothing moves
    const uint32_t frame = g_truefpsEntFrame;
    const uint64_t now = g_truefpsUiTickTotal;
    EventMoveClock* clock = nullptr;
    EventMoveClock* empty = nullptr;
    EventMoveClock* stale = nullptr;
    for (auto& c : g_truefpsEventClocks) {
        if (c.runner == runner) { clock = &c; break; }
        if (c.runner == 0) { if (!empty) empty = &c; }
        else if (!stale && now - c.seen > 60) stale = &c;
    }
    if (clock && clock->frame == frame) return clock->value;   // the same move call's later step reads
    uint16_t cursor = 0, index = 0;
    uint32_t actor = 0;
    float pos[3] = {};
    const bool readable = readValue(runner + 0x256, cursor) && readValue(runner + 2, index) && readValue(runner + 0x260, actor) && actor &&
                          readRaw(uintptr_t(actor) + 0x140, pos, sizeof pos);
    if (!clock) {
        clock = empty ? empty : stale;
        if (!clock) return base;
        *clock = EventMoveClock{};
        clock->runner = runner;
    } else {
        const bool same = readable && cursor == clock->cursor && std::memcmp(pos, clock->pos, sizeof pos) == 0;
        clock->stalls = same ? clock->stalls + 1 : 0;
    }
    float value = base;
    const long me = g_truefpsPlayerIndex;
    if (readable && me >= 0 && clock->stalls >= kEventStallPulses && long(index) != me) {
        const int32_t level = 1 + (clock->stalls - kEventStallPulses) / kEventStallPulses;
        value = (std::max)(base, (std::min)(20.0f, base * float(1 + level)));   // 20 ticks is a long frame's own step
        if (value > base) _InterlockedIncrement(&g_truefpsEventBoosts);
    }
    clock->frame = frame;
    clock->seen = now;
    clock->cursor = cursor;
    std::memcpy(clock->pos, pos, sizeof pos);
    clock->value = value;
    return value;
}

inline float __cdecl smoothPolicyValue(uint8_t kind, uintptr_t esi) {
    switch (kind) {
    case kPolicyReset: return resetIterations();
    case kPolicyEntArrive: return g_truefpsEntArrive;
    case kPolicyEntDec: return g_truefpsEntDec;
    case kPolicyEntRot: return g_truefpsEntRot;
    case kPolicyTrail: return trailLifetimeStep();
    case kPolicyEventMove: return eventMoveValue(esi);
    case kPolicyEventTimer: return g_truefpsEventStep;   // SMove's countdown: native steps, never the stall help
    case kPolicyNativeReal: return g_truefpsNativeRealStep;
    default: return float(g.frame.w);
    }
}

inline constexpr const char* kLocLookAt = "E8 ?? ?? ?? ?? 8D 84 24 A0 00 00 00 8D 4C 24 70";
inline constexpr const char* kLocSoundGrace = "D9 86 E4 01 00 00 D8 25 ?? ?? ?? ?? D9 9E E4 01 00 00";
// The asynchronous job table's walk (0x0f10f0). Call at + 0x12.
inline constexpr const char* kLocJobWalk = "A1 ?? ?? ?? ?? 8D 74 07 04 8A 44 07 05 84 C0 74 2C 56 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 1E 8B 4E 04";
// The mount load wait (0x08ba1d), phase 0xe (the counter's tests against 150 and 151). Increment at + 0x1f.
inline constexpr const char* kLocMountWait =
    "3D 96 00 00 00 72 18 8B 86 A0 00 00 00 89 A8 9C 05 00 00 C7 86 CC 01 00 00 97 00 00 00 EB 24 40 89 86 CC 01 00 00 8B 8E A4 00 00 00 3B CD 74 13 E8";
// The Trust emote weapon hide (0x08be8f), in phase 0xb. Increment at + 6.
inline constexpr const char* kLocEmoteHide =
    "8B 8E CC 01 00 00 41 84 DB 89 8E CC 01 00 00 0F 84 ?? ?? ?? ?? 8B 96 C8 01 00 00 8B 8E A0 00 00 00 52 8B 96 C4 01 00 00";
// The preview camera (0x24de20), yaw and pitch, told apart by the read-back (ecx yaw, eax pitch). Away + 0, to + 8.
inline constexpr const char* kLocPreviewYaw = "D8 25 ?? ?? ?? ?? EB 06 D8 05 ?? ?? ?? ?? D9 5C 24 40 8B 4C 24 40";
inline constexpr const char* kLocPreviewPitch = "D8 25 ?? ?? ?? ?? EB 06 D8 05 ?? ?? ?? ?? D9 5C 24 40 8B 44 24 40";
// Its two step calls, one locator for both (yaw ends `fld [esi+44h]`, pitch `fld [esi+48h]`). First + 11, then + 0xb6.
inline constexpr const char* kLocPreviewCamera =
    "D9 E1 DD 5C 24 14 E8 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? DC 5C 24 14 DF E0 F6 C4 05 7A 26 D9 44 24 08 D8 1D ?? ?? ?? ?? D9 46 44";
inline constexpr const char* kLocWindGust =
    "8B 86 40 01 00 00 85 C0 74 29 E8 ?? ?? ?? ?? 99 F7 BE 3C 01 00 00 03 96 38 01 00 00 89 54 24 04 DB 44 24 04 "
    "D8 0D ?? ?? ?? ?? D9 9E 34 01 00 00 5E 59 C3 D9 86 34 01 00 00 D8 25 ?? ?? ?? ?? D9 96 34 01 00 00 "
    "D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 0A C7 86 34 01 00 00 00 00 00 00 5E 59 C3";
inline constexpr const char* kCtxWindGust = "51 56 8B F1 E8 ?? ?? ?? ?? D8 AE 44 01 00 00 D9 96 44 01 00 00";
inline constexpr const char* kLocTrail = "8B 86 C0 14 00 00 85 C0 74 04 8B 28 EB 02 33 ED E8 ?? ?? ?? ?? D8 8E BC 14 00 00";
inline constexpr const char* kLocGlow = "E8 ?? ?? ?? ?? C1 E0 04 99 B9 68 01 00 00 89 6C 24 10 F7 F9";
inline constexpr const char* kLocVis = "8B BE F0 09 00 00 E8 ?? ?? ?? ?? 25 03 00 00 80 79 05 48 83 C8 FC 40 3B C7";
inline constexpr const char* kLocRenderX = "D8 0D ?? ?? ?? ?? 83 C4 0C D9 5C 24 10 D9 44 24 14";
inline constexpr const char* kLocLightBlend = "D8 0D ?? ?? ?? ?? D9 54 24 10 D8 1D ?? ?? ?? ?? DF E0 25 00 01 00 00 75 08 C7 44 24 10 00 00 80 3F";
inline constexpr const char* kLocShadowFirst = "C7 46 08 00 00 00 00 E8 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D9 54 24 18 D8 1D";
inline constexpr const char* kLocShadowSecond = "50 E8 ?? ?? ?? ?? 83 C4 10 E8 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D9 54 24 18 D8 1D";
// Wildcard the trailing default zoom distance: other tools retune it, and the locator is unique without it.
inline constexpr const char* kLocZoomReturn =
    "E8 ?? ?? ?? ?? D8 2D ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D9 5C 24 10 D9 05 ?? ?? ?? ?? D8 5C 24 10 DF E0 F6 C4 05 7A 2A "
    "D9 44 24 10 D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 19 C7 44 24 10 ?? ?? ?? ??";

// clang-format off
inline const SiteSpec kSites[] = {
    // P1 look-at follow loop
    {"look-at factor call", 0x1f5d9, kLocLookAt, 0, "68 ?? ?? ?? ?? 52", -6, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::Scale, Global::None, 0, &scaleStub025, nullptr, nullptr, 0, 0, 0, FloatBand::Exact, -5, &g_truefpsLookAtKBits},
    // P2 recenter loop
    {"recenter factor call", 0x1f6d1, "E8 ?? ?? ?? ?? 8D 44 24 34 8D 4C 24 34 50 55 51", 0, "68 ?? ?? ?? ?? 8D 54 24 2C 51 52", -11, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::ScaleOut, Global::None, 0, &scaleStub005, nullptr, nullptr, 0, 0, 0, FloatBand::Exact, -10, &g_truefpsRecenterKBits},
    // P3 eye follow loop
    {"eye hold countdown", 0x1fa55, "D8 25 ?? ?? ?? ?? D9 15 ?? ?? ?? ?? D8 1D ?? ?? ?? ?? DF E0", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits1, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsSigma, nullptr, 0},
    {"eye auto-rotate hold", 0x1fb96, "A1 ?? ?? ?? ?? 48 A3 ?? ?? ?? ?? 79 0C C7 05 ?? ?? ?? ?? 00 00 00 00", 0, nullptr, 0, SiteKind::ReplaceCall, 11, 0, 0, 0, 0, CallTarget::None, Global::D8C, 1, &d8cStub, nullptr, nullptr, 0},
    {"eye auto-rotate factor", 0x1fcc8, "D8 0D ?? ?? ?? ?? D9 1C 24 50 51 E8 ?? ?? ?? ??", 0, nullptr, 0, SiteKind::ReplaceCall, 6, 2, kBits005, 0, 0, CallTarget::None, Global::None, 0, &rotStub, nullptr, &g_truefpsK005, 0, 0, 0, FloatBand::UnitPositive},
    // Native factor 0.5; another tool's 1.0 snaps without decay. No shared jitter operand, so no persistent cell.
    {"eye spring beyond 100", 0x1fd26, "D8 0D ?? ?? ?? ?? EB 24 D9 44 24 10 D8 1D ?? ?? ?? ??", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits05, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF05, nullptr, 0, kBits1},
    // The site is the seven bytes after the client's own `fmul [0.012]` (0x1fd4c), which stays in: floatAt -4 names
    // that constant. The 6.0 distance two instructions earlier is identification only (the context), not a pinned value.
    {"eye spring 6..100", 0x1fd52, "D8 0D ?? ?? ?? ?? D9 1C 24 8D 44 24 2C 50 E8 ?? ?? ?? ??", 6, "D9 44 24 10 D8 25 ?? ?? ?? ?? 51", -17, SiteKind::ReplaceCall, 7, -4, 0x3C449BA6, 0, 0, CallTarget::None, Global::None, 0, &springStub, nullptr, &g_truefpsK0012, 0, 0, 0, FloatBand::UnitPositive},
    // No float2 pin: the 3.0 rest point 13 bytes back is a camera-distance constant other tools rewrite, and
    // pushFactor takes x and d live, so it never reads that constant. The context already wildcards its operand.
    {"eye push-out", 0x1fda5, "D8 0D ?? ?? ?? ?? D9 1C 24 50 E8 ?? ?? ?? ?? 8D 4C 24 30", 0, "D9 05 ?? ?? ?? ?? D8 64 24 10 51 8D 44 24 2C", -15, SiteKind::ReplaceCall, 6, 2, 0xBE000000, 0, 0, CallTarget::None, Global::None, 0, &pushStub, nullptr, &g_truefpsKm0125, 0, 0, 0, FloatBand::UnitNegative},
    // The two push multipliers are the jitter another tool removes by pointing them at its own 1.0: no decay to pace.
    // Both name one client constant, so they share one cell: xicamera saves the x operand and writes it back into both.
    {"eye horizontal push x", 0x1fe36, "D8 0D ?? ?? ?? ?? D9 5C 24 38 D9 44 24 40 D8 C9", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits0125, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF0125, nullptr, 0, kBits1, 1},
    {"eye horizontal push z", 0x1fe46, "D8 0D ?? ?? ?? ?? D9 5C 24 40 DD D8 E8 ?? ?? ?? ??", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits0125, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF0125, nullptr, 0, kBits1, 1},
    // P4 reset-camera key
    {"reset-key factor call", 0x2017c, "E8 ?? ?? ?? ?? 8D 94 24 C0 00 00 00 8D 84 24 80 00 00 00", 0, "68 ?? ?? ?? ?? 51", -6, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::Scale, Global::None, 0, &scaleStubReset, nullptr, nullptr, 0, 0, 0, FloatBand::Exact, -5, &g_truefpsResetKBits},
    {"reset-key frame counter", 0x201a8, "FF 0D ?? ?? ?? ?? 8D 8C 24 F4 00 00 00 E8 ?? ?? ?? ??", 0, nullptr, 0, SiteKind::ReplaceCall, 6, 0, 0, 0, 0, CallTarget::None, Global::D88, 2, &d88Stub, nullptr, nullptr, 0},
    // P5 lock-on distance
    {"lock-on factor", 0x205cd, "D9 05 ?? ?? ?? ?? D8 C9 D9 C0 DE C3 DE E9 D9 CA", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits025, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF025, nullptr, 0},
    // P6 first-person camera (the view toggle's camera, 0x21110)
    {"first-person camera look-at call", 0x2119d, "E8 ?? ?? ?? ?? 8D 54 24 14 52 56 E8 ?? ?? ?? ??", 0, "68 ?? ?? ?? ?? 51", -6, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::Scale, Global::None, 0, &scaleStub0125, nullptr, nullptr, 0, 0, 0, FloatBand::Exact, -5, &g_truefpsFirstPersonKBits},
    {"first-person camera eye call", 0x2121f, "E8 ?? ?? ?? ?? 8D 4C 24 14 51 56 E8 ?? ?? ?? ??", 0, "68 ?? ?? ?? ?? 50", -6, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::Scale, Global::None, 0, &scaleStub0125, nullptr, nullptr, 0, 0, 0, FloatBand::Exact, -5, &g_truefpsFirstPersonKBits},
    // P7 render position
    // All three use the camera jitter constant; an external 1.0 disables smoothing.
    // Share a cell so another tool can restore one saved operand to all three.
    {"render position x", 0xc65ee, kLocRenderX, 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits0125, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF0125, nullptr, 0, kBits1, 2},
    {"render position y", 0xc65ff, "D8 0D ?? ?? ?? ?? D9 5C 24 14 D9 44 24 18 D8 0D ?? ?? ?? ?? D9 5C 24 18 D9 44 24 3C", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits0125, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF0125, nullptr, 0, kBits1, 2},
    {"render position z", 0xc660d, "D8 0D ?? ?? ?? ?? D9 5C 24 18 D9 44 24 3C D8 44 24 10", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits0125, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF0125, nullptr, 0, kBits1, 2},
    // P8 heading
    {"heading loop A call", 0xc6882, "FF 90 3C 01 00 00 89 44 24 14 DB 44 24 14 D8 0D ?? ?? ?? ?? D9 5C 24 14 E8 ?? ?? ?? ??", 24, "8B 6C 24 14", 0x1C, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::StepAccessor, Global::None, 0, &headingStub, nullptr, nullptr, 0},
    {"heading loop B call", 0xc6c64, "FF 92 3C 01 00 00 89 44 24 14 DB 44 24 14 D8 0D ?? ?? ?? ?? D9 5C 24 14 E8 ?? ?? ?? ??", 24, "8B 6C 24 14", 0x1C, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::StepAccessor, Global::None, 0, &headingStub, nullptr, nullptr, 0},
    // P9 locomotion animation rate EMA
    {"animation rate old", 0xc8b36, "D8 0D ?? ?? ?? ?? D9 C9 D8 0D ?? ?? ?? ?? DE C1 D9 5C 24 04", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits075, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsQ075, nullptr, 0},
    {"animation rate new", 0xc8b3e, "D8 0D ?? ?? ?? ?? DE C1 D9 5C 24 04 8B 06 8B CE", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits025, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsP025, nullptr, 0},
    // P9 effects engine: the three readers of the cached step that truncate it read the whole-tick copy
    {"effects colour fade", 0x3e50e, "D9 80 B0 0E 00 00 8B F1 0F BF 4F 02 89 4C 24 18", 0, "A1 ?? ?? ?? ??", -12, SiteKind::ReplaceFld, 6, 0, 0, 0, 0, CallTarget::None, Global::Effects, -11, nullptr, &g_truefpsWEB0, nullptr, 0},
    {"effects +0xD6 A", 0x4e823, "D9 81 B0 0E 00 00 D8 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B CF", 0, "8B 0D ?? ?? ?? ??", -6, SiteKind::ReplaceFld, 6, 0, 0, 0, 0, CallTarget::None, Global::Effects, -4, nullptr, &g_truefpsWEB0, nullptr, 0},
    {"effects +0xD6 B", 0x5396d, "D9 81 B0 0E 00 00 D8 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B CE", 0, "8B 0D ?? ?? ?? ?? 66 8B AE D6 00 00 00", -13, SiteKind::ReplaceFld, 6, 0, 0, 0, 0, CallTarget::None, Global::Effects, -11, nullptr, &g_truefpsWEB0, nullptr, 0},
    // P10 remote-entity interpolation (install order: the divisor stub before the fild -> fld byte)
    {"entity path 1 divisor", 0x8d340, "0F BF AE F0 00 00 00 89 6C 24 18 E8 ?? ?? ?? ??", 0, nullptr, 0, SiteKind::ReplaceCall, 11, 0, 0, 0, 0, CallTarget::None, Global::None, 0, &entStub1, nullptr, nullptr, 0},
    {"entity path 1 fild", 0x8d359, "DB 44 24 18 D9 46 24 D8 64 24 2C D8 F1 D9 5E 54", 0, nullptr, 0, SiteKind::Byte, 4, 0, 0, 0, 0, CallTarget::None, Global::None, 0, nullptr, nullptr, nullptr, 0xD9},
    {"entity path 2 divisor", 0x8d5c2, "0F BF C8 89 4C 24 18 DB 44 24 18 D9 46 24 D8 66 04", 0, nullptr, 0, SiteKind::ReplaceCall, 11, 0, 0, 0, 0, CallTarget::None, Global::None, 0, &entStub2, nullptr, nullptr, 0},
    // Target name pulse (per-frame counter)
    {"target name pulse counter call", 0x83197, kLocGlow, 0, "F6 87 88 00 00 00 04 74 52 33 C9 8A 4E 03 8B E9", -16, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::FrameCounter, Global::None, 0, &glowCounterStub, nullptr, nullptr, 0},
    // Light visibility re-check (per-frame counter mod 4 against the actor's phase at +0x9F0)
    {"light visibility counter call", 0xcc58a, kLocVis, 6, "25 03 00 00 80 79 05 48 83 C8 FC 40 3B C7 74", 5, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::FrameCounter, Global::None, 0, &visCounterStub, nullptr, nullptr, 0},
// Scale the 0.01/frame movement deadband by native time; otherwise walking stops above ~140 FPS.
// Sites cover animation and facing length/z/x checks.
    {"movement deadband", 0xc7100, "D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 27 8B 16", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits001, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF001, nullptr, 0},
    {"facing deadband length", 0xc6bbc, "D8 1D ?? ?? ?? ?? DF E0 25 00 01 00 00 75 5E", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits001, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF001, nullptr, 0},
    {"facing deadband z", 0xc6be2, "D8 1D ?? ?? ?? ?? DF E0 25 00 41 00 00 74 26 D9 44 24 18", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits001, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF001, nullptr, 0},
    {"facing deadband x", 0xc6c08, "D8 1D ?? ?? ?? ?? DF E0 25 00 41 00 00 75 12 D9 44 24 20", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits001, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF001, nullptr, 0},
    // Part of the movement deadband group: the walk-rate function's own "did it move" gate, 0.0001 per frame (0x0C8A74).
    {"movement deadband fine", 0xc8a74, "D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 0D C7 44 24 04 00 00 00 00", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits00001, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsF00001, nullptr, 0},
// Camera collision gates (0x020796, 0x020B37) suppress small movements.
// Use zero on sub-native frames, otherwise preserve the native thresholds.
    {"camera collision gate", 0x20796, "D8 1D ?? ?? ?? ?? 83 C4 08 5D DF E0 25 00 41 00 00 74 0D A1", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits001, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFC001, nullptr, 0},
    {"camera settle gate", 0x20b37, "D8 1D ?? ?? ?? ?? 83 C4 08 DF E0 F6 C4 05 7A 16 8D 84 24 D4", 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits00001, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFC00001, nullptr, 0},
    {"network icon call", 0x20143c, "8B 4C 24 18 E8 ?? ?? ?? ?? 8B 5C 24 24 8B 54 24 10 0F BF FD", 4, nullptr, 0, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::NetIcon, Global::None, 0, &netIconStub, nullptr, nullptr, 0},
    {"camera history call", 0x1ff3f, "6A 01 51 8D 8F BC 00 00 00 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? A1", 9, nullptr, 0, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::HistoryWriter, Global::None, 0, &historyStub, nullptr, nullptr, 0},
    {"camera obstruction test", 0x1ff29, "8B 0D ?? ?? ?? ?? 8D 94 24 8C 00 00 00 8D 6F 44 52 8D 44 24 60 55 50 E8 ?? ?? ?? ?? 84 C0 75 17", 23, nullptr, 0, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::SegmentTest, Global::None, 0, &obstructionStub, nullptr, nullptr, 0},
    {"zoom return factor", 0x1f782, kLocZoomReturn, 11, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits025, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFZ025, nullptr, 0},
    {"zoom return snap low", 0x1f78c, kLocZoomReturn, 21, nullptr, 0, SiteKind::SwapImm, 6, 2, kBitsM1, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFZLow, nullptr, 0},
    {"zoom return snap high", 0x1f7a1, kLocZoomReturn, 42, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits1, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFZHigh, nullptr, 0},
    // The actor light blend, `min(0.4 * step, 1)` once per actor draw; with S the operand is the frame's nativeEase.
    {"actor light blend", 0xcb4f2, kLocLightBlend, 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits04, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFL04, nullptr, 0},
    // The projected-shadow direction, two filters of `min(0.04 * step, 1)`; the same conversion.
    {"shadow direction first", 0x33ed6, kLocShadowFirst, 12, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits004, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFL004, nullptr, 0},
    {"shadow direction second", 0x33fb0, kLocShadowSecond, 14, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits004, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFL004, nullptr, 0},
    {"weapon trail update", 0x1bc9c, kLocTrail, 16, nullptr, 0, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::TrailStep, Global::None, 0, &trailStub, nullptr, nullptr, 0},
    // The sound-start grace, counted down in native frames (g_truefpsFSound).
    {"sound start grace", 0x3650a, kLocSoundGrace, 6, nullptr, 0, SiteKind::SwapImm, 6, 2, kBits1, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsFSound, nullptr, 0},
// Help Desk polls trunc(step/2) times. At 0/1 ticks, native jle returns ECX as a record.
// Retarget it to the EAX=0 exit; float2Bits holds the original displacement.
    {"help desk poll A zero budget", 0x2160b9, "8B F8 33 F6 85 FF 7E 18 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 75 11", 6, nullptr, 0, SiteKind::Disp8, 2, 0, 0, 0, 0x18, CallTarget::None, Global::None, 0, nullptr, nullptr, nullptr, 0x12},
    {"help desk poll B zero budget", 0x2164d9, "8B F8 33 F6 85 FF 7E 12 E8 ?? ?? ?? ?? 85 C0 75 0D 46 3B F7 7C F2", 6, nullptr, 0, SiteKind::Disp8, 2, 0, 0, 0, 0x12, CallTarget::None, Global::None, 0, nullptr, nullptr, nullptr, 0x0E},
    {"connection retry pacing", 0xf1116, kLocJobWalk, 0x12, nullptr, 0, SiteKind::CallToStub, 5, 0, 0, 0, 0, CallTarget::JobWalker, Global::None, 0, &retryPaceStub, nullptr, nullptr, 0},

    {"mount load wait", 0x8ba1d, kLocMountWait, 0x1f, nullptr, 0, SiteKind::ReplaceCall, 7, 0, 0, 0, 0, CallTarget::None, Global::None, 0, &mountCounterStub, nullptr, nullptr, 0},
    {"trust emote weapon hide", 0x8be8f, kLocEmoteHide, 6, nullptr, 0, SiteKind::ReplaceCall, 9, 0, 0, 0, 0, CallTarget::None, Global::None, 0, &emoteCounterStub, nullptr, nullptr, 0},

    // Preview camera turns 6 degrees (float bits 0x3DD67750) on native frame boundaries, zero between.
    {"preview camera yaw away", 0x24dec2, kLocPreviewYaw, 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBitsSixDeg, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsSixDegreeNative, nullptr, 0},
    {"preview camera yaw towards", 0x24deca, kLocPreviewYaw, 8, nullptr, 0, SiteKind::SwapImm, 6, 2, kBitsSixDeg, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsSixDegreeNative, nullptr, 0},
    {"preview camera pitch away", 0x24df78, kLocPreviewPitch, 0, nullptr, 0, SiteKind::SwapImm, 6, 2, kBitsSixDeg, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsSixDegreeNative, nullptr, 0},
    {"preview camera pitch towards", 0x24df80, kLocPreviewPitch, 8, nullptr, 0, SiteKind::SwapImm, 6, 2, kBitsSixDeg, 0, 0, CallTarget::None, Global::None, 0, nullptr, &g_truefpsSixDegreeNative, nullptr, 0},

    // The cloth wind gusts: float2Bits pins the decay's 0.01 (imm32 at + 0x3B); the context pins the routine's head.
    {"cloth wind gusts", 0x188aa6, kLocWindGust, 0, kCtxWindGust, -0x56, SiteKind::ReplaceCall, 10, 0, 0, 0x3B, kBits001, CallTarget::None, Global::None, 0, &windGustStub, nullptr, nullptr, 0},
};
// clang-format on
inline constexpr size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

// Step-site return RVA, policy and expected consumer opcodes. Failed validation falls back to whole ticks.
struct EntrySpec { uint32_t rva; uint8_t kind; const char* use; };
// The client's __ftol (truncation via the control word). A `use` that starts with a call must call it.
inline constexpr const char* kFtolBytes = "55 8B EC 83 C4 F4 9B D9 7D FE 9B 66 8B 45 FE 80 CC 0C 66 89 45 FC D9 6D FC DF 7D F4 D9 6D FE 8B 45 F4 8B 55 F8 C9 C3";
struct GroupSpec {
    const char* name;
    uint8_t firstSite, siteCount;
    const char* anchor;            // locates the group's call sites: return address = match + anchorOffset + (rva - anchorRva)
    int8_t anchorOffset;
    uint32_t anchorRva;
    EntrySpec entries[16];         // PolicyEntry local[] in resolve is sized from this
    uint8_t entryCount;
    bool optional = false;         // missing anchor and marker mean absent, not failed
    const char* marker = nullptr;  // feature signature: if present, the optional routine must resolve
    const char* markerName = nullptr;   // marker description for failures
    const char* absentNote = nullptr;   // diagnostic for an absent routine
};
// clang-format off
inline constexpr GroupSpec kGroups[kGroupCount] = {
    // Entries 0x1f024 and 0x1f0f7 overlap another tool's pan-speed signatures. Keep them read-only policy entries.
    {"camera orbit, pitch and zoom", 0, 0, kLocLookAt, 0, 0x1f5d9, {{0x1f024, kPolicyS, "D8 4C 24 ?? 8B 06"}, {0x1f0f7, kPolicyS, "D8 4C 24 ?? 8B 16"}, {0x1f834, kPolicyS, "D8 0D ?? ?? ?? ?? D8 44 24 ??"}, {0x1f884, kPolicyS, "D8 0D ?? ?? ?? ?? D8 6C 24 ??"}, {0x1f8da, kPolicyS, "D8 0D ?? ?? ?? ?? D8 44 24 ??"}, {0x1f922, kPolicyS, "D8 0D ?? ?? ?? ?? D8 6C 24 ??"}}, 6},
    {"camera look-at follow", 0, 1, kLocLookAt, 0, 0x1f5d9, {{0x1f5a7, kPolicyN, "E8 ?? ?? ?? ?? 85 C0"}}, 1},
    {"camera recenter", 1, 1, "E8 ?? ?? ?? ?? 8D 44 24 34 8D 4C 24 34 50 55 51", 0, 0x1f6d1, {{0x1f670, kPolicyN, "E8 ?? ?? ?? ?? 85 C0"}, {0x1f716, kPolicyN, "E8 ?? ?? ?? ?? 3B E8"}}, 2},
    {"camera eye follow", 2, 8, "D8 25 ?? ?? ?? ?? D9 15 ?? ?? ?? ?? D8 1D ?? ?? ?? ?? DF E0", 0, 0x1fa55, {{0x1fa3d, kPolicyN, "E8 ?? ?? ?? ?? 85 C0"}, {0x1fe81, kPolicyN, "E8 ?? ?? ?? ?? 3B E8"}}, 2},
    {"camera reset key", 10, 2, "E8 ?? ?? ?? ?? 8D 94 24 C0 00 00 00 8D 84 24 80 00 00 00", 0, 0x2017c, {{0x2014d, kPolicyReset, "E8 ?? ?? ?? ?? 85 C0"}}, 1},
    {"camera lock-on distance", 12, 1, "D9 05 ?? ?? ?? ?? D8 C9 D9 C0 DE C3 DE E9 D9 CA", 0, 0x205cd, {{0x205b6, kPolicyN, "D8 15 ?? ?? ?? ?? DF E0"}}, 1},
    {"first-person camera", 13, 2, "E8 ?? ?? ?? ?? 8D 54 24 14 52 56 E8 ?? ?? ?? ??", 0, 0x2119d, {{0x2118a, kPolicyN, "E8 ?? ?? ?? ?? 85 C0"}, {0x211c6, kPolicyN, "E8 ?? ?? ?? ?? 3B F8"}, {0x2120c, kPolicyN, "E8 ?? ?? ?? ?? 85 C0"}, {0x21248, kPolicyN, "E8 ?? ?? ?? ?? 3B F8"}}, 4},
    {"actor render position", 15, 3, kLocRenderX, 0, 0xc65ee, {{0xc65bf, kPolicyN, "D9 54 24 ?? D8 1D ?? ?? ?? ??"}}, 1},
    {"actor heading", 18, 2, "FF 90 3C 01 00 00 89 44 24 14 DB 44 24 14 D8 0D ?? ?? ?? ?? D9 5C 24 14 E8 ?? ?? ?? ??", 24, 0xc6882, {{0xc6887, kPolicyN, "D9 54 24 ?? D8 1D ?? ?? ?? ??"}, {0xc6c69, kPolicyN, "D9 54 24 ?? D8 1D ?? ?? ?? ??"}}, 2},
    {"actor fades", 0, 0, kLocRenderX, 0, 0xc65ee, {{0xc76cf, kPolicyS, "D8 0D ?? ?? ?? ?? D8 44 24 ??"}, {0xc7f18, kPolicyS, "D8 0D ?? ?? ?? ?? D8 6C 24 ??"}}, 2},
    {"walk animation rate", 20, 2, "D8 0D ?? ?? ?? ?? D9 C9 D8 0D ?? ?? ?? ?? DE C1 D9 5C 24 04", 0, 0xc8b36, {{0xc8acd, kPolicyS, "D8 4C 24 ?? EB ??"}, {0xc8aec, kPolicyS, "D8 4C 24 ?? D9 54 24 ??"}}, 2},
    {"motion playback", 0, 0, "D8 05 ?? ?? ?? ?? EB 0C E8 ?? ?? ?? ?? D8 4C 24 10", 0, 0x1a9a3, {{0x1a9b0, kPolicyS, "D8 4C 24 ?? D8 46 ??"}}, 1},
    {"effects engine", 22, 3, "E8 ?? ?? ?? ?? D9 9E B0 0E 00 00 5F 5E 83 C4 08", 0, 0x6a000, {{0x6a005, kPolicyS, "D9 9E ?? ?? ?? ?? 5F"}}, 1},
    {"other actors' movement", 25, 3, "0F BF AE F0 00 00 00 89 6C 24 18 E8 ?? ?? ?? ??", 0, 0x8d340, {{0x8d350, kPolicyEntArrive, "E8 ?? ?? ?? ?? 3B E8"}, {0x8d388, kPolicyS, "D8 4E ?? D8 07"}, {0x8d394, kPolicyS, "D8 4E ?? D8 46 ??"}, {0x8d3a2, kPolicyS, "D8 4E ?? D8 46 ??"}, {0x8d3c3, kPolicyEntDec, "E8 ?? ?? ?? ?? 66 29 86 ?? ?? ?? ??"}, {0x8d622, kPolicyS, "D8 4E ?? D8 46 ??"}, {0x8d630, kPolicyS, "D8 4E ?? D8 46 ??"}, {0x8d63e, kPolicyS, "D8 4E ?? D8 46 ??"}, {0x8d64c, kPolicyEntRot, "D8 4E ?? D8 46 ??"}, {0x8d65a, kPolicyEntRot, "D8 4E ?? D8 46 ??"}, {0x8d668, kPolicyEntRot, "D8 4E ?? D8 46 ??"}, {0x8d676, kPolicyEntDec, "E8 ?? ?? ?? ?? 66 29 86 ?? ?? ?? ??"}}, 12},
    {"target name pulse", 28, 1, kLocGlow, 0, 0x83197, {}, 0},
    {"light visibility re-check", 29, 1, kLocVis, 0, 0xcc584, {}, 0},
    {"player movement", 0, 0, "81 EC 88 00 00 00 56 57 8B F1 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? D9 5C 24 20", 0, 0xa7b80, {{0xa7b8f, kPolicyS, "8B 0D ?? ?? ?? ?? D9 5C 24 ??"}}, 1},
    {"movement deadband", 30, 5, "D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 27 8B 16", 0, 0xc7100, {}, 0},
    // Crossfade 0x01B5D0 advances by step*increment and decrements duration by the same step.
    {"motion transition fade", 0, 0, "E8 ?? ?? ?? ?? D9 5C 24 04 33 C0 B9 00 00 80 3F", 0, 0x1b5d4, {{0x1b5d9, kPolicyS, "D9 5C 24 04 33 C0"}}, 1},
    {"camera collision gate", 35, 2, "D8 1D ?? ?? ?? ?? 83 C4 08 5D DF E0 25 00 41 00 00 74 0D A1", 0, 0x20796, {}, 0},
    {"network activity icon", 37, 1, "8B 4C 24 18 E8 ?? ?? ?? ?? 8B 5C 24 24 8B 54 24 10 0F BF FD", 4, 0x20143c, {}, 0},
    {"camera recovery history", 38, 1, "6A 01 51 8D 8F BC 00 00 00 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? A1", 9, 0x1ff3f, {}, 0},
    {"camera wall contact", 39, 1, "8B 0D ?? ?? ?? ?? 8D 94 24 8C 00 00 00 8D 6F 44 52 8D 44 24 60 55 50 E8 ?? ?? ?? ?? 84 C0 75 17", 23, 0x1ff29, {}, 0},
    {"zoom return", 40, 3, kLocZoomReturn, 11, 0x1f782, {}, 0},
    // The light blend's step call, and the target highlight pulse in the same actor draw: a float timer restarting at 40.
    {"actor light blend and highlight pulse", 43, 1, kLocLightBlend, 0, 0xcb4f2, {{0xcb4f2, kPolicyS, "D8 0D ?? ?? ?? ?? D9 54 24 ??"}, {0xcbc6d, kPolicyS, "D8 AF ?? ?? ?? ?? D9 97 ?? ?? ?? ??"}}, 2},
    {"shadow direction", 44, 2, kLocShadowFirst, 12, 0x33ed6, {{0x33ed6, kPolicyS, "D8 0D ?? ?? ?? ?? D9 54 24 ??"}, {0x33fb0, kPolicyS, "D8 0D ?? ?? ?? ?? D9 54 24 ??"}}, 2},
    // The cast bar (casttime, +0x18): a float countdown that fills the bar every frame with S.
    {"cast bar", 0, 0, "E8 ?? ?? ?? ?? D8 6E 18 D9 56 18 D8 1D ?? ?? ?? ?? DF E0 F6 C4", 5, 0x12c17e, {{0x12c17e, kPolicyS, "D8 6E ?? D9 56 ??"}}, 1},
    // An actor's animation/effect-state timer: when a state change starts, to the frame instead of the tick.
    {"actor state timer", 0, 0, "E8 ?? ?? ?? ?? D8 6C 24 08 D9 96 20 08 00 00 D8 1D", 5, 0xcaeef, {{0xcaeef, kPolicyS, "D8 6C 24 ?? D9 96 ?? ?? ?? ??"}}, 1},
    // World mesh deformation: a node's float phase (wraps past 300 keeping the remainder) weights its morph every frame.
    {"world deformation phase", 0, 0, "E8 ?? ?? ?? ?? D8 44 24 10 D8 15 ?? ?? ?? ?? DF E0 25 00 41 00 00", 5, 0x17a4c0, {{0x17a4c0, kPolicyS, "D8 44 24 ?? D8 15 ?? ?? ?? ??"}}, 1},
    // Weapon trails: the paced update and the lifetime countdown in its aging helper, on the same ticks.
    {"weapon trails", 46, 1, kLocTrail, 16, 0x1bc9c, {{0x1c5b4, kPolicyTrail, "D8 AE C8 14 00 00 8B 86 ?? ?? ?? ??"}}, 1},
    // Cutscene moves (g_truefpsEventStep): each handler's arrival test and per-axis moves; anchor = its `fstp [dist]; jcc`.
    {"cutscene walking", 0, 0, "D9 5C 24 10 0F 84 ?? ?? ?? ?? 8B 96 60 02 00 00 8B 82 68 01 00 00 89 44 24 14 E8 ?? ?? ?? ?? D8 4C 24 14 D8 0D ?? ?? ?? ?? D8 5C 24 10 DF E0 25 00 01 00 00 0F 84 ?? ?? ?? ?? D9 05 ?? ?? ?? ?? D8 74 24 10 8B 86 60 02 00 00 D9 54 24 10",
     0, 0xb2f34,
     {{0xb2f53, kPolicyEventMove, "D8 4C 24 14 D8 0D ?? ?? ?? ?? D8 5C 24 10"}, {0xb2fa3, kPolicyEventMove, "D8 4C 24 14 8B 86 60 02 00 00 D8 0D"},
      {0xb2fe6, kPolicyEventMove, "D8 4C 24 18 8B 86 60 02 00 00 D8 0D"}}, 3},
    {"cutscene moves", 0, 0, "D9 5C 24 10 0F 84 ?? ?? ?? ?? 8B 96 60 02 00 00 8B 82 68 01 00 00 89 44 24 14 E8 ?? ?? ?? ?? D8 4C 24 14 D8 0D ?? ?? ?? ?? D8 5C 24 10 DF E0 25 00 01 00 00 0F 84 ?? ?? ?? ?? D9 05 ?? ?? ?? ?? D8 74 24 10 8B 86 60 02 00 00 8B 88 40 01 00 00",
     0, 0xb328e,
     {{0xb32ad, kPolicyEventMove, "D8 4C 24 14 D8 0D ?? ?? ?? ?? D8 5C 24 10"}, {0xb32f5, kPolicyEventMove, "D8 4C 24 14 8B 86 60 02 00 00 D8 0D"},
      {0xb3338, kPolicyEventMove, "D8 4C 24 18 8B 86 60 02 00 00 D8 0D"}, {0xb337b, kPolicyEventMove, "D8 4C 24 18 8B 86 60 02 00 00 D8 0D"}}, 4},
    // SMove also counts its move time down by the step and truncates it (0xb5790, __ftol): whole ticks.
    {"cutscene timed moves", 0, 0, "D9 5C 24 10 0F 8E ?? ?? ?? ?? 8B 96 60 02 00 00 8B 82 68 01 00 00 89 44 24 14 E8 ?? ?? ?? ?? D8 4C 24 14 D8 0D ?? ?? ?? ?? D8 5C 24 10 DF E0 25 00 01 00 00 0F 85",
     0, 0xb56d8,
     {{0xb56f7, kPolicyEventMove, "D8 4C 24 14 D8 0D ?? ?? ?? ?? D8 5C 24 10"}, {0xb5790, kPolicyEventTimer, "D8 6C 24 18 E8 ?? ?? ?? ??"},
      {0xb5802, kPolicyEventMove, "D8 4C 24 14 8B 86 60 02 00 00 D8 0D"}, {0xb5845, kPolicyEventMove, "D8 4C 24 18 8B 86 60 02 00 00 D8 0D"},
      {0xb5882, kPolicyEventMove, "D8 4C 24 18 8B 86 60 02 00 00 D8 0D"}}, 5},
    {"sound start grace", 47, 1, kLocSoundGrace, 6, 0x3650a, {}, 0},
    // Mouse repeat 0x1248a0 truncates through __ftol: supply native 0/2 ticks for its 24/12-tick countdown.
    {"held mouse button repeat", 0, 0, "81 7E 70 00 C0 79 C4 75 0B C7 46 70 00 00 C0 41 5F 5E 59 C3 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F BF C8", 0, 0x1248c8,
     {{0x1248e1, kPolicyNativeReal, "E8 ?? ?? ?? ?? 0F BF C8"}}, 1},
    // Help Desk pollers use native steps. Wildcard branch displacements changed by site patches.
    {"help desk polling", 48, 2, "51 56 57 E8 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F8 33 F6 85 FF 7E 18", 8, 0x2160a8,
     {{0x2160a8, kPolicyNativeReal, "D8 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F8 33 F6 85 FF 7E ??"}, {0x2164c8, kPolicyNativeReal, "D8 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F8 33 F6 85 FF 7E ??"}}, 2},
    // PlayOnline connection retries: no step call of its own, so the group is its one site and retryDue behind it.
    {"connection retry pacing", 50, 1, kLocJobWalk, 0x12, 0xf1116, {}, 0},
    // The mount load wait and the Trust emote weapon hide: two drawn-frame counters with no step call of their own.
    {"mount and emote counters", 51, 2, kLocMountWait, 0x1f, 0x8ba1d, {}, 0},
    // Preview camera 0x24DE20 turns 6 degrees per native frame; snap threshold = step * 6 degrees.
    {"creation preview camera", 53, 4, kLocPreviewCamera, 11, 0x24de9d,
     {{0x24de9d, kPolicyNativeReal, "D8 0D ?? ?? ?? ?? DC 5C 24 14 DF E0 F6 C4 05 7A 26"},
      {0x24df53, kPolicyNativeReal, "D8 0D ?? ?? ?? ?? DC 5C 24 14 DF E0 F6 C4 05 7A 26"}}, 2},
    // Wind gust timer +0x144 keeps whole ticks; only resampling/decay is paced.
    {"cloth wind gusts", 57, 1, kLocWindGust, 0, 0x188aa6, {}, 0},
    // Optional and separate from the cast bar for clients without holdtime (HorizonXI).
    // Missing code is allowed only when the "menu    holdtime" marker is also absent.
    // The hold bar (holdtime, +0x1C): a float countdown that fills the bar every frame with S.
    {"hold bar", 0, 0, "E8 ?? ?? ?? ?? D8 6E 1C D9 56 1C D8 1D ?? ?? ?? ?? DF E0 F6 C4", 5, 0x12cbd4, {{0x12cbd4, kPolicyS, "D8 6E ?? D9 56 ??"}}, 1, true,
     "6D 65 6E 75 20 20 20 20 68 6F 6C 64 74 69 6D 65", "hold-time window", "not in this client (normal on HorizonXI; retail clients have it)"},
};
// Every group's sites lie inside kSites: a row appended to one table and not the other fails here, not at resolve.
constexpr bool groupSitesInRange() {
    for (const auto& group : kGroups)
        if (size_t(group.firstSite) + size_t(group.siteCount) > kSiteCount) return false;
    return true;
}
static_assert(groupSitesInRange(), "a group in kGroups names sites past the end of kSites");
// Optional groups need a marker and diagnostics, and cannot own sites: the absence check skips site validation.
constexpr bool optionalGroupsHaveNoSites() {
    for (const auto& group : kGroups)
        if (group.optional && (group.siteCount != 0 || !group.marker || !group.markerName || !group.absentNote)) return false;
    return true;
}
static_assert(optionalGroupsHaveNoSites(), "an optional group in kGroups names sites, or lacks its marker, markerName or absentNote");
static_assert(kGroups[GroupHoldBar].optional && !kGroups[GroupCastBar].optional, "the hold bar is the optional routine; the cast bar is not");
// clang-format on
// The routine a kSites row belongs to (kGroupCount: none).
constexpr uint8_t siteGroup(size_t site) {
    for (uint8_t gi = 0; gi < kGroupCount; gi++)
        if (site >= size_t(kGroups[gi].firstSite) && site < size_t(kGroups[gi].firstSite) + size_t(kGroups[gi].siteCount)) return gi;
    return kGroupCount;
}
inline constexpr uint32_t kEffectsCacheRva = 0x6a005;  // its return address also fills g_truefpsWEB0
inline constexpr uint32_t kEntityPath1StepRva = 0x8d350;   // entity path 1's step call, between its divisor and fild sites

// The client image (or a copy of it): `data` holds the bytes of [base, base + size).
struct ImageView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uintptr_t base = 0;
    bool has(uintptr_t abs, size_t n) const { return abs >= base && n <= size && abs - base <= size - n; }
    uint32_t u32(uintptr_t abs) const { uint32_t v = 0; std::memcpy(&v, data + (abs - base), 4); return v; }
};

// Validate network icon 0x2012D0: phase +0x34, wrap at 4, period 11-quality/10. Wildcard the call counter.
inline constexpr const char* kNetIconBytes =
    "57 8B 7C 24 08 85 FF 75 06 33 C0 5F C2 08 00 8B 54 24 0C B8 67 66 66 66 F7 EA C1 FA 02 8B C2 53 56 "
    "8B 35 ?? ?? ?? ?? C1 E8 1F 03 D0 BB 0B 00 00 00 8B C6 2B DA 99 F7 FB 46 89 35 ?? ?? ?? ?? 5E 5B 85 D2 75 15 "
    "8B 51 34 42 8B C2 89 51 34 83 F8 04 7C 07 C7 41 34 00 00 00 00 8B 41 34 03 C7 5F C2 08 00";
// Job walker 0x0f1160, whole. Wildcarded: log calls, resource-bit calls, the switch table address. Pinned: state +0x24 and its four cases,
// start callback +0x10, result +0x20, retry count +0x28 (limit 0x32), pacing states (retryable error > -0x800 and != -0x14CC keeps state 2 and raises the count; result >= 0 clears it).
inline constexpr const char* kJobWalkerBytes =
    "53 56 8B 74 24 0C 57 33 DB 33 FF 8B 46 24 83 F8 03 0F 87 E4 00 00 00 FF 24 85 ?? ?? ?? ?? 8B 46 08 "
    "50 E8 ?? ?? ?? ?? 83 C4 04 84 C0 0F 85 C9 00 00 00 8B 4E 08 51 E8 ?? ?? ?? ?? 8B 46 24 83 C4 04 40 "
    "89 46 24 8B 46 0C 3B C3 75 1F FF 46 24 8B 56 18 52 FF 56 10 83 C4 04 3B C3 89 46 20 7D 49 3D 34 EB "
    "FF FF 75 1B 8B F8 EB 7B FF D0 85 C0 0F 84 86 00 00 00 8B 46 24 40 89 46 24 8B C7 5F 5E 5B C3 3D 01 "
    "F8 FF FF 7C 1B 8B 4E 28 83 F9 32 7D 13 41 50 89 4E 28 E8 ?? ?? ?? ?? 83 C4 04 8B C7 5F 5E 5B C3 8B "
    "F8 50 EB 31 8B 46 24 89 5E 28 40 89 46 24 8B C7 5F 5E 5B C3 8B 46 1C 8B 4E 20 50 51 FF 56 14 8B F8 "
    "83 C4 08 3B FB 74 2E 81 FF 34 EB FF FF 74 0D 3B FB 7D 0B 57 E8 ?? ?? ?? ?? 83 C4 04 3B FB 74 15 8B "
    "56 08 89 5E 28 52 89 5E 24 89 5E 20 E8 ?? ?? ?? ?? 83 C4 04 8B C7 5F 5E 5B C3";
// The world segment test thunk 0x1815b0: `add ecx,178h; jmp 0x169140`.
inline constexpr const char* kSegmentTestBytes = "81 C1 78 01 00 00 E9";
// Validate history writer 0x1E320: three shifts, oldest +0x24, count +0x34.
// Wildcard helpers and the sample-distance address.
inline constexpr const char* kHistoryWriterBytes =
    "55 8B 6C 24 08 57 8B F9 57 55 E8 ?? ?? ?? ?? D8 1D ?? ?? ?? ?? 83 C4 08 DF E0 F6 C4 05 7B 30 53 56 8D 47 24 "
    "BB 03 00 00 00 8D 70 F4 56 50 E8 ?? ?? ?? ?? 83 C4 08 4B 8B C6 75 EE 55 57 E8 ?? ?? ?? ?? 8A 47 34 83 C4 08 FE C0 5E 88 47 34 5B 5F 5D C2 08 00";
inline constexpr const char* kHelperScaleBytes = "8B 44 24 04 D9 44 24 08 D8 08 D9 18 D9 44 24 08 D8 48 04 D9 58 04 D9 44 24 08 D8 48 08 D9 58 08 C3";
inline constexpr const char* kHelperScaleOutBytes = "8B 44 24 08 8B 4C 24 04 D9 44 24 0C D8 08 D9 19 D9 44 24 0C D8 48 04 D9 59 04 D9 44 24 0C D8 48 08 D9 59 08 C3";

struct Site {
    const SiteSpec* spec = nullptr;
    uintptr_t at = 0;
    uintptr_t imageLo = 0, imageHi = 0;   // the client image it was located in: an operand outside it is another tool's
    uint8_t original[16] = {};
    uint8_t patch[16] = {};
    bool patched = false;   // truefps's bytes are (or may be) there
    bool kept = false;      // left in for the session (retireGroup)
    bool neutral = false;   // another tool's operand (SiteSpec::neutralBits): never written, never restored
    bool unsafeOriginal = false;   // saved operand is unreadable or no longer holds an allowed value
    bool adopted = false;   // another tool replaced our persistent cell operand; no module pin needed
    bool inImage(uintptr_t addr, size_t n) const {
        return imageHi > imageLo && n <= imageHi - imageLo && addr >= imageLo && addr - imageLo <= imageHi - imageLo - n;
    }
};
// Sites left to another tool, and sites taken back from one: a change is worth one log line each.
inline uint32_t g_truefpsNeutralChanges = 0;
inline uint32_t g_truefpsNeutralNow = 0;      // how many sites another tool owns right now
struct GroupRuntime {
    bool found = false;     // every site and call site located and verified
    bool on = false;        // patches in, policy on
    bool failed = false;    // a patch could not go in, or the routine ran off the game thread: whole ticks
    bool stuck = false;     // removal failed: whole ticks until every site is restored
    bool retired = false;   // its own step call ran on another thread (retireGroup)
    bool absent = false;    // optional anchor and marker both missing; found stays false
    std::string why;
    uint32_t quietRetryAt = 0;   // SmoothSites::frame to wait for before trying a multi-byte write again
    uint32_t quietMisses = 0;    // tries in a row that found a thread in the way; a page that could not be opened is not one of them
    bool quietLogged[2] = {};        // one latch per direction: [0] putting the patches in, [1] taking them out
    bool quietPagesFailed = false;   // the last miss was a page that could not be made writable, not a thread in the way
    bool quietPagesLogged[2] = {};   // its own latch per direction: the two causes are two statements, and either can still be written
    char quietWhy[400] = "";     // the last miss's reason, filled with no allocation
};
struct SmoothSites {
    Site sites[kSiteCount];
    GroupRuntime groups[kGroupCount];
    uint32_t frame = 0;          // runSmoothPatches / stopSmoothPatches calls, for the back-off
    PolicyEntry entries[kMaxPolicy];
    size_t entryCount = 0;
    uintptr_t helperScale = 0, helperScaleOut = 0, stepAccessor = 0, d8c = 0, d88 = 0, effects = 0, frameCounter = 0, netIcon = 0, historyWriter = 0, segmentTest = 0, recoveryHold = 0, trailTail = 0, jobWalker = 0;
    uintptr_t eb0Ret = 0;
    CodeRange path1Calls[2] = {};         // what path 1 calls between its sites: the step accessor, __ftol
    uint32_t floatBits[kSiteCount] = {};  // the verified constant per site (fast-path copies)
    float springFar = 0.0f;               // the eye loop's own far-distance threshold, read from the client
    bool live = false;                    // smooth mode started and has not ended since
    bool sessionOver = false;             // a retired group's code stays in: smooth mode off for the session
};

// Read the float named by imm32 at `immAddr`; both must lie in the image.
inline bool floatAtRead(const ImageView& img, uintptr_t immAddr, uint32_t& bits) {
    if (!img.has(immAddr, 4)) return false;
    const uint32_t target = img.u32(immAddr);
    if (!img.has(target, 4)) return false;
    bits = img.u32(target);
    return true;
}
inline bool floatAtIs(const ImageView& img, uintptr_t immAddr, uint32_t bits) {
    uint32_t held = 0;
    return floatAtRead(img, immAddr, held) && held == bits;
}
// Accept the exact constant or a factor in the site's band. Banded comparisons reject NaN.
inline bool constantInBand(const SiteSpec& spec, uint32_t bits) {
    const float k = floatFromBits(bits);
    switch (spec.band) {
    case FloatBand::UnitPositive: return k > 0.0f && k <= 1.0f;
    case FloatBand::UnitNegative: return k >= -1.0f && k < 0.0f;
    case FloatBand::Exact: break;
    }
    return bits == spec.floatBits;
}
// True when a float operand has been pointed outside the client image at the value the spec allows there
// (SiteSpec::neutralBits): another tool owns it, and its value needs no pacing. The read is guarded.
inline bool neutralOperand(const SiteSpec& spec, uint32_t operand, bool insideImage) {
    if (!spec.neutralBits || spec.kind != SiteKind::SwapImm || !operand || insideImage) return false;
    uint32_t held = 0;
    return readValue(uintptr_t(operand), held) && held == spec.neutralBits;
}

// Require one locator match and verified context, constants and call targets.
inline bool locateSiteFrom(const std::vector<size_t>& hits, const SiteSpec& spec, const ImageView& img, const uintptr_t* accessors, size_t accessorCount,
                           Site& out, SmoothSites& sm, size_t index, std::string& why) {
    if (hits.size() != 1) { why = std::string(spec.name) + (hits.empty() ? " not found" : " matched more than once"); return false; }
    const uintptr_t at = img.base + hits[0] + uintptr_t(intptr_t(spec.locatorOffset));
    if (!img.has(at, spec.length) || spec.length > sizeof out.original) { why = std::string(spec.name) + " is outside the image"; return false; }
    if (spec.context) {
        const auto ctx = parsePattern(spec.context);
        const uintptr_t c = at + uintptr_t(intptr_t(spec.contextOffset));
        if (!img.has(c, ctx.size()) || !matchesAt(img.data, img.size, c - img.base, ctx)) { why = std::string(spec.name) + ": the code around it is not the expected code"; return false; }
    }
    // A float operand pointed away from the client's own data. Holding the site's own value (an earlier load's cell,
    // or another tool's copy of it) is still "original": it is verified as it is, and it is what removal writes back.
    // Holding the value the spec allows another tool to leave there makes the site that tool's.
    bool neutral = false, repointed = false;
    if (spec.kind == SiteKind::SwapImm && spec.floatAt == 2 && img.has(at + 2, 4)) {
        const uint32_t operand = img.u32(at + 2);
        const bool inside = img.has(operand, 4);
        uint32_t held = 0;
        if (operand && !inside && readValue(uintptr_t(operand), held) && held == spec.floatBits) repointed = true;
        else neutral = neutralOperand(spec, operand, inside);
    }
    uint32_t foundBits = 0;
    const bool foundOk = spec.floatAt != 0 && floatAtRead(img, at + uintptr_t(intptr_t(spec.floatAt)), foundBits);
    if (!neutral && !repointed && spec.floatAt && !(foundOk && constantInBand(spec, foundBits))) { why = std::string(spec.name) + ": its constant is not the expected value"; return false; }
    // Read the wildcarded push immediate. The table requires it to lie within the verified context.
    uint32_t passedBits = 0;
    if (spec.contextFloatAt) {
        const uintptr_t imm = at + uintptr_t(intptr_t(spec.contextFloatAt));
        if (!img.has(imm, 4)) { why = std::string(spec.name) + ": the factor it passes is outside the image"; return false; }
        passedBits = img.u32(imm);
        const float k = floatFromBits(passedBits);
        if (!(k > 0.0f && k <= 1.0f)) { why = std::string(spec.name) + ": the factor it passes is not a fraction"; return false; }
    }
    // A Disp8 site keeps its original displacement byte in float2Bits, so the float check does not apply to it.
    if (spec.float2At && spec.kind != SiteKind::Disp8 && !floatAtIs(img, at + uintptr_t(intptr_t(spec.float2At)), spec.float2Bits)) { why = std::string(spec.name) + ": a nearby constant is not the expected value"; return false; }
    const uint8_t* p = img.data + (at - img.base);
    switch (spec.kind) {
    // D8 0D fmul, D8 25 fsub, D8 1D fcomp, D8 15 fcom, D9 05 fld use a four-byte address at +2.
    // Some operands are unaligned; x86 locked compare-exchange still swaps them atomically.
    case SiteKind::SwapImm: if (!(p[0] == 0xD8 || p[0] == 0xD9) || !(p[1] == 0x0D || p[1] == 0x25 || p[1] == 0x05 || p[1] == 0x1D || p[1] == 0x15)) { why = std::string(spec.name) + " is not a float operand"; return false; } break;
    case SiteKind::CallToStub: if (p[0] != 0xE8) { why = std::string(spec.name) + " is not a call"; return false; } break;
    case SiteKind::ReplaceFld: if (p[0] != 0xD9 || !(p[1] == 0x80 || p[1] == 0x81)) { why = std::string(spec.name) + " is not the cached-step read"; return false; } break;
    case SiteKind::Byte: if (p[0] != 0xDB) { why = std::string(spec.name) + " is not an fild"; return false; } break;
    case SiteKind::Disp8: if (p[0] != 0x7E || p[1] != uint8_t(spec.float2Bits)) { why = std::string(spec.name) + " is not the expected jle"; return false; } break;
    case SiteKind::ReplaceCall: break;
    }
    if (spec.kind == SiteKind::CallToStub) {
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, 4);
        const uintptr_t target = at + 5 + uintptr_t(intptr_t(rel));
        if (spec.target == CallTarget::FrameCounter) {
            // `mov eax,[app slot]; mov eax,[eax+34h]; ret`, on the same app object pointer as the step accessors.
            uint32_t appSlot = 0;
            const bool shape = img.has(target, 9) && img.data[target - img.base] == 0xA1 && img.data[target - img.base + 5] == 0x8B &&
                               img.data[target - img.base + 6] == 0x40 && img.data[target - img.base + 7] == 0x34 && img.data[target - img.base + 8] == 0xC3;
            if (shape) appSlot = img.u32(target + 1);
            bool sameApp = false;
            for (size_t i = 0; i < accessorCount; i++) sameApp = sameApp || (img.has(accessors[i] + 2, 4) && img.u32(accessors[i] + 2) == appSlot);
            if (!shape || !sameApp || (sm.frameCounter && sm.frameCounter != target)) { why = std::string(spec.name) + " does not call the frame counter"; return false; }
            sm.frameCounter = target;
        } else if (spec.target == CallTarget::TrailStep) {
            bool known = false;
            for (size_t i = 0; i < accessorCount; i++) known = known || accessors[i] == target;
            const uintptr_t prologue = uintptr_t(intptr_t(at) + kTrailPrologueFromCall), tail = uintptr_t(intptr_t(at) + kTrailTailFromCall);
            const uintptr_t jump = uintptr_t(intptr_t(at) + kTrailNoSampleJumpFromCall);
            const auto pro = parsePattern(kTrailPrologue), tl = parsePattern(kTrailTailBytes);
            const bool frameOk = img.has(prologue, pro.size()) && matchesAt(img.data, img.size, prologue - img.base, pro);
            const bool tailOk = img.has(tail, tl.size()) && matchesAt(img.data, img.size, tail - img.base, tl);
            const bool jumpOk = img.has(jump, 6) && img.data[jump - img.base] == 0x0F && img.data[jump - img.base + 1] == 0x84 &&
                                uintptr_t(jump + 6 + int32_t(img.u32(jump + 2))) == tail;
            if (!known || !frameOk || !tailOk || !jumpOk || (sm.stepAccessor && sm.stepAccessor != target)) {
                why = std::string(spec.name) + " is not the expected trail update (step call, frame, draw tail)"; return false;
            }
            sm.stepAccessor = target;
            sm.trailTail = tail;
        } else if (spec.target == CallTarget::StepAccessor) {
            bool known = false;
            for (size_t i = 0; i < accessorCount; i++) known = known || accessors[i] == target;
            if (!known || (sm.stepAccessor && sm.stepAccessor != target)) { why = std::string(spec.name) + " does not call the game step"; return false; }
            sm.stepAccessor = target;
        } else if (spec.target == CallTarget::SegmentTest) {
            const auto pat = parsePattern(kSegmentTestBytes);
            if (!img.has(target, pat.size()) || !matchesAt(img.data, img.size, target - img.base, pat) || (sm.segmentTest && sm.segmentTest != target)) {
                why = std::string(spec.name) + " does not call the world segment test"; return false;
            }
            // The hit continuation: the recovery hold it reads, and camera+F0 compared with 4.
            const auto hitPat = parsePattern(kObstructionHitPattern);
            const uintptr_t hitAt = at + kObstructionHitOffset;
            const uintptr_t hold = img.has(hitAt, hitPat.size()) && matchesAt(img.data, img.size, hitAt - img.base, hitPat) ? img.u32(hitAt + 1) : 0;
            if (!hold || !img.has(hold, 4)) { why = std::string(spec.name) + ": the hit branch is not the expected recovery test"; return false; }
            sm.segmentTest = target;
            sm.recoveryHold = hold;
        } else if (spec.target == CallTarget::HistoryWriter) {
            const auto pat = parsePattern(kHistoryWriterBytes);
            if (!img.has(target, pat.size()) || !matchesAt(img.data, img.size, target - img.base, pat) || (sm.historyWriter && sm.historyWriter != target)) {
                why = std::string(spec.name) + " does not call the camera history writer"; return false;
            }
            sm.historyWriter = target;
        } else if (spec.target == CallTarget::JobWalker) {
            const auto pat = parsePattern(kJobWalkerBytes);
            if (!img.has(target, pat.size()) || !matchesAt(img.data, img.size, target - img.base, pat) || (sm.jobWalker && sm.jobWalker != target)) {
                why = std::string(spec.name) + " does not call the job walker"; return false;
            }
            sm.jobWalker = target;
        } else if (spec.target == CallTarget::NetIcon) {
            const auto pat = parsePattern(kNetIconBytes);
            if (!img.has(target, pat.size()) || !matchesAt(img.data, img.size, target - img.base, pat) || (sm.netIcon && sm.netIcon != target)) {
                why = std::string(spec.name) + " does not call the network icon function"; return false;
            }
            sm.netIcon = target;
        } else {
            const auto pat = parsePattern(spec.target == CallTarget::Scale ? kHelperScaleBytes : kHelperScaleOutBytes);
            uintptr_t& known = spec.target == CallTarget::Scale ? sm.helperScale : sm.helperScaleOut;
            if (!img.has(target, pat.size()) || !matchesAt(img.data, img.size, target - img.base, pat) || (known && known != target)) {
                why = std::string(spec.name) + " does not call the expected vector helper"; return false;
            }
            known = target;
        }
    }
    if (spec.global != Global::None) {
        const uintptr_t imm = at + uintptr_t(intptr_t(spec.globalAt));
        const uintptr_t addr = img.has(imm, 4) ? img.u32(imm) : 0;
        uintptr_t& known = spec.global == Global::D8C ? sm.d8c : spec.global == Global::D88 ? sm.d88 : sm.effects;
        bool ok = img.has(addr, 4) && (!known || known == addr);
        if (spec.global == Global::D8C) ok = ok && img.u32(at + 7) == addr && img.u32(at + 15) == addr;  // dec and write-back
        if (!ok) { why = std::string(spec.name) + ": its global is not consistent"; return false; }
        known = addr;
    }
    out = Site{};
    out.spec = &spec;
    out.at = at;
    out.imageLo = img.base;
    out.imageHi = img.base + img.size;
    out.neutral = neutral;
    std::memcpy(out.original, p, spec.length);
    if (neutral) { ++g_truefpsNeutralChanges; ++g_truefpsNeutralNow; }   // another tool's constant: none of the client's to copy
    else if (repointed) sm.floatBits[index] = spec.floatBits;   // verified live: it is not in the image to read
    else if (spec.floatAt) sm.floatBits[index] = foundBits;
    else if (spec.contextFloatAt) sm.floatBits[index] = passedBits;   // context factors have no floatAt
    return true;
}
inline bool locateSite(const SiteSpec& spec, const ImageView& img, const uintptr_t* accessors, size_t accessorCount, Site& out,
                       SmoothSites& sm, size_t index, std::string& why) {
    return locateSiteFrom(findAll(img.data, img.size, parsePattern(spec.locator), 2), spec, img, accessors, accessorCount, out, sm, index, why);
}

// Far-distance test, 0x53 before the spring site: `fld [esp+10h]; fcomp [threshold]; fnstsw ax;
// and eax, 4100h; jnz`. Require this test and copy its threshold for springStub; refreshConstantCopies updates it.
inline constexpr size_t kSpringSite = 6;   // kSites index ("eye spring 6..100")
inline constexpr int32_t kSpringFarTestFromSite = -0x53, kSpringFarImmFromSite = -0x4D;
// The site locator fixes the position; the jnz displacement is not needed for identification.
inline constexpr const char* kSpringFarTest = "D9 44 24 10 D8 1D ?? ?? ?? ?? DF E0 25 00 41 00 00 75 ??";
inline bool resolveSpringFar(const ImageView& img, const Site& spring, float& out) {
    if (!spring.spec) return false;
    const uintptr_t test = spring.at + uintptr_t(intptr_t(kSpringFarTestFromSite));
    const auto pat = parsePattern(kSpringFarTest);
    if (!img.has(test, pat.size()) || !matchesAt(img.data, img.size, test - img.base, pat)) return false;
    uint32_t bits = 0;
    if (!floatAtRead(img, spring.at + uintptr_t(intptr_t(kSpringFarImmFromSite)), bits)) return false;
    const float threshold = floatFromBits(bits);
    if (!(threshold > 0.0f)) return false;
    out = threshold;
    return true;
}

// The instructions after a step call are the expected consumer; a call among them must be the client's __ftol.
inline bool entryUseMatches(const ImageView& img, uintptr_t ret, const char* use) {
    if (!use || !*use) return false;
    const auto pat = parsePattern(use);
    if (!img.has(ret, pat.size()) || !matchesAt(img.data, img.size, ret - img.base, pat)) return false;
    if (img.data[ret - img.base] != 0xE8) return true;
    const uintptr_t target = ret + 5 + uintptr_t(intptr_t(int32_t(img.u32(ret + 1))));
    const auto ftol = parsePattern(kFtolBytes);
    return img.has(target, ftol.size()) && matchesAt(img.data, img.size, target - img.base, ftol);
}

// Locates every group: found when all its sites and call sites are; the policy table gets their call sites.
inline size_t resolveSmooth(const ImageView& img, const uintptr_t* accessors, size_t accessorCount, SmoothSites& sm) {
    sm = SmoothSites{};
    std::vector<std::vector<int>> patterns;   // every site locator, then every group anchor: one pass over the image
    for (size_t i = 0; i < kSiteCount; i++) patterns.push_back(parsePattern(kSites[i].locator));
    for (size_t i = 0; i < kGroupCount; i++) patterns.push_back(parsePattern(kGroups[i].anchor));
    size_t markerAt[kGroupCount] = {};   // the pattern index of each optional group's marker (0: none)
    for (size_t i = 0; i < kGroupCount; i++)
        if (kGroups[i].marker) { markerAt[i] = patterns.size(); patterns.push_back(parsePattern(kGroups[i].marker)); }
    const auto hits = findAllMany(img.data, img.size, patterns, 2);
    std::vector<bool> siteOk(kSiteCount, false);
    std::vector<std::string> siteWhy(kSiteCount);
    for (size_t i = 0; i < kSiteCount; i++) siteOk[i] = locateSiteFrom(hits[i], kSites[i], img, accessors, accessorCount, sm.sites[i], sm, i, siteWhy[i]);
    // Calls sharing a stub must pass the same factor: the stub has only one comparison value.
    // Reject mismatches; the table requires each stub to use one contextBits destination.
    for (size_t i = 0; i < kSiteCount; i++) {
        if (!siteOk[i] || !kSites[i].contextBits || !kSites[i].stub) continue;
        for (size_t j = i + 1; j < kSiteCount; j++) {
            if (!siteOk[j] || !kSites[j].contextBits || kSites[j].stub != kSites[i].stub || sm.floatBits[j] == sm.floatBits[i]) continue;
            siteOk[i] = siteOk[j] = false;
            siteWhy[i] = std::string(kSites[i].name) + ": it and another call on the same stub no longer pass the same factor";
            siteWhy[j] = std::string(kSites[j].name) + ": it and another call on the same stub no longer pass the same factor";
        }
    }
    if (siteOk[kSpringSite] && !resolveSpringFar(img, sm.sites[kSpringSite], sm.springFar)) {
        siteOk[kSpringSite] = false;
        siteWhy[kSpringSite] = std::string(kSites[kSpringSite].name) + ": the loop's far-distance test is not the expected code";
    }
    size_t found = 0;
    for (uint8_t gi = 0; gi < kGroupCount; gi++) {
        const GroupSpec& gs = kGroups[gi];
        GroupRuntime& gr = sm.groups[gi];
        // A missing optional routine is absent only if its feature marker is also missing.
        if (gs.optional && hits[kSiteCount + gi].empty()) {
            // Zero is the missing-marker sentinel, not a marker index into hits.
            if (markerAt[gi] && hits[markerAt[gi]].empty()) { gr.absent = true; gr.why = gs.absentNote; continue; }
            gr.why = std::string("its code was not found, although this client has the ") + gs.markerName;
            continue;
        }
        gr.found = true;
        for (uint8_t k = 0; k < gs.siteCount && gr.found; k++)
            if (!siteOk[gs.firstSite + k]) { gr.found = false; gr.why = siteWhy[gs.firstSite + k]; }
        uintptr_t anchor = 0;
        if (gr.found) {
            const auto& anchorHits = hits[kSiteCount + gi];
            if (anchorHits.size() != 1) { gr.found = false; gr.why = "its call sites were not found"; }
            else anchor = img.base + anchorHits[0] + uintptr_t(intptr_t(gs.anchorOffset));
        }
        PolicyEntry local[sizeof(GroupSpec::entries) / sizeof(GroupSpec::entries[0])];
        for (uint8_t e = 0; e < gs.entryCount && gr.found; e++) {
            const uintptr_t ret = anchor + uintptr_t(intptr_t(int64_t(gs.entries[e].rva) - int64_t(gs.anchorRva)));
            bool ok = img.has(ret - 5, 5) && img.data[ret - 5 - img.base] == 0xE8;
            if (ok) {
                const uintptr_t target = ret + uintptr_t(intptr_t(int32_t(img.u32(ret - 4))));
                ok = false;
                for (size_t a = 0; a < accessorCount; a++) ok = ok || accessors[a] == target;
            }
            if (!ok) { gr.found = false; char b[96]; _snprintf_s(b, sizeof b, _TRUNCATE, "call site RVA 0x%05X does not call the game step", gs.entries[e].rva); gr.why = b; break; }
            if (!entryUseMatches(img, ret, gs.entries[e].use)) {
                gr.found = false;
                char b[120];
                _snprintf_s(b, sizeof b, _TRUNCATE, "call site RVA 0x%05X: the code that uses the step is not the expected code (client update?)", gs.entries[e].rva);
                gr.why = b;
                break;
            }
            local[e] = PolicyEntry{ret, gs.entries[e].kind, gi};
            if (gs.entries[e].rva == kEffectsCacheRva) sm.eb0Ret = ret;
            if (gi == GroupEntities && gs.entries[e].rva == kEntityPath1StepRva) {   // its use starts with __ftol
                const uintptr_t accessor = ret + uintptr_t(intptr_t(int32_t(img.u32(ret - 4))));
                const uintptr_t ftol = ret + 5 + uintptr_t(intptr_t(int32_t(img.u32(ret + 1))));
                sm.path1Calls[0] = CodeRange{accessor, stepPadAt(accessor) + 5};
                sm.path1Calls[1] = CodeRange{ftol, ftol + parsePattern(kFtolBytes).size()};
            }
        }
        if (!gr.found) {
            if (gi == GroupEffects) sm.eb0Ret = 0;
            if (gi == GroupEntities) sm.path1Calls[0] = sm.path1Calls[1] = CodeRange{};
            continue;
        }
        if (sm.entryCount + gs.entryCount > kMaxPolicy) { gr.found = false; gr.why = "policy table full"; continue; }
        for (uint8_t e = 0; e < gs.entryCount; e++) sm.entries[sm.entryCount++] = local[e];
        ++found;
    }
    return found;
}
// Count all groups except those absent from this client build.
inline size_t groupsPresent(const SmoothSites& sm) {
    size_t n = 0;
    for (const auto& gr : sm.groups) n += gr.absent ? 0 : 1;
    return n;
}

// Makes the resolved addresses live for the stubs. Call before any patch goes in.
inline bool allocatePersistentCells();
inline void publishSmoothAddresses(const SmoothSites& sm) {
    allocatePersistentCells();   // before patching, outside a freeze; the caller reports failure
    g_truefpsHelperScale = sm.helperScale;
    g_truefpsHelperScaleOut = sm.helperScaleOut;
    g_truefpsStepAccessor = sm.stepAccessor;
    g_truefpsD8CAddr = sm.d8c;
    g_truefpsD88Addr = sm.d88;
    g_truefpsEffectsGlobal = sm.effects;
    g_truefpsFrameCounter = sm.frameCounter;
    g_truefpsNetIcon = sm.netIcon;
    g_truefpsHistoryWriter = sm.historyWriter;
    g_truefpsSegmentTest = sm.segmentTest;
    g_truefpsJobWalker = sm.jobWalker;
    g_truefpsRecoveryHold = sm.recoveryHold;
    g_truefpsTrailTail = sm.trailTail;
    g_truefpsEb0Ret = sm.eb0Ret;
    if (sm.springFar > 0.0f) g_truefpsSpringFar = sm.springFar;
    for (size_t i = 0; i < kSiteCount; i++) {
        if (kSites[i].constantCopy && sm.floatBits[i]) *kSites[i].constantCopy = floatFromBits(sm.floatBits[i]);
        if (kSites[i].contextBits && sm.floatBits[i]) *kSites[i].contextBits = sm.floatBits[i];
    }
}

// Other tools may restore saved cell pointers after this module unloads. Allocate a separate page and never free it.
// Cells hold client constants while their groups are off; syncSiteCells copies active slots each frame.
inline constexpr size_t kPersistentCells = kSiteCellCount;
inline float* g_truefpsCellPage = nullptr;
inline bool g_truefpsCellPageFailed = false;   // allocation failure reported at resolve and group installation
// The first row naming a cell sets its source slot, rest constant and group; all rows sharing it must agree.
// Never allocate under a freeze: VirtualAlloc takes the address-space lock.
inline bool allocatePersistentCells() {
    if (!g_truefpsCellPage) g_truefpsCellPage = static_cast<float*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_truefpsCellPageFailed = g_truefpsCellPage == nullptr;
    if (!g_truefpsCellPage) return false;
    for (size_t row = 0; row < kSiteCount; row++) {
        const SiteSpec& spec = kSites[row];
        if (!spec.cell || spec.cell > kPersistentCells || !spec.slot) continue;
        const size_t i = size_t(spec.cell) - 1u;
        if (g_truefpsSiteCells[i]) continue;             // shared cell already initialized
        g_truefpsSiteCellRest[i] = floatFromBits(spec.floatBits);
        g_truefpsSiteCellSource[i] = spec.slot;
        g_truefpsSiteCellGroup[i] = siteGroup(row);
        g_truefpsCellPage[i] = g_truefpsSiteCellRest[i];   // its routine is not on yet
        g_truefpsSiteCells[i] = g_truefpsCellPage + i;
    }
    return true;
}
// The cell a site's operand points at, or nullptr when the page could not be allocated. Never allocates.
inline float* persistentCell(uint8_t index) {
    if (!index || index > kPersistentCells) return nullptr;
    return g_truefpsSiteCells[index - 1];
}
// Leave the client's own constant in every cell. Called first at unload, adopted site or not, and from the failure
// path: no allocation, no logging.
inline void restorePersistentCells() {
    for (const auto& spec : kSites)
        if (spec.cell && spec.cell <= kPersistentCells && g_truefpsSiteCells[spec.cell - 1])
            *g_truefpsSiteCells[spec.cell - 1] = floatFromBits(spec.floatBits);
}
// The address truefps swaps into this site's operand.
inline const float* siteSlot(const SiteSpec& spec) {
    if (spec.cell && spec.cell <= kPersistentCells && g_truefpsSiteCells[spec.cell - 1]) return g_truefpsSiteCells[spec.cell - 1];
    return spec.slot;
}

// Verify before writing and read back afterward. Restore only bytes we still own.
inline void buildPatch(Site& s) {
    const SiteSpec& spec = *s.spec;
    std::memcpy(s.patch, s.original, spec.length);
    switch (spec.kind) {
    case SiteKind::SwapImm: { const uint32_t slot = uint32_t(reinterpret_cast<uintptr_t>(siteSlot(spec))); std::memcpy(s.patch + 2, &slot, 4); break; }
    case SiteKind::CallToStub: { const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(spec.stub) - (s.at + 5)); std::memcpy(s.patch + 1, &rel, 4); break; }
    case SiteKind::ReplaceCall: {
        s.patch[0] = 0xE8;
        const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(spec.stub) - (s.at + 5));
        std::memcpy(s.patch + 1, &rel, 4);
        if (spec.length > 5) std::memset(s.patch + 5, 0x90, size_t(spec.length) - 5u);   // pad after the five-byte call
        break;
    }
    case SiteKind::ReplaceFld: { s.patch[0] = 0xD9; s.patch[1] = 0x05; const uint32_t slot = uint32_t(reinterpret_cast<uintptr_t>(spec.slot)); std::memcpy(s.patch + 2, &slot, 4); break; }
    case SiteKind::Byte: s.patch[0] = spec.newByte; break;
    case SiteKind::Disp8: s.patch[1] = spec.newByte; break;
    }
}

// A SwapImm operand another tool has taken over: the instruction itself is unchanged (`head` holds the two opcode
// bytes truefps expects there), the operand points outside the client image, and it holds the value the spec allows
// (neutralBits). The site is then left exactly as that tool set it: never written, never restored, never stuck.
inline bool takeNeutralOperand(Site& s, const uint8_t* head) {
    const SiteSpec& spec = *s.spec;
    if (spec.kind != SiteKind::SwapImm || !spec.neutralBits || spec.length != 6) return false;
    if (s.imageHi <= s.imageLo) return false;   // without the image's bounds there is no "outside it" to judge
    uint8_t now[6] = {};
    if (!readRaw(s.at, now, sizeof now) || std::memcmp(now, head, 2) != 0) return false;   // only the operand may differ
    uint32_t operand = 0;
    std::memcpy(&operand, now + 2, 4);
    const float* ours = siteSlot(spec);
    if (ours && operand == uint32_t(reinterpret_cast<uintptr_t>(ours))) return false;   // truefps's own slot, not another tool's
    if (!neutralOperand(spec, operand, s.inImage(operand, 4))) return false;
    s.neutral = true;
    s.patched = false;
    ++g_truefpsNeutralChanges;
    ++g_truefpsNeutralNow;
    return true;
}
// Accept takeover after our patch only for persistent cells: the tool may restore our pointer after unload.
// restorePersistentCells leaves the client constant there. Without a cell, the saved pointer names this module,
// so removal must fail and keep the module mapped.
inline bool adoptForeignOperand(Site& s) {
    if (!s.spec || !s.spec->cell) return false;
    if (!takeNeutralOperand(s, s.patch)) return false;
    s.adopted = true;
    return true;
}

inline bool installSite(Site& s, CodeWriter write = writeCode) {
    if (s.neutral) return true;   // another tool's operand: nothing to write, and the group installs around it
    if (s.patched) return true;
    if (!s.spec) return false;
    // Another tool may have taken the operand over since it was located; accept that as at resolve.
    if (!bytesAre(s.at, s.original, s.spec->length)) return takeNeutralOperand(s, s.original);
    const SiteSpec& spec = *s.spec;
    if (spec.cell && !persistentCell(spec.cell)) return false;   // no slot that outlives the module, no swap
    if (spec.floatAt) {
        uint32_t addr = 0, bits = 0;
        // Read the operand from the code: it can sit before the site, where the client's own instruction stays in.
        if (!readValue(s.at + uintptr_t(intptr_t(spec.floatAt)), addr) || !readValue(uintptr_t(addr), bits) || !constantInBand(spec, bits)) return false;
    }
    buildPatch(s);
    bool ok = false;
    switch (spec.kind) {
    case SiteKind::SwapImm: { uint32_t a = 0, b = 0; std::memcpy(&a, s.original + 2, 4); std::memcpy(&b, s.patch + 2, 4); ok = swapCode32(s.at + 2, a, b); break; }
    case SiteKind::CallToStub: { uint32_t a = 0, b = 0; std::memcpy(&a, s.original + 1, 4); std::memcpy(&b, s.patch + 1, 4); ok = swapCode32(s.at + 1, a, b); break; }
    case SiteKind::Byte: ok = swapCode8(s.at, s.original[0], s.patch[0]); break;
    case SiteKind::Disp8: ok = swapCode8(s.at + 1, s.original[1], s.patch[1]); break;
    case SiteKind::ReplaceCall:
    case SiteKind::ReplaceFld: ok = write(s.at, s.patch, spec.length); break;
    }
    if (ok && bytesAre(s.at, s.patch, spec.length)) { s.patched = true; return true; }
    // Not verified: the site stays tracked, and removal restores it only from an exact patch.
    if (!bytesAre(s.at, s.original, spec.length)) s.patched = true;
    return false;
}

// Transitions to unsafe saved operands, for logging.
inline uint32_t g_truefpsUnsafeOriginals = 0;
// Client-image operands remain valid for the image's lifetime. Saved external operands may have been unmapped
// or changed; re-read them and require an allowed value before restoration.
inline bool originalOperandSafe(Site& s) {
    const SiteSpec& spec = *s.spec;
    if (spec.kind != SiteKind::SwapImm || spec.floatAt != 2) return true;
    uint32_t addr = 0;
    std::memcpy(&addr, s.original + 2, 4);
    if (s.inImage(uintptr_t(addr), 4)) { s.unsafeOriginal = false; return true; }
    uint32_t bits = 0;
    if (readValue(uintptr_t(addr), bits) && constantInBand(spec, bits)) { s.unsafeOriginal = false; return true; }
    // Count each transition once across repeated removal attempts.
    if (!s.unsafeOriginal) {
        s.unsafeOriginal = true;
        ++g_truefpsUnsafeOriginals;
    }
    return false;
}

// True once the site holds its original bytes. A site holding neither those nor the patch is left alone, as is a kept one.
inline bool removeSite(Site& s, CodeWriter write = writeCode) {
    if (s.neutral) return true;   // another tool's operand: never written, so never restored
    if (!s.patched) return true;
    if (s.kept) return false;
    const uint8_t n = s.spec->length;
    if (bytesAre(s.at, s.original, n)) { s.patched = false; return true; }
    if (!bytesAre(s.at, s.patch, n)) return adoptForeignOperand(s);
    // Keep our valid slot if the saved operand is unsafe; failed removal keeps this module mapped.
    if (!originalOperandSafe(s)) return false;
    bool ok = false;
    switch (s.spec->kind) {
    case SiteKind::SwapImm: { uint32_t a = 0, b = 0; std::memcpy(&a, s.patch + 2, 4); std::memcpy(&b, s.original + 2, 4); ok = swapCode32(s.at + 2, a, b); break; }
    case SiteKind::CallToStub: { uint32_t a = 0, b = 0; std::memcpy(&a, s.patch + 1, 4); std::memcpy(&b, s.original + 1, 4); ok = swapCode32(s.at + 1, a, b); break; }
    case SiteKind::Byte: ok = swapCode8(s.at, s.patch[0], s.original[0]); break;
    case SiteKind::Disp8: ok = swapCode8(s.at + 1, s.patch[1], s.original[1]); break;
    case SiteKind::ReplaceCall:
    case SiteKind::ReplaceFld: ok = write(s.at, s.original, n); break;
    }
    // A swap that lost its race: another tool wrote the operand between the check above and the exchange.
    if (ok && bytesAre(s.at, s.original, n)) { s.patched = false; return true; }
    return adoptForeignOperand(s);
}

// Report takeovers of our persistent cell operands. Their lifetime does not require a module pin.
inline bool anyAdoptedSite(const SmoothSites& sm) {
    for (const auto& s : sm.sites) if (s.adopted) return true;
    return false;
}
// Reclaim an adopted site once it holds the original or our patch again; only the latter remains patched.
// Returns whether state changed. Safe under the unload freeze: no writes to code, allocation or logging.
inline bool reclaimAdoptedSite(Site& s) {
    if (!s.adopted || !s.neutral || !s.spec || s.spec->length != 6) return false;
    uint8_t now[6] = {};
    if (!readRaw(s.at, now, sizeof now)) return false;
    const bool original = std::memcmp(now, s.original, sizeof now) == 0;
    if (!original && std::memcmp(now, s.patch, sizeof now) != 0) return false;   // still the other tool's
    s.neutral = false;
    s.adopted = false;
    s.patched = !original;
    ++g_truefpsNeutralChanges;
    if (g_truefpsNeutralNow) --g_truefpsNeutralNow;
    return true;
}

// Describe ownership or a restoration failure from the current bytes in `now`; nullptr for normal state.
inline const char* siteStateNote(const Site& s, const uint8_t* now) {
    if (!s.spec) return nullptr;
    const size_t n = s.spec->length;
    if (s.neutral) {
        // Detect returned operands before the next reclaim pass, including while smooth mode is off.
        uint32_t operand = 0;
        if (n >= 6) std::memcpy(&operand, now + 2, 4);
        const bool handedBack = n >= 6 && ((s.adopted && (std::memcmp(now, s.patch, n) == 0 || std::memcmp(now, s.original, n) == 0)) || s.inImage(uintptr_t(operand), 4));
        if (handedBack) return "handed back by the other tool; TrueFPS takes it back on its next pass or at unload";
        return s.adopted ? "another tool's; it took the operand over after TrueFPS's patch was in" : "another tool's; it set the operand before TrueFPS's patch went in";
    }
    if (s.kept) return "kept in; its routine ran on another thread";
    if (s.unsafeOriginal) return "TrueFPS's patch kept in; the address it found no longer holds the game's value";
    if (s.patched && std::memcmp(now, s.patch, n) != 0) return "holds neither TrueFPS's patch nor the bytes it found";
    if (!s.patched && std::memcmp(now, s.original, n) != 0) return "no longer holds the bytes TrueFPS found";
    return nullptr;
}
// Classify a SwapImm operand by address; nullptr for other sites.
// External addresses do not distinguish older cell pages from another tool's slots.
inline const char* operandWhere(const Site& s, const uint8_t* now) {
    if (!s.spec || s.spec->kind != SiteKind::SwapImm || s.spec->length < 6) return nullptr;
    uint32_t operand = 0;
    std::memcpy(&operand, now + 2, 4);
    for (const float* cell : g_truefpsSiteCells)
        if (cell && operand == uint32_t(reinterpret_cast<uintptr_t>(cell))) return "a cell of this TrueFPS";
    for (const SiteSpec& spec : kSites)
        if (spec.slot && operand == uint32_t(reinterpret_cast<uintptr_t>(spec.slot))) return "a slot of this TrueFPS";
    return s.inImage(uintptr_t(operand), 4) ? "an address in the client image" : "an address outside the client image";
}

// A neutral site whose operand names the client's own value again - the other tool unloaded and wrote back what it
// had saved - can rejoin its group. `original` becomes the operand that is there now, which is what removal writes
// back from then on; an operand that is truefps's own slot again means the patch itself is back in.
inline bool retakeNeutralSite(Site& s) {
    if (!s.neutral || !s.spec || s.spec->length != 6) return false;
    const SiteSpec& spec = *s.spec;
    uint8_t now[6] = {};
    if (!readRaw(s.at, now, sizeof now) || std::memcmp(now, s.original, 2) != 0) return false;   // the instruction changed
    uint32_t operand = 0, held = 0;
    std::memcpy(&operand, now + 2, 4);
    const float* ours = siteSlot(spec);
    // Recognize our patch only after adoption. Initially neutral sites saved the other tool's operand,
    // so they cannot use it to remove a returned pointer to our slot.
    const bool mine = s.adopted && ours && operand == uint32_t(reinterpret_cast<uintptr_t>(ours));
    if (!mine && (!readValue(uintptr_t(operand), held) || held != spec.floatBits)) return false;
    if (!mine) std::memcpy(s.original, now, spec.length);
    s.neutral = false;
    s.adopted = false;
    s.patched = mine;
    if (mine) buildPatch(s);   // the bytes in the client are truefps's own again
    return true;
}
// Offer the neutral sites of running groups back to truefps, every g_truefpsRetakeFrames frames. Only atomic swaps,
// so no freeze is needed; a site changes state only once its swap is in.
inline uint32_t g_truefpsRetakeFrames = 60;   // about once a second at 60 fps; 0 never
// Refresh constants on this interval even when no sites are neutral.
inline bool refreshDue(uint32_t frame) { return g_truefpsRetakeFrames != 0 && frame % g_truefpsRetakeFrames == 0; }
// The gate runSmoothPatches uses: only while a site is another tool's, and only on one frame in g_truefpsRetakeFrames.
inline bool retakeDue(uint32_t frame, size_t neutralNow) { return neutralNow != 0 && refreshDue(frame); }

// Read a replaced imm32 from the saved bytes; read untouched instructions live (the spring's preceding fmul).
inline bool siteConstantAddr(const Site& s, uintptr_t& addr) {
    if (!s.spec || !s.spec->floatAt) return false;
    const SiteSpec& spec = *s.spec;
    uint32_t imm = 0;
    if (spec.floatAt > 0 && size_t(spec.floatAt) + 4 <= size_t(spec.length)) std::memcpy(&imm, s.original + spec.floatAt, 4);
    else if (!readValue(s.at + uintptr_t(intptr_t(spec.floatAt)), imm)) return false;
    addr = uintptr_t(imm);
    return addr != 0;
}
inline uint32_t g_truefpsConstantChanges = 0;   // accepted constant changes, for logging
// Follow client constant edits in the stub copies; ignore values outside each site's band.
// In particular, springStub must divide by the same factor as the client's untouched fmul. Returns the change count.
inline size_t refreshConstantCopies(SmoothSites& sm) {
    size_t changed = 0;
    for (size_t i = 0; i < kSiteCount; i++) {
        const SiteSpec& spec = kSites[i];
        Site& s = sm.sites[i];
        if (!spec.constantCopy || !s.spec || s.neutral) continue;
        uintptr_t addr = 0;
        uint32_t bits = 0;
        if (!siteConstantAddr(s, addr) || !readValue(addr, bits) || bits == sm.floatBits[i]) continue;
        if (!constantInBand(spec, bits)) continue;
        sm.floatBits[i] = bits;
        *spec.constantCopy = floatFromBits(bits);
        ++changed;
        ++g_truefpsConstantChanges;
    }
    // Push immediates remain outside the patches. Update a stub's comparison value only when all located
    // calls using that stub agree on the new factor.
    for (size_t i = 0; i < kSiteCount; i++) {
        const SiteSpec& spec = kSites[i];
        const Site& s = sm.sites[i];
        if (!spec.contextBits || !spec.contextFloatAt || !s.spec) continue;
        uint32_t bits = 0;
        if (!readValue(s.at + uintptr_t(intptr_t(spec.contextFloatAt)), bits) || bits == sm.floatBits[i]) continue;
        const float k = floatFromBits(bits);
        if (!(k > 0.0f && k <= 1.0f)) continue;
        bool agreed = true;
        for (size_t j = 0; j < kSiteCount && agreed; j++) {
            const SiteSpec& other = kSites[j];
            if (j == i || other.stub != spec.stub || !other.contextBits || !sm.sites[j].spec) continue;
            uint32_t held = 0;
            agreed = readValue(sm.sites[j].at + uintptr_t(intptr_t(other.contextFloatAt)), held) && held == bits;
        }
        if (!agreed) continue;
        for (size_t j = 0; j < kSiteCount; j++)
            if (kSites[j].contextBits == spec.contextBits && sm.sites[j].spec) sm.floatBits[j] = bits;
        *spec.contextBits = bits;
        ++changed;
        ++g_truefpsConstantChanges;
    }
    // Refresh the far-distance threshold only after its test resolved; require a positive value.
    const Site& spring = sm.sites[kSpringSite];
    if (spring.spec && sm.springFar > 0.0f) {
        uint32_t imm = 0, bits = 0;
        if (readValue(spring.at + uintptr_t(intptr_t(kSpringFarImmFromSite)), imm) && readValue(uintptr_t(imm), bits) && bits != bitsOf(sm.springFar)) {
            const float threshold = floatFromBits(bits);
            if (threshold > 0.0f) {
                sm.springFar = threshold;
                g_truefpsSpringFar = threshold;
                ++changed;
                ++g_truefpsConstantChanges;
            }
        }
    }
    return changed;
}
// Retake neutral sites in active groups. In inactive groups, only reclaim adopted sites and remove returned
// patches with an atomic swap.
inline size_t retakeNeutralSites(SmoothSites& sm, CodeWriter write = writeCode) {
    size_t taken = 0;
    for (uint8_t i = 0; i < kGroupCount; i++) {
        const GroupSpec& gs = kGroups[i];
        for (uint8_t k = 0; k < gs.siteCount; k++) {
            Site& site = sm.sites[gs.firstSite + k];
            if (!site.neutral) continue;
            if (!sm.groups[i].on) {
                if (reclaimAdoptedSite(site) && site.patched) removeSite(site, write);
                continue;
            }
            Site probe = site;
            if (!retakeNeutralSite(probe)) continue;
            // Nothing written: leave the site to the other tool and try again next pass. A swap that went in but
            // could not be read back still belongs to truefps, so that one is taken.
            if (!probe.patched && !installSite(probe, write) && !probe.patched) continue;
            site = probe;
            // A competing write made it neutral again. Undo takeNeutralOperand's duplicate ownership count.
            if (probe.neutral) {
                if (g_truefpsNeutralNow) --g_truefpsNeutralNow;
                continue;
            }
            ++taken;
            ++g_truefpsNeutralChanges;
            if (g_truefpsNeutralNow) --g_truefpsNeutralNow;
        }
    }
    return taken;
}

// The unload's pin decision, kept next to the reason it leaves out: a site another tool took over needs no pin,
// because the cell its operand names outlives this module and holds the client's own constant.
inline bool unloadMustStay(bool restored, bool orphanedTimer, bool offThread) { return !restored || orphanedTimer || offThread; }

// Installs a group's sites in order, or none: a failure takes out what went in. `stuck`: a site could not come out.
inline bool installGroupSites(SmoothSites& sm, uint8_t group, bool& stuck, CodeWriter write = writeCode) {
    const GroupSpec& gs = kGroups[group];
    stuck = false;
    for (uint8_t k = 0; k < gs.siteCount; k++) {
        if (installSite(sm.sites[gs.firstSite + k], write)) continue;
        for (int j = int(k); j >= 0; j--)
            if (!removeSite(sm.sites[gs.firstSite + j], write)) stuck = true;
        return false;
    }
    return true;
}
// Takes a group's sites out in reverse order. False if any site could not be restored (left as it is).
inline bool removeGroupSites(SmoothSites& sm, uint8_t group, CodeWriter write = writeCode) {
    const GroupSpec& gs = kGroups[group];
    bool ok = true;
    for (int k = int(gs.siteCount) - 1; k >= 0; k--)
        if (!removeSite(sm.sites[gs.firstSite + k], write)) ok = false;
    return ok;
}
inline bool anySitePatched(const SmoothSites& sm) {
    for (const auto& s : sm.sites) if (s.patched) return true;
    return false;
}

// A fresh start for the counter shadows (smooth mode starting again).
inline void resetShadows() { g_truefpsShadowD8C = ShadowCounter{}; g_truefpsShadowD88 = ShadowCounter{}; }

// The effects engine's cached step, read live to seed g_truefpsWEB0 before its readers are switched over.
inline bool effectsCachedStep(float& out) {
    uintptr_t object = 0;
    return g_truefpsEffectsGlobal && readValue(g_truefpsEffectsGlobal, object) && object && readValue(object + 0xEB0, out);
}

// The groups' lifecycle, called at Present on the game thread. `log` gets each group's problems once.
using GroupLog = void (*)(uint8_t group, const char* what);

inline bool atomicSite(SiteKind kind) { return kind == SiteKind::SwapImm || kind == SiteKind::CallToStub || kind == SiteKind::Byte || kind == SiteKind::Disp8; }

// Entity path 1 spans a step call and __ftol between the divisor and fild->fld patches.
// Freeze the full 0x8D340..0x8D35D span so no thread retains an incompatible stack value.
inline constexpr size_t kEntityPath1Divisor = 25, kEntityPath1Fild = 26;   // kSites indices ("entity path 1 divisor", "entity path 1 fild")
inline bool entityPath1Span(const SmoothSites& sm, CodeRange& out) {
    const Site& a = sm.sites[kEntityPath1Divisor];
    const Site& b = sm.sites[kEntityPath1Fild];
    if (!(a.patched || b.patched) || !a.spec || !b.spec) return false;
    out = CodeRange{a.at, b.at + b.spec->length};
    return true;
}

// Thread ids the runtime freeze leaves out: truefps's own writer threads, which run DLL code but never a patched path.
inline DWORD g_truefpsFreezeSkip[2] = {};

// Freezes a runtime multi-byte write tries per go, and the frames it waits after a go that found a thread in the way.
inline constexpr int kRuntimeQuietAttempts = 2;
inline uint32_t g_truefpsQuietRetryFrames = 30;   // zero retries on the next call

// Require clear site ranges, this DLL and entity path 1; installation also checks the accessor and __ftol.
// Atomic-only groups do not need a freeze.
inline size_t groupQuietRanges(const SmoothSites& sm, uint8_t group, bool patchedOnly, CodeRange* ranges, size_t max, bool& needsQuiet) {
    const GroupSpec& gs = kGroups[group];
    size_t n = 0;
    needsQuiet = false;
    for (uint8_t k = 0; k < gs.siteCount; k++) {
        const Site& site = sm.sites[gs.firstSite + k];
        if (!site.spec || site.neutral || (patchedOnly && !site.patched)) continue;   // a neutral site is never written
        if (!atomicSite(site.spec->kind)) needsQuiet = true;
        if (n + 4 < max) ranges[n++] = CodeRange{site.at, site.at + site.spec->length};   // 4 spare for image/span/calls
    }
    if (!needsQuiet) return n;
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&stepStub), &self) || !self)
        ranges[n++] = CodeRange{0, UINTPTR_MAX};   // the DLL's span is unknown: treat every thread as busy
    else {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<uintptr_t>(self) + uintptr_t(dos->e_lfanew));
        ranges[n++] = CodeRange{reinterpret_cast<uintptr_t>(self), reinterpret_cast<uintptr_t>(self) + nt->OptionalHeader.SizeOfImage};
    }
    const Site& a = sm.sites[kEntityPath1Divisor];
    const Site& b = sm.sites[kEntityPath1Fild];
    if (group == GroupEntities && a.spec && b.spec && (!patchedOnly || a.patched || b.patched)) ranges[n++] = CodeRange{a.at, b.at + b.spec->length};
    if (group == GroupEntities && !patchedOnly)
        for (const CodeRange& r : sm.path1Calls)
            if (r.hi) ranges[n++] = r;
    return n;
}

// Open only written site spans. Extra thread-check ranges must remain read-only.
inline size_t groupWriteRanges(const SmoothSites& sm, uint8_t group, bool patchedOnly, CodeRange* ranges, size_t max) {
    const GroupSpec& gs = kGroups[group];
    size_t n = 0;
    for (uint8_t k = 0; k < gs.siteCount && n < max; k++) {
        const Site& site = sm.sites[gs.firstSite + k];
        if (!site.spec || site.neutral || (patchedOnly && !site.patched)) continue;   // a neutral site is never written
        ranges[n++] = CodeRange{site.at, site.at + site.spec->length};
    }
    return n;
}

// Run act only after a quiet freeze and retry backoff. Open written pages before freezing,
// restore after thawing. putIn distinguishes install failures from retryable removal failures.
template <typename Act>
inline bool groupQuietly(SmoothSites& sm, uint8_t group, bool putIn, const CodeRange* ranges, size_t n, const CodeRange* writes, size_t writeCount, int attempts, Act&& act) {
    GroupRuntime& gr = sm.groups[group];
    if (sm.frame < gr.quietRetryAt) return false;
    OpenPages pages(writes, writeCount);
    if (!pages.ok()) {
        _snprintf_s(gr.quietWhy, sizeof gr.quietWhy, _TRUNCATE, "the pages holding its code could not be opened for writing (error %lu)",
                    static_cast<unsigned long>(pages.error()));
        gr.quietPagesFailed = true;
        // Latch page failures on install; failed removals must remain retryable.
        if (putIn) {
            gr.failed = true;
            gr.why = gr.quietWhy;
        }
        gr.quietRetryAt = sm.frame + g_truefpsQuietRetryFrames;
        return false;
    }
    gr.quietPagesFailed = false;   // the pages opened: a miss below is a thread in the way
    bool raced = false;
    const bool quiet = whenNoThreadIn(ranges, n, g_truefpsFreezeSkip, 2, attempts, [&] {
        // Remote entities ran on another thread since the caller looked: write nothing; the next frame retires the group.
        if (group == GroupEntities && g_truefpsGroupOffThread[GroupEntities]) { raced = true; return; }
        act();
    }, gr.quietWhy, sizeof gr.quietWhy);
    if (quiet && !raced) { gr.quietMisses = 0; gr.quietRetryAt = 0; return true; }
    // A raced write backs off but does not count as a busy-thread failure; retire the group next frame.
    if (raced) _snprintf_s(gr.quietWhy, sizeof gr.quietWhy, _TRUNCATE, "its code ran on a thread other than the game's just as the write began");
    else ++gr.quietMisses;
    gr.quietRetryAt = sm.frame + g_truefpsQuietRetryFrames;
    return false;
}
// Report each direction once: after eight busy freezes, or immediately on page failure.
inline void logQuietMisses(SmoothSites& sm, uint8_t group, GroupLog log, bool putIn) {
    GroupRuntime& gr = sm.groups[group];
    if (!log) return;
    const char* const doing = putIn ? "put in" : "take out";
    const size_t which = putIn ? 0 : 1;   // putting the patches in and taking them out are two statements
    char line[560];
    if (gr.quietPagesFailed) {   // its own latch, so one cause never silences the other
        if (gr.quietPagesLogged[which]) return;
        gr.quietPagesLogged[which] = true;
        _snprintf_s(line, sizeof line, _TRUNCATE, "cannot %s its patches: %s; a page that cannot be made writable does not pass by itself", doing,
                    gr.quietWhy[0] ? gr.quietWhy : "no detail");
    } else {
        if (gr.quietLogged[which] || gr.quietMisses < 8) return;
        gr.quietLogged[which] = true;
        _snprintf_s(line, sizeof line, _TRUNCATE, "is waiting to %s its patches: a game thread was in its code on %u tries in a row; it keeps trying (last try: %s)", doing,
                    gr.quietMisses, gr.quietWhy[0] ? gr.quietWhy : "no detail");
    }
    log(group, line);
}

// Instruction-boundary changes require freezing threads outside the DLL, sites and entity path 1.
inline bool removeGroupSitesQuiet(SmoothSites& sm, uint8_t group, bool& quiet, CodeWriter write = writeCode, int attempts = 20) {
    quiet = true;
    CodeRange ranges[40];
    bool needsQuiet = false;
    const size_t n = groupQuietRanges(sm, group, true, ranges, 40, needsQuiet);
    if (!needsQuiet) return removeGroupSites(sm, group, write);
    CodeRange writes[40];
    const size_t writeCount = groupWriteRanges(sm, group, true, writes, 40);
    bool out = false;
    quiet = groupQuietly(sm, group, false, ranges, n, writes, writeCount, attempts, [&] { out = removeGroupSites(sm, group, write); });
    return quiet && out;
}

// Puts a group's sites in at runtime; multi-byte sites under the same freeze as removal. `quiet` false: nothing written.
inline bool installGroupSitesQuiet(SmoothSites& sm, uint8_t group, bool& stuck, bool& quiet, CodeWriter write = writeCode, int attempts = 20) {
    quiet = true;
    stuck = false;
    CodeRange ranges[40];
    bool needsQuiet = false;
    const size_t n = groupQuietRanges(sm, group, false, ranges, 40, needsQuiet);
    if (!needsQuiet) return installGroupSites(sm, group, stuck, write);
    CodeRange writes[40];
    const size_t writeCount = groupWriteRanges(sm, group, false, writes, 40);
    bool in = false;
    quiet = groupQuietly(sm, group, true, ranges, n, writes, writeCount, attempts, [&] { in = installGroupSites(sm, group, stuck, write); });
    return quiet && in;
}

// After an off-thread call, retain multi-byte or previously kept sites and all later sites in the group.
// Earlier atomic sites can retire; any retained site disables smooth mode for the session.
inline bool retireGroup(SmoothSites& sm, uint8_t group, GroupLog log) {
    const GroupSpec& gs = kGroups[group];
    GroupRuntime& gr = sm.groups[group];
    g_truefpsGroupOn[group] = 0;
    gr.on = false;
    int firstKept = gs.siteCount;
    for (int k = 0; k < gs.siteCount && firstKept == gs.siteCount; k++) {
        const Site& s = sm.sites[gs.firstSite + k];
        if (s.kept || (s.patched && !atomicSite(s.spec->kind))) firstKept = k;
    }
    bool kept = false, removed = true;
    for (int k = int(gs.siteCount) - 1; k >= 0; k--) {
        Site& s = sm.sites[gs.firstSite + k];
        if (!s.patched) continue;
        if (k >= firstKept) { s.kept = kept = true; continue; }
        if (!removeSite(s)) removed = false;   // a compare-and-swap site: no multi-byte write
    }
    if (kept) sm.sessionOver = true;
    gr.stuck = !removed;   // clear after a later attempt restores all removable sites
    if (!gr.retired) {
        gr.retired = true;
        gr.failed = true;
        gr.why = "its code runs on a thread other than the game's";
        if (log) log(group, kept ? "runs on a thread other than the game's: smooth mode is off for this session; its call patches stay in, running the original arithmetic, and TrueFPS stays in memory until the game closes"
                            : !removed ? "runs on a thread other than the game's; its patches could not all be removed"
                                       : "runs on a thread other than the game's: whole ticks for this session");
    }
    return removed && !kept;
}

// Distinguish a missing persistent cell from a changed client or a failed patch write.
inline bool groupNeedsCellPage(uint8_t group) {
    const GroupSpec& gs = kGroups[group];
    for (uint8_t k = 0; k < gs.siteCount; k++) {
        const SiteSpec& spec = kSites[gs.firstSite + k];
        if (spec.cell && !persistentCell(spec.cell)) return true;
    }
    return false;
}

// Any site still in keeps the step accessors in: they fill the effects readers' copy and the path-1 off-thread flag.
inline bool stepPatchWanted(bool speedUp, bool smoothOn, const SmoothSites& sm) { return speedUp || smoothOn || anySitePatched(sm); }

// Enable resolved groups; retire off-thread consumers. moveReal requires both camera follow loops.
inline void runSmoothPatches(SmoothSites& sm, GroupLog log, CodeWriter write = writeCode) {
    ++sm.frame;
    if (!sm.live) { resetShadows(); resetEntityClocks(); resetTrailClocks(); resetEventClocks(); sm.live = true; }
    for (uint8_t i = 0; i < kGroupCount; i++)
        if (i != GroupPlayer && sm.groups[i].found && g_truefpsGroupOffThread[i]) retireGroup(sm, i, log);
    GroupRuntime& player = sm.groups[GroupPlayer];
    if (player.found && !player.failed && g_truefpsGroupOffThread[GroupPlayer]) {
        player.failed = true;
        player.why = "its code runs on a thread other than the game's";
        if (log) log(GroupPlayer, "runs on a thread other than the game's: whole ticks for this session");
    }
    if (sm.sessionOver) {
        smoothClockOff();   // every slot the client's constant, every policy off
        player.on = false;
        return;
    }
    // Retry stuck removals periodically; full restoration allows reinstallation below.
    // Failed and retired groups stay off.
    if (refreshDue(sm.frame)) {
        for (uint8_t i = 0; i < kGroupCount; i++) {
            GroupRuntime& gr = sm.groups[i];
            if (!gr.stuck || gr.failed || i == GroupPlayer) continue;
            bool quiet = true;
            if (!removeGroupSitesQuiet(sm, i, quiet, write, kRuntimeQuietAttempts)) continue;
            gr.stuck = false;
            gr.why.clear();
        }
    }
    for (uint8_t i = 0; i < kGroupCount; i++) {
        GroupRuntime& gr = sm.groups[i];
        if (!gr.found || gr.failed || gr.stuck || i == GroupPlayer) continue;
        if (gr.on) { g_truefpsGroupOn[i] = 1; continue; }   // smoothClockOff clears the policy under it
        if (i == GroupEffects) { float cached = 0.0f; if (effectsCachedStep(cached)) g_truefpsWEB0 = cached; }
        bool stuck = false, quiet = true;
        const bool in = installGroupSitesQuiet(sm, i, stuck, quiet, write, kRuntimeQuietAttempts);
        if (!in && !quiet) { logQuietMisses(sm, i, log, true); continue; }   // putting in; nothing written: whole ticks for now
        if (!in) {
            gr.failed = true;
            gr.stuck = stuck;
            const bool cellPage = groupNeedsCellPage(i);
            gr.why = cellPage ? "the page its cells need could not be reserved" : "a patch could not be written or read back";
            if (log) log(i, stuck      ? "could not be patched, and a patch could not be removed again"
                           : cellPage  ? "could not be patched (the page its cells need could not be reserved): whole ticks for this session"
                                       : "could not be patched (the code changed since load, or the write failed): whole ticks for this session");
            continue;
        }
        if (i == GroupTrails) resetTrailClocks();   // clocks from before the group was off may name dead trails
        if (i == GroupConnectionRetry) resetRetryClocks();   // the same for a record's clock
        if (i == GroupActorCounters) resetCounterCarries();  // and for an entity's carry
        if (i == GroupEventWalk || i == GroupEventMove || i == GroupEventTimedMove) resetEventClocks();
        gr.on = true;
        g_truefpsGroupOn[i] = 1;
    }
    // Refresh retuned constants and reclaim operands returned by other tools.
    if (refreshDue(sm.frame)) refreshConstantCopies(sm);
    if (retakeDue(sm.frame, g_truefpsNeutralNow)) retakeNeutralSites(sm, write);
    const bool real = player.found && !player.failed && sm.groups[GroupLookAt].on && sm.groups[GroupEye].on;
    player.on = real;
    g_truefpsGroupOn[GroupPlayer] = real ? 1 : 0;
    g.moveReal = real;
}

// Include patched and adopted sites in the unload write ranges: adopted operands may have been returned.
// Their pages must be writable before the freeze. Returns the number of ranges stored in `out`.
inline size_t unloadSiteRanges(const SmoothSites& sm, CodeRange* out, size_t max) {
    size_t n = 0;
    for (const auto& site : sm.sites)
        if ((site.patched || site.adopted) && site.spec && n < max) out[n++] = CodeRange{site.at, site.at + site.spec->length};
    return n;
}
// Any effects cached-step reader still holding truefps's bytes; their copy comes only from the patched step accessors.
inline bool effectsReadersPatched(const SmoothSites& sm) {
    const GroupSpec& gs = kGroups[GroupEffects];
    for (uint8_t k = 0; k < gs.siteCount; k++) if (sm.sites[gs.firstSite + k].patched) return true;
    return false;
}
// Unload under a quiet freeze. Retain off-thread entity path 1: __ftol may still hold its float divisor.
// Report those sites in keptPath1; no allocation or logging. Reclaim returned adopted operands before removal.
inline bool removeAllSitesFrozen(SmoothSites& sm, CodeWriter write = writeCode, bool* keptPath1 = nullptr) {
    bool ok = true;
    if (keptPath1) *keptPath1 = false;
    for (int i = int(kGroupCount) - 1; i >= 0; i--) {
        g_truefpsGroupOn[i] = 0;
        sm.groups[i].on = false;
        const GroupSpec& gs = kGroups[i];
        for (int k = int(gs.siteCount) - 1; k >= 0; k--) {
            const size_t index = size_t(gs.firstSite) + size_t(k);
            Site& site = sm.sites[index];
            if (i == GroupEntities && g_truefpsGroupOffThread[GroupEntities] && (index == kEntityPath1Divisor || index == kEntityPath1Fild) && site.patched) {
                site.kept = true;
                if (keptPath1) *keptPath1 = true;
                else ok = false;
                continue;
            }
            site.kept = false;
            reclaimAdoptedSite(site);
            if (!removeSite(site, write)) ok = false;
        }
    }
    g.moveReal = false;
    sm.live = false;
    return ok;
}

// Disable policies and restore owned patches. Failed restoration leaves the group on whole ticks until all sites
// can be restored by a later call.
inline bool stopSmoothPatches(SmoothSites& sm, GroupLog log, CodeWriter write = writeCode) {
    ++sm.frame;
    bool ok = true;
    for (int i = int(kGroupCount) - 1; i >= 0; i--) {
        g_truefpsGroupOn[i] = 0;
        GroupRuntime& gr = sm.groups[i];
        gr.on = false;
        if (i != GroupPlayer && g_truefpsGroupOffThread[i]) {
            if (!retireGroup(sm, uint8_t(i), log)) ok = false;
            continue;
        }
        bool quiet = true;
        if (removeGroupSitesQuiet(sm, uint8_t(i), quiet, write, kRuntimeQuietAttempts)) {
            if (gr.stuck) {   // all sites restored; reinstallation is allowed unless failed
                gr.stuck = false;
                if (!gr.failed) gr.why.clear();
            }
            continue;
        }
        ok = false;
        if (!quiet) { logQuietMisses(sm, uint8_t(i), log, false); continue; }   // taking out; policy off, patches out later
        if (!gr.stuck) {
            gr.stuck = true;
            // Report the first site removal left patched.
            char found[200] = "a patch could not be taken back out";
            const GroupSpec& gs = kGroups[i];
            for (uint8_t k = 0; k < gs.siteCount; k++) {
                const Site& s = sm.sites[gs.firstSite + k];
                if (!s.spec || !s.patched) continue;
                uint8_t now[16] = {};
                const char* note = readRaw(s.at, now, s.spec->length) ? siteStateNote(s, now) : "could not be read";
                _snprintf_s(found, sizeof found, _TRUNCATE, "%s: %s", s.spec->name, note ? note : "TrueFPS's patch kept in; writing the original back failed");
                break;
            }
            if (!gr.failed) gr.why = found;
            if (log) {
                char line[400];
                _snprintf_s(line, sizeof line, _TRUNCATE, "could not take every patch back out (%s), so it stays on whole ticks; the take-out is tried again every frame while smooth mode is off and once a second while it runs",
                            found);
                log(uint8_t(i), line);
            }
        }
    }
    g.moveReal = false;
    sm.live = false;
    return ok;
}

}  // namespace truefps
