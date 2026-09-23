// Client code writes: atomic step-accessor hot patches and game-thread call replacements.
#pragma once
#include "limiter.h"

namespace truefps {

// VirtualProtect can deadlock if a frozen thread holds the address-space lock.
// OpenPages changes protection before freezing and flushes the cache after thawing.
inline bool pagesAlreadyWritable(uintptr_t at, size_t n);
// Instrument protection changes made outside OpenPages.
inline volatile long g_truefpsProtectCalls = 0;

inline bool writeCode(uintptr_t at, const uint8_t* bytes, size_t n) {
    const bool open = pagesAlreadyWritable(at, n);
    DWORD old = 0;
    if (!open) _InterlockedIncrement(&g_truefpsProtectCalls);
    if (!open && !VirtualProtect(reinterpret_cast<LPVOID>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    const bool wrote = writeRaw(at, bytes, n);
    if (!open) {
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), n, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), n);
    }
    uint8_t back[16] = {};
    return wrote && n <= sizeof back && readRaw(at, back, n) && std::memcmp(back, bytes, n) == 0;
}

inline bool bytesAre(uintptr_t at, const uint8_t* expected, size_t n) {
    uint8_t now[16] = {};
    return n <= sizeof now && readRaw(at, now, n) && std::memcmp(now, expected, n) == 0;
}

// Replace an aligned operand only while it matches expected.
inline bool swapCode32(uintptr_t at, uint32_t expected, uint32_t replacement) {
    const bool open = pagesAlreadyWritable(at, 4);
    DWORD old = 0;
    if (!open) _InterlockedIncrement(&g_truefpsProtectCalls);
    if (!open && !VirtualProtect(reinterpret_cast<LPVOID>(at), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        ok = uint32_t(InterlockedCompareExchange(reinterpret_cast<LONG*>(at), LONG(replacement), LONG(expected))) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!open) {
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), 4, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), 4);
    }
    return ok;
}
inline bool swapCode8(uintptr_t at, uint8_t expected, uint8_t replacement) {
    const bool open = pagesAlreadyWritable(at, 1);
    DWORD old = 0;
    if (!open) _InterlockedIncrement(&g_truefpsProtectCalls);
    if (!open && !VirtualProtect(reinterpret_cast<LPVOID>(at), 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        ok = uint8_t(_InterlockedCompareExchange8(reinterpret_cast<char*>(at), char(replacement), char(expected))) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!open) {
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), 1, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), 1);
    }
    return ok;
}

// Points the limiter loop's `mov ebp,[imm32]` at `to`, only while it still reads `from`.
inline bool retargetSleepSlot(uintptr_t loopSite, uint32_t from, uint32_t to) {
    uint8_t expected[6] = {0x8B, 0x2D};
    std::memcpy(expected + kLoopImmOffset, &from, 4);
    if (!bytesAre(loopSite, expected, sizeof expected)) return false;
    uint8_t imm[4];
    std::memcpy(imm, &to, 4);
    return writeCode(loopSite + kLoopImmOffset, imm, 4);
}

