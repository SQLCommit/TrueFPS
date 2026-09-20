// Character settings, logs and captures. Paths are relative to the Ashita root.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstring>

namespace truefps {

struct Identity {
    std::string name;
    uint32_t serverId = 0;
    bool known() const { return !name.empty(); }
    bool operator==(const Identity& o) const { return name == o.name && serverId == o.serverId; }
    bool operator!=(const Identity& o) const { return !(*this == o); }
};

// Debounce empty identity reads during zoning; accept a new character immediately.
inline constexpr int kEmptyPollsForLogout = 10;
inline bool identityGone(int emptyPolls) { return emptyPolls >= kEmptyPollsForLogout; }
// Packet identity survives empty reads until logout or a one-minute timeout.
inline constexpr int kEmptyPollsForDisconnect = 60;
inline bool connectionLost(int emptyPolls) { return emptyPolls >= kEmptyPollsForDisconnect; }
// Zone-in packet 0x000A: server id at +0x04, name (16 bytes, NUL-padded) at +0x84. Zone-out packet 0x000B, +0x04:
// 0 nothing, 1 logout to character select, 2 zone change, 3 Mog House, 4 cancelled logout, 5-9 other retail exits.
inline constexpr size_t kZoneInMinSize = 0x94;
inline bool identityFromZoneIn(const uint8_t* data, uint32_t size, Identity& out) {
    if (!data || size < kZoneInMinSize) return false;
    uint32_t serverId = 0;
    std::memcpy(&serverId, data + 0x04, 4);
    char name[17] = {};
    std::memcpy(name, data + 0x84, 16);
    if (!serverId || !name[0]) return false;
    out = Identity{name, serverId};
    return true;
}
inline bool logoutFromZoneOut(const uint8_t* data, uint32_t size) { return data && size >= 5 && data[4] != 0 && data[4] != 2 && data[4] != 3 && data[4] != 4; }

inline std::string characterKey(const Identity& id) {
    if (!id.known()) return std::string();
    std::string key;
    for (unsigned char c : id.name) key += (isalnum(c) || c == '_' || c == '-') ? char(c) : '_';
    return key + "_" + std::to_string(id.serverId);
}

// Resolve the Ashita root above the plugins directory.
inline std::string ashitaRoot(const std::string& dllPath) {
    const size_t slash = dllPath.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    const std::string dir = dllPath.substr(0, slash + 1);
    const size_t up = dir.size() > 1 ? dir.find_last_of("\\/", dir.size() - 2) : std::string::npos;
    return up == std::string::npos ? dir : dir.substr(0, up + 1);
}
inline std::string configDir(const std::string& root) { return root + "config\\truefps\\"; }
inline std::string characterConfigDir(const std::string& root, const std::string& key) { return configDir(root) + key + "\\"; }
inline std::string logsDir(const std::string& root) { return root + "logs\\truefps\\"; }
inline std::string characterLogDir(const std::string& root, const std::string& key) { return logsDir(root) + key + "\\"; }
inline std::string settingsPath(const std::string& root, const Identity& id) {
    return id.known() ? characterConfigDir(root, characterKey(id)) + "truefps.ini" : configDir(root) + "truefps.ini";
}
// Use character settings when logged in; otherwise prefer shared settings over the legacy file.
inline std::string settingsSource(const std::string& root, const Identity& id, const std::string& legacyPath) {
    const std::string wanted = settingsPath(root, id);
    if (id.known()) return wanted;
    if (GetFileAttributesA(wanted.c_str()) != INVALID_FILE_ATTRIBUTES) return wanted;
    return GetFileAttributesA(legacyPath.c_str()) != INVALID_FILE_ATTRIBUTES ? legacyPath : wanted;
}
inline std::string folderOf(const std::string& path) { return path.substr(0, path.find_last_of("\\/") + 1); }
inline void ensureDirs(const std::string& root, const std::string& dir) {
    for (size_t i = root.size(); i < dir.size(); i++)
        if (dir[i] == '\\' || dir[i] == '/') CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
}
inline std::string characterLogPath(const std::string& root, const std::string& key) { return characterLogDir(root, key) + "truefps.log"; }

// Disambiguate captures saved in the same second with -2, -3, etc.
inline std::string captureFileName(const SYSTEMTIME& t, int copy = 0) {
    char name[48];
    if (copy > 0) _snprintf_s(name, sizeof name, _TRUNCATE, "frames_%04u%02u%02u-%02u%02u%02u-%d.csv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, copy + 1);
    else _snprintf_s(name, sizeof name, _TRUNCATE, "frames_%04u%02u%02u-%02u%02u%02u.csv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return name;
}

// CSV capture metadata.
struct CaptureInfo {
    std::string character;   // empty: not logged in
    uint32_t serverId = 0;
    unsigned pluginBuild = 0, clientBuild = 0;
    std::string version, rate, background, smooth, cutscene, display;
    size_t frames = 0;
    double seconds = 0.0;
    std::string ashitaInterface;
};
inline std::string captureHeaderLines(const CaptureInfo& c, const SYSTEMTIME& t) {
    char line[256];
    std::string out;
    _snprintf_s(line, sizeof line, _TRUNCATE, "# TrueFPS %s build %08X, Ashita interface %s, client build %08X\n", c.version.c_str(), c.pluginBuild, c.ashitaInterface.c_str(), c.clientBuild);
    out += line;
    if (c.character.empty()) _snprintf_s(line, sizeof line, _TRUNCATE, "# not logged in, saved %04u-%02u-%02u %02u:%02u:%02u\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    else _snprintf_s(line, sizeof line, _TRUNCATE, "# character %s (id %u), saved %04u-%02u-%02u %02u:%02u:%02u\n", c.character.c_str(), c.serverId, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    out += line;
    _snprintf_s(line, sizeof line, _TRUNCATE, "# settings: frame rate %s, background %s, smooth %s, cutscene %s\n", c.rate.c_str(), c.background.c_str(), c.smooth.c_str(), c.cutscene.c_str());
    out += line;
    _snprintf_s(line, sizeof line, _TRUNCATE, "# display %s; %zu frames over %.1f s\n", c.display.c_str(), c.frames, c.seconds);
    out += line;
    return out;
}

}  // namespace truefps
