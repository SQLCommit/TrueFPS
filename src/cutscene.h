// Local event lifetime, read only.
#pragma once
#include "limiter.h"

namespace truefps {

// The action gate reads this byte; final event cleanup clears it. Both sequences must name the same byte.
inline constexpr const char* kEventActivePattern =
    "A0 ?? ?? ?? ?? 84 C0 74 03 B0 01 C3 66 83 3D ?? ?? ?? ?? 00";
inline constexpr const char* kEventCleanupPattern =
    "C6 05 ?? ?? ?? ?? 00 C6 05 ?? ?? ?? ?? 00 C6 05 ?? ?? ?? ?? 00 66 89 3D ?? ?? ?? ??";

inline uintptr_t resolveEventActive(const uint8_t* data, size_t size, uintptr_t base) {
    const auto readers = findAll(data, size, parsePattern(kEventActivePattern));
    const auto cleanup = findAll(data, size, parsePattern(kEventCleanupPattern));
    if (readers.size() != 1 || cleanup.size() != 1) return 0;
    uint32_t slot = 0, cleared = 0;
    std::memcpy(&slot, data + readers[0] + 1, 4);
    std::memcpy(&cleared, data + cleanup[0] + 16, 4);
    if (!base || slot != cleared || slot < base || uint64_t(slot) - base >= size) return 0;
    return slot;
}

// Apply target exclusions on entry; an untargeted event is valid.
inline bool cutsceneTargetAllowed(const char* name, const std::vector<std::string>& excluded) {
    return !(name && *name) || !nameExcluded(name, excluded);
}

// Recheck local event activity every frame; target exclusions stay latched.
inline bool cutsceneActive(bool wasActive, bool serverEvent, bool localEvent, bool allowedTarget) {
    return serverEvent && localEvent && (wasActive || allowedTarget);
}

} // namespace truefps