// Suspend other threads without heap allocation: a frozen thread may hold the heap lock.
// An incomplete freeze makes anyInRanges() fail closed.
class ThreadFreeze {
public:
    ThreadFreeze() {
        using NextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
        static const auto nextThread = reinterpret_cast<NextThreadFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtGetNextThread"));
        if (!nextThread) { complete_ = false; failStep_ = "NtGetNextThread missing"; return; }
        const DWORD self = GetCurrentThreadId();
        const ACCESS_MASK access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;
        bool settled = false;
        for (int pass = 0; pass < kMaxPasses && complete_; pass++) {
            bool added = false;
            HANDLE current = nullptr;
            for (;;) {
                HANDLE next = nullptr;
                const LONG status = nextThread(GetCurrentProcess(), current, access, 0, 0, &next);
                if (current) CloseHandle(current);
                current = nullptr;
                if (status == LONG(0x8000001A)) break;           // STATUS_NO_MORE_ENTRIES
                if (status < 0 || !next) { complete_ = false; failStep_ = "walk"; failErr_ = DWORD(status); break; }
                const DWORD id = GetThreadId(next);
                if (id == self || holds(id)) { current = next; continue; }
                if (count_ == kMaxThreads) { current = next; complete_ = false; failStep_ = "too many threads"; break; }
                // an exited thread is still walked while a handle keeps its object alive; it runs nothing
                { DWORD code = 0; if (GetExitCodeThread(next, &code) && code != STILL_ACTIVE) { current = next; continue; } }
                if (SuspendThread(next) == DWORD(-1)) { DWORD code = 0; if (GetExitCodeThread(next, &code) && code != STILL_ACTIVE) { current = next; continue; } current = next; complete_ = false; failStep_ = "SuspendThread"; failTid_ = id; failErr_ = GetLastError(); break; }
                Thread& t = threads_[count_++];
                t.id = id;
                t.handle = next;                                  // closed by the destructor
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;           // also waits for the suspension to take effect
                t.ipKnown = GetThreadContext(next, &context) != FALSE;
                if (!t.ipKnown) { t.ctxErr = GetLastError(); ++unknown_; }
                t.ip = t.ipKnown ? uintptr_t(context.Eip) : 0;
                added = true;
                // NtGetNextThread continues from a handle; take a second one to keep this thread's.
                if (!DuplicateHandle(GetCurrentProcess(), next, GetCurrentProcess(), &current, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    complete_ = false; failStep_ = "DuplicateHandle"; failErr_ = GetLastError();
                    break;
                }
            }
            if (current) CloseHandle(current);
            if (complete_ && !added) { settled = true; break; }
        }
        if (!settled) { complete_ = false; if (!failStep_) failStep_ = "not settled"; }
    }
    ~ThreadFreeze() {
        for (size_t i = 0; i < count_; i++) { ResumeThread(threads_[i].handle); CloseHandle(threads_[i].handle); }
    }
    ThreadFreeze(const ThreadFreeze&) = delete;
    ThreadFreeze& operator=(const ThreadFreeze&) = delete;

    bool complete() const { return complete_; }
    // Collect raw thread state while frozen; format it after thawing.
    struct Busy {
        bool complete = true;
        const char* failStep = nullptr;
        DWORD failTid = 0, failErr = 0;
        unsigned threads = 0, unknown = 0;
        struct Hit { DWORD id; uintptr_t ip; bool ipKnown; DWORD ctxErr; } hits[16] = {};
        size_t hitCount = 0, hitsTotal = 0;
    };
    void collect(Busy& out, const struct CodeRange* ranges, size_t n, const DWORD* skip = nullptr, size_t skipCount = 0) const;
    static void format(const Busy& b, char* out, size_t size);
    size_t suspended() const { return count_; }
    // Treat unreadable IPs and incomplete freezes as busy. Exclude our writer threads via skip.
    bool anyInRanges(const struct CodeRange* ranges, size_t n, const DWORD* skip = nullptr, size_t skipCount = 0) const;

private:
    static constexpr size_t kMaxThreads = 512;
    static constexpr int kMaxPasses = 8;
    struct Thread { DWORD id = 0; HANDLE handle = nullptr; uintptr_t ip = 0; bool ipKnown = false; DWORD ctxErr = 0; };
    bool holds(DWORD id) const {
        for (size_t i = 0; i < count_; i++) if (threads_[i].id == id) return true;
        return false;
    }
    Thread threads_[kMaxThreads];
    size_t count_ = 0;
    bool complete_ = true;
        const char* failStep_ = nullptr;
        DWORD failTid_ = 0, failErr_ = 0;
        size_t unknown_ = 0;
};

// Half-open code span that must be free of executing threads during patching.
struct CodeRange { uintptr_t lo = 0, hi = 0; };

// Make written spans writable before freezing; restore protection and flush after thawing.
// Do not include spans checked only for executing threads.
class OpenPages {
public:
    OpenPages(const CodeRange* ranges, size_t n) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        grain_ = info.dwPageSize ? uintptr_t(info.dwPageSize) : 0x1000;
        for (size_t i = 0; i < n && ok_; i++) {
            if (!ranges[i].lo || ranges[i].hi <= ranges[i].lo) continue;
            for (uintptr_t page = ranges[i].lo & ~(grain_ - 1); page < ranges[i].hi; page += grain_) {
                if (holds(page)) continue;
                if (count_ == kMaxPages) { error_ = ERROR_INSUFFICIENT_BUFFER; ok_ = false; break; }
                DWORD old = 0;
                if (!VirtualProtect(reinterpret_cast<LPVOID>(page), size_t(grain_), PAGE_EXECUTE_READWRITE, &old)) { error_ = GetLastError(); ok_ = false; break; }
                pages_[count_].at = page;
                pages_[count_].old = old;
                ++count_;
            }
        }
        if (ok_) { previous_ = current(); current() = this; }
        else restore();
    }
    ~OpenPages() {
        if (ok_ && current() == this) current() = previous_;
        restore();
    }
    OpenPages(const OpenPages&) = delete;
    OpenPages& operator=(const OpenPages&) = delete;
    // False when a page could not be opened: the caller must not run the act.
    bool ok() const { return ok_; }
    DWORD error() const { return error_; }
    bool covers(uintptr_t at, size_t n) const {
        if (!at || !n) return false;
        for (uintptr_t page = at & ~(grain_ - 1); page < at + n; page += grain_)
            if (!holds(page)) return false;
        return true;
    }
    // The pages opened on this thread, or nullptr.
    static const OpenPages*& current() {
        static thread_local const OpenPages* open = nullptr;
        return open;
    }

private:
    static constexpr size_t kMaxPages = 256;
    struct Page { uintptr_t at = 0; DWORD old = 0; };
    bool holds(uintptr_t page) const {
        for (size_t i = 0; i < count_; i++) if (pages_[i].at == page) return true;
        return false;
    }
    void restore() {
        for (size_t i = count_; i-- > 0;) {
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<LPVOID>(pages_[i].at), size_t(grain_), pages_[i].old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(pages_[i].at), size_t(grain_));
        }
        count_ = 0;
    }
    Page pages_[kMaxPages];
    size_t count_ = 0;
    uintptr_t grain_ = 0x1000;
    bool ok_ = true;
    DWORD error_ = 0;
    const OpenPages* previous_ = nullptr;
};
inline bool pagesAlreadyWritable(uintptr_t at, size_t n) {
    const OpenPages* open = OpenPages::current();
    return open && open->covers(at, n);
}
inline bool ThreadFreeze::anyInRanges(const CodeRange* ranges, size_t n, const DWORD* skip, size_t skipCount) const {
    if (!complete_) return true;
    for (size_t i = 0; i < count_; i++) {
        bool skipped = false;
        for (size_t k = 0; k < skipCount && !skipped; k++) skipped = skip[k] != 0 && skip[k] == threads_[i].id;
        if (skipped) continue;
        if (!threads_[i].ipKnown) return true;
        for (size_t r = 0; r < n; r++)
            if (threads_[i].ip >= ranges[r].lo && threads_[i].ip < ranges[r].hi) return true;
    }
    return false;
}
inline void ThreadFreeze::collect(Busy& b, const CodeRange* ranges, size_t n, const DWORD* skip, size_t skipCount) const
    {
        b.complete = complete_;
        b.failStep = failStep_;
        b.failTid = failTid_;
        b.failErr = failErr_;
        b.threads = unsigned(count_);
        b.unknown = unsigned(unknown_);
        b.hitCount = b.hitsTotal = 0;
        for (size_t i = 0; i < count_; i++)
        {
            bool skipped = false;
            for (size_t k = 0; k < skipCount && !skipped; k++) skipped = skip[k] != 0 && skip[k] == threads_[i].id;
            if (skipped) continue;
            bool hit = !threads_[i].ipKnown;
            for (size_t r = 0; r < n && !hit; r++) hit = threads_[i].ip >= ranges[r].lo && threads_[i].ip < ranges[r].hi;
            if (!hit) continue;
            b.hitsTotal++;
            if (b.hitCount < sizeof b.hits / sizeof b.hits[0]) b.hits[b.hitCount++] = Busy::Hit{threads_[i].id, threads_[i].ip, threads_[i].ipKnown, threads_[i].ctxErr};
        }
    }
