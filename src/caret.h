// Chat caret: 26-draw cycle (10 hidden, 15 visible, 1 hidden).
// Draw RVA 0x1B7FD0; counter at object+0x20.
#pragma once
#include "limiter.h"

namespace truefps {

// The timer block in the inline draw.
inline constexpr const char* kCaretTimerPattern =
    "8B 46 20 83 F8 19 C6 44 24 10 00 7C 09 C7 46 20 00 00 00 00 EB 0E 83 F8 0A 7C 05 C6 44 24 10 01 40 89 46 20";
// The constructor publishing the inline object.
inline constexpr const char* kCaretSlotPattern = "A1 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? 68 24 22 00 00 89 46 1C";
inline constexpr int kCaretSlotOffset = 7;          // imm32 of `mov [slot],esi`
inline constexpr uint32_t kCaretCounterOffset = 0x20;
inline constexpr int kCaretPeriod = 26;              // draws
inline constexpr uintptr_t kCaretDrawReach = 0x200;  // the timer block lies this close after the draw's start

struct CaretSite {
    uintptr_t timer = 0;   // in the object's draw method
    uintptr_t slot = 0;    // the client's pointer to the inline object
    bool ok() const { return timer && slot; }
};

inline CaretSite resolveCaret(const uint8_t* data, size_t size, uintptr_t base) {
    const auto timers = findAll(data, size, parsePattern(kCaretTimerPattern), 2);
    const auto slots = findAll(data, size, parsePattern(kCaretSlotPattern), 2);
    if (timers.size() != 1 || slots.size() != 1 || !base) return {};
    uint32_t slot = 0;
    std::memcpy(&slot, data + slots[0] + kCaretSlotOffset, 4);
    if (slot < base || uint64_t(slot) - base + 4 > size) return {};
    return CaretSite{base + timers[0], slot};
}

// Drive the blink counter at 30 Hz; realTicks counts native frames.
struct CaretClock {
    double carry = 0.0;
    int phase = 0;
    bool started = false;
};
inline int phaseAdvance(CaretClock& c, int64_t clientCounter, double realTicks, int period) {
    if (!c.started) {                    // take over from the client's counter
        c.started = true;
        c.phase = (clientCounter >= 0 && clientCounter < period) ? int(clientCounter) : 0;
        c.carry = 0.0;
        return c.phase;
    }
    if (realTicks > 0.0) c.carry += realTicks;
    const double whole = std::floor(c.carry + 1e-9);
    c.carry = c.carry - whole > 0.0 ? c.carry - whole : 0.0;
    if (whole > 0.0) c.phase = int((uint64_t(c.phase) + uint64_t(whole)) % uint64_t(period));
    return c.phase;
}
inline int caretAdvance(CaretClock& c, int clientCounter, double realTicks) { return phaseAdvance(c, clientCounter, realTicks, kCaretPeriod); }
// Pace a frame-based countdown at perSecond. Only raise it; an upward jump resets the clock.
struct PacedCountdown {
    int32_t last = 0;
    int32_t start = 0;
    double elapsed = 0.0;
    bool active = false;
};
inline bool pacedCountdown(PacedCountdown& c, int32_t value, double frameSeconds, double perSecond, int32_t* write) {
    if (value <= 0) { c.active = false; c.last = value; return false; }
    if (!c.active || value > c.last) { c.active = true; c.start = value; c.elapsed = 0.0; c.last = value; return false; }
    c.elapsed += frameSeconds > 0.0 ? frameSeconds : 0.0;
    const double allowed = std::ceil(double(c.start) - c.elapsed * perSecond - 1e-9);   // a clock exactly on a count is that count
    if (allowed >= 1.0 && double(value) < allowed) {
        *write = int32_t(allowed);
        c.last = *write;
        return true;
    }
    c.last = value;
    return false;
}

// Camera collision refresh: set to 60 at 0x01E2B0, checked at 0x0207A9, decremented here.
// A positive counter forces collision checks.
inline constexpr const char* kCameraGracePattern = "A1 ?? ?? ?? ?? 85 C0 7E 06 48 A3 ?? ?? ?? ?? D9 47 48 D8 67 54";
inline uintptr_t resolveCameraGrace(const uint8_t* data, size_t size, uintptr_t base) {
    const auto hits = findAll(data, size, parsePattern(kCameraGracePattern), 2);
    if (hits.size() != 1 || !base) return 0;
    uint32_t read = 0, write = 0;
    std::memcpy(&read, data + hits[0] + 1, 4);
    std::memcpy(&write, data + hits[0] + 11, 4);
    if (read != write || read < base || uint64_t(read) - base + 4 > size) return 0;
    // the setter must name the same counter, with 60
    uint8_t setter[10] = {0xC7, 0x05, 0, 0, 0, 0, 0x3C, 0, 0, 0};
    std::memcpy(setter + 2, &read, 4);
    const std::vector<int> pat(setter, setter + 10);
    if (findAll(data, size, pat, 2).size() != 1) return 0;
    return read;
}

// FAQ and character-name cursors use sprite (c >> 2) % 6: a 24-draw cycle.
// Write only while the slot holds the expected window class.
inline constexpr int kMenuCursorPeriod = 24;
struct MenuCursorSpec {
    const char* name;          // the registry name, 8 characters, space padded
    const char* counterCode;   // the draw's counter block
    uint32_t offset;           // the counter's field in the window object
    uint8_t width;             // 2 (a WORD taken mod 24) or 4 (a DWORD that just counts)
};
inline constexpr MenuCursorSpec kMenuCursors[] = {
    {"faqsub  ", "66 8B 45 2C C1 E8 02 99 F7 FE 8B 01 8B 0C 90 E8 ?? ?? ?? ?? 66 8B 45 2C B9 18 00 00 00 66 40 5F 25 FF FF 00 00 5E 99 F7 F9 66 89 55 2C", 0x2C, 2},
    {"faqmain ", "66 8B 86 C2 02 00 00 03 D3 B9 06 00 00 00 C1 E8 02 52 99 F7 F9 8B 07 8B 0C 90 E8 ?? ?? ?? ?? 66 8B 86 C2 02 00 00 B9 18 00 00 00 66 40 5F 25 FF FF 00 00 99 F7 F9 66 89 96 C2 02 00 00", 0x2C2, 2},
    {"hnhead  ", "8B 46 1C 83 C2 06 BF 06 00 00 00 52 33 D2 C1 E8 02 F7 F7 8B 01 8B 0C 90 E8 ?? ?? ?? ?? FF 46 1C", 0x1C, 4},
    {"hnnaming", "8B 46 2C BF 06 00 00 00 99 83 E2 03 8B 09 03 C2 C1 F8 02 99 F7 FF 8B 0C 91 E8 ?? ?? ?? ?? 8B 46 2C 5F 40 89 46 2C", 0x2C, 4},
};
inline constexpr size_t kMenuCursorCount = sizeof(kMenuCursors) / sizeof(kMenuCursors[0]);

struct MenuCursorSite {
    uintptr_t slot = 0;      // the window's global slot
    uint32_t vtable = 0;     // the class constructed into it
    uint32_t offset = 0;
    uint8_t width = 0;
    bool ok() const { return slot && vtable && width; }
};

inline MenuCursorSite resolveMenuCursor(const uint8_t* data, size_t size, uintptr_t base, const MenuCursorSpec& spec) {
    if (!base || findAll(data, size, parsePattern(spec.counterCode), 2).size() != 1) return {};
    const auto inImage = [&](uint32_t va, size_t bytes) { return va >= base && uint64_t(va) - base + bytes <= size; };
    const auto u32 = [&](size_t off) { uint32_t v = 0; std::memcpy(&v, data + off, 4); return v; };
    // the registry record: "menu    " + name, 16 zero bytes, the global slot at +0x20
    std::vector<int> record;
    for (const char c : std::string("menu    ") + spec.name) record.push_back(uint8_t(c));
    for (int k = 0; k < 16; k++) record.push_back(0);
    const auto records = findAll(data, size, record, 2);
    if (records.size() != 1 || records[0] + 0x24 > size) return {};
    const uint32_t slot = u32(records[0] + 0x20);
    if (!inImage(slot, 4)) return {};
    // the one construction site: `call ctor; mov [slot],eax` or `mov dword [esi],vtable; call init; mov [slot],esi`
    std::vector<int> storeEax = {0xA3}, storeEsi = {0x89, 0x35};
    for (int k = 0; k < 4; k++) { storeEax.push_back(int((slot >> (8 * k)) & 0xFF)); storeEsi.push_back(int((slot >> (8 * k)) & 0xFF)); }
    uint32_t vtable = 0;
    int sites = 0;
    for (const size_t at : findAll(data, size, storeEax, 64)) {
        if (at < 5 || data[at - 5] != 0xE8) continue;
        ++sites;
        const uint32_t ctor = uint32_t(base + at) + int32_t(u32(at - 4));
        if (!inImage(ctor, 0x200)) return {};
        for (size_t i = ctor - base; i + 6 <= ctor - base + 0x200 && data[i] != 0xC3; i++)
            if (data[i] == 0xC7 && data[i + 1] == 0x06) vtable = u32(i + 2);
    }
    for (const size_t at : findAll(data, size, storeEsi, 64)) {
        if (at < 11 || data[at - 11] != 0xC7 || data[at - 10] != 0x06 || data[at - 5] != 0xE8) continue;
        ++sites;
        vtable = u32(at - 9);
    }
    if (sites != 1 || !inImage(vtable, 0x14) || !inImage(u32(vtable - base + 0x10), 1)) return {};
    return MenuCursorSite{slot, vtable, spec.offset, spec.width};
}

// One window's clock, restarted when the slot holds a different object.
struct MenuCursorClock {
    uint32_t object = 0;
    CaretClock clock;
};

} // namespace truefps