inline void ThreadFreeze::format(const Busy& b, char* out, size_t size)
    {
        int len = _snprintf_s(out, size, _TRUNCATE, "freeze %s (%s tid %lu err %lu), %u threads, %u without a readable context; in range:",
                              b.complete ? "complete" : "INCOMPLETE", b.failStep ? b.failStep : "-", static_cast<unsigned long>(b.failTid),
                              static_cast<unsigned long>(b.failErr), b.threads, b.unknown);
        for (size_t i = 0; i < b.hitCount && len > 0 && size_t(len) < size; i++)
        {
            char unreadable[40] = "";
            if (!b.hits[i].ipKnown) _snprintf_s(unreadable, sizeof unreadable, _TRUNCATE, " (context unreadable, error %lu)", static_cast<unsigned long>(b.hits[i].ctxErr));
            const int added = _snprintf_s(out + len, size - size_t(len), _TRUNCATE, " tid %lu ip %08X%s", static_cast<unsigned long>(b.hits[i].id), unsigned(b.hits[i].ip), unreadable);
            if (added < 0) return;   // truncated: stop
            len += added;
        }
        if (b.hitsTotal > b.hitCount && len > 0 && size_t(len) < size) _snprintf_s(out + len, size - size_t(len), _TRUNCATE, " (+%u more)", unsigned(b.hitsTotal - b.hitCount));
    }
// Try up to attempts freezes, 1 ms apart, requiring clear ranges and zero inflight calls.
// The caller must hold OpenPages; act must not allocate, lock or change protection.
template <class Act>
inline bool whenNoThreadIn(const CodeRange* ranges, size_t n, const DWORD* skip, size_t skipCount, int attempts, Act&& act, char* why = nullptr, size_t whySize = 0,
                           const volatile long* inflight = nullptr)
    {
        for (int i = 0; i < attempts; i++)
        {
            ThreadFreeze::Busy busy;
            bool report = false;
            long calls = 0;
            {
                ThreadFreeze freeze;
                calls = inflight ? *inflight : 0;
                if (calls == 0 && !freeze.anyInRanges(ranges, n, skip, skipCount)) { act(); return true; }
                if (why && whySize && i == attempts - 1) {   // the last try: gather why it was busy
                    freeze.collect(busy, ranges, n, skip, skipCount);
                    report = true;
                }
            }
            if (report) {   // outside the freeze: formatting may take the CRT's locks
                ThreadFreeze::format(busy, why, whySize);
                if (calls != 0) {
                    const size_t used = strnlen_s(why, whySize);
                    _snprintf_s(why + used, whySize - used, _TRUNCATE, "; %ld call(s) inside TrueFPS code that calls out", calls);
                }
            }
            if (i + 1 < attempts) Sleep(1);   // the last try is not followed by a wait
        }
        return false;
    }

// Write jmp target into trailing NOP padding, then atomically replace the entry
// with a 16-bit short jump. The original first instruction spans more than two bytes.
struct JumpSite {
    enum class State { Original, Jump };
    uintptr_t at = 0;
    uint8_t original[5] = {};   // the function's first bytes
    uintptr_t pad = 0;          // five bytes of NOP padding after the function, within a short jump of `at`
    uint8_t padOriginal[5] = {};   // padding bytes recorded at resolve
    bool padClaimed = false;       // recorded padding passed padClaimable
    State state = State::Original;
};
// A step accessor's jump pad: the padding after its code.
inline uintptr_t stepPadAt(uintptr_t accessor) { return accessor + parsePattern(kStepPattern).size(); }
// This module's image bounds.
inline bool ownImageSpan(uintptr_t& lo, uintptr_t& hi) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&stepPadAt), &self) ||
        !self)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<uintptr_t>(self) + uintptr_t(dos->e_lfanew));
    lo = reinterpret_cast<uintptr_t>(self);
    hi = lo + nt->OptionalHeader.SizeOfImage;
    return true;
}
// Accept five NOPs, a jump into this module, or a jump whose target is no longer executable.
// Leave jumps into other executable mappings alone, including a module reusing an earlier load's address.
inline bool padClaimable(const uint8_t* pad, uintptr_t at) {
    const uint8_t nops[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
    if (std::memcmp(pad, nops, 5) == 0) return true;
    if (pad[0] != 0xE9) return false;
    int32_t rel = 0;
    std::memcpy(&rel, pad + 1, 4);
    const uintptr_t target = at + 5 + uintptr_t(intptr_t(rel));
    uintptr_t lo = 0, hi = 0;
    if (ownImageSpan(lo, hi) && target >= lo && target < hi) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(target), &mbi, sizeof mbi) != sizeof mbi) return true;   // no target mapping found
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return !(mbi.State == MEM_COMMIT && (mbi.Protect & exec) != 0);
}
// Record pad bytes and ownership at resolve. Never call under a freeze: VirtualQuery takes a lock.
inline bool recordPad(JumpSite& site) {
    site.padClaimed = readRaw(site.pad, site.padOriginal, 5) && padClaimable(site.padOriginal, site.pad);
    return site.padClaimed;
}
enum class PatchResult { Done, Failed };
using CodeWriter = bool (*)(uintptr_t at, const uint8_t* bytes, size_t n);

// The two bytes `EB rel8` jumping from `at` to `pad`; false when out of range.
inline bool shortJump(const JumpSite& site, uint16_t& out) {
    const intptr_t rel = intptr_t(site.pad) - intptr_t(site.at + 2);
    if (rel < -128 || rel > 127) return false;
    out = uint16_t(0xEB | (uint16_t(uint8_t(int8_t(rel))) << 8));
    return true;
}

// One atomic 16-bit write of client code: only if it still holds `expected`.
inline bool swapHead(uintptr_t at, uint16_t expected, uint16_t replacement) {
    const bool open = pagesAlreadyWritable(at, 2);
    DWORD old = 0;
    if (!open) _InterlockedIncrement(&g_truefpsProtectCalls);
    if (!open && !VirtualProtect(reinterpret_cast<LPVOID>(at), 2, PAGE_EXECUTE_READWRITE, &old)) return false;
    short previous = 0;
    bool ok = false;
    __try {
        previous = InterlockedCompareExchange16(reinterpret_cast<short*>(at), short(replacement), short(expected));
        ok = uint16_t(previous) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!open) {
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), 2, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), 2);
    }
    return ok;
}

// Require original heads and claimable pads holding either their recorded bytes or this session's jump.
// Refuse later writes by other tools; `why` names the first failure.
inline bool jumpsInstallable(const JumpSite* sites, size_t n, uintptr_t target, const char** why = nullptr) {
    const auto refuse = [&](const char* text) { if (why) *why = text; return false; };
    uint16_t head = 0;
    for (size_t i = 0; i < n; i++) {
        if (sites[i].state != JumpSite::State::Original || !bytesAre(sites[i].at, sites[i].original, 5) || !shortJump(sites[i], head))
            return refuse("a game step accessor no longer holds the code TrueFPS found");
        if (!sites[i].padClaimed) return refuse("the padding after a game step accessor was already another tool's when TrueFPS loaded");
        uint8_t padNow[5] = {}, ours[5] = {};
        makeJump(ours, sites[i].pad, target);
        if (!readRaw(sites[i].pad, padNow, 5)) return refuse("the padding after a game step accessor could not be read");
        if (std::memcmp(padNow, sites[i].padOriginal, 5) != 0 && std::memcmp(padNow, ours, 5) != 0)
            return refuse("the padding after a game step accessor has been written by another tool since TrueFPS loaded");
    }
    if (why) *why = nullptr;
    return true;
}
// Installs the hot patch at every site, or at none: a failure swaps back the heads already swapped.
inline PatchResult installJumps(JumpSite* sites, size_t n, uintptr_t target, CodeWriter write = writeCode) {
    uint16_t head = 0;
    if (!jumpsInstallable(sites, n, target)) return PatchResult::Failed;
    for (size_t i = 0; i < n; i++) {
        uint8_t jump[5];
        makeJump(jump, sites[i].pad, target);
        const uint16_t original = uint16_t(sites[i].original[0] | (sites[i].original[1] << 8));
        shortJump(sites[i], head);
        if (!write(sites[i].pad, jump, 5) || !swapHead(sites[i].at, original, head)) {
            for (size_t j = 0; j < i; j++) {
                uint16_t h = 0;
                shortJump(sites[j], h);
                if (swapHead(sites[j].at, h, uint16_t(sites[j].original[0] | (sites[j].original[1] << 8)))) sites[j].state = JumpSite::State::Original;
            }
            return PatchResult::Failed;
        }
        sites[i].state = JumpSite::State::Jump;
    }
    return PatchResult::Done;
}

// Swaps the original first bytes back at every patched site; a head that is neither is left alone (Failed).
inline PatchResult removeJumps(JumpSite* sites, size_t n, uintptr_t, CodeWriter = writeCode) {
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        if (sites[i].state == JumpSite::State::Original) continue;
        uint16_t head = 0;
        const uint16_t original = uint16_t(sites[i].original[0] | (sites[i].original[1] << 8));
        if (!shortJump(sites[i], head)) { ok = false; continue; }
        if (swapHead(sites[i].at, head, original) || bytesAre(sites[i].at, sites[i].original, 2)) sites[i].state = JumpSite::State::Original;
        else ok = false;
    }
    return ok ? PatchResult::Done : PatchResult::Failed;
}

// Non-atomic player-movement call replacement; only the game thread executes this site.
struct CallSite {
    uintptr_t at = 0;
    uint8_t original[5] = {};
    uint8_t left[5] = {};   // what a failed write left at the site; only these bytes may be written over while `dirty`
    bool patched = false;
    bool dirty = false;   // a write that failed and could not be rolled back: truefps's bytes, whole or in part, are there
};
// True while the site holds the `call target` installCall writes.
inline bool callSiteHolds(const CallSite& site, uintptr_t target) {
    uint8_t call[5];
    makeJump(call, site.at, target);
    call[0] = 0xE8;
    return bytesAre(site.at, call, 5);
}
// Verify installed bytes on sampled frames to detect failed removal within 16 frames.
inline bool installCall(CallSite& site, uintptr_t target, CodeWriter write = writeCode, bool verify = true) {
    if (site.patched) return !verify || callSiteHolds(site, target);   // a removal that failed leaves `patched` true: the site is in only while it still holds this call
    if (site.dirty || !bytesAre(site.at, site.original, 5)) return false;
    uint8_t call[5];
    makeJump(call, site.at, target);
    call[0] = 0xE8;
    if (!write(site.at, call, 5)) {
        // A partial write is still ours to restore.
        if (!bytesAre(site.at, site.original, 5) && !write(site.at, site.original, 5)) {
            readRaw(site.at, site.left, 5);   // what is there now: the dirty arm below writes over nothing else
            site.dirty = true;
        }
        return false;
    }
    site.dirty = false;
    site.patched = true;
    return true;
}
// Puts the original instruction back if the site still holds this call, or the bytes a failed write left there.
inline bool removeCall(CallSite& site, uintptr_t target, CodeWriter write = writeCode) {
    if (!site.patched && !site.dirty) return true;
    if (bytesAre(site.at, site.original, 5)) { site.patched = site.dirty = false; return true; }
    if (site.dirty) {   // truefps's own part-written call: the original goes back over those bytes, and over nothing else
        if (!bytesAre(site.at, site.left, 5) || !write(site.at, site.original, 5)) return false;   // rewritten since: foreign, as the arm below reports it
        site.patched = site.dirty = false;
        return true;
    }
    uint8_t call[5];
    makeJump(call, site.at, target);
    call[0] = 0xE8;
    if (!bytesAre(site.at, call, 5) || !write(site.at, site.original, 5)) return false;
    site.patched = false;
    return true;
}

inline bool anyJumpLeft(const JumpSite* sites, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (sites[i].state != JumpSite::State::Original) return true;
    return false;
}

}  // namespace truefps
