// TrueFPS frame pacing and UI. One game tick is 1/60 s; native 30 FPS advances two ticks per frame.
#include "Ashita.h"
#include "smooth.h"
#include "watchdog.h"
#include "caret.h"
#include "cutscene.h"
#include "plugin_log.h"
#include "persist.h"
#include "profile.h"
#include "ui_layout.h"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <memory>
#include <psapi.h>
#include <set>

namespace {

constexpr double pluginVersion = 1.1;
constexpr const char* kFontAlias = "__truefps_overlay";

IAshitaCore* core = nullptr;
plog::FileLog fileLog;      // per-character log
truefps::DiskWriter disk;   // settings and CSV writer
std::string root;           // the Ashita folder (above plugins\)
std::string ownDir;         // capture folder: logs\truefps\ before login, the character's after it
plog::Run run;              // the run tag on the session and unload lines
truefps::Identity identity; // the character logged in now (empty before login and at character select)
int emptyPolls = 0;         // consecutive empty identity polls

truefps::Module client;

uintptr_t loopSite = 0;          // the limiter loop's `mov ebp, [imm32]` instruction
uint32_t originalSleepImm = 0;   // its original imm32 (the Sleep IAT slot)
bool codeInstalled = false;

truefps::JumpSite stepSites[2];
// Pace the connection timeout counter to preserve a roughly 120-second wait.
truefps::WatchdogSite watchdogSite;
truefps::WatchdogClock watchdogClock;
bool watchdogResolved = false;
bool watchdogWaiting = false;
double watchdogLastWait = 0.0;      // seconds the last wait lasted
int64_t watchdogLastWaitAt = 0;     // qpc when it ended (shown for kZoneWaitShownSeconds)
constexpr double kZoneWaitShownSeconds = 30.0;
// Pace the chat caret at native 30 FPS.
truefps::CaretSite caretSite;
truefps::CaretClock caretClock;
bool caretResolved = false;
truefps::MenuCursorSite menuCursorSites[truefps::kMenuCursorCount];
truefps::MenuCursorClock menuCursorClocks[truefps::kMenuCursorCount];
// Keep the 60-frame camera collision countdown at two seconds.
uintptr_t cameraGraceSlot = 0;
truefps::PacedCountdown cameraGrace;
// Preserve the 30-second queue-flush and socket-task timeouts.
bool shutdownWaitResolved = false;
truefps::PacedCountdown shutdownWait;
bool stepResolved = false;
bool stepStuck = false;          // a jump could not be removed: no more tries this session
bool stepFound = false;          // resolve found both step accessors (stepResolved can go false later if patching fails)
bool pinned = false;
DWORD drawThread = 0;            // the thread Ashita draws on (Direct3DPresent)
bool callbackThreadSaid = false; // the one-time note when a callback runs off the drawing thread
void logLine(Ashita::LogLevel level, const char* text);
std::string routineNames(const std::vector<const char*>& names);   // "a, b and c"
std::string logPathText();
// Ashita 4.30 runs queued commands on its own worker threads, but PluginManager enters one critical section around
// HandleCommand, HandleIncomingPacket and Direct3D_Present (Ashita.dll 0x10182DBD, 0x101847F5, 0x101869F0), so a
// command or packet callback never runs while the Present callback does. The first callback seen on another thread is
// a log fact, said once.
void noteCallbackThread(const char* what) {
    if (callbackThreadSaid || !drawThread) return;
    const DWORD here = GetCurrentThreadId();
    if (here == drawThread) return;
    callbackThreadSaid = true;
    char line[200];
    _snprintf_s(line, sizeof line, _TRUNCATE, "%s callback on thread %lu, not the drawing thread %lu (Ashita runs it under the same lock as the frame)", what,
                static_cast<unsigned long>(here), static_cast<unsigned long>(drawThread));
    logLine(Ashita::LogLevel::Info, line);
}
uintptr_t lastLoggedOffThreadRet = 0;
long lastLoggedOffThreadTid = 0;
int offThreadLogs = 0;

// Settings (truefps.ini).
int frameRate = truefps::kRateMax;   // the frame rate: kRateMax (the monitor's), kRateUncapped, or 5-1000 fps
bool smoothOn = true;
bool overlay = true;
int overlayX = 1, overlayY = 1, overlaySize = 12;
uint32_t overlayColor = 0xFFFF0000;
bool overlayBands = false;      // the counter's colour follows the frame rate instead of overlayColor
int bandHigh = 60, bandLow = 30;   // the two frame rates it changes colour at
uint32_t bandColorHigh = 0xFF40D040, bandColorMid = 0xFF4090FF, bandColorLow = 0xFFFF4040;
int cutsceneSpeed = truefps::kCutsceneSpeedDefault;   // 1 = off
std::vector<std::string> cutsceneExclude;
truefps::FrameLog frameLog;         // /truefps frames
int64_t quietFrom = 0, lastLongFrameAt = 0;   // the long-frame warning: load or login time, and the last warning
unsigned longFramesUnsaid = 0;                 // long frames inside the minute after a warning, counted into the next
bool prevFrameBackground = false;
int64_t framePrevAt = 0, framePrevPaceEnd = 0;
uint64_t framePrevWait = 0, framePrevLate = 0, framePrevLimiterWait = 0, framePrevCycles = 0, framePrevPaceEndCycles = 0, framePrevIo = 0;
// Packet identity handoff: the packet callback writes; Present consumes.
truefps::Identity packetIdentity;          // its thread only
char packetName[17] = {};                  // the handoff: written before the flag, read after it
uint32_t packetServerId = 0;
volatile long packetIdentityPending = 0;   // 1: a login/zone-in waits in packetName/packetServerId; 2: a logout waits
volatile long packetForget = 0;            // the game thread asks the callback to drop its record
bool packetKnownOnGame = false;
uint32_t frameIndex = 0;      // frames recorded this session, for the sampled telemetry
bool sampledFrame = true;     // this frame reads the sampled telemetry (recordFrame decides)
double cyclesPerMs = 0.0;           // the game thread's CPU cycles per millisecond (measured at load)
double lastWorstMs = 0.0, secondWorstMs = 0.0;
bool overlayDetail = false;         // the overlay adds the worst frame of each second
bool overlayLocked = false;
int backgroundFps = 0;           // while another window is in front: kBackgroundOff, kBackgroundUncapped, kRateMax, or 5-1000

truefps::LimitState limitState;
uint64_t holdCorrections = 0;
bool resolveFailed = false;      // the client code was not found: truefps does nothing this session
bool refusedForGood = false;     // the precise limiter could not be installed: it stays off this session
bool orphanedTimer = false;      // the client replaced a timer that still carries the copy: never unmap
std::string pendingNotice;
unsigned char pendingColor = 0x6A;
std::atomic<bool> zoning{false};
bool inCutscene = false;
uintptr_t eventActiveSlot = 0;     // the client's local event flag; read only
bool eventReadFailed = false;
uintptr_t menuFocusSlot = 0;        // the menu manager's focused window pointer (0: not found)
bool menuPaused = false;            // a cutscene menu has focus: the speed-up holds at 1x
std::string focusedMenu;
std::set<std::string> menusSeen;      // each menu's first focus in a cutscene is logged once a session
std::string pausedMenu;               // the menu the speed-up is paused for, "" when not paused
int64_t pausedSince = 0;              // qpc when that pause began
std::vector<std::string> cutsceneMenus = truefps::kCutsceneMenusDefault;
bool smoothActive = false;       // the game uncapped, paced to pacedFps
bool smoothLoggedOn = false;     // last logged state; a failed movement write can temporarily clear smoothActive
int pacedFps = 0;                // smooth mode's rate this frame before the background cap (0: uncapped)
int64_t paceLast = 0;
truefps::CallSite moveSite;      // player movement's read of app+0x2C (smooth mode puts it on the camera's clock)
bool moveResolved = false;
bool moveStuck = false;          // the movement site was found but cannot be patched: no more tries this session
int moveWriteFails = 0;          // writes that failed in a row with the site's own bytes still there (a run is given up on)
constexpr int kMoveWriteTries = 10;
bool moveLogged = false;         // the install failure, said once
bool moveRemoveLogged = false;   // the remove failure, said once: a different failure from the one above
bool smoothFallbackSaid = false;  // the "smooth mode unavailable" line has been said in chat
truefps::SmoothSites smooth;     // smooth mode's code patches, by routine (smooth.h)
size_t smoothFound = 0;          // routines whose code was found on this client build
// A site another tool owns is left exactly as that tool set it, at resolve or when it takes the operand over later,
// and is taken back when that tool puts the client's value back. The routine stays smooth either way, so each change
// is a log fact, said once, and never a chat line.
bool siteNeutralSaid[truefps::kSiteCount] = {};
uint32_t neutralSitesSaid = 0;
void logNeutralSites() {
    if (truefps::g_truefpsNeutralChanges == neutralSitesSaid) return;
    neutralSitesSaid = truefps::g_truefpsNeutralChanges;
    for (size_t i = 0; i < truefps::kSiteCount; i++) {
        const bool neutral = smooth.sites[i].neutral;
        if (neutral == siteNeutralSaid[i]) continue;
        siteNeutralSaid[i] = neutral;
        char line[200];
        if (neutral)
            _snprintf_s(line, sizeof line, _TRUNCATE, "smooth mode: %s: left as another tool set it (%.1f)", truefps::kSites[i].name,
                        double(truefps::floatFromBits(truefps::kSites[i].neutralBits)));
        else if (smooth.sites[i].patched)
            _snprintf_s(line, sizeof line, _TRUNCATE, "smooth mode: %s: taken back", truefps::kSites[i].name);
        else   // an adopted site handed back while its routine was not running, or at unload: restored, not taken back
            _snprintf_s(line, sizeof line, _TRUNCATE, "smooth mode: %s: the other tool handed it back, and it is restored", truefps::kSites[i].name);
        logLine(Ashita::LogLevel::Info, line);
    }
}
// The factor copies the fast paths read are re-taken once a second (refreshConstantCopies). A constant another tool
// retuned is a log fact, said once per change: the routine keeps running, so it is never a chat line.
uint32_t siteConstantSaid[truefps::kSiteCount] = {};
float springFarSaid = 0.0f;   // the eye loop's far-distance threshold, as last said (resolve's value is not a change)
uint32_t constantChangesSaid = 0;
void logConstantCopies() {
    if (truefps::g_truefpsConstantChanges == constantChangesSaid) return;
    constantChangesSaid = truefps::g_truefpsConstantChanges;
    for (size_t i = 0; i < truefps::kSiteCount; i++) {
        if (!truefps::kSites[i].constantCopy && !truefps::kSites[i].contextBits) continue;
        if (smooth.floatBits[i] == siteConstantSaid[i]) continue;
        siteConstantSaid[i] = smooth.floatBits[i];
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE, "smooth mode: %s: the game's own %s is now %g; TrueFPS's copy follows it", truefps::kSites[i].name,
                    truefps::kSites[i].contextBits ? "factor at the call" : "constant", double(truefps::floatFromBits(smooth.floatBits[i])));
        logLine(Ashita::LogLevel::Info, line);
    }
    if (truefps::bitsOf(smooth.springFar) != truefps::bitsOf(springFarSaid)) {
        springFarSaid = smooth.springFar;
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE, "smooth mode: the camera eye loop's far-distance threshold is now %g; TrueFPS's copy follows it", double(smooth.springFar));
        logLine(Ashita::LogLevel::Info, line);
    }
}
// A recorded operand that could no longer be written back: TrueFPS's own slot is left in the game's code instead of
// a dead address. Removal is retried every frame while the site is still in, so this is latched per site, as the
// neutral-site lines are; the unload's own report says the changes are not all undone.
bool siteUnsafeSaid[truefps::kSiteCount] = {};
uint32_t unsafeOriginalsSaid = 0;
void logUnsafeOperands() {
    if (truefps::g_truefpsUnsafeOriginals == unsafeOriginalsSaid) return;
    unsafeOriginalsSaid = truefps::g_truefpsUnsafeOriginals;
    for (size_t i = 0; i < truefps::kSiteCount; i++) {
        if (!smooth.sites[i].unsafeOriginal || siteUnsafeSaid[i]) continue;
        siteUnsafeSaid[i] = true;
        char line[320];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "smooth mode: %s: the copy of the game's constant another tool had pointed this operand at no longer holds that value, so TrueFPS left its own slot in the game's code rather than write an address that may be gone; TrueFPS stays in memory until the game closes",
                    truefps::kSites[i].name);
        logLine(Ashita::LogLevel::Warn, line);
    }
}
void logSiteChanges() {
    logNeutralSites();
    logConstantCopies();
    logUnsafeOperands();
}
IFontObject* font = nullptr;
bool panelOpen = false;             // /truefps, /truefps ui
truefps::PanelSizing panelSizing;
float panelScale = 1.0f;            // [gui] scale, 1.0-2.0

bool inBackground = false;
int64_t sceneFirstBegin = 0, sceneLastEnd = 0;   // this frame's first BeginScene after the timer reset and its last EndScene
uint16_t sceneCount = 0;
int appliedLimit = -1;           // the limit applyLimit last aimed for

int64_t windowStart = 0;
uint64_t windowFrames = 0;
double lastFps = 0.0, lastSpinMs = 0.0;
uint64_t spinStartCounts = 0;

std::string resolvedLine;              // the load's "build ... resolved" line, for /truefps diag
std::string* chatCapture = nullptr;   // /truefps diag: what would go to chat goes into the report instead
// Strip chat colour tokens from log and diagnostic output.
constexpr char kHighlightOn = '\x11', kHighlightOff = '\x12';
constexpr unsigned char kColorCommand = 0x02;
void chat(const char* text, unsigned char color = 0x6A) {
    if (!text) return;
    constexpr size_t kBodyMax = 489;   // what the chat line below carries: built to fit, so a colour escape is never cut in half
    char body[512];   // each marker becomes a two-byte colour escape
    size_t w = 0;
    for (size_t r = 0; text[r] != '\0' && w < kBodyMax; r++) {
        if (text[r] == kHighlightOn || text[r] == kHighlightOff) {
            if (w + 2 > kBodyMax) break;
            body[w++] = '\x1E';
            body[w++] = char(text[r] == kHighlightOn ? kColorCommand : color);
        } else body[w++] = text[r];
    }
    body[w] = '\0';
    char plain[512];
    size_t p = 0;
    for (size_t r = 0; text[r] != '\0' && p + 1 < sizeof plain; r++)
        if (text[r] != kHighlightOn && text[r] != kHighlightOff) plain[p++] = text[r];
    plain[p] = '\0';
    if (chatCapture) { *chatCapture += "      "; *chatCapture += plain; *chatCapture += '\n'; return; }
    if (!core || !core->GetChatManager()) return;
    char buf[512];
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "\x1E\x51" "[" "\x1E\x06" "TrueFPS" "\x1E\x51" "]" "\x1E\x01" " " "\x1E" "%c%.489s" "\x1E\x01", color, body);   // the closing colour always fits
    core->GetChatManager()->AddChatMessage(1, false, buf);
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "chat: %s", plain);
    fileLog.write(color == 0x44 ? "error" : color == 0x68 ? "warn" : "info", buf);
}
void logLine(Ashita::LogLevel level, const char* text) {
    fileLog.write(level == Ashita::LogLevel::Error ? "error" : level == Ashita::LogLevel::Warn ? "warn" : level == Ashita::LogLevel::Debug ? "debug" : "info", text);
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = char(tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string dllPath() {
    static std::string cached;
    if (!cached.empty()) return cached;
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&dllPath), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    cached = path;
    return cached;
}
std::string legacyIniPath() {
    const std::string p = dllPath();
    const auto slash = p.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "truefps.ini";
}
void ensureDir(const std::string& dir) { truefps::ensureDirs(root, dir); }
std::string underRoot(const std::string& path) {
    return (!root.empty() && path.compare(0, root.size(), root) == 0) ? path.substr(root.size()) : path;
}
std::string logPathText() { return underRoot(fileLog.path()) + (fileLog.atStartupFile() ? " (it moves into your character's log at login)" : ""); }
std::string cutsceneFlagText() { return eventActiveSlot ? "TrueFPS cannot read the cutscene flag right now, so the speed-up is off." : "TrueFPS cannot tell when a cutscene is playing on this client build, so the speed-up is off."; }
// Read character/shared settings, falling back to the legacy file before login. Save to settingsPath().
bool legacySettingsSaid = false;   // the "read from the old truefps.ini" line, once a session
std::string activeSettingsPath() {
    const std::string source = truefps::settingsSource(root, identity, legacyIniPath());
    if (!legacySettingsSaid && source == legacyIniPath()) {
        legacySettingsSaid = true;
        logLine(Ashita::LogLevel::Info, ("settings: read from the old " + underRoot(source) + "; every save from now goes to " +
                                         underRoot(truefps::settingsPath(root, identity)) + " (the old file is left where it is)").c_str());
    }
    return source;
}

// Remove obsolete keys on save. Migrate limit/fps only when rate is absent.
struct IniKey { const char* section; const char* key; };
constexpr IniKey kRetiredKeys[] = {
    {"truefps", "enabled"}, {"truefps", "hold"}, {"smooth", "spread"}, {"smooth", "visibility"},
    {"truefps", "limit"}, {"truefps", "watchdog_hold"}, {"truefps", "zoning_safety"}, {"truefps", "caret_pace"},
    {"smooth", "fps"}, {"smooth", "last"}, {"smooth", "twomotion"}, {"smooth", "head"}, {"smooth", "trails"},
    {"smooth", "events"}, {"smooth", "effects"}, {"smooth", "blend"}, {"smooth", "windows"}, {"smooth", "ships"},
    {"smooth", "shake"}, {"smooth", "shadows"}, {"smooth", "chatscroll"}, {"smooth", "chatheight"}, {"smooth", "native"},
    {"overlay", "colour_high"}, {"overlay", "colour_mid"}, {"overlay", "colour_low"},   // the band colours' first spelling
};
std::vector<const IniKey*> retiredPresent;

struct SettingsSet {
    int frameRate = truefps::kRateMax;
    bool smoothOn = true, overlay = true, overlayDetail = false, overlayLocked = false, overlayBands = false;
    int overlayX = 1, overlayY = 1, overlaySize = 12;
    int bandHigh = 60, bandLow = 30;
    uint32_t overlayColor = 0xFFFF0000;
    uint32_t bandColorHigh = 0xFF40D040, bandColorMid = 0xFF4090FF, bandColorLow = 0xFFFF4040;
    float panelScale = 1.0f;
    int cutsceneSpeed = truefps::kCutsceneSpeedDefault;
    std::vector<std::string> cutsceneExclude, cutsceneMenus = truefps::kCutsceneMenusDefault;
    int backgroundFps = truefps::kBackgroundOff;
    std::vector<const IniKey*> retiredPresent;
};
SettingsSet defaultSettings() { return SettingsSet{}; }
void applySettingsSet(const SettingsSet& s) {
    // Copy lists before scalars so allocation failure cannot mix characters' settings.
    std::vector<std::string> exclude = s.cutsceneExclude, menus = s.cutsceneMenus;
    std::vector<const IniKey*> retired = s.retiredPresent;
    frameRate = s.frameRate;
    smoothOn = s.smoothOn;
    overlay = s.overlay;
    overlayX = s.overlayX;
    overlayY = s.overlayY;
    overlaySize = s.overlaySize;
    overlayColor = s.overlayColor;
    overlayDetail = s.overlayDetail;
    overlayLocked = s.overlayLocked;
    overlayBands = s.overlayBands;
    bandHigh = s.bandHigh;
    bandLow = s.bandLow;
    bandColorHigh = s.bandColorHigh;
    bandColorMid = s.bandColorMid;
    bandColorLow = s.bandColorLow;
    panelScale = s.panelScale;
    cutsceneSpeed = s.cutsceneSpeed;
    backgroundFps = s.backgroundFps;
    cutsceneExclude = std::move(exclude);
    cutsceneMenus = std::move(menus);
    retiredPresent = std::move(retired);
}
void resetSettingsToDefaults() { applySettingsSet(defaultSettings()); }

// Read settings without touching globals; a missing file preserves the supplied values.
void readSettingsInto(const std::string& path, bool scanRetired, SettingsSet& s) {
    const char* f = path.c_str();
    const int kAbsent = 0x7FFF0000;
    const int rate = int(GetPrivateProfileIntA("truefps", "rate", kAbsent, f));
    if (rate != kAbsent) {
        s.frameRate = truefps::rateValid(rate) ? rate : truefps::kRateMax;
        s.smoothOn = GetPrivateProfileIntA("smooth", "on", 1, f) != 0;
    } else {
        const int oldSmooth = int(GetPrivateProfileIntA("smooth", "fps", kAbsent, f));
        const int oldLimit = int(GetPrivateProfileIntA("truefps", "limit", 60, f));
        if (oldSmooth == kAbsent) { s.smoothOn = true; s.frameRate = truefps::kRateMax; }
        else if (truefps::normalizeSmoothFps(oldSmooth) != 0) { s.smoothOn = true; s.frameRate = truefps::normalizeSmoothFps(oldSmooth); }
        else { s.smoothOn = false; s.frameRate = truefps::divisorForLimit(oldLimit) >= 0 && truefps::rateValid(oldLimit) ? oldLimit : 60; }
    }
    s.overlay = GetPrivateProfileIntA("overlay", "visible", 1, f) != 0;
    s.overlayDetail = GetPrivateProfileIntA("overlay", "detail", 0, f) != 0;
    s.overlayLocked = GetPrivateProfileIntA("overlay", "locked", 0, f) != 0;
    {
        char scale[16] = {};
        GetPrivateProfileStringA("gui", "scale", "1.0", scale, sizeof scale, f);
        const float sc = float(atof(scale));
        s.panelScale = truefps::guiScaleValid(sc) ? sc : 1.0f;
    }
    s.overlayX = truefps::clampOverlayPos(int(GetPrivateProfileIntA("overlay", "x", 1, f)), 0);   // no window to ask here: the wide bound
    s.overlayY = truefps::clampOverlayPos(int(GetPrivateProfileIntA("overlay", "y", 1, f)), 0);
    s.overlaySize = GetPrivateProfileIntA("overlay", "size", 12, f);
    if (s.overlaySize < 6 || s.overlaySize > 72) s.overlaySize = 12;
    char buf[2048] = {};
    GetPrivateProfileStringA("overlay", "color", "FFFF0000", buf, sizeof buf, f);
    s.overlayColor = uint32_t(strtoul(buf, nullptr, 16));
    s.overlayBands = GetPrivateProfileIntA("overlay", "bands", 0, f) != 0;
    s.bandHigh = int(GetPrivateProfileIntA("overlay", "band_high", 60, f));
    s.bandLow = int(GetPrivateProfileIntA("overlay", "band_low", 30, f));
    truefps::orderBands(s.bandHigh, s.bandLow, true);   // a hand-edited file with the two the wrong way round
    // Migrate legacy colour_* keys when color_* is absent; remove legacy keys on save.
    const auto readBandColour = [&](const char* key, const char* was, const char* fallback) {
        GetPrivateProfileStringA("overlay", key, "", buf, sizeof buf, f);
        if (!buf[0]) GetPrivateProfileStringA("overlay", was, fallback, buf, sizeof buf, f);
        return uint32_t(strtoul(buf, nullptr, 16));
    };
    s.bandColorHigh = readBandColour("color_high", "colour_high", "FF40D040");
    s.bandColorMid = readBandColour("color_mid", "colour_mid", "FF4090FF");
    s.bandColorLow = readBandColour("color_low", "colour_low", "FFFF4040");
    s.cutsceneSpeed = truefps::readCutsceneSpeed(f);
    truefps::readExcludeList(f, s.cutsceneExclude);
    truefps::readNameList(f, "pause_menus", s.cutsceneMenus);
    const int background = GetPrivateProfileIntA("truefps", "background", 0, f);
    s.backgroundFps = truefps::backgroundValid(background) ? background : truefps::kBackgroundOff;
    s.retiredPresent.clear();
    if (!scanRetired) return;
    for (const auto& k : kRetiredKeys)
        if (GetPrivateProfileStringA(k.section, k.key, "\x01", buf, sizeof buf, f) != 1 || buf[0] != '\x01')   // the default comes back only when the key is absent
            s.retiredPresent.push_back(&k);
}
void loadSettingsFrom(const std::string& path, bool scanRetired) {
    SettingsSet s = defaultSettings();
    readSettingsInto(path, scanRetired, s);
    applySettingsSet(s);
}
struct LoadedSettings { uint64_t seq = 0; std::string source; bool exists = true, legacy = false, failed = false; SettingsSet set; };
std::mutex loadedMutex;
std::shared_ptr<LoadedSettings> loadedReady;
uint64_t loadSeq = 0, appliedSeq = 0;   // the identity change a read belongs to; a late older read is dropped
bool settingsReadFailed = false;        // the read threw: the defaults are in use and nothing is saved for this character
bool settingsLoading() { return loadSeq != appliedSeq; }

// Do not save during a pending load: current values still belong to the previous character.
bool saveSettings() {
    if (settingsLoading() || settingsReadFailed) return false;
    truefps::IniSnapshot v;
    const auto put = [&](const char* section, const char* key, std::string value) { v.push_back(truefps::IniValue{section, key, std::move(value)}); };
    put("truefps", "rate", std::to_string(frameRate));
    put("truefps", "background", std::to_string(backgroundFps));
    put("overlay", "visible", overlay ? "1" : "0");
    put("overlay", "detail", overlayDetail ? "1" : "0");
    put("overlay", "locked", overlayLocked ? "1" : "0");
    char scale[16];
    _snprintf_s(scale, sizeof scale, _TRUNCATE, "%.2f", panelScale);
    put("gui", "scale", scale);
    put("overlay", "x", std::to_string(overlayX));
    put("overlay", "y", std::to_string(overlayY));
    put("overlay", "size", std::to_string(overlaySize));
    char color[16];
    _snprintf_s(color, sizeof color, _TRUNCATE, "%08X", overlayColor);
    put("overlay", "color", color);
    put("overlay", "bands", overlayBands ? "1" : "0");
    put("overlay", "band_high", std::to_string(bandHigh));
    put("overlay", "band_low", std::to_string(bandLow));
    _snprintf_s(color, sizeof color, _TRUNCATE, "%08X", bandColorHigh);
    put("overlay", "color_high", color);
    _snprintf_s(color, sizeof color, _TRUNCATE, "%08X", bandColorMid);
    put("overlay", "color_mid", color);
    _snprintf_s(color, sizeof color, _TRUNCATE, "%08X", bandColorLow);
    put("overlay", "color_low", color);
    put("cutscene", "speed", std::to_string(cutsceneSpeed));
    std::string all;
    for (const auto& e : cutsceneExclude) all += (all.empty() ? "" : "|") + e;
    put("cutscene", "exclude", all);
    all.clear();
    for (const auto& e : cutsceneMenus) all += (all.empty() ? "" : "|") + e;
    put("cutscene", "pause_menus", all);
    put("smooth", "on", smoothOn ? "1" : "0");
    // After `rate` is written, and repeated by every save this session. Deleting an absent key is harmless.
    for (const auto* k : retiredPresent) v.push_back(truefps::IniValue{k->section, k->key, "", true});
    const std::string file = truefps::settingsPath(root, identity);
    disk.saveSettings(file, std::move(v), underRoot(file));   // a notice names it the way every other line does
    return true;
}
// Include save failures in the result so the panel reports them in chat.
struct SavedLine {
    std::string text;
    bool saved = true;
};
SavedLine savedLine(std::string line, bool saved) {
    if (saved) return SavedLine{std::move(line), true};
    return SavedLine{truefps::lineWithNote(std::move(line), settingsReadFailed ? "not saved: the settings file could not be read"
                                                                               : "not saved: the settings are still loading, try again in a moment"),
                     false};
}
void ensureSettingsDir() {
    const std::string path = truefps::settingsPath(root, identity);
    ensureDir(path.substr(0, path.find_last_of("\\/") + 1));
}


unsigned diskBuildStamp() {
    FILE* f = _fsopen(dllPath().c_str(), "rb", _SH_DENYNO);
    if (!f) return 0;
    unsigned char head[0x400] = {};
    const size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    if (n < 0x40) return 0;
    uint32_t pe = 0;
    std::memcpy(&pe, head + 0x3C, 4);
    if (pe > n - 12 || std::memcmp(head + pe, "PE\0\0", 4) != 0) return 0;   // n is at least 0x40 here, so the subtraction cannot wrap; `pe + 12` could
    uint32_t stamp = 0;
    std::memcpy(&stamp, head + pe + 8, 4);
    return stamp;
}
unsigned buildStamp() { return plog::ownImageStamp(reinterpret_cast<const void*>(&buildStamp)); }
unsigned clientBuildStamp() { return plog::imageStamp(GetModuleHandleA("FFXiMain.dll")); }

bool findClient() {
    const HMODULE h = GetModuleHandleA("FFXiMain.dll");
    MODULEINFO info{};
    if (!h || !GetModuleInformation(GetCurrentProcess(), h, &info, sizeof info)) return false;
    client.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    client.size = info.SizeOfImage;
    return true;
}

bool resolve(std::string& why) {
    if (!findClient()) { why = "FFXiMain.dll is not loaded"; return false; }
    std::vector<uint8_t> image(client.size);
    size_t readable = 0;
    // Zero unreadable pages in the scan image.
    for (size_t off = 0; off < client.size; off += 0x1000) {
        const size_t n = (client.size - off < 0x1000) ? client.size - off : 0x1000;
        if (truefps::readRaw(client.base + off, image.data() + off, n)) readable += n;
    }
    if (readable == 0) { why = "the client image could not be read"; return false; }
    const auto loops = truefps::findAll(image.data(), image.size(), truefps::parsePattern(truefps::kLoopPattern));
    if (loops.size() != 1) { why = loops.empty() ? "the frame limiter code was not found (client update?)" : "the frame limiter code matched more than once"; return false; }
    const auto apps = truefps::findAll(image.data(), image.size(), truefps::parsePattern(truefps::kAppPattern));
    if (apps.size() != 1) { why = apps.empty() ? "the fps divisor code was not found (client update?)" : "the fps divisor code matched more than once"; return false; }

    loopSite = client.base + loops[0];
    std::memcpy(&originalSleepImm, image.data() + loops[0] + truefps::kLoopImmOffset, 4);
    uint32_t appSlot = 0;
    std::memcpy(&appSlot, image.data() + apps[0] + truefps::kAppImmOffset, 4);
    uintptr_t sleepFn = 0;
    if (!client.contains(originalSleepImm, 4) || !truefps::readValue(originalSleepImm, sleepFn) || !sleepFn) {
        why = "the frame limiter's Sleep slot is not valid"; return false;
    }
    if (!client.contains(appSlot, 4)) { why = "the app object pointer is outside the client image"; return false; }
    truefps::g.appSlot = appSlot;

    truefps::g_truefpsAppSlot = appSlot;
    const auto moves = truefps::findAll(image.data(), image.size(), truefps::parsePattern(truefps::kMovePattern));
    moveResolved = false;
    if (moves.size() == 1) {
        uint32_t imm = 0;
        std::memcpy(&imm, image.data() + moves[0] + truefps::kMoveImmOffset, 4);
        moveResolved = imm == appSlot;
        moveSite.at = client.base + moves[0];
        std::memcpy(moveSite.original, image.data() + moves[0], 5);
    }
    const auto steps = truefps::findAll(image.data(), image.size(), truefps::parsePattern(truefps::kStepPattern), 3);
    stepResolved = steps.size() == 2;
    stepFound = stepResolved;
    for (size_t i = 0; stepResolved && i < 2; i++) {
        uint32_t imm = 0;
        std::memcpy(&imm, image.data() + steps[i] + truefps::kStepImmOffset, 4);
        if (imm != appSlot) stepResolved = false;  // both accessors must read the same app object pointer
        stepSites[i].at = client.base + steps[i];
        std::memcpy(stepSites[i].original, image.data() + steps[i], 5);
        stepSites[i].pad = truefps::stepPadAt(stepSites[i].at);  // NOP padding after the ret
        // Recorded here, outside any freeze: the install writes the pad only while it still holds these bytes.
        if (!truefps::recordPad(stepSites[i]))
            logLine(Ashita::LogLevel::Warn, "the padding after a game step accessor is not the game's own and is not TrueFPS's to claim: the cutscene speed-up and smooth mode will not start");
    }
    const auto focus = truefps::findAll(image.data(), image.size(), truefps::parsePattern(truefps::kMenuFocusPattern), 2);
    menuFocusSlot = 0;
    if (focus.size() == 1) {
        uint32_t imm = 0;
        std::memcpy(&imm, image.data() + focus[0] + truefps::kMenuFocusImmOffset, 4);
        if (client.contains(imm, 4)) menuFocusSlot = imm;
    }
    if (!menuFocusSlot) logLine(Ashita::LogLevel::Warn, "the focused menu pointer was not found: the cutscene speed-up will not pause for menus");
    watchdogSite = truefps::resolveWatchdog(image.data(), image.size(), client.base);
    watchdogResolved = watchdogSite.ok();
    if (!watchdogResolved) logLine(Ashita::LogLevel::Warn, "the connection timeout counter was not found on this client build: it cannot be kept on a clock");
    else {
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "connection timeout counter at RVA 0x%06X (poll RVA 0x%06X), gives up after %u polls: %.0f s at 30 polls a second, and the client polls about twice a frame",
                    unsigned(watchdogSite.counter - client.base), unsigned(watchdogSite.poll - client.base), watchdogSite.limit,
                    truefps::watchdogTimeoutSeconds(watchdogSite.limit, truefps::kWatchdogPollsPerSecond));
        logLine(Ashita::LogLevel::Info, line);
    }
    shutdownWaitResolved = truefps::resolveShutdownWait(image.data(), image.size(), client.base, truefps::g.appSlot);
    if (!shutdownWaitResolved) logLine(Ashita::LogLevel::Warn, "the network shutdown wait was not found on this client build: it stays frame-counted");
    cameraGraceSlot = truefps::resolveCameraGrace(image.data(), image.size(), client.base);
    if (!cameraGraceSlot) logLine(Ashita::LogLevel::Warn, "the camera's collision refresh countdown was not found on this client build: it stays frame-counted");
    caretSite = truefps::resolveCaret(image.data(), image.size(), client.base);
    caretResolved = caretSite.ok();
    if (!caretResolved) logLine(Ashita::LogLevel::Warn, "the chat cursor's blink counter was not found on this client build: it keeps the client's frame-counted blink");
    else {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE, "chat cursor blink timer at RVA 0x%06X, window slot RVA 0x%06X: 26 draws a cycle, paced to 26 native 30 fps frames",
                    unsigned(caretSite.timer - client.base), unsigned(caretSite.slot - client.base));
        logLine(Ashita::LogLevel::Info, line);
    }
    {
        size_t menuCursorsResolved = 0;   // the line logged below is its only reader
        std::string missing;
        for (size_t i = 0; i < truefps::kMenuCursorCount; i++) {
            menuCursorSites[i] = truefps::resolveMenuCursor(image.data(), image.size(), client.base, truefps::kMenuCursors[i]);
            if (menuCursorSites[i].ok()) ++menuCursorsResolved;
            else {
                std::string name = truefps::kMenuCursors[i].name;
                name.erase(name.find_last_not_of(' ') + 1);
                missing += (missing.empty() ? "" : ", ") + name;
            }
        }
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE, "menu cursors: %zu of %zu found (24 draws a cycle, paced to 24 native 30 fps frames)%s%s", menuCursorsResolved,
                    truefps::kMenuCursorCount, missing.empty() ? "" : "; not found: ", missing.c_str());
        logLine(missing.empty() ? Ashita::LogLevel::Info : Ashita::LogLevel::Warn, line);
    }
    eventActiveSlot = truefps::resolveEventActive(image.data(), image.size(), client.base);
    if (!eventActiveSlot) logLine(Ashita::LogLevel::Warn, "the local event flag was not verified: cutscene speed-up is disabled");
    else {
        char line[128];
        _snprintf_s(line, sizeof line, _TRUNCATE, "local event flag verified at RVA 0x%06X (reader and cleanup agree)", unsigned(eventActiveSlot - client.base));
        logLine(Ashita::LogLevel::Info, line);
    }
    smoothFound = 0;
    if (stepResolved) {
        const truefps::ImageView view{image.data(), image.size(), client.base};
        const uintptr_t accessors[2] = {stepSites[0].at, stepSites[1].at};
        smoothFound = truefps::resolveSmooth(view, accessors, 2, smooth);
        truefps::publishSmoothAddresses(smooth);
        for (size_t i = 0; i < truefps::kSiteCount; i++) siteConstantSaid[i] = smooth.floatBits[i];   // what resolve found is not a change
        springFarSaid = smooth.springFar;
        if (truefps::g_truefpsCellPageFailed)
            logLine(Ashita::LogLevel::Warn, "smooth mode: the page for TrueFPS's persistent cells could not be reserved: the camera eye follow and actor render position routines stay on whole ticks");
        if (smooth.springFar > 0.0f) {
            char threshold[160];   // not `far`: windef.h keeps that as an empty macro
            _snprintf_s(threshold, sizeof threshold, _TRUNCATE, "smooth mode: the camera eye loop's far-distance threshold is %g on this client build", double(smooth.springFar));
            logLine(Ashita::LogLevel::Info, threshold);
        }
        if (!truefps::setPolicyTable(smooth.entries, smooth.entryCount)) {
            smoothFound = 0;
            for (auto& gr : smooth.groups) {
                if (gr.absent) continue;
                gr.found = false;
                gr.why = "TrueFPS could not set up its timing table";
            }
        }
        for (uint8_t i = 0; i < truefps::kGroupCount; i++) {
            if (smooth.groups[i].found) continue;
            char miss[300];
            if (smooth.groups[i].absent) {
                _snprintf_s(miss, sizeof miss, _TRUNCATE, "smooth mode: %s is %s", truefps::kGroups[i].name, smooth.groups[i].why.c_str());
                logLine(Ashita::LogLevel::Info, miss);
                continue;
            }
            _snprintf_s(miss, sizeof miss, _TRUNCATE, "smooth mode: %s stays on whole ticks (%s)", truefps::kGroups[i].name, smooth.groups[i].why.c_str());
            logLine(Ashita::LogLevel::Warn, miss);
        }
        std::vector<const char*> missing;
        for (uint8_t i = 0; i < truefps::kGroupCount; i++)
            if (!smooth.groups[i].found && !smooth.groups[i].absent) missing.push_back(truefps::kGroups[i].name);
        if (!missing.empty())
            chat(("smooth mode: " + std::to_string(missing.size()) + " of " + std::to_string(truefps::groupsPresent(smooth)) +
                  " routines are not smooth on this client build; see the Routines tab. Details in " + logPathText() + ".").c_str(), 0x68);
        logSiteChanges();
    }
    char line[400];
    _snprintf_s(line, sizeof line, _TRUNCATE, "build %08X resolved: limiter RVA 0x%06X, app slot RVA 0x%06X, Sleep slot RVA 0x%06X (Sleep %s), step accessors %s, player movement %s, smooth routines %zu of %d",
                buildStamp(), unsigned(loopSite - client.base), unsigned(appSlot - client.base), unsigned(originalSleepImm - client.base),
                sleepFn == reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "Sleep")) ? "matches kernel32" : "is not kernel32's",
                stepResolved ? "found (2)" : "NOT found: cutscene speed-up and smooth mode unavailable", moveResolved ? "found" : "NOT found: smooth mode unavailable",
                smoothFound, int(truefps::groupsPresent(smooth)));
    logLine(Ashita::LogLevel::Info, line);
    resolvedLine = line;
    return true;
}

bool writeDivisor(int divisor) {
    uintptr_t app = 0;
    if (!truefps::g.appSlot || !truefps::readValue(truefps::g.appSlot, app) || !app) return false;
    const int32_t value = divisor;
    return truefps::writeRaw(app + truefps::kAppDivisor, &value, 4);
}

uintptr_t currentTimer() {
    uintptr_t app = 0, timer = 0;
    if (!truefps::readValue(truefps::g.appSlot, app) || !app || !truefps::readValue(app + truefps::kAppTimer, timer)) return 0;
    return timer;
}

// Pin permanently at unload if a patch, timer table or writer may still execute in this DLL.
bool pinFailed = false;      // pinSelf could not pin; logPinFailure says so from a path that may allocate
DWORD pinFailError = 0;
bool pinSelf() {
    if (pinned) return true;
    HMODULE self = nullptr;
    pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&truefps::preciseSleep), &self) != FALSE && self;
    if (!pinned) {   // only the latch here: this can run inside a catch for an allocation failure, and logging allocates
        pinFailed = true;
        pinFailError = GetLastError();
    }
    return pinned;
}
void logPinFailure() {
    if (!pinFailed) return;
    pinFailed = false;
    char line[120];
    _snprintf_s(line, sizeof line, _TRUNCATE, "TrueFPS could not keep itself loaded (error %lu)", static_cast<unsigned long>(pinFailError));
    logLine(Ashita::LogLevel::Error, line);
}

// Merge pending notices; errors take priority over warnings.
void raiseNotice(const std::string& text, unsigned char color) {
    if (pendingNotice.empty()) { pendingNotice = text; pendingColor = color; return; }
    pendingNotice += " " + text;
    if (color == 0x44) pendingColor = 0x44;
}

void refuse(const std::string& why) {
    refusedForGood = true;
    const std::string text = "precise limiter not active: " + why + ".";
    raiseNotice(text, 0x44);
    logLine(Ashita::LogLevel::Error, text.c_str());
}

void installLimiter() {
    auto& g = truefps::g;
    const uintptr_t timer = currentTimer();
    if (!timer) return;  // the client's app object does not exist yet
    if (g.timer && g.timer != timer) {
        // The client replaced the timer object; the old one is left alone (its memory may be freed) and the DLL is pinned at unload.
        orphanedTimer = true;
        g.timer = 0;
        logLine(Ashita::LogLevel::Warn, "the client replaced its frame timer; switching the new one");
    }
    if (!g.timer) {
        uintptr_t vtable = 0;
        std::string why;
        if (!truefps::readValue(timer, vtable) || !truefps::verifyTimerVtable(vtable, client, why)) {
            refuse(why.empty() ? "the frame timer could not be read" : why);
            return;
        }
        if (!truefps::swapTimer(timer, vtable)) { refuse("the frame timer could not be switched"); return; }
    }
    if (!codeInstalled) {
        g.sleepTarget = reinterpret_cast<uintptr_t>(&truefps::preciseSleep);
        if (!truefps::retargetSleepSlot(loopSite, originalSleepImm, uint32_t(reinterpret_cast<uintptr_t>(&g.sleepTarget)))) {
            truefps::restoreTimer(timer);
            refuse("the frame limiter code changed since load, or could not be patched");
            return;
        }
        codeInstalled = true;
        logLine(Ashita::LogLevel::Info, "precise limiter installed");
    }
}

// Before restoreTimer clears g.timer: whether the timer switched this load can still call into this module. The object
// is read only while the app's timer slot still names it (timerOrphaned says why).
void noteOrphanedTimer() {
    const uintptr_t timer = currentTimer();
    uintptr_t vtable = 0;
    const bool read = timer && timer == truefps::g.timer && truefps::readValue(timer, vtable);
    if (truefps::timerOrphaned(truefps::g.timer, timer, read, vtable, reinterpret_cast<uintptr_t>(truefps::g.vtableCopy), truefps::g.originalVtable, client))
        orphanedTimer = true;
}

// Keep atomic step-accessor patches while cutscene acceleration or any smooth consumer needs them.
uintptr_t stubAddress() { return reinterpret_cast<uintptr_t>(&truefps::stepStub); }
bool stepInstalled() {
    return stepSites[0].state == truefps::JumpSite::State::Jump && stepSites[1].state == truefps::JumpSite::State::Jump;
}
std::string fallbackText();
bool installStep() {
    if (stepInstalled()) return true;
    if (!stepResolved || stepStuck) return false;
    const char* why = nullptr;
    const bool installable = truefps::jumpsInstallable(stepSites, 2, stubAddress(), &why);
    truefps::g.stepThread = GetCurrentThreadId();
    const auto result = installable ? truefps::installJumps(stepSites, 2, stubAddress()) : truefps::PatchResult::Failed;
    if (result == truefps::PatchResult::Failed) {
        stepResolved = false;  // do not retry every frame
        stepStuck = truefps::anyJumpLeft(stepSites, 2);
        const std::string text = "the cutscene speed-up and smooth mode are off this session: TrueFPS could not take over the game's timing" + (smoothOn ? "; " + fallbackText() : std::string()) + ".";
        raiseNotice(text, 0x44);
        smoothFallbackSaid = smoothOn;
        logLine(Ashita::LogLevel::Error, text.c_str());
        // The chat line names the outcome; the log names which check refused it.
        if (why) logLine(Ashita::LogLevel::Error, (std::string("the game timing patch was refused: ") + why).c_str());
        return false;
    }
    return true;
}
bool uninstallStep() {
    if (!truefps::anyJumpLeft(stepSites, 2)) return true;
    const auto result = truefps::removeJumps(stepSites, 2, stubAddress());
    if (result == truefps::PatchResult::Failed && !stepStuck) {
        stepStuck = true;
        stepResolved = false;
        logLine(Ashita::LogLevel::Error, "a game step accessor holds code TrueFPS did not write and was left alone; TrueFPS stays in memory until the game closes");
    }
    return result == truefps::PatchResult::Done;
}


bool selfImage(uintptr_t& lo, uintptr_t& hi) {
    HMODULE self = nullptr;
    MODULEINFO info{};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&truefps::stepStub), &self) ||
        !GetModuleInformation(GetCurrentProcess(), self, &info, sizeof info)) return false;
    lo = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    hi = lo + info.SizeOfImage;
    return true;
}

// Gate server cutscene status on the local event flag, which clears first at final cleanup.
// Menu eligibility uses the client's 0x15edb0 check; the 16-byte name is at [window+4]+0x46.
std::string readFocusedMenu() {
    uintptr_t win = 0, def = 0;
    int32_t state = 0;
    uint8_t disabled = 1;
    char raw[16] = {};
    if (!menuFocusSlot || !truefps::readValue(menuFocusSlot, win) || win < 0x10000) return std::string();
    if (!truefps::readValue(win + 0x10, state) || state == 0xE || !truefps::readValue(win + 0x70, disabled) || disabled != 0) return std::string();
    if (!truefps::readValue(win + 0x04, def) || def < 0x10000 || !truefps::readRaw(def + 0x46, raw, sizeof raw)) return std::string();
    return truefps::menuShortName(raw);
}

void updateCutscene() {
    auto* mm = core ? core->GetMemoryManager() : nullptr;
    const auto* party = mm ? mm->GetParty() : nullptr;
    const auto* entity = mm ? mm->GetEntity() : nullptr;
    const auto* target = mm ? mm->GetTarget() : nullptr;
    const uint32_t me = party ? party->GetMemberTargetIndex(0) : 0;
    truefps::g_truefpsPlayerIndex = me ? long(me) : -1;   // the cutscene move help never moves the player
    const uint32_t status = me && entity ? entity->GetStatusServer(me) : 0;
    uint8_t localEvent = 0;
    const bool readable = eventActiveSlot && truefps::readValue(eventActiveSlot, localEvent);
    if (eventActiveSlot && !readable && !eventReadFailed)
        logLine(Ashita::LogLevel::Warn, "the local event flag could not be read: cutscene speed-up is disabled until readable");
    if (readable && eventReadFailed) logLine(Ashita::LogLevel::Info, "the local event flag is readable again");
    eventReadFailed = eventActiveSlot && !readable;
    bool allowedTarget = false;
    if (!inCutscene && entity && target && status == 4 && readable && localEvent) {
        const uint32_t ti = target->GetTargetIndex(0);
        const char* name = ti ? entity->GetName(ti) : nullptr;
        allowedTarget = truefps::cutsceneTargetAllowed(name, cutsceneExclude);
    }
    const bool wasActive = inCutscene;
    inCutscene = truefps::cutsceneActive(wasActive, party && entity && target && status == 4,
                                        readable && localEvent != 0, allowedTarget);
    if (inCutscene != wasActive) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE, "cutscene: %s (server status %u, local event %u, readable %u)",
                    inCutscene ? "started" : "ended", unsigned(status), unsigned(localEvent), unsigned(readable));
        logLine(Ashita::LogLevel::Info, line);
    }
    focusedMenu = inCutscene ? readFocusedMenu() : std::string();
    menuPaused = !focusedMenu.empty() && std::find(cutsceneMenus.begin(), cutsceneMenus.end(), focusedMenu) != cutsceneMenus.end();
    if (inCutscene && !focusedMenu.empty() && menusSeen.insert(focusedMenu).second)
        logLine(Ashita::LogLevel::Info, ("cutscene: menu \"" + focusedMenu + "\" " + (menuPaused ? "(pauses the speed-up)" : "(not on the pause list)")).c_str());
    const std::string nowPaused = menuPaused ? focusedMenu : std::string();
    if (nowPaused != pausedMenu) {
        if (!pausedMenu.empty() && truefps::g.frequency) {
            char line[200];
            _snprintf_s(line, sizeof line, _TRUNCATE, "cutscene: speed-up paused %.1f s for \"%s\"",
                        double(truefps::qpcNow() - pausedSince) / double(truefps::g.frequency), pausedMenu.c_str());
            logLine(Ashita::LogLevel::Info, line);
        }
        pausedMenu = nowPaused;
        pausedSince = truefps::qpcNow();
    }
}

bool activeDisplayPaths(std::vector<DISPLAYCONFIG_PATH_INFO>& pathInfo, UINT32& paths) {
    std::vector<DISPLAYCONFIG_MODE_INFO> modeInfo;
    UINT32 modes = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || paths == 0) return false;
        pathInfo.assign(paths, DISPLAYCONFIG_PATH_INFO{});
        modeInfo.assign(modes, DISPLAYCONFIG_MODE_INFO{});
        const LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, pathInfo.data(), &modes, modeInfo.data(), nullptr);
        if (result == ERROR_SUCCESS) return true;
        if (result != ERROR_INSUFFICIENT_BUFFER) return false;
    }
    return false;
}
void logExactRefreshRates() {
    std::vector<DISPLAYCONFIG_PATH_INFO> pathInfo;
    UINT32 paths = 0;
    if (!activeDisplayPaths(pathInfo, paths)) return;
    for (UINT32 i = 0; i < paths; i++) {
        const auto& rate = pathInfo[i].targetInfo.refreshRate;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = pathInfo[i].sourceInfo.adapterId;
        source.header.id = pathInfo[i].sourceInfo.id;
        char name[64] = "?";
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) WideCharToMultiByte(CP_ACP, 0, source.viewGdiDeviceName, -1, name, sizeof name, nullptr, nullptr);
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE, "display %s: refresh %.3f Hz (%u/%u)", name, rate.Denominator ? double(rate.Numerator) / double(rate.Denominator) : 0.0,
                    rate.Numerator, rate.Denominator);
        logLine(Ashita::LogLevel::Info, line);
    }
}

// Poll the monitor each second; refresh exact Hz only when dirty or the monitor/mode changes.
// Fall back to integer Hz, then 60.
struct RefreshCache { double hz = 0.0; int64_t checkedAt = 0; bool dirty = true, exactLogged = false; std::string device = "?"; truefps::RefreshProbe probe; };
RefreshCache refreshCache;
double exactRefreshHz(const std::string& device) {
    double hz = 0.0;
    std::vector<DISPLAYCONFIG_PATH_INFO> pathInfo;
    UINT32 paths = 0;
    if (!activeDisplayPaths(pathInfo, paths)) return hz;
    for (UINT32 i = 0; i < paths && hz <= 0.0; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = pathInfo[i].sourceInfo.adapterId;
        source.header.id = pathInfo[i].sourceInfo.id;
        char name[64] = "";
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        WideCharToMultiByte(CP_ACP, 0, source.viewGdiDeviceName, -1, name, sizeof name, nullptr, nullptr);
        if (device != name) continue;
        const auto& rate = pathInfo[i].targetInfo.refreshRate;
        if (rate.Denominator) hz = double(rate.Numerator) / double(rate.Denominator);
    }
    return hz;
}
void refreshPoll() {
    const int64_t now = truefps::qpcNow();
    if (!refreshCache.dirty && refreshCache.hz > 0.0 && now - refreshCache.checkedAt < truefps::g.frequency) return;
    refreshCache.checkedAt = now;
    const HWND wnd = core && core->GetProperties() ? core->GetProperties()->GetFinalFantasyHwnd() : nullptr;
    const HMONITOR monitor = wnd ? MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXA info{};
    info.cbSize = sizeof info;
    const bool haveMonitor = GetMonitorInfoA(monitor, &info) != 0;
    DEVMODEA mode{};
    mode.dmSize = sizeof mode;
    const int wholeHz = haveMonitor && EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1 ? int(mode.dmDisplayFrequency) : 0;
    const bool changed = truefps::refreshProbe(refreshCache.probe, haveMonitor ? info.szDevice : "?", wholeHz);
    if (!changed && !refreshCache.dirty && refreshCache.hz > 0.0) return;   // the same display as last poll: keep the exact rate
    const std::string device = refreshCache.probe.device;
    double hz = haveMonitor && device != "?" ? exactRefreshHz(device) : 0.0;
    if (hz <= 0.0) hz = double(refreshCache.probe.wholeHz);
    if (hz <= 0.0) hz = 60.0;
    if (hz != refreshCache.hz || device != refreshCache.device) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE, "refresh rate: %.3f Hz on %s", hz, device != "?" ? device.c_str() : "an unknown display");
        logLine(Ashita::LogLevel::Info, line);
    }
    if (!refreshCache.exactLogged) {
        refreshCache.exactLogged = true;
        logExactRefreshRates();
    }
    refreshCache.hz = hz;
    refreshCache.device = device;
    refreshCache.dirty = false;
}
double refreshHz() { refreshPoll(); return refreshCache.hz; }
truefps::RefreshInfo refreshInfo() { refreshPoll(); return truefps::RefreshInfo{refreshCache.hz, refreshCache.device}; }
int refreshRate() { return int(refreshHz() + 0.5); }
std::string hzStr(double hz) { char b[16]; _snprintf_s(b, sizeof b, _TRUNCATE, "%.2f", hz); return b; }
std::string displayText() {
    const auto info = refreshInfo();
    return info.device + " " + hzStr(info.hz) + " Hz";
}
double rateNowHz() { return truefps::rateInHz(frameRate, frameRate == truefps::kRateMax ? refreshHz() : 0.0); }
double backgroundNowHz() { return truefps::rateInHz(backgroundFps, backgroundFps == truefps::kRateMax ? refreshHz() : 0.0); }
int rateNowFps() { return int(rateNowHz() + 0.5); }
int backgroundNowFps() { return truefps::rateInFps(backgroundFps, backgroundFps == truefps::kRateMax ? refreshRate() : 0); }
double maxHz() { return truefps::rateInHz(truefps::kRateMax, refreshHz()); }
std::string backgroundNowText() {
    if (backgroundFps == truefps::kBackgroundUncapped) return "uncapped";
    return std::to_string(backgroundNowFps()) + " fps";
}
int heldNowFps() { return truefps::heldFps(rateNowFps(), backgroundNowFps(), inBackground); }
int fallbackNowFps() { return 60 / truefps::gameDivisorFor(truefps::smoothFallbackFps(heldNowFps())); }
// Respect an uncap set by another tool: do not overwrite its divisor or add pacing.
int holdingNowFps() {
    if (!smoothActive && limitState.applied && truefps::divisorNow() == 0 && !truefps::ownsDivisor(limitState, 0)) return 0;
    return smoothOn && !smoothActive ? fallbackNowFps() : heldNowFps();
}
std::string fallbackText() { return "the game's own limiter holds " + std::to_string(fallbackNowFps()) + " fps"; }
std::string rateText(int rate) {
    if (rate == truefps::kRateMax) return "Max (" + hzStr(rateNowHz()) + " Hz)";
    return rate == truefps::kRateUncapped ? std::string("uncapped") : std::to_string(rate) + " fps";
}
bool smoothBlocked();
// Report the native fallback cap (at most 60 FPS) when smooth mode is blocked.
std::string rateReportText(int rate) {
    return !resolveFailed && smoothOn && smoothBlocked() ? rateText(rate) + " (" + fallbackText() + ")" : rateText(rate);   // not active: the divisor was never written, so the game is on its own default
}

void paceTo(double hz);
void paceTo(int fps) { paceTo(fps > 0 ? double(fps) : 0.0); }
void paceTo(double hz) {
    auto& g = truefps::g;
    const int64_t period = truefps::pacePeriod(g.frequency, hz);
    if (period <= 0) { paceLast = 0; return; }
    const int64_t now = truefps::qpcNow();
    if (paceLast && now < paceLast + period) {
        truefps::waitUntil(paceLast + period);
        g.waitCounts += uint64_t(truefps::qpcNow() - now);
        paceLast += period;
    } else {
        paceLast = now;
    }
}

// Present pacing rate in Hz; zero means no wait. Background Uncapped overrides the foreground rate.
double pacingNowHz() {
    if (smoothActive) {
        if (inBackground && backgroundFps == truefps::kBackgroundUncapped) return 0.0;
        return truefps::heldHz(rateNowHz(), backgroundNowHz(), inBackground);
    }
    return double(truefps::pacedWithoutSmooth(smoothOn, heldNowFps(), truefps::ownsDivisor(limitState, truefps::divisorNow())));
}
// Smooth mode uncaps the client and paces at Present. Without it, use native divisors where possible.
// Blocked smooth mode falls back to the native limiter at no more than 60 FPS.
bool smoothBlocked() { return resolveFailed || !stepResolved || !moveResolved || moveStuck || refusedForGood || smooth.sessionOver; }
std::string smoothUnavailable();
void governSmooth() {
    const bool wanted = smoothOn && !smoothBlocked() && codeInstalled && truefps::g.timer != 0;
    if (wanted && !smoothActive) refreshCache.dirty = true;   // look again when smooth mode starts
    const int target = wanted ? rateNowFps() : 0;
    if (wanted != smoothLoggedOn || (wanted && target != pacedFps)) {
        logLine(Ashita::LogLevel::Info, wanted ? ("smooth mode: " + (target ? std::to_string(target) + " fps" : std::string("uncapped"))).c_str() : "smooth mode off");
        smoothLoggedOn = wanted;
        paceLast = 0;
    }
    smoothActive = wanted;
    pacedFps = target;
    paceTo(pacingNowHz());
    const bool fallback = smoothOn && smoothBlocked();
    if (fallback && !smoothFallbackSaid) {
        const std::string text = "smooth mode " + smoothUnavailable() + ": " + fallbackText() + ".";
        logLine(Ashita::LogLevel::Warn, text.c_str());
        raiseNotice(text, 0x68);
    }
    smoothFallbackSaid = fallback;
}

bool routineSaid[truefps::kGroupCount] = {};
void logGroup(uint8_t i, const char* what) {
    char line[700];
    _snprintf_s(line, sizeof line, _TRUNCATE, "smooth mode: %s %s", truefps::kGroups[i].name, what);
    logLine(Ashita::LogLevel::Warn, line);
    const auto& gr = smooth.groups[i];
    if ((gr.failed || gr.retired || gr.stuck) && !routineSaid[i]) {
        routineSaid[i] = true;
        // A routine stuck only on a take-out comes back once its patches are out, so it is not off for the session.
        const char* when = gr.failed || gr.retired ? " is off for this session" : " is off for now";
        chat(("smooth mode: " + std::string(truefps::kGroups[i].name) + when + ". Details in " + logPathText() + ".").c_str(), 0x68);
    }
}
void logRetryPacing() {
    const long paced = _InterlockedExchange(&truefps::g_truefpsRetryPaced, 0);   // read and reset in one step: the stub adds to it from another thread
    if (!paced) return;
    char line[128];
    _snprintf_s(line, sizeof line, _TRUNCATE, "connection retry: %ld attempts paced", paced);
    logLine(Ashita::LogLevel::Info, line);
}
// A routine stuck on a take-out whose patches have all come out since: said once, at Info. A failed or retired routine
// stays off whatever comes out, so it is not said for those.
struct StuckBefore {
    bool was[truefps::kGroupCount] = {};
    StuckBefore() { for (uint8_t i = 0; i < truefps::kGroupCount; i++) was[i] = smooth.groups[i].stuck; }
    void sayCleared(const char* then) const {
        for (uint8_t i = 0; i < truefps::kGroupCount; i++)
            if (was[i] && !smooth.groups[i].stuck && !smooth.groups[i].failed)
                logLine(Ashita::LogLevel::Info, ("smooth mode: " + std::string(truefps::kGroups[i].name) + ": every patch of it is out now, so it " + then).c_str());
    }
};
bool removeSmoothPatches() {
    const StuckBefore stuckBefore;
    const bool out = truefps::stopSmoothPatches(smooth, &logGroup);
    stuckBefore.sayCleared("goes back in when smooth mode runs");
    logSiteChanges();   // a removal can find another tool's value in a site and leave it there
    logRetryPacing();
    return out;
}

// Restore the requested divisor unless another tool uncaps it. Smooth mode requires divisor zero;
// blocked smooth mode uses the native fallback cap.
void applyLimit() {
    if (!truefps::g.appSlot || currentTimer() == 0) return;
    const int current = truefps::divisorNow();
    const int limit = smoothOn && !smoothActive ? truefps::smoothFallbackFps(heldNowFps()) : heldNowFps();
    if (limit != appliedLimit) { appliedLimit = limit; limitState.applied = false; }
    const bool held = limitState.applied && current != limitState.lastWritten;
    const int fix = truefps::limitTarget(limitState, smoothActive, current, limit);
    if (fix < 0 || !writeDivisor(fix)) return;
    truefps::limitWritten(limitState, smoothActive, fix);
    if (held) {
        ++holdCorrections;
        if (holdCorrections <= 20 || holdCorrections % 1000 == 0) {  // the first 20, then every thousandth
            char line[128];
            _snprintf_s(line, sizeof line, _TRUNCATE, "limit: the game changed the divisor to %d; set back to %d (%llu so far)", current, fix, holdCorrections);
            logLine(Ashita::LogLevel::Info, line);
        }
    }
}

void clientSize(int& width, int& height);
// Delete fonts only on the drawing thread.
bool onDrawThread() { return !drawThread || GetCurrentThreadId() == drawThread; }
void destroyFont() {
    if (font && core && core->GetFontManager()) core->GetFontManager()->Delete(kFontAlias);
    font = nullptr;
}
// The detail text shares the counter's colour.
uint32_t overlayColorNow() {
    return overlayBands ? truefps::bandColour(lastFps, bandHigh, bandLow, bandColorHigh, bandColorMid, bandColorLow) : overlayColor;
}
// Preserve the font's dragged position when applying appearance changes.
void applyFontLook() {
    if (!font) return;
    font->SetFontFamily("Arial");
    font->SetFontHeight(uint32_t(overlaySize));
    font->SetBold(true);
    font->SetColor(overlayColorNow());
    font->SetColorOutline(0xFF000000);
    font->SetLocked(overlayLocked);   // unlocked: drag it with the mouse
    font->SetVisible(true);
}
// Only explicit position changes move the font.
void applyFontSettings() {
    if (!font) return;
    applyFontLook();
    font->SetPositionX(float(overlayX));
    font->SetPositionY(float(overlayY));
}
bool dragSaveOk = true;   // the drag save's last result: a change of state is logged, never chatted
void updateOverlay(bool newSecond) {
    if (!overlay) { destroyFont(); return; }
    if (!font && core && core->GetFontManager()) {
        font = core->GetFontManager()->Create(kFontAlias);
        applyFontSettings();
        newSecond = true;
    }
    if (!font) return;
    if (!newSecond) return;
    if (overlayBands) font->SetColor(overlayColorNow());   // the second's rate decides the band
    char text[64], worst[24] = "";
    if (overlayDetail) _snprintf_s(worst, sizeof worst, _TRUNCATE, " %.1f ms", lastWorstMs);
    const char* tag = (!resolveFailed && inBackground && backgroundFps != truefps::kBackgroundOff) ? " bg" : "";
    if (truefps::g.speed > 1.0f) _snprintf_s(text, sizeof text, _TRUNCATE, "%.0f x%.0f%s%s", lastFps, double(truefps::g.speed), worst, tag);
    else _snprintf_s(text, sizeof text, _TRUNCATE, "%.0f%s%s", lastFps, worst, tag);
    font->SetText(text);
    int width = 0, height = 0;
    clientSize(width, height);
    const int x = truefps::clampOverlayPos(int(font->GetPositionX()), width), y = truefps::clampOverlayPos(int(font->GetPositionY()), height);
    if (x != overlayX || y != overlayY) {
        overlayX = x;
        overlayY = y;
        if (x != int(font->GetPositionX()) || y != int(font->GetPositionY())) applyFontSettings();
        const bool saved = saveSettings();   // queued: the write is not in this frame
        if (saved != dragSaveOk) {
            dragSaveOk = saved;
            if (!saved) logLine(Ashita::LogLevel::Warn, "the fps counter's new position could not be saved: the settings are still loading or could not be read");
        }
    }
}


void applyLoadedSettings() {
    int width = 0, height = 0;
    clientSize(width, height);
    overlayX = truefps::clampOverlayPos(overlayX, width);
    overlayY = truefps::clampOverlayPos(overlayY, height);
    if (!overlay) destroyFont();          // the overlay goes now, not at the next second
    else if (font) applyFontSettings();
    refreshCache.dirty = true;            // Max may mean another rate on this monitor
    appliedLimit = -1;                    // the loaded rate is written at the next applyLimit
    truefps::limitReapply(limitState);
    panelSizing.requested = true;         // the window fits itself to the loaded [gui] scale
}

bool loggedIn() {
    if (!core) return false;
    IMemoryManager* memory = core->GetMemoryManager();
    if (!memory) return false;
    IPlayer* player = memory->GetPlayer();
    return player && player->GetLoginStatus() == 2;
}
truefps::Identity readIdentity() {
    if (!loggedIn()) return truefps::Identity{};
    IMemoryManager* memory = core->GetMemoryManager();
    IParty* party = memory->GetParty();
    if (!party) return truefps::Identity{};
    const char* name = party->GetMemberName(0);   // party slot 0 is always you
    if (!name || !*name) return truefps::Identity{};
    const uint32_t serverId = party->GetMemberServerId(0);
    if (!serverId) return truefps::Identity{};   // 0: no server id yet, never a real character
    return truefps::Identity{name, serverId};
}


// Queue character settings and log changes; apply loaded values on a later frame.
void onIdentityChanged(const truefps::Identity& now) {
    identity = now;
    const std::string key = truefps::characterKey(identity);
    if (!identity.known()) logLine(Ashita::LogLevel::Info, "character: none (logged out)");   // a login's "character:" line is the log writer's

    const uint64_t seq = ++loadSeq;
    const std::string rootCopy = root, legacy = legacyIniPath(), settingsFile = truefps::settingsPath(root, identity);
    const std::string logDir = identity.known() ? truefps::characterLogDir(root, key) : std::string();
    const truefps::Identity who = identity;
    std::shared_ptr<LoadedSettings> r;
    try { r = std::make_shared<LoadedSettings>(); } catch (...) { r = nullptr; }
    if (!r) { settingsReadFailed = true; appliedSeq = seq; }   // the log and capture folder below still follow the character
    else r->seq = seq;
    const auto read = [r, rootCopy, legacy, settingsFile, logDir, who]() {
        try {
            truefps::ensureDirs(rootCopy, truefps::folderOf(settingsFile));   // once per character
            if (!logDir.empty()) truefps::ensureDirs(rootCopy, logDir);
            r->source = truefps::settingsSource(rootCopy, who, legacy);
            r->legacy = r->source == legacy;
            r->exists = !who.known() || GetFileAttributesA(r->source.c_str()) != INVALID_FILE_ATTRIBUTES;
            r->set = defaultSettings();
            if (r->exists) readSettingsInto(r->source, false, r->set);
        } catch (...) {
            r->failed = true;
        }
        std::lock_guard<std::mutex> lock(loadedMutex);
        loadedReady = r;
    };
    if (r && !disk.runJob(read)) read();   // no writer: read here, in the frame

    // Keep the character log through logout.
    if (identity.known()) {
        fileLog.moveToCharacter(truefps::characterLogPath(root, key), identity.name);
        quietFrom = truefps::qpcNow();   // no long-frame warning in the 30 s after a login
        ownDir = logDir;
    }   // logged out: captures stay with the character you just left
}
// Ignore stale results from a previous character load.
void applySettingsWhenReady() {
    std::shared_ptr<LoadedSettings> r;
    {
        std::lock_guard<std::mutex> lock(loadedMutex);
        r = std::move(loadedReady);
    }
    if (!r || r->seq != loadSeq) return;
    settingsReadFailed = r->failed;
    if (r->failed) {   // the read failed: the defaults, kept in memory only
        applySettingsSet(defaultSettings());
        appliedSeq = r->seq;   // latched once the values are in: a throw above leaves the read unfinished, and nothing is saved
        logLine(Ashita::LogLevel::Error, "settings: the file could not be read (out of memory); the defaults are in use and nothing is saved for this character");
        applyLoadedSettings();
        return;
    }
    applySettingsSet(r->set);
    appliedSeq = r->seq;
    if (r->legacy && !legacySettingsSaid) {
        legacySettingsSaid = true;
        logLine(Ashita::LogLevel::Info, ("settings: read from the old " + underRoot(r->source) + "; every save from now goes to " +
                                         underRoot(truefps::settingsPath(root, identity)) + " (the old file is left where it is)").c_str());
    }
    if (r->exists) logLine(Ashita::LogLevel::Info, ("settings: " + underRoot(r->source)).c_str());
    else {
        saveSettings();
        logLine(Ashita::LogLevel::Info, ("settings: " + underRoot(r->source) + " does not exist yet: the defaults, written there now").c_str());
    }
    applyLoadedSettings();
}

std::string settingsText() {
    const std::string where = underRoot(truefps::settingsPath(root, identity));
    if (!identity.known()) return "settings: shared (not logged in): " + where;
    return "settings: " + identity.name + " (id " + std::to_string(identity.serverId) + "): " + where;   // the character's id on its server, which Ashita keys its folders by too
}

std::string smoothUnavailable() {
    if (resolveFailed) return "unavailable, TrueFPS is not active this session (see " + logPathText() + ")";
    if (!stepResolved) return stepFound ? "off this session, TrueFPS could not take over the game's timing (see " + logPathText() + ")" : std::string("unavailable, the game step was not found on this client build");
    if (!moveResolved) return "unavailable, the player movement code was not found on this client build";
    if (moveStuck) return "off this session, the player movement site could not be patched (see " + logPathText() + ")";
    if (refusedForGood) return "unavailable, the precise limiter is not active this session (see " + logPathText() + ")";
    if (smooth.sessionOver) return "off this session, a routine it patches runs on another game thread (see " + logPathText() + ")";
    return "";
}
std::string routineNames(const std::vector<const char*>& names) {
    if (names.size() > 6) return std::to_string(names.size()) + " routines (see the Routines tab)";
    std::string t;
    for (const char* n : names) t += (t.empty() ? "" : ", ") + std::string(n);
    return t;
}
std::string smoothStatus() {
    if (!smoothOn) return "smooth mode: off (the game's own timing).";
    const std::string why = smoothUnavailable();
    if (!why.empty()) return "smooth mode: " + why + ".";
    if (!smoothActive) return "smooth mode: on (" + rateText(frameRate) + "), waiting for the game to start.";
    size_t on = 0;
    bool logged = false;
    std::vector<const char*> whole;
    for (uint8_t i = 0; i < truefps::kGroupCount; i++) {
        const auto& gr = smooth.groups[i];
        if (gr.absent) continue;   // not in this client build
        if (gr.on) { ++on; continue; }
        whole.push_back(truefps::kGroups[i].name);
        logged = logged || !gr.found || gr.failed || gr.stuck || gr.retired;
    }
    std::string text = "smooth mode: " + rateText(frameRate) + ", " +
                       std::to_string(on) + " of " + std::to_string(truefps::groupsPresent(smooth)) + " routines smooth";
    if (!whole.empty()) text += "; whole ticks: " + routineNames(whole) + (logged ? " (see " + logPathText() + ")" : std::string());
    return text + ".";
}

std::string watchdogText();
std::string backgroundText() {
    const std::string where = inBackground ? "; another window is in front now." : "; the game window is in front now.";
    if (backgroundFps == truefps::kBackgroundOff) return "in the background: off, your frame rate does not change behind other windows" + where;
    if (backgroundFps == truefps::kBackgroundUncapped) return "in the background: uncapped, no limit at all while another window is in front" + where;
    const std::string cap = backgroundFps == truefps::kRateMax ? "Max (" + hzStr(backgroundNowHz()) + " Hz)" : std::to_string(backgroundFps) + " fps";
    return "in the background: capped to " + cap + " while another window is in front" + where;
}
std::string backgroundCaptureText() {
    if (backgroundFps == truefps::kBackgroundOff) return "off";
    if (backgroundFps == truefps::kRateMax) return "Max (" + hzStr(backgroundNowHz()) + " Hz)";
    return backgroundNowText();   // uncapped, or "<n> fps"
}

std::string cutsceneSpeedText() {
    return cutsceneSpeed > 1 ? "cutscenes play " + std::to_string(cutsceneSpeed) + "x faster, at your normal frame rate."
                             : std::string("cutscenes play at normal speed.");
}
// Full status adds limiter timing, background caps and unresolved routines for diagnostics.
void printStatus(bool full = false) {
    char line[320];
    if (resolveFailed) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "%.1f fps. TrueFPS is not active this session: the game's own frame limiter is in use (see %s).", lastFps, logPathText().c_str());
        chat(line, 0x68);
        return;
    }
    const int held = holdingNowFps();
    std::string rate = "Frame rate " + rateText(frameRate);
    if (smoothOn && smoothBlocked()) rate += ", " + fallbackText();
    else if (held > 0) rate += ", holding " + std::to_string(held) + " fps";
    else if (frameRate != truefps::kRateUncapped) rate += ", uncapped";   // held is 0 with a rate set only when the background cap removed the limit
    _snprintf_s(line, sizeof line, _TRUNCATE, "%.1f fps. %s.", lastFps, rate.c_str());
    chat(line);
    chat(smoothStatus().c_str(), smoothOn && !smoothUnavailable().empty() ? 0x68 : 0x6A);
    if (!stepResolved) {
        chat(stepFound ? ("cutscene speed-up: off this session, TrueFPS could not take over the game's timing (see " + logPathText() + ").").c_str()
                       : "cutscene speed-up: unavailable on this client build.", 0x68);
    } else {
        _snprintf_s(line, sizeof line, _TRUNCATE, "cutscenes: %s%s%s.", cutsceneSpeed > 1 ? (std::to_string(cutsceneSpeed) + "x").c_str() : "normal speed", inCutscene ? " (in one now)" : "",
                    menuPaused ? (", paused for the " + focusedMenu + " menu").c_str() : "");
        chat(line);
    }
    if (full) {
        const int32_t divisor = truefps::divisorNow();
        if (refusedForGood) chat(("precise limiter: not active this session (see " + logPathText() + " for why).").c_str(), 0x68);
        else if (!codeInstalled || !truefps::g.timer) chat("precise limiter: waiting for the game to start.");
        else {
            const char* const waits = truefps::hrTimerAvailable()                            ? "the high-resolution timer"
                                      : truefps::g_hrTimerCheckLateMs > truefps::kTimerCheckLateMs ? "Sleep(1), because this system's timer is not precise enough"
                                                                                                  : "Sleep(1), because this system has no high-resolution timer";
            const double pacing = divisor > 0 ? 0.0 : pacingNowHz();
            if (divisor > 0) _snprintf_s(line, sizeof line, _TRUNCATE, "precise limiter: each frame waits exactly %.2f ms (%.2f ms of it on the CPU, the rest on %s).", 1000.0 * divisor / 60.0, lastSpinMs, waits);
            else if (pacing > 0.0) _snprintf_s(line, sizeof line, _TRUNCATE, "precise limiter: installed; the game's own limit is off, so TrueFPS does the waiting to hold %s Hz (%.2f ms of each frame on the CPU, the rest on %s).", hzStr(pacing).c_str(), lastSpinMs, waits);
            else _snprintf_s(line, sizeof line, _TRUNCATE, "precise limiter: installed; the game's own limit is off and no rate is being held, so no frame waits.");
            chat(line);
        }
        if (backgroundFps != truefps::kBackgroundOff) chat(backgroundText().c_str());
        if (!watchdogResolved || watchdogWaiting || watchdogClock.corrections) chat(watchdogText().c_str(), watchdogResolved ? 0x6A : 0x68);
        if (stepResolved && !menuFocusSlot) chat("cutscene menus: the focused menu pointer was not found on this client build, so the cutscene speed-up does not pause for menus.", 0x68);
        if (!eventActiveSlot || eventReadFailed) chat(cutsceneFlagText().c_str(), 0x68);
    }
    chat(settingsText().c_str());
}

double measureCyclesPerMs() {
    double best = 0.0;
    const int64_t f = truefps::g.frequency;
    for (int i = 0; i < 4 && f; i++) {
        ULONG64 c0 = 0, c1 = 0;
        if (!QueryThreadCycleTime(GetCurrentThread(), &c0)) return 0.0;
        const int64_t t0 = truefps::qpcNow();
        int64_t t = t0;
        while ((t = truefps::qpcNow()) - t0 < f / 200) {}
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        const double ms = double(t - t0) * 1000.0 / double(f);
        if (ms > 0.0) best = (std::max)(best, double(c1 - c0) / ms);
    }
    return best;
}

const char* const kNoHitchesLine = "no hitches (frames over 1.5x the median).";
// Full frame reports include the breakdown and slowest frames.
void printFrames(bool full = false) {
    const int64_t now = truefps::qpcNow();
    const auto sum = truefps::summarizeFrames(frameLog, now, truefps::g.frequency, 10.0);
    if (sum.frames < 2) { chat("no frames recorded yet."); return; }
    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE, "last %.1f s: %zu frames presented = %.1f fps (1%% low %.1f fps). Frame time %.1f ms average, %.1f ms median, %.1f ms worst.",
                sum.seconds, sum.frames, sum.fps, sum.low1Fps, sum.avgMs, sum.medianMs, sum.worstMs);
    chat(line);
    if (!resolveFailed) {   // no tick was read this session: the number would be 0, which reads as a stopped game
        _snprintf_s(line, sizeof line, _TRUNCATE, "game: %.2f ticks per second (60 is normal speed), %.3f ticks per frame.", sum.ticksPerSecond, sum.ticksPerFrame);
        chat(line);
    }
    if (!full) { chat(sum.hitches ? "the Frames tab lists the slowest frames." : kNoHitchesLine); return; }
    if (sum.avgUpdateMs >= 0.0) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "where the time went (average): game logic %.2f ms, rendering %.2f ms, addons & plugins %.2f ms, GPU & driver %.2f ms, TrueFPS %.2f ms, frame limiter %.2f ms.",
                    sum.avgUpdateMs, sum.avgDrawMs, sum.avgAfterDrawMs, sum.avgGpuMs, sum.avgTruefpsOwnMs, sum.avgWaitMs);   // a frame split by its scenes has its Present span, so the GPU average is known here too
        chat(line);
    }
    if (sum.hitches == 0) { chat(kNoHitchesLine); return; }
    _snprintf_s(line, sizeof line, _TRUNCATE, "%zu hitches (frames over 1.5x the median), %zu severe (over 2x the median and 4 ms more). The worst:", sum.hitches, sum.severe);
    chat(line, 0x68);
    for (const auto& r : sum.worst) {
        const auto cause = truefps::slowCause(r, sum);
        chat(("  slowed by " + std::string(cause.name) + ":" + truefps::frameDetailText(r, double(now - r.at) / double(truefps::g.frequency)).substr(1)).c_str());
    }
}
// Shared command/panel operations. Commands print every result; panels log all results and chat problems.
// once suppresses repeated panel warnings until it reopens.
struct OpLine {
    std::string text;
    unsigned char color = 0x6A;
    bool once = false;
};
struct OpResult {
    std::vector<OpLine> lines;
    OpResult& add(std::string text, unsigned char color = 0x6A) { lines.push_back(OpLine{std::move(text), color, false}); return *this; }
    OpResult& add(SavedLine line, unsigned char color = 0x6A) { lines.push_back(OpLine{std::move(line.text), line.saved ? color : static_cast<unsigned char>(0x68), false}); return *this; }
    OpResult& addOnce(SavedLine line, unsigned char color) { lines.push_back(OpLine{std::move(line.text), line.saved ? color : static_cast<unsigned char>(0x68), line.saved}); return *this; }
    OpResult& addOnce(std::string text, unsigned char color) { lines.push_back(OpLine{std::move(text), color, true}); return *this; }
};
const char* const kInactiveNote = "TrueFPS is not active this session, so this changes nothing in the game until it loads on a build it recognises.";
const char* const kRateUsage = "usage: /truefps limit 30 | 60 | max | off  (the frame rate; 5 to 1000 fps; max follows your monitor; off is uncapped).";
const char* const kBackgroundUsage = "usage: /truefps background 30 | 60 | max | uncapped | off  (while another window is in front; 5 to 1000 fps; max follows your monitor; off keeps your rate).";
const char* const kCutsceneUsage = "usage: /truefps cutscene off | 2 | 3 | 4 | 5 | 6  (excluded targets and pausing menus are in the settings).";
const char* const kOverlayUsage = "usage: /truefps overlay on | off, or lock | detail | bands with on | off  (the rest is in the settings).";
const char* const kSmoothUsage = "usage: /truefps smooth on | off.";
void clientSize(int& width, int& height) {
    width = height = 0;
    RECT rc{};
    const HWND wnd = core && core->GetProperties() ? core->GetProperties()->GetFinalFantasyHwnd() : nullptr;
    if (wnd && GetClientRect(wnd, &rc)) {
        width = int(rc.right);
        height = int(rc.bottom);
    }
}
// Capture operations remain available on unsupported clients.
void toChat(const OpResult& r, bool changesSettings = true) {
    for (const auto& line : r.lines) chat(line.text.c_str(), line.color);
    if (changesSettings && resolveFailed && !r.lines.empty()) chat(kInactiveNote, 0x68);
}


// Format and write the frame snapshot on the disk writer.
OpResult opSaveFrames() {
    if (frameLog.count == 0) return OpResult().add("no frames recorded yet.", 0x68);
    if (disk.fileBusy()) return OpResult().add("the last frame save is still being written; try again in a moment.", 0x68);
    std::shared_ptr<std::vector<truefps::FrameRecord>> rows;
    try {   // 1.3 MB for a full ring
        rows = std::make_shared<std::vector<truefps::FrameRecord>>();
        rows->reserve(frameLog.count);
        for (size_t i = frameLog.count; i-- > 0;) rows->push_back(frameLog.newest(i));
    } catch (const std::bad_alloc&) {
        return OpResult().add("not enough memory to save the frames.", 0x44);
    }
    const int64_t now = truefps::qpcNow(), frequency = truefps::g.frequency;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char version[16], ashitaInterface[16];
    _snprintf_s(version, sizeof version, _TRUNCATE, "%.1f", pluginVersion);
    _snprintf_s(ashitaInterface, sizeof ashitaInterface, _TRUNCATE, "%.2f", ASHITA_INTERFACE_VERSION);
    const std::string cutsceneText = cutsceneSpeed > 1 ? std::to_string(cutsceneSpeed) + "x" : std::string("normal");
    const double seconds = double(rows->back().at - rows->front().at) / double(frequency);
    const truefps::CaptureInfo info{identity.name,  identity.serverId, buildStamp(),     clientBuildStamp(), version,
                                     rateReportText(frameRate), backgroundCaptureText(), smoothOn ? "on" : "off", cutsceneText,
                                     displayText(),  rows->size(),      seconds,          ashitaInterface};
    std::string path;
    for (int copy = 0; copy < 100; copy++) {   // every name is tested, the hundredth included; all taken: the last is written over
        path = ownDir + truefps::captureFileName(t, copy);
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    const std::string where = underRoot(path);
    const auto make = [rows, info, t, now, frequency]() { return truefps::captureText(info, t, *rows, now, frequency); };
    if (!disk.saveFile(path, make, "saved " + std::to_string(rows->size()) + " frames to " + where + ".", where))
        return OpResult().add("the frames could not be queued for saving.", 0x44);
    return OpResult();   // the result arrives from the disk writer
}

OpResult opRate(int rate) {
    OpResult r;
    if (!truefps::rateValid(rate)) return r.add(kRateUsage, 0x68);
    frameRate = rate;
    truefps::limitReapply(limitState);   // written again at the next frame, even over another tool's 0
    refreshCache.dirty = true;
    const bool saved = saveSettings();
    const int fps = rateNowFps();
    if (smoothOn) return r.add(savedLine("frame rate " + rateReportText(rate) + ".", saved));
    if (fps > 0 && truefps::divisorForLimit(fps) >= 0) return r.add(savedLine("frame rate " + rateReportText(rate) + " on the game's own timing, held.", saved));
    return r.add(savedLine("frame rate " + rateReportText(rate) + " with smooth mode off: the game's speed follows the frame rate.", saved), 0x68);
}
// Only position edits update the font's position; other changes preserve dragging.
OpResult overlayApplied(const std::string& what, bool position = false) {
    const bool saved = saveSettings();
    if (position) applyFontSettings();
    else applyFontLook();
    return OpResult().add(savedLine(what + ".", saved));
}
OpResult opOverlay(bool on) {
    overlay = on;
    return overlayApplied(overlay ? "the fps counter is on" : "the fps counter is off");
}
OpResult opOverlaySize(int size) {
    overlaySize = (std::min)(72, (std::max)(6, size));
    return overlayApplied("fps counter size " + std::to_string(overlaySize) + " px");
}
OpResult opOverlayColor(uint32_t color) {
    overlayColor = color;
    char hex[16];
    _snprintf_s(hex, sizeof hex, _TRUNCATE, "%08X", overlayColor);
    return overlayApplied(std::string("fps counter colour ") + hex);
}
OpResult opOverlayBands(bool on) {
    overlayBands = on;
    return overlayApplied(on ? "fps counter colour threshold on" : "fps counter colour threshold off");
}
OpResult opOverlayBandLimits(int high, int low, bool highMoved) {
    truefps::orderBands(high, low, highMoved);
    bandHigh = high;
    bandLow = low;
    return overlayApplied("fps counter colours change at " + std::to_string(bandHigh) + " and " + std::to_string(bandLow) + " fps");
}
uint32_t* bandColorSlot(int band) { return band == 0 ? &bandColorHigh : band == 1 ? &bandColorMid : &bandColorLow; }
OpResult opOverlayBandColor(int band, uint32_t color) {
    const int which = band < 0 ? 0 : (band > 2 ? 2 : band);
    *bandColorSlot(which) = color;
    char hex[16];
    _snprintf_s(hex, sizeof hex, _TRUNCATE, "%08X", color);
    const std::string where = which == 0   ? "above " + std::to_string(bandHigh) + " fps"
                              : which == 1 ? "between " + std::to_string(bandLow) + " and " + std::to_string(bandHigh) + " fps"
                                           : "below " + std::to_string(bandLow) + " fps";
    return overlayApplied("fps counter colour " + where + " " + hex);
}
std::string overlayText() {
    return std::string("the fps counter is ") + (overlay ? "on" : "off") + "; longest frame " + (overlayDetail ? "on" : "off") + "; " +
           (overlayLocked ? "locked" : "unlocked") + "; colour threshold " + (overlayBands ? "on" : "off") + ".";
}
OpResult opOverlayPos(int x, int y) {
    int width = 0, height = 0;
    clientSize(width, height);
    overlayX = truefps::clampOverlayPos(x, width);
    overlayY = truefps::clampOverlayPos(y, height);
    return overlayApplied("fps counter at " + std::to_string(overlayX) + ", " + std::to_string(overlayY), true);
}
OpResult opOverlayLock(bool on) {
    overlayLocked = on;
    const bool saved = saveSettings();
    applyFontLook();
    return OpResult().add(savedLine(on ? "fps counter locked: it cannot be dragged." : "fps counter unlocked: drag it with the mouse; the position is saved.", saved));
}
OpResult opBackground(int fps) {
    OpResult r;
    if (!truefps::backgroundValid(fps)) return r.add(kBackgroundUsage, 0x68);
    backgroundFps = fps;
    const bool saved = saveSettings();
    return r.add(savedLine(backgroundText(), saved));
}
std::string watchdogText() {
    if (!watchdogResolved) return "zoning safety: the client's connection timeout counter was not found on this client build, so it is left to the frame rate.";
    char line[280];
    const double timeout = truefps::watchdogTimeoutSeconds(watchdogSite.limit, truefps::kWatchdogPollsPerSecond);
    if (watchdogWaiting)
        _snprintf_s(line, sizeof line, _TRUNCATE, "zoning safety: waiting for the server now, %.1f s of %.0f (held back on %llu frames this session).",
                    watchdogClock.waited, timeout, static_cast<unsigned long long>(watchdogClock.corrections));
    else if (watchdogClock.corrections)
        _snprintf_s(line, sizeof line, _TRUNCATE, "zoning safety: on, the client waits %.0f s for the server at any frame rate. Held back on %llu frames so far; the last wait took %.1f s.",
                    timeout, static_cast<unsigned long long>(watchdogClock.corrections), watchdogLastWait);
    else
        _snprintf_s(line, sizeof line, _TRUNCATE, "zoning safety: on, the client waits %.0f s for the server at any frame rate.", timeout);
    return line;
}
OpResult opOverlayDetail(bool on) {
    overlayDetail = on;
    const bool saved = saveSettings();
    return OpResult().add(savedLine(on ? "fps counter detail on: the longest frame of each second follows the fps." : "fps counter detail off.", saved));
}
void addCutsceneCaveats(OpResult& r) {
    if (!stepResolved) r.addOnce(stepFound ? "the cutscene speed-up is off this session: TrueFPS could not take over the game's timing (see " + logPathText() + ")." : std::string("the cutscene speed-up is unavailable on this client build."), 0x68);
    if (!eventActiveSlot || eventReadFailed) r.addOnce(cutsceneFlagText(), 0x68);
}
OpResult opCutsceneSpeed(int speed) {
    OpResult r;
    if (speed < 1 || speed > 6) return r.add(kCutsceneUsage, 0x68);
    cutsceneSpeed = speed;
    const bool saved = saveSettings();
    r.add(savedLine(cutsceneSpeedText(), saved));
    addCutsceneCaveats(r);
    return r;
}
OpResult opExclude(bool add, const std::string& name) {
    const std::string who = lower(name);
    if (who.empty()) return OpResult().add("no target name given, so nothing changed.", 0x68);
    auto it = std::find(cutsceneExclude.begin(), cutsceneExclude.end(), who);
    if (add == (it != cutsceneExclude.end()))
        return OpResult().add(add ? "\"" + who + "\" is already in the list of targets never sped up." : "\"" + who + "\" is not in the list of targets never sped up.", 0x68);
    if (add) cutsceneExclude.push_back(who);
    else cutsceneExclude.erase(it);
    const bool saved = saveSettings();
    return OpResult().add(savedLine(std::to_string(cutsceneExclude.size()) + " names excluded from the cutscene speed-up.", saved));
}
OpResult opPauseMenu(bool add, const std::string& name) {
    OpResult r;
    const std::string menu = lower(name);
    if (menu.empty()) return r.add("no menu name given, so nothing changed.", 0x68);
    auto it = std::find(cutsceneMenus.begin(), cutsceneMenus.end(), menu);
    if (add == (it != cutsceneMenus.end()))
        return r.add(add ? "\"" + menu + "\" is already in the list of menus that pause the cutscene speed-up." : "\"" + menu + "\" is not in the list of menus that pause the cutscene speed-up.", 0x68);
    if (add) cutsceneMenus.push_back(menu);
    else cutsceneMenus.erase(it);
    const bool saved = saveSettings();
    std::string all;
    for (const auto& e : cutsceneMenus) all += (all.empty() ? "" : ", ") + e;
    r.add(savedLine("the cutscene speed-up pauses while these menus have focus: " + (all.empty() ? std::string("none") : all) + ". Focused now: " +
                    (focusedMenu.empty() ? std::string("none") : focusedMenu) + (inCutscene ? "." : " (not in a cutscene)."), saved));
    if (!menuFocusSlot) r.add("the focused menu pointer was not found on this client build, so the speed-up does not pause.", 0x68);
    return r;
}
OpResult opSmoothOn(bool on) {
    OpResult r;
    smoothOn = on;
    refreshCache.dirty = true;
    const bool saved = saveSettings();
    if (!on) {
        const int fps = rateNowFps();   // the same test opRate uses: a rate the game's own limiter can hold keeps normal speed
        if (resolveFailed || (fps > 0 && truefps::divisorForLimit(fps) >= 0)) return r.add(savedLine("smooth mode off: the game's own timing.", saved));
        const std::string at = frameRate == truefps::kRateUncapped ? std::string("uncapped") : "at " + rateText(frameRate);
        return r.add(savedLine("smooth mode off: the game's own timing, and " + at + " the game's speed follows the frame rate.", saved), 0x68);
    }
    const std::string why = smoothUnavailable();
    if (!why.empty()) {
        smoothFallbackSaid = true;
        return r.addOnce(savedLine("smooth mode " + why + (resolveFailed ? std::string(".") : ": " + fallbackText() + "."), saved), 0x68);
    }
    return r.add(savedLine("smooth mode on at " + rateText(frameRate) + ": the game keeps normal speed.", saved));
}
float fpsHistory[120] = {};
int fpsHistoryNext = 0, fpsHistoryCount = 0;
truefps::FrameSummary panelSummary;
int64_t panelSummaryAt = 0;
truefps::CustomChoice panelRate{120}, panelBg{45};   // the Custom choices: their values, and Custom picked at a preset value
bool ratePreset(int rate) { return rate == 30 || rate == 60 || rate == truefps::kRateMax || rate == truefps::kRateUncapped; }
bool backgroundPreset(int cap) { return cap == truefps::kBackgroundOff || cap == truefps::kBackgroundUncapped || cap == 30 || cap == 60 || cap == truefps::kRateMax; }
int panelCsStop = 0;          // the Live tab's cutscene slider (0 = normal, 5 = 6x)
bool panelCsDragging = false;
char panelMenuBuf[32] = "", panelExcludeBuf[64] = "";

const ImVec4 kColText(0.93f, 0.91f, 0.86f, 1.0f), kColDim(0.72f, 0.74f, 0.80f, 1.0f), kColLume(0.58f, 0.90f, 0.80f, 1.0f),
             kColSteel(0.44f, 0.62f, 1.0f, 1.0f), kColGood(0.38f, 0.76f, 0.57f, 1.0f), kColWarn(0.91f, 0.70f, 0.31f, 1.0f),
             kColBad(0.94f, 0.42f, 0.36f, 1.0f), kColHead(0.55f, 0.71f, 1.0f, 1.0f);

bool panelInactiveSaid = false;   // kInactiveNote: once each time the settings window is opened
std::set<std::string> panelSaidOnce;   // the OpLine::once lines this window session has already chatted
void panelRun(const OpResult& r, bool changesSettings = true) {
    for (const auto& line : r.lines) {
        logLine(Ashita::LogLevel::Info, ("panel: " + line.text).c_str());
        if (line.color == 0x6A) continue;
        if (line.once && !panelSaidOnce.insert(line.text).second) continue;
        chat(line.text.c_str(), line.color);
    }
    if (changesSettings && resolveFailed && !r.lines.empty() && !panelInactiveSaid) {
        panelInactiveSaid = true;
        chat(kInactiveNote, 0x68);
    }
}
void showDiskNotices() {
    for (const auto& n : disk.takeNotices()) chat(n.text.c_str(), n.color);
}
void noteSecond(double fps) {
    fpsHistory[fpsHistoryNext] = float(fps);
    fpsHistoryNext = (fpsHistoryNext + 1) % 120;
    if (fpsHistoryCount < 120) ++fpsHistoryCount;
}

float frameMsAt(void* data, int idx) {
    const int count = *static_cast<int*>(data);
    return frameLog.newest(size_t(count - 1 - idx)).ms;
}
float fpsAt(void*, int idx) {
    const int start = fpsHistoryCount < 120 ? 0 : fpsHistoryNext;
    return fpsHistory[(start + idx) % 120];
}

void panelText(IGuiManager* g, const ImVec4& colour, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    g->PushTextWrapPos(0.0f);
    g->TextColoredV(colour, fmt, args);
    g->PopTextWrapPos();
    va_end(args);
}
void note(IGuiManager* g, const char* text) {
    g->PushTextWrapPos(0.0f);
    g->TextDisabled("%s", text);
    g->PopTextWrapPos();
}
bool fitsOnLine(IGuiManager* g, float width) {
    const float right = g->GetCursorScreenPos().x + g->GetContentRegionAvail().x;
    return g->GetItemRectMax().x + g->GetStyle().ItemSpacing.x + width <= right;
}
void sideNote(IGuiManager* g, const char* text) {
    if (fitsOnLine(g, g->CalcTextSize(text).x)) g->SameLine();
    note(g, text);
}
// Call after the item's edit checks; tooltip contents change ImGui's last item.
void panelTooltip(IGuiManager* g, const char* text) {
    if (!text || !g->BeginItemTooltip()) return;
    const auto* viewport = g->GetMainViewport();
    const float maxWidth = viewport ? (std::max)(80.0f, viewport->WorkSize.x - 32.0f) : 400.0f;
    g->PushTextWrapPos((std::min)(g->GetFontSize() * 26.0f, maxWidth));
    g->TextUnformatted(text);
    g->PopTextWrapPos();
    g->EndTooltip();
}

void panelSameLine(IGuiManager* g, float nextWidth) {
    if (fitsOnLine(g, nextWidth)) g->SameLine();
}

void panelSameRadio(IGuiManager* g, const char* label) {
    panelSameLine(g, g->GetFrameHeight() + g->GetStyle().ItemInnerSpacing.x + g->CalcTextSize(label, nullptr, true).x);
}

// Keep Remove reachable when names wrap.
std::string panelNameList(IGuiManager* g, const char* id, const std::vector<std::string>& names, const char* removeHelp) {
    std::string removed;
    if (names.empty()) return removed;
    if (g->BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        g->TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        g->TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed,
            g->CalcTextSize("Remove").x + 2.0f * g->GetStyle().FramePadding.x);
        for (size_t i = 0; i < names.size(); ++i) {
            g->PushID(int(i));
            g->TableNextRow();
            g->TableNextColumn();
            g->TextWrapped("%s", names[i].c_str());
            g->TableNextColumn();
            if (g->SmallButton("Remove")) removed = names[i];
            panelTooltip(g, removeHelp);
            g->PopID();
        }
        g->EndTable();
    }
    return removed;
}

// Size the shadow from the previous frame's panel height.
class Section {
    IGuiManager* g;
    float s;
    bool open = false;
    std::string title_;
    static std::map<std::string, float>& heights() {
        static std::map<std::string, float> h;
        return h;
    }
    float topY;   // where the tab's content starts: a panel below anything gets the 10 px gap
public:
    Section(IGuiManager* gui, float scale) : g(gui), s(scale), topY(gui->GetCursorPosY()) {}
    ~Section() { end(); }
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
    void begin(const char* title) {
        end();
        title_ = title;
        if (g->GetCursorPosY() > topY + 0.5f) g->Dummy(ImVec2(0.0f, (std::max)(0.0f, 10.0f * s - 2.0f * g->GetStyle().ItemSpacing.y)));
        const float shadow = 3.0f * s;
        const float width = (std::max)(1.0f, g->GetContentRegionAvail().x - shadow);
        const ImVec2 start = g->GetCursorScreenPos();
        g->PushID(title);
        const auto it = heights().find(title_);
        if (it != heights().end() && it->second > 0.0f) {
            g->SetCursorScreenPos(ImVec2(start.x + shadow, start.y + shadow));
            g->PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
            g->BeginChild("##shadow", ImVec2(width, it->second), ImGuiChildFlags_None, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar);
            g->EndChild();
            g->PopStyleColor();
            g->SetCursorScreenPos(start);
        }
        g->PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 8.0f * s));
        g->BeginChild("##section", ImVec2(width, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        g->PopStyleVar();
        open = true;
        g->PushStyleColor(ImGuiCol_Text, kColHead);
        g->SeparatorText(title);
        g->PopStyleColor();
    }
    void end() {
        if (!open) return;
        g->EndChild();
        heights()[title_] = g->GetItemRectSize().y;
        g->PopID();
        open = false;
    }
};
bool radio(IGuiManager* g, const char* label, bool on) {
    if (on) g->PushStyleColor(ImGuiCol_Text, kColLume);
    const bool clicked = g->RadioButton(label, on);
    if (on) g->PopStyleColor();
    return clicked;
}

// Use a shared label-column width across tabs.
bool rowsBegin(IGuiManager* g, const char* id) {
    if (!g->BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    g->TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, g->CalcTextSize("Show the fps counter").x + 2.0f * g->GetStyle().ItemSpacing.x);
    g->TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
void row(IGuiManager* g, const char* label, const char* help = nullptr) {
    g->TableNextRow();
    g->TableNextColumn();
    g->AlignTextToFramePadding();
    g->TextUnformatted(label);
    panelTooltip(g, help);
    g->TableNextColumn();
}
void rowValue(IGuiManager* g, const char* label, const char* help, const char* value, const ImVec4* colour = nullptr) {
    row(g, label, help);
    g->AlignTextToFramePadding();
    g->TextColored(colour ? *colour : kColLume, "%s", value);
    panelTooltip(g, help);
}

void labelValue(IGuiManager* g, const char* label, const ImVec4& colour, const char* fmt, double value, const char* help = nullptr) {
    g->TableNextRow();
    g->TableNextColumn();
    panelText(g, kColDim, "%s", label);
    panelTooltip(g, help);
    g->TableNextColumn();
    char text[48];
    _snprintf_s(text, sizeof text, _TRUNCATE, fmt, value);
    panelText(g, colour, "%s", text);
    panelTooltip(g, help);
}
void labelText(IGuiManager* g, const char* label, const ImVec4& colour, const char* value, const char* help) {
    g->TableNextRow();
    g->TableNextColumn();
    panelText(g, kColDim, "%s", label);
    panelTooltip(g, help);
    g->TableNextColumn();
    panelText(g, colour, "%s", value);
    panelTooltip(g, help);
}
// Bound readout width by the refresh-rate label; abbreviate larger values to fit.
const char* const kReadoutWidest = "143.86 Hz";
bool readoutTable(IGuiManager* g, const char* id) {
    if (!g->BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    g->TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
    g->TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, g->CalcTextSize(kReadoutWidest).x + 8.0f);
    return true;
}
void panelReadout(IGuiManager* g, float s) {
    const auto& sum = panelSummary;
    g->PushFont(g->GetFont(), 44.0f * s);
    panelText(g, kColLume, "%.0f", lastFps);
    g->PopFont();
    const int held = resolveFailed ? 0 : holdingNowFps();
    if (held > 0) panelText(g, kColDim, "fps, holding %d", held);
    else panelText(g, kColDim, "fps, uncapped");
    panelTooltip(g, "Drawn frames per second, updated about once a second, and the rate TrueFPS is holding.");
    g->PlotLines("##fpshist", &fpsAt, nullptr, fpsHistoryCount, 0, "fps, last 2 min", 0.0f, FLT_MAX, ImVec2(-1.0f, 50.0f * s));
    Section sec(g, s);
    sec.begin("Frames");
    if (readoutTable(g, "##rdframes")) {
        const double frameMs = lastFps > 0.0 ? 1000.0 / lastFps : 0.0;
        labelValue(g, "Frame time", kColText, frameMs >= 1000.0 ? "%.0f ms" : "%.1f ms", frameMs, "Average milliseconds per frame in the last second. 60 fps is 16.7 ms.");
        labelValue(g, "Longest frame (1 s)", kColText, lastWorstMs >= 1000.0 ? "%.0f ms" : "%.1f ms", lastWorstMs, "The longest frame in the last second. A high value is a stutter the average hides.");
        labelValue(g, "1% low (10 s)", kColText, sum.low1Fps >= 999.95 ? "%.0f fps" : "%.1f fps", sum.low1Fps, "The rate of the slowest 1% of frames over 10 seconds. Close to the average means steady.");
        labelValue(g, "Hitches (10 s)", sum.hitches ? kColWarn : kColText, "%.0f", double(sum.hitches), "Frames over 1.5x the median frame time in the last 10 seconds. The Frames tab shows the worst.");
        g->EndTable();
    }
    if (!resolveFailed) {
        sec.begin("Game");
        if (readoutTable(g, "##rdgame")) {
            const bool speedUp = inCutscene && !menuPaused && cutsceneSpeed > 1;
            const double pct = sum.ticksPerSecond / 0.6;
            const bool normal = (sum.ticksPerSecond >= 59.5 && sum.ticksPerSecond <= 60.5) || (speedUp && sum.ticksPerSecond > 60.5);
            labelValue(g, "Game speed", normal ? kColGood : kColWarn, "%.0f%%", pct, "How fast the game has run over the last 10 seconds: 100% is normal speed, 60 ticks a second, at any frame rate. Above 100% during a cutscene speed-up is expected.");
            labelValue(g, "Ticks/s", kColText, "%.2f", sum.ticksPerSecond, "Ticks the game advanced per second over the last 10 seconds. A tick is 1/60 s of game time; 60 a second is normal speed.");
            g->EndTable();
        }
    }
    sec.begin("Active");
    if (readoutTable(g, "##rdactive")) {
        char v[48];
        if (smoothActive) {
            if (frameRate == truefps::kRateMax) _snprintf_s(v, sizeof v, _TRUNCATE, "%.2f Hz", rateNowHz());
            else if (frameRate == truefps::kRateUncapped) strcpy_s(v, "Uncapped");
            else _snprintf_s(v, sizeof v, _TRUNCATE, "%d fps", frameRate);
        } else strcpy_s(v, !smoothOn ? "off" : smoothBlocked() ? "n/a" : "waiting");
        const bool smoothOut = smoothOn && !smoothActive && smoothBlocked();
        labelText(g, "Smooth mode", smoothActive ? kColLume : smoothOut ? kColWarn : kColDim, v,
                  smoothOut ? "Smooth mode cannot run this session; the Frame rate tab says why. The game keeps its own timing."
                            : "Whether smooth mode is running (normal speed and smooth motion at any frame rate), and the frame rate you set for it (the Background row shows a cap in effect).");
        if (!smoothOn) {
            const int fps = rateNowFps();
            if (fps > 0) _snprintf_s(v, sizeof v, _TRUNCATE, "%d fps", fps);
            else strcpy_s(v, "Uncapped");
            labelText(g, "Frame rate", kColText, v, "The frame rate with smooth mode off. On the game's own timing the game is at normal speed only at rates that divide 60 (60, 30, 20 ...).");
        }
        if (backgroundFps != truefps::kBackgroundOff) {
            _snprintf_s(v, sizeof v, _TRUNCATE, "%s", backgroundNowText().c_str());
            labelText(g, "Background", inBackground ? kColWarn : kColDim, v, "The frame rate while another window is in front. Amber while it applies.");
        }
        if (inCutscene) {
            if (menuPaused) strcpy_s(v, "paused");
            else if (cutsceneSpeed > 1) _snprintf_s(v, sizeof v, _TRUNCATE, "%dx", cutsceneSpeed);
            else strcpy_s(v, "normal");
            labelText(g, "Cutscene", menuPaused ? kColWarn : kColGood, v, "The cutscene speed-up right now.");
        }
        if (!resolveFailed && !watchdogResolved) {
            labelText(g, "Zone wait", kColWarn, "n/a", "Zoning safety is unavailable: the game's connection timeout was not found on this client build, so at high frame rates a zone change can disconnect you (FFXI-3001).");
        } else if (watchdogWaiting || (watchdogLastWait > 0.0 && truefps::g.frequency > 0 &&
                                       double(truefps::qpcNow() - watchdogLastWaitAt) / double(truefps::g.frequency) < kZoneWaitShownSeconds)) {
            _snprintf_s(v, sizeof v, _TRUNCATE, "%.1f s", watchdogWaiting ? watchdogClock.waited : watchdogLastWait);
            char tip[340];
            _snprintf_s(tip, sizeof tip, _TRUNCATE,
                        "How long the game waited for the server on %s zone change (shown for 30 s after it). At high frame rates the game would give up on that wait early and disconnect you (FFXI-3001); TrueFPS keeps it to the patience of native 30 (the game at its default 30 fps). Held back on %llu frames this session.",
                        watchdogWaiting ? "this" : "the last", static_cast<unsigned long long>(watchdogClock.corrections));
            labelText(g, "Zone wait", watchdogWaiting ? kColWarn : kColDim, v, tip);
        }
        g->EndTable();
    }
}

// Keep Custom selected even when its value equals a preset.
void chooseCustomRate() {
    if (frameRate != panelRate.value) panelRun(opRate(panelRate.value));
    if (frameRate == panelRate.value) panelRate.chose(frameRate);
}
void rateRadios(IGuiManager* g) {
    const bool custom = panelRate.follow(frameRate, ratePreset(frameRate));
    char maxLabel[32], customLabel[32];
    _snprintf_s(maxLabel, sizeof maxLabel, _TRUNCATE, "Max (%.2f Hz)##rate", maxHz());
    _snprintf_s(customLabel, sizeof customLabel, _TRUNCATE, "Custom (%d)##rate", panelRate.value);
    const auto pick = [](int rate) { panelRate.other(); if (frameRate != rate) panelRun(opRate(rate)); };
    if (radio(g, "30##rate", frameRate == 30 && !custom)) pick(30);
    panelSameRadio(g, "60##rate");
    if (radio(g, "60##rate", frameRate == 60 && !custom)) pick(60);
    panelSameRadio(g, maxLabel);
    if (radio(g, maxLabel, frameRate == truefps::kRateMax && !custom)) pick(truefps::kRateMax);
    panelSameRadio(g, "Uncapped##rate");
    if (radio(g, "Uncapped##rate", frameRate == truefps::kRateUncapped && !custom)) pick(truefps::kRateUncapped);
    panelSameRadio(g, customLabel);
    if (radio(g, customLabel, custom) && !custom) chooseCustomRate();
}
void sideText(IGuiManager* g, const ImVec4& colour, const char* text) {
    if (fitsOnLine(g, g->CalcTextSize(text).x)) g->SameLine();
    panelText(g, colour, "%s", text);
}

void panelLive(IGuiManager* g, float s) {
    Section sec(g, s);
    const float w = 160.0f * s;
    sec.begin("Frame times");
    int count = int((std::min)(frameLog.count, size_t(360)));
    const float top = truefps::plotTopMs(frameLog, size_t(count));
    char caption[120];
    const int held = resolveFailed ? 0 : holdingNowFps();
    _snprintf_s(caption, sizeof caption, _TRUNCATE, "Last %d frames, 0 to %.0f ms, newest on the right%s", count, double(top),
                held > 0 ? (". Holding " + std::to_string(held) + " fps").c_str() : "");
    note(g, caption);
    g->PlotHistogram("##frames", &frameMsAt, &count, count, 0, nullptr, 0.0f, top, ImVec2(-1.0f, 150.0f * s));
    sec.begin("Quick settings");
    if (rowsBegin(g, "##live")) {
        row(g, "Smooth mode", "Normal speed and smooth motion at any frame rate, matching native 30 (the game at its default 30 fps). More in the Frame rate tab.");
        bool on = smoothOn;
        if (g->Checkbox("##qsmooth", &on)) panelRun(opSmoothOn(on));
        sideText(g, smoothActive ? kColLume : (smoothOn && smoothBlocked()) ? kColWarn : !smoothOn ? g->GetStyle().Colors[ImGuiCol_TextDisabled] : kColDim,
                 smoothActive ? "game at normal speed" : !smoothOn ? "off" : smoothBlocked() ? "unavailable" : "waiting");
        row(g, "Frame rate", "30 and 60: steady caps. Max: your monitor's refresh rate. Uncapped: as fast as the PC can. Custom: the rate set in the Frame rate tab. Also /truefps limit <number>.");
        rateRadios(g);
        row(g, "Cutscene speed", "How much faster cutscenes play. Pause menus and excluded targets are in the Cutscenes tab. Letting go of the slider saves it.");
        if (!panelCsDragging) panelCsStop = cutsceneSpeed - 1;
        char csText[16];
        if (panelCsStop <= 0) strcpy_s(csText, "Normal");
        else _snprintf_s(csText, sizeof csText, _TRUNCATE, "%dx", panelCsStop + 1);
        g->SetNextItemWidth(w);
        g->SliderInt("##qcs", &panelCsStop, 0, 5, csText, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);   // a format without %d shows the label
        panelCsDragging = g->IsItemActive();
        if (g->IsItemDeactivatedAfterEdit()) panelRun(opCutsceneSpeed(panelCsStop + 1));
        if (!stepResolved || !eventActiveSlot || eventReadFailed) sideText(g, kColWarn, "speed-up unavailable");
        else if (!inCutscene) sideText(g, g->GetStyle().Colors[ImGuiCol_TextDisabled], "not in a cutscene");
        else if (menuPaused) sideText(g, kColWarn, ("paused (" + focusedMenu + " menu)").c_str());
        else sideText(g, kColGood, cutsceneSpeed > 1 ? ("playing " + std::to_string(cutsceneSpeed) + "x faster").c_str() : "playing at normal speed");
        row(g, "FPS counter", "The fps counter in game, and whether it adds the longest frame of each second. More in the FPS Counter tab.");
        bool ov = overlay;
        if (g->Checkbox("Show##qoverlay", &ov)) panelRun(opOverlay(ov));
        panelSameLine(g, g->GetFrameHeight() + g->GetStyle().ItemInnerSpacing.x + g->CalcTextSize("Longest frame").x);
        bool det = overlayDetail;
        if (g->Checkbox("Longest frame##qdetail", &det)) panelRun(opOverlayDetail(det));
        g->EndTable();
    }
}

void panelFrameRate(IGuiManager* g, float s) {
    Section sec(g, s);
    const float w = 160.0f * s;
    // Round refresh Hz up so the slider can reach fractional display rates.
    const int customTop = (std::max)(truefps::kRateMinFps, (std::min)(truefps::kRateMaxFps, int(std::ceil(refreshHz()))));
    char customTip[160];
    _snprintf_s(customTip, sizeof customTip, _TRUNCATE, "%d to %d fps (your monitor's %.2f Hz, rounded up). Choose Custom to use it; letting go of the slider saves it.",
                truefps::kRateMinFps, customTop, refreshHz());
    sec.begin("Frame rate");
    if (rowsBegin(g, "##rate")) {
        row(g, "Smooth mode", "On: the game keeps normal speed (60 ticks a second) at any frame rate, including rates the game cannot hold itself and when your PC falls below the rate you picked, and motion is smooth, matching native 30 (the game at its default 30 fps). Off: the game's own timing, as without TrueFPS.");
        bool on = smoothOn;
        if (g->Checkbox("##smooth", &on)) panelRun(opSmoothOn(on));
        row(g, "Frame rate", "30 and 60: steady caps. Max: your monitor's refresh rate. Uncapped: as fast as the PC can. Custom: the rate on the slider below. Also /truefps limit <number>.");
        rateRadios(g);
        row(g, "Custom rate", customTip);
        const bool custom = panelRate.follow(frameRate, ratePreset(frameRate));
        g->BeginDisabled(!custom);
        g->SetNextItemWidth(w);
        g->SliderInt("##ratecustom", &panelRate.value, truefps::kRateMinFps, customTop, "%d fps", ImGuiSliderFlags_AlwaysClamp);
        if (g->IsItemDeactivatedAfterEdit() && custom) chooseCustomRate();
        g->EndDisabled();
        if (!custom) sideNote(g, "pick Custom above to use this");
        row(g, "");
        g->AlignTextToFramePadding();
        const int held = heldNowFps();
        if (smoothActive) panelText(g, kColGood, "Running at %.1f fps, game at normal speed", lastFps);
        else if (smoothOn) {
            const std::string why = smoothUnavailable();   // empty: it is only waiting for the game, which is not a warning
            if (why.empty()) panelText(g, kColDim, "%s", "Smooth mode: waiting for the game to start");
            else panelText(g, kColWarn, "%s", ("Smooth mode: " + why + (resolveFailed ? std::string() : "; " + fallbackText())).c_str());
        } else if (held > 0 && truefps::divisorForLimit(held) > 0) panelText(g, g->GetStyle().Colors[ImGuiCol_TextDisabled], "Running at %.1f fps on the game's own timing", lastFps);
        else panelText(g, kColWarn, "Smooth mode is off: at this rate the game runs %s than normal", held == 0 || held > 60 ? "faster" : "slower");
        g->EndTable();
    }

    sec.begin("In the background");
    if (rowsBegin(g, "##background")) {
        const bool bgCustom = panelBg.follow(backgroundFps, backgroundPreset(backgroundFps));
        char maxLabel[32], customLabel[32];
        _snprintf_s(maxLabel, sizeof maxLabel, _TRUNCATE, "Max (%.2f Hz)##bg", maxHz());
        _snprintf_s(customLabel, sizeof customLabel, _TRUNCATE, "Custom (%d)##bg", panelBg.value);
        row(g, "Background cap", "The frame rate while another window is in front. Max: your monitor's refresh rate. Uncapped: no limit at all. Off: your rate does not change. A cap only ever lowers the rate; your normal rate comes back when you switch back.");
        const auto pick = [](int cap) { panelBg.other(); if (backgroundFps != cap) panelRun(opBackground(cap)); };
        const auto chooseCustom = []() {
            if (backgroundFps != panelBg.value) panelRun(opBackground(panelBg.value));
            if (backgroundFps == panelBg.value) panelBg.chose(backgroundFps);
        };
        if (radio(g, "30##bg", backgroundFps == 30 && !bgCustom)) pick(30);
        panelSameRadio(g, "60##bg");
        if (radio(g, "60##bg", backgroundFps == 60 && !bgCustom)) pick(60);
        panelSameRadio(g, maxLabel);
        if (radio(g, maxLabel, backgroundFps == truefps::kRateMax && !bgCustom)) pick(truefps::kRateMax);
        panelSameRadio(g, "Uncapped##bg");
        if (radio(g, "Uncapped##bg", backgroundFps == truefps::kBackgroundUncapped && !bgCustom)) pick(truefps::kBackgroundUncapped);
        panelSameRadio(g, "Off##bg");
        if (radio(g, "Off##bg", backgroundFps == truefps::kBackgroundOff && !bgCustom)) pick(truefps::kBackgroundOff);
        panelSameRadio(g, customLabel);
        if (radio(g, customLabel, bgCustom) && !bgCustom) chooseCustom();
        row(g, "Custom cap", customTip);
        g->BeginDisabled(!bgCustom);
        g->SetNextItemWidth(w);
        g->SliderInt("##bgcustom", &panelBg.value, truefps::kRateMinFps, customTop, "%d fps", ImGuiSliderFlags_AlwaysClamp);
        if (g->IsItemDeactivatedAfterEdit() && bgCustom) chooseCustom();
        g->EndDisabled();
        if (!bgCustom) sideNote(g, "pick Custom above to use this");
        row(g, "Status", "What the background cap is doing now.");
        g->AlignTextToFramePadding();
        note(g, backgroundText().c_str());
        g->EndTable();
    }
}

void panelCutscenes(IGuiManager* g, float s) {
    Section sec(g, s);
    const float w = 160.0f * s;
    sec.begin("Speed");
    if (rowsBegin(g, "##csspeed")) {
        row(g, "Cutscene speed", "How much faster cutscenes play. Pause menus and excluded targets below stay at normal speed.");
        if (radio(g, "Normal##cs", cutsceneSpeed == 1)) panelRun(opCutsceneSpeed(1));
        for (int v = 2; v <= 6; v++) {
            const std::string lbl = std::to_string(v) + "x##cs" + std::to_string(v);
            panelSameRadio(g, lbl.c_str());
            if (radio(g, lbl.c_str(), cutsceneSpeed == v)) panelRun(opCutsceneSpeed(v));
        }
        row(g, "Right now", "What the cutscene speed-up is doing now.");
        g->AlignTextToFramePadding();
        if (!stepResolved) panelText(g, kColWarn, "%s", stepFound ? "unavailable this session: TrueFPS could not take over the game's timing" : "unavailable on this client build");
        else if (!eventActiveSlot || eventReadFailed) panelText(g, kColWarn, "%s", cutsceneFlagText().c_str());
        else if (!inCutscene) note(g, "not in a cutscene");
        else if (menuPaused) panelText(g, kColWarn, "paused while %s has focus", focusedMenu.c_str());
        else panelText(g, kColGood, "in a cutscene%s", cutsceneSpeed > 1 ? (", " + std::to_string(cutsceneSpeed) + "x faster").c_str() : "");
        g->EndTable();
    }

    sec.begin("Menus that pause the speed-up");
    note(g, "Cutscenes play at normal speed while one of these menus has focus.");
    const std::string removeMenu = panelNameList(g, "##pausemenus", cutsceneMenus, "Stop pausing for this menu.");
    if (!removeMenu.empty()) panelRun(opPauseMenu(false, removeMenu));
    if (cutsceneMenus.empty()) g->TextDisabled("None.");
    if (rowsBegin(g, "##csmenus")) {
        row(g, "Add a menu", "The menu's internal name, for example shop. Press Enter or Add.");
        g->SetNextItemWidth(w);
        const bool enterMenu = g->InputTextWithHint("##addmenu", "menu name", panelMenuBuf, sizeof panelMenuBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        g->SameLine();
        if ((g->Button("Add##menu") || enterMenu) && panelMenuBuf[0]) { panelRun(opPauseMenu(true, panelMenuBuf)); panelMenuBuf[0] = '\0'; }
        row(g, "Focused menu", "The menu that has focus now, to find a menu's name during a cutscene.");
        g->AlignTextToFramePadding();
        if (!menuFocusSlot) g->TextDisabled("not readable on this client build");
        else if (focusedMenu.empty()) g->TextDisabled("none");
        else {
            g->TextColored(kColLume, "%s", focusedMenu.c_str());
            if (std::find(cutsceneMenus.begin(), cutsceneMenus.end(), focusedMenu) == cutsceneMenus.end()) {
                g->SameLine();
                if (g->SmallButton("Add it##focused")) panelRun(opPauseMenu(true, focusedMenu));
            }
        }
        g->EndTable();
    }

    sec.begin("Targets never sped up");
    note(g, "Cutscenes with these targets always play at normal speed.");
    const std::string removeName = panelNameList(g, "##excludednames", cutsceneExclude, "Speed up cutscenes with this target again.");
    if (!removeName.empty()) panelRun(opExclude(false, removeName));
    if (cutsceneExclude.empty()) g->TextDisabled("None.");
    if (rowsBegin(g, "##csexclude")) {
        row(g, "Add a target", "The full target name; letter case does not matter. Press Enter or Add.");
        g->SetNextItemWidth(w);
        const bool enterName = g->InputTextWithHint("##addexclude", "target name", panelExcludeBuf, sizeof panelExcludeBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        g->SameLine();
        if ((g->Button("Add##exclude") || enterName) && panelExcludeBuf[0]) { panelRun(opExclude(true, panelExcludeBuf)); panelExcludeBuf[0] = '\0'; }
        g->EndTable();
    }
}

// Preview while editing; save when the picker closes.
void bandColorRow(IGuiManager* g, const char* id, int band, uint32_t& value, float width) {
    float col[4] = {float((value >> 16) & 0xFF) / 255.0f, float((value >> 8) & 0xFF) / 255.0f, float(value & 0xFF) / 255.0f, float((value >> 24) & 0xFF) / 255.0f};
    g->SetNextItemWidth(width);
    if (g->ColorEdit4(id, col, ImGuiColorEditFlags_AlphaBar)) {
        const auto b = [](float v) { return uint32_t(std::lround((std::min)(1.0f, (std::max)(0.0f, v)) * 255.0f)); };
        value = (b(col[3]) << 24) | (b(col[0]) << 16) | (b(col[1]) << 8) | b(col[2]);
        if (font) font->SetColor(value);   // the row previews itself; the next second restores the band in force
    }
    if (g->IsItemDeactivatedAfterEdit()) panelRun(opOverlayBandColor(band, value));
}
void panelOverlay(IGuiManager* g, float s) {
    Section sec(g, s);
    const float w = 160.0f * s;
    const float wide = w * 1.75f;   // the colour rows; the pair of threshold boxes shares it
    sec.begin("FPS Counter");
    if (rowsBegin(g, "##ovshow")) {
        row(g, "Show the fps counter", "The small fps counter in game.");
        bool ov = overlay;
        if (g->Checkbox("##ovon", &ov)) panelRun(opOverlay(ov));
        row(g, "Show longest frame", "Adds the longest frame of each second, in ms, after the fps.");
        bool det = overlayDetail;
        if (g->Checkbox("##ovdetail", &det)) panelRun(opOverlayDetail(det));
        row(g, "Lock its position", "Stops the counter being dragged by accident.");
        bool locked = overlayLocked;
        if (g->Checkbox("##ovlock", &locked)) panelRun(opOverlayLock(locked));
        g->EndTable();
    }
    // Balance disabled state within each child; never carry it across child boundaries.
    if (!overlay) sideNote(g, "turn the counter on above to change these");
    sec.end();
    g->BeginDisabled(!overlay);
    sec.begin("Look");
    if (rowsBegin(g, "##ovlook")) {
        row(g, "Text size", "Size of the counter's text. Saved when you let go of the slider.");
        int size = overlaySize;
        g->SetNextItemWidth(w);
        if (g->SliderInt("##ovsize", &size, 6, 72, "%d px", ImGuiSliderFlags_AlwaysClamp)) { overlaySize = size; applyFontLook(); }
        if (g->IsItemDeactivatedAfterEdit()) panelRun(opOverlaySize(overlaySize));
        // Balance disabled state inside the table, label included: the colour threshold, not this row, owns the
        // counter's colour while it is on. Opened before the row, as the Thresholds row below opens its own.
        g->BeginDisabled(overlayBands);
        row(g, "Colour", "Text colour; A is opacity. Click the swatch for a picker. Saved when you finish editing. The colour threshold below takes its place while it is on.");
        float col[4] = {float((overlayColor >> 16) & 0xFF) / 255.0f, float((overlayColor >> 8) & 0xFF) / 255.0f, float(overlayColor & 0xFF) / 255.0f, float((overlayColor >> 24) & 0xFF) / 255.0f};
        g->SetNextItemWidth(wide);
        if (g->ColorEdit4("##ovcolour", col, ImGuiColorEditFlags_AlphaBar)) {
            const auto b = [](float v) { return uint32_t(std::lround((std::min)(1.0f, (std::max)(0.0f, v)) * 255.0f)); };
            overlayColor = (b(col[3]) << 24) | (b(col[0]) << 16) | (b(col[1]) << 8) | b(col[2]);
            if (font) font->SetColor(overlayColor);   // the row previews itself while it is the colour in force
        }
        if (g->IsItemDeactivatedAfterEdit()) panelRun(opOverlayColor(overlayColor));
        g->EndDisabled();
        if (overlayBands) sideNote(g, "the colour threshold is on");
        row(g, "Colour threshold", "The counter takes one of three colours by the frame rate instead of the colour above.");
        bool bands = overlayBands;
        if (g->Checkbox("##ovbandson", &bands)) panelRun(opOverlayBands(bands));
        // Balance disabled state inside the table.
        g->BeginDisabled(!overlayBands);
        row(g, "Thresholds", "The two frame rates that divide the three colours: slow below the first, fast above the second. Type a number and press Enter, or use - and +. Saved when you leave the box.");
        // Commit typed values on blur; +/- buttons apply immediately.
        const float sqB = g->GetFrameHeight(), gapB = g->GetStyle().ItemInnerSpacing.x;
        const float toW = g->CalcTextSize("to").x + 2.0f * g->GetStyle().ItemSpacing.x;
        const float half = (wide - toW) / 2.0f - 2.0f * (sqB + gapB);   // box, - and +, "to", box, - and +: the pair takes a colour row's width
        const auto thresholdBox = [&](const char* id, const char* minus, const char* plus, int& value, bool highMoved) {
            int typed = value;
            g->SetNextItemWidth(half);
            g->InputInt(id, &typed, 0, 0);
            if (g->IsItemDeactivatedAfterEdit() && typed != value) {
                value = truefps::clampBandFps(typed);
                truefps::orderBands(bandHigh, bandLow, highMoved);   // a number past the other threshold pushes it along
                panelRun(opOverlayBandLimits(bandHigh, bandLow, highMoved));
            }
            g->SameLine(0.0f, gapB);
            int step = 0;
            if (g->Button(minus, ImVec2(sqB, 0.0f))) step = -1;
            g->SameLine(0.0f, gapB);
            if (g->Button(plus, ImVec2(sqB, 0.0f))) step = 1;
            if (step != 0) {
                value = truefps::clampBandFps(value + step);
                truefps::orderBands(bandHigh, bandLow, highMoved);
                panelRun(opOverlayBandLimits(bandHigh, bandLow, highMoved));
            }
        };
        thresholdBox("##ovbandlow", "-##ovbandlow", "+##ovbandlow", bandLow, false);
        g->SameLine();
        g->TextDisabled("to");
        g->SameLine();
        thresholdBox("##ovbandhigh", "-##ovbandhigh", "+##ovbandhigh", bandHigh, true);
        char bandLabel[32];   // each colour row is named by the thresholds in force
        _snprintf_s(bandLabel, sizeof bandLabel, _TRUNCATE, "Over %d fps", bandHigh);
        row(g, bandLabel, "The colour above the second threshold. Saved when you finish editing.");
        bandColorRow(g, "##ovcolhigh", 0, bandColorHigh, wide);
        _snprintf_s(bandLabel, sizeof bandLabel, _TRUNCATE, "%d to %d fps", bandLow, bandHigh);
        row(g, bandLabel, "The colour between the two thresholds. Saved when you finish editing.");
        bandColorRow(g, "##ovcolmid", 1, bandColorMid, wide);
        _snprintf_s(bandLabel, sizeof bandLabel, _TRUNCATE, "Under %d fps", bandLow);
        row(g, bandLabel, "The colour below the first threshold. Saved when you finish editing.");
        bandColorRow(g, "##ovcollow", 2, bandColorLow, wide);
        g->EndDisabled();
        g->EndTable();
    }
    sec.begin("Position");
    if (rowsBegin(g, "##ovpos")) {
        // Save slider edits on release; +/- buttons save immediately.
        int maxX = 1920, maxY = 1080;
        int clientW = 0, clientH = 0;
        clientSize(clientW, clientH);
        if (clientW > 0) maxX = clientW;
        if (clientH > 0) maxY = clientH;
        const float sq = g->GetFrameHeight(), gap = g->GetStyle().ItemInnerSpacing.x;
        int x = overlayX, y = overlayY;
        row(g, "X (px)", "Pixels from the left edge of the game window. Drag the slider, or - and + for one pixel at a time. Saved when you let go.");
        g->SetNextItemWidth(w - 2.0f * (sq + gap));
        if (g->SliderInt("##ovx", &x, 0, maxX, "%d px", ImGuiSliderFlags_AlwaysClamp)) { overlayX = x; if (font) applyFontSettings(); }
        if (g->IsItemDeactivatedAfterEdit()) panelRun(opOverlayPos(overlayX, overlayY));
        g->SameLine(0.0f, gap);
        if (g->Button("-##ovx", ImVec2(sq, 0.0f))) panelRun(opOverlayPos(truefps::clampOverlayPos(overlayX - 1, maxX), overlayY));
        g->SameLine(0.0f, gap);
        if (g->Button("+##ovx", ImVec2(sq, 0.0f))) panelRun(opOverlayPos(truefps::clampOverlayPos(overlayX + 1, maxX), overlayY));
        row(g, "Y (px)", "Pixels from the top edge of the game window. Drag the slider, or - and + for one pixel at a time. Saved when you let go.");
        g->SetNextItemWidth(w - 2.0f * (sq + gap));
        if (g->SliderInt("##ovy", &y, 0, maxY, "%d px", ImGuiSliderFlags_AlwaysClamp)) { overlayY = y; if (font) applyFontSettings(); }
        if (g->IsItemDeactivatedAfterEdit()) panelRun(opOverlayPos(overlayX, overlayY));
        g->SameLine(0.0f, gap);
        if (g->Button("-##ovy", ImVec2(sq, 0.0f))) panelRun(opOverlayPos(overlayX, truefps::clampOverlayPos(overlayY - 1, maxY)));
        g->SameLine(0.0f, gap);
        if (g->Button("+##ovy", ImVec2(sq, 0.0f))) panelRun(opOverlayPos(overlayX, truefps::clampOverlayPos(overlayY + 1, maxY)));
        row(g, "");
        if (g->Button("Top left##ov")) panelRun(opOverlayPos(1, 1));
        sideNote(g, overlayLocked ? "unlock it to drag it in game" : "or drag it in game");
        g->EndTable();
    }
    sec.end();   // the last child closes at the depth it was opened at, before the pop
    g->EndDisabled();
}

void panelFrames(IGuiManager* g, float s) {
    Section sec(g, s);
    const auto& sum = panelSummary;
    if (sum.frames < 2) { g->TextDisabled("No frames recorded yet."); return; }
    char v[64];
    sec.begin("Last 10 seconds");
    if (rowsBegin(g, "##framesum")) {
        _snprintf_s(v, sizeof v, _TRUNCATE, "%.1f fps", sum.fps);
        rowValue(g, "Average fps", "Frames drawn per second over the last 10 seconds.", v);
        _snprintf_s(v, sizeof v, _TRUNCATE, "%.1f fps", sum.low1Fps);
        rowValue(g, "1% low", "The rate of the slowest 1% of frames. Closer to the average means steadier.", v);
        _snprintf_s(v, sizeof v, _TRUNCATE, "%.1f ms", sum.medianMs);
        rowValue(g, "Median frame", "Half the frames took this long or less.", v);
        _snprintf_s(v, sizeof v, _TRUNCATE, "%.1f ms", sum.worstMs);
        rowValue(g, "Longest frame (10 s)", "The longest frame in the last 10 seconds.", v);
        if (!resolveFailed) {   // the readout's Game panel is hidden for the same reason: no tick was read this session
            _snprintf_s(v, sizeof v, _TRUNCATE, "%.2f", sum.ticksPerSecond);
            rowValue(g, "Ticks/s", "Ticks the game advanced per second. 60 is normal speed.", v);
        }
        _snprintf_s(v, sizeof v, _TRUNCATE, "%zu (%zu severe)", sum.hitches, sum.severe);
        rowValue(g, "Hitches", "Frames over 1.5x the median. Severe: over 2x the median and 4 ms more.", v, sum.hitches ? &kColWarn : nullptr);
        g->EndTable();
    }
    sec.begin("Where the time goes (average per frame)");
    if (sum.avgUpdateMs < 0.0) g->TextDisabled("Not available in these samples.");
    else if (rowsBegin(g, "##framewhere")) {
        // A known update span requires a valid Present span, so the GPU average is also available.
        const auto part = [&](const char* name, double ms) {
            _snprintf_s(v, sizeof v, _TRUNCATE, "%.2f ms", ms);
            rowValue(g, name, truefps::slowCauseHelp(name), v);
        };
        part(truefps::kPartGame, sum.avgUpdateMs);
        part(truefps::kPartRender, sum.avgDrawMs);
        part(truefps::kPartAddons, sum.avgAfterDrawMs);
        part(truefps::kPartGpu, sum.avgGpuMs);
        part(truefps::kPartTruefps, sum.avgTruefpsOwnMs);
        _snprintf_s(v, sizeof v, _TRUNCATE, "%.2f ms", sum.avgWaitMs);
        rowValue(g, truefps::kPartLimiter, "Waiting on purpose to hold your frame rate.", v);
        g->EndTable();
    }
    sec.begin("Worst frames");
    const std::string frameFolder = underRoot(ownDir);
    if (g->Button("Save frames to CSV")) panelRun(opSaveFrames(), false);   // a capture works on any client build
    panelTooltip(g, ("Writes every recorded frame to " + frameFolder + ".").c_str());
    sideNote(g, frameFolder.c_str());
    if (sum.worst.empty()) { note(g, "No slow frames in the last 10 seconds."); return; }
    std::vector<truefps::SlowCause> causes;
    for (const auto& r : sum.worst) causes.push_back(truefps::slowCause(r, sum));
    const char* most = causes[0].name;
    size_t mostTimes = 0;
    for (const auto& c : causes) {
        size_t n = 0;
        for (const auto& d : causes) n += d.name == c.name;
        if (n > mostTimes) { mostTimes = n; most = c.name; }
    }
    char summary[160];
    _snprintf_s(summary, sizeof summary, _TRUNCATE, "Most often: %s (%zu of %zu). Hover a row for the full breakdown.", most, mostTimes, causes.size());
    note(g, summary);
    const int64_t now = truefps::qpcNow();
    if (g->BeginTable("##worst", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        g->TableSetupColumn("When");
        g->TableSetupColumn("Frame");
        g->TableSetupColumn("Slowed by");
        g->TableSetupColumn("That part", ImGuiTableColumnFlags_WidthStretch);
        g->TableHeadersRow();
        for (size_t i = 0; i < sum.worst.size(); i++) {
            const auto& r = sum.worst[i];
            const auto& c = causes[i];
            const double ago = double(now - r.at) / double(truefps::g.frequency);
            const bool severe = r.ms > sum.medianMs * 2.0 && r.ms > sum.medianMs + 4.0;
            std::string parts = truefps::framePartsText(r);
            parts[0] = char(std::toupper(static_cast<unsigned char>(parts[0])));
            char tip[512];
            _snprintf_s(tip, sizeof tip, _TRUNCATE, "%.1f ms, %.1fx a normal frame (%.1f ms), %.0f s ago.\n%s\n%s", double(r.ms), sum.medianMs > 0.0 ? double(r.ms) / sum.medianMs : 0.0,
                        sum.medianMs, ago, parts.c_str(), truefps::slowCauseHelp(c.name));
            g->TableNextRow();
            g->TableNextColumn();
            panelText(g, kColText, "%.0f s ago", ago);
            panelTooltip(g, tip);
            g->TableNextColumn();
            panelText(g, severe ? kColWarn : kColLume, "%.1f ms", double(r.ms));
            panelTooltip(g, tip);
            g->TableNextColumn();
            panelText(g, kColWarn, "%s", c.name);
            panelTooltip(g, tip);
            g->TableNextColumn();
            if (c.ms >= 0.0 && c.usualMs >= 0.0) {   // slowCause sets the two together
                panelText(g, kColText, "%.1f ms", c.ms);
                panelTooltip(g, tip);   // a tooltip covers only the item before it
                g->SameLine();
                g->TextDisabled("(usually %.1f)", c.usualMs);
            } else {
                g->TextDisabled("-");
            }
            panelTooltip(g, tip);
        }
        g->EndTable();
    }
}

void panelRoutines(IGuiManager* g, float s) {
    Section sec(g, s);
    std::string status = smoothStatus();   // every line it returns begins "smooth mode: "
    status[0] = 'S';
    note(g, status.c_str());
    note(g, "A routine is one piece of the game TrueFPS re-times. Smooth: it moves in fractions of a tick. Whole ticks: it still moves, in whole ticks only (two a frame at 30 fps), as the game itself does.");
    sec.begin("Routines");
    if (g->BeginTable("##routines", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 300.0f * s))) {
        g->TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch);
        g->TableSetupColumn("This session", ImGuiTableColumnFlags_WidthStretch);
        g->TableSetupScrollFreeze(0, 1);
        g->TableHeadersRow();
        for (uint8_t i = 0; i < truefps::kGroupCount; i++) {
            const auto& gr = smooth.groups[i];
            g->TableNextRow();
            g->TableNextColumn();
            g->TextWrapped("%s", truefps::kGroups[i].name);
            g->TableNextColumn();
            g->PushTextWrapPos(0.0f);
            if (gr.absent) g->TextColored(kColDim, "%s", gr.why.c_str());
            else if (!gr.found) g->TextColored(kColWarn, "not found%s%s", gr.why.empty() ? "" : ": ", gr.why.c_str());
            else if (!stepResolved) g->TextDisabled("unavailable: TrueFPS has not taken over the game's timing");
            else if (gr.retired) g->TextColored(kColWarn, "off this session: its code ran on another thread");
            else if (gr.failed || gr.stuck) g->TextColored(kColWarn, "whole ticks: %s", gr.why.c_str());
            else if (gr.on) g->TextColored(kColGood, "smooth");
            else if (!smoothOn) g->TextDisabled("game's own (smooth mode off)");
            else if (smoothBlocked()) g->TextColored(kColWarn, "off this session");   // nothing is patched while smooth mode cannot run; the reason is in the line above the table
            else if (i == truefps::GroupPlayer && smoothActive) g->TextDisabled("whole ticks: needs both camera follow routines");
            else g->TextDisabled("ready");
            g->PopTextWrapPos();
        }
        g->EndTable();
    }
    sec.end();
    char footer[400];
    _snprintf_s(footer, sizeof footer, _TRUNCATE, "Settings: %s%s. Log: %s. Build %08X.", underRoot(truefps::settingsPath(root, identity)).c_str(),
                identity.known() ? "" : " (shared, not logged in)", underRoot(fileLog.path()).c_str(), buildStamp());
    note(g, footer);
}

void renderPanel() {
    IGuiManager* g = (core && panelOpen) ? core->GetGuiManager() : nullptr;
    if (!g) {
        panelInactiveSaid = false;
        if (!panelSaidOnce.empty()) panelSaidOnce.clear();   // every frame the window is closed: nothing to do on an empty set
        return;
    }
    const int64_t now = truefps::qpcNow();
    if (!panelSummaryAt || now - panelSummaryAt > truefps::g.frequency / 4) {
        panelSummary = truefps::summarizeFrames(frameLog, now, truefps::g.frequency, 10.0);
        panelSummaryAt = now;
    }
    const float s = panelScale;
    const bool scaledFont = s != 1.0f;
    if (scaledFont) g->PushFont(g->GetFont(), g->GetStyle().FontSizeBase * s);
    const auto* viewport = g->GetMainViewport();
    const ImVec2 available = viewport ? viewport->WorkSize : ImVec2(1920.0f, 1080.0f);
    const ImVec2 maxSize((std::max)(1.0f, available.x - 24.0f), (std::max)(1.0f, available.y - 24.0f));
    const auto fittedSize = truefps::panelWindowSize(s, available.x, available.y);
    const bool resizeForScale = panelSizing.takeResize(s);
    g->SetNextWindowSize(ImVec2(fittedSize.width, fittedSize.height), resizeForScale ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    g->SetNextWindowSizeConstraints(ImVec2((std::min)(720.0f * s, maxSize.x), (std::min)(380.0f * s, maxSize.y)), maxSize);
    if (viewport) g->SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f), ImGuiCond_FirstUseEver);
    g->PushStyleColor(ImGuiCol_Text, kColText);
    char title[48];
    _snprintf_s(title, sizeof title, _TRUNCATE, "TrueFPS v%.1f###truefps_panel", pluginVersion);   // ###: the window keeps its place across versions
    if (g->Begin(title, &panelOpen, 0)) {
        if (resolveFailed) {   // the tabs' settings still save, but nothing reaches the game
            panelText(g, kColBad, "TrueFPS is not active this session (see %s).", logPathText().c_str());
            g->Separator();
        }
        const float readoutWidth = (std::max)(220.0f * s, g->CalcTextSize("Longest frame (1 s)").x + g->CalcTextSize(kReadoutWidest).x + 8.0f + 4.0f * g->GetStyle().CellPadding.x +
                                                         20.0f * s + 3.0f * s + 2.0f + 12.0f);
        const bool stacked = truefps::panelStacked(g->GetContentRegionAvail().x, readoutWidth, s);
        if (g->BeginTable("##layout_v2", stacked ? 1 : 2, ImGuiTableFlags_BordersInnerV)) {   // one id: the selected tab survives stacking
            if (stacked) g->TableSetupColumn("content", ImGuiTableColumnFlags_WidthStretch);
            else {
                g->TableSetupColumn("readout", ImGuiTableColumnFlags_WidthFixed, readoutWidth);
                g->TableSetupColumn("main", ImGuiTableColumnFlags_WidthStretch);
            }
            g->TableNextRow();
            g->TableNextColumn();
            panelReadout(g, s);
            if (stacked) g->TableNextRow();
            g->TableNextColumn();
            g->PushStyleColor(ImGuiCol_TabSelectedOverline, kColSteel);
            if (g->BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_DrawSelectedOverline)) {
                if (g->BeginTabItem("Live")) { panelLive(g, s); g->EndTabItem(); }
                if (g->BeginTabItem("Frame rate")) { panelFrameRate(g, s); g->EndTabItem(); }
                if (g->BeginTabItem("Cutscenes")) { panelCutscenes(g, s); g->EndTabItem(); }
                if (g->BeginTabItem("FPS Counter")) { panelOverlay(g, s); g->EndTabItem(); }
                if (g->BeginTabItem("Frames")) { panelFrames(g, s); g->EndTabItem(); }
                if (g->BeginTabItem("Routines")) { panelRoutines(g, s); g->EndTabItem(); }
                g->EndTabBar();
            }
            g->PopStyleColor();
            g->EndTable();
        }
    }
    g->End();
    g->PopStyleColor();
    if (scaledFont) g->PopFont();
}

std::string stateText(double fps) {
    size_t on = 0;
    for (uint8_t i = 0; i < truefps::kGroupCount; i++) on += smooth.groups[i].on ? 1 : 0;
    char line[320];
    _snprintf_s(line, sizeof line, _TRUNCATE, "state: %.0f fps, rate %s, held %d, background %s (%s), smooth %s%s %zu/%d, cutscene %s%s%s, character %s%s",
                fps, rateText(frameRate).c_str(), heldNowFps(), backgroundCaptureText().c_str(), inBackground ? "another window in front" : "in front",
                smoothOn ? "on" : "off", smoothActive ? " active" : smoothOn ? " waiting" : "", on, int(truefps::groupsPresent(smooth)),
                cutsceneSpeed > 1 ? (std::to_string(cutsceneSpeed) + "x").c_str() : "normal", inCutscene ? " (in one)" : "", zoning ? ", zoning" : "",
                identity.known() ? truefps::characterKey(identity).c_str() : "none", settingsLoading() ? " (settings loading)" : "");
    return line;
}

// "XX XX ... " for up to 16 bytes of code, as the unload and the diagnostics print them.
std::string hexBytes(const uint8_t* b, size_t n) {
    char out[16 * 3 + 1] = "";
    if (n > 16) n = 16;
    for (size_t k = 0; k < n; k++) _snprintf_s(out + k * 3, sizeof out - k * 3, _TRUNCATE, "%02X ", b[k]);
    return std::string(out);
}

void runDiag() {
    std::string body;
    chatCapture = &body;
    struct CaptureScope { ~CaptureScope() { chatCapture = nullptr; } } captureScope;   // a throw below must not leave chatCapture pointing at this frame
    printStatus(true);
    printFrames(true);
    chatCapture = nullptr;
    body += "      character " + (identity.known() ? truefps::characterKey(identity) : std::string("none")) + "\n";
    body += "      " + resolvedLine + "\n";
    for (uint8_t i = 0; i < truefps::kGroupCount; i++) {
        const auto& gr = smooth.groups[i];
        const char* state = gr.absent ? gr.why.c_str() : !gr.found ? "not found" : gr.retired ? "off (ran on another thread)" : gr.failed || gr.stuck ? "whole ticks" : gr.on ? "smooth" : "ready";
        body += "      routine " + std::string(truefps::kGroups[i].name) + ": " + state + (gr.why.empty() || gr.absent ? std::string() : " (" + gr.why + ")") + "\n";
    }
    // Each site that is not as TrueFPS left it: what it is, and what its code holds now.
    for (const auto& site : smooth.sites) {
        if (!site.spec) continue;
        const uint8_t n = site.spec->length < 16 ? site.spec->length : uint8_t(16);
        uint8_t now[16] = {};
        const bool read = truefps::readRaw(site.at, now, n);
        const char* siteNote = read ? truefps::siteStateNote(site, now) : "its code could not be read";
        if (!siteNote) continue;
        const char* pointsAt = read ? truefps::operandWhere(site, now) : nullptr;
        char rva[16];
        _snprintf_s(rva, sizeof rva, _TRUNCATE, "0x%06X", unsigned(site.at - site.imageLo));
        body += "      site " + std::string(site.spec->name) + " at RVA " + rva + ": " + siteNote + (read ? "; holds " + hexBytes(now, n) : std::string()) +
                (pointsAt ? "(its operand is " + std::string(pointsAt) + ")" : std::string()) + "\n";
    }
    std::string settings;
    plog::readText(truefps::settingsPath(root, identity), settings, 65536);   // on the game thread, capped at 64 KB
    body += "      settings file " + underRoot(truefps::settingsPath(root, identity)) + ":\n";
    for (size_t at = 0; at < settings.size();) {
        const size_t nl = settings.find('\n', at);
        body += "        " + settings.substr(at, (nl == std::string::npos ? settings.size() : nl) - at) + "\n";
        at = nl == std::string::npos ? settings.size() : nl + 1;
    }
    char who[64];
    _snprintf_s(who, sizeof who, _TRUNCATE, "TrueFPS %.1f build %08X", pluginVersion, buildStamp());
    fileLog.writeDiag(who, body);
    chat(("diagnostics written to " + logPathText() + ".").c_str());
}

void printHelp() {
    chat("/truefps - open or close the settings window.");
    chat("/truefps limit 60 - hold 60 fps (5 to 1000, max, or off).");
    chat("/truefps smooth on - keep the game at normal speed.");
    chat("/truefps cutscene 3 - play cutscenes 3x faster (off for normal).");
    chat("/truefps background 30 - cap the rate behind other windows.");
    chat("/truefps overlay on - show the fps counter.");
    chat("/truefps status, frames, diag - what it is doing now.");
}

}  // namespace

class TrueFps final : public IPlugin {
public:
    const char* GetName() const override { return "TrueFPS"; }
    const char* GetAuthor() const override { return "SQLCommit"; }
    const char* GetDescription() const override { return "FFXI at normal speed with a precise frame limiter, smooth mode, an fps counter and faster cutscenes."; }
    const char* GetLink() const override { return "https://github.com/SQLCommit/TrueFPS"; }
    double GetVersion() const override { return pluginVersion; }
    uint32_t GetFlags() const override {
        return uint32_t(Ashita::PluginFlags::UseCommands) | uint32_t(Ashita::PluginFlags::UseDirect3D) | uint32_t(Ashita::PluginFlags::UsePackets);
    }

    bool Initialize(IAshitaCore* c, ILogManager*, uint32_t) override {
        const std::string name = "Local\\truefps-sole-instance-" + std::to_string(GetCurrentProcessId());
        soleInstance = CreateMutexA(nullptr, TRUE, name.c_str());
        const DWORD mutexError = GetLastError();   // ERROR_ALREADY_EXISTS with a handle, or why there is none
        if (!soleInstance || mutexError == ERROR_ALREADY_EXISTS) {
            if (soleInstance) { CloseHandle(soleInstance); soleInstance = nullptr; }
            refused = true;
            core = c;
            if (mutexError == ERROR_ALREADY_EXISTS) chat("TrueFPS is already loaded in this client, or an earlier load could not take all of its code back out and stays in memory; restart the game to load it again.", 0x44);
            else chat(("TrueFPS could not create its instance lock (error " + std::to_string(mutexError) + "), so it did not load.").c_str(), 0x44);
            core = nullptr;
            return false;
        }
        core = c;
        // Contain initialization exceptions at the plugin boundary.
        try {
            return initialize();
        } catch (const std::exception& e) {
            return loadFailed(e.what());
        } catch (...) {
            return loadFailed("an unexpected error");
        }
    }
    // No client patches exist before the first frame. Only a writer that fails to stop requires pinning.
    bool loadFailed(const char* what) {
        refused = true;
        try {
            const bool diskStopped = disk.stop(), logStopped = fileLog.stop();   // each waits up to 2 s for a stalled share
            const bool alive = !diskStopped || !logStopped;
            if (alive) pinSelf();   // a writer still running keeps this image mapped
            logPinFailure();
            char line[320];
            _snprintf_s(line, sizeof line, _TRUNCATE, "TrueFPS could not load (%s); %s", what,
                        pinned ? "a file was still being written, so it stays in memory until the game closes; do not load it again before then." : "nothing in the game was changed.");
            chat(line, 0x44);
            // A pinned module may still use the timer.
            if (!pinned) truefps::closeHrTimer();
            if (periodSet) { timeEndPeriod(1); periodSet = false; }
            if (soleInstance && !pinned && !alive) { ReleaseMutex(soleInstance); CloseHandle(soleInstance); soleInstance = nullptr; }
        } catch (...) {
            pinSelf();   // the state is unknown: keep the image mapped rather than guess
            if (periodSet) { timeEndPeriod(1); periodSet = false; }   // the process-wide 1 ms timer period is never left raised
        }
        core = nullptr;
        return false;
    }
    bool initialize() {
        identity = truefps::Identity{};
        emptyPolls = 0;
        packetIdentity = truefps::Identity{};
        _InterlockedExchange(&packetIdentityPending, 0);
        _InterlockedExchange(&packetForget, 0);
        packetKnownOnGame = false;
        { std::lock_guard<std::mutex> lock(loadedMutex); loadedReady.reset(); }
        loadSeq = appliedSeq = 0;
        settingsReadFailed = false;
        truefps::resetHrTimer();
        root = truefps::ashitaRoot(dllPath());
        ownDir = truefps::logsDir(root);   // captures until a character logs in
        ensureDir(ownDir);
        // Merge the startup log into the first character log.
        run = plog::thisRun();
        quietFrom = truefps::qpcNow();   // no long-frame warning in the 30 s after a load
        fileLog.open(root, plog::startupLogPath(root, "truefps", run));
        char ver[16], iface[16];
        _snprintf_s(ver, sizeof ver, _TRUNCATE, "%.1f", pluginVersion);
        _snprintf_s(iface, sizeof iface, _TRUNCATE, "%.2f", ASHITA_INTERFACE_VERSION);
        const std::string session = plog::sessionText("TrueFPS", ver, buildStamp(), clientBuildStamp(), iface, run);
        fileLog.setSession(session, run);
        fileLog.write("info", session);
        fileLog.start();
        {
            const std::string rootCopy = root;
            const plog::Run runCopy = run;
            fileLog.post([rootCopy, runCopy] {
                plog::deleteFiles(rootCopy, {"logs\\truefps\\truefps.log", "logs\\truefps\\truefps.log.old", "logs\\truefps\\truefps-frames.csv"});
                plog::deleteInCharacterFolders(rootCopy, "truefps", "truefps.log.old");
                plog::cleanupStartupFiles(rootCopy, "truefps", runCopy);
            });
        }
        char hello[160];
        if (const unsigned onDisk = diskBuildStamp(); onDisk && onDisk != buildStamp()) {   // the old image was mapped again
            _snprintf_s(hello, sizeof hello, _TRUNCATE, "the running build is %08X, the file on disk is %08X", buildStamp(), onDisk);
            logLine(Ashita::LogLevel::Warn, hello);
            chat("the game still runs the TrueFPS it loaded before, not the newer one on disk. Close the game and start it again to run the new build.", 0x68);
        }
        {   // the shared settings, or the legacy file beside the DLL
            const std::string source = activeSettingsPath();
            resetSettingsToDefaults();
            loadSettingsFrom(source, true);
            ensureSettingsDir();
            logLine(Ashita::LogLevel::Info, ("settings: " + underRoot(source)).c_str());
            applyLoadedSettings();   // the loaded position through the window clamp, before the first frame draws the counter
        }
        disk.start();
        if (!retiredPresent.empty()) saveSettings();   // tidies keys an earlier build left behind
        truefps::g_truefpsFreezeSkip[0] = fileLog.threadId();
        truefps::g_truefpsFreezeSkip[1] = disk.threadId();
        truefps::g.waiting = true;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        truefps::g.frequency = f.QuadPart;
        timeBeginPeriod(1);  // Sleep(1) far from the deadline should be about 1 ms
        cyclesPerMs = measureCyclesPerMs();
        periodSet = true;
        {
            const double late = truefps::checkHrTimer();
            char line[200];
            if (late < 0.0) _snprintf_s(line, sizeof line, _TRUNCATE, "wait timer: no high-resolution timer on this system; waits use Sleep(1) with a 3 ms margin");
            else if (truefps::timerPrecise(late)) _snprintf_s(line, sizeof line, _TRUNCATE, "wait timer: high-resolution (a %.1f ms test wait woke %.2f ms late at best)", truefps::kTimerCheckMs, late);
            else _snprintf_s(line, sizeof line, _TRUNCATE, "wait timer: a %.1f ms test wait woke %.2f ms late at best, so the timer is not used; waits use Sleep(1) with a 3 ms margin", truefps::kTimerCheckMs, late);
            logLine(Ashita::LogLevel::Info, line);
        }
        std::string why;
        if (!resolve(why)) {
            resolveFailed = true;
            refusedForGood = true;
            std::string text = "not active: " + why + ". The game's frame rate is untouched.";
            logLine(Ashita::LogLevel::Error, text.c_str());
            chat(text.c_str(), 0x44);
        }
        chat("\x11" "/truefps" "\x12" " opens or closes the settings   (or " "\x11" "/tf" "\x12" ").");   // once per load; chat() logs it too
        return true;
    }

    // Under /EHa, Release must also catch access violations and pin the DLL: partial cleanup
    // may leave live patches or writers. Normal callbacks let access violations propagate.
    void Release() override {
        try {
            releaseNow();
        } catch (...) {
            // Isolate cleanup steps so one failure does not skip the rest. The persistent cells come first: another
            // tool may point the client's code back at one after this module is gone (no allocation here).
            try { truefps::restorePersistentCells(); } catch (...) {}
            try { pinSelf(); } catch (...) {}
            try { logPinFailure(); } catch (...) {}
            try { if (onDrawThread()) destroyFont(); } catch (...) {}
            try { disk.stop(); } catch (...) {}
            try { fileLog.stop(); } catch (...) {}
            if (periodSet) { timeEndPeriod(1); periodSet = false; }   // the process-wide 1 ms timer period is never left raised
            try { chat("could not finish unloading; its changes and its code stay in until you close the game.", 0x44); } catch (...) {}
            try { logLine(Ashita::LogLevel::Error, "unload: releaseNow did not finish; the image is kept"); } catch (...) {}
            core = nullptr;
        }
    }
    void releaseNow() {
        if (refused) return;
        auto& g = truefps::g;
        // First of all: the persistent cells hold the client's own constant from here on, whether or not a site still
        // names one. Another tool can write one of those addresses back long after this module is gone.
        truefps::restorePersistentCells();
        g.waiting = false;
        g.speed = 1.0f;
        truefps::smoothClockOff();
        // Restore a native divisor on unload; otherwise cap at 60. Preserve uncapped only without smooth mode.
        if (limitState.applied) {
            const int d = truefps::divisorForLimit(rateNowFps());
            writeDivisor((d > 0 || (d == 0 && !smoothOn)) ? d : 1);
        }
        // Do not delete fonts off the drawing thread; that unload path retains the DLL.
        if (onDrawThread()) destroyFont();
        else if (font) logLine(Ashita::LogLevel::Warn, "unload: the fps counter is left on screen; it can only be taken down on the drawing thread");
        const bool fontLeft = font != nullptr;   // nothing else takes it down: the off-thread chat below says so

        // Restore patches only while other threads are outside the DLL and all patched spans.
        // Exclude our writers from range checks; no allocation while frozen.
        // An adopted site counts: the other tool may have handed it back holding TrueFPS's own slot, to be restored.
        const bool anything = codeInstalled || g.timer || truefps::anyJumpLeft(stepSites, 2) || moveSite.patched || moveSite.dirty || truefps::anySitePatched(smooth) ||
                              truefps::anyAdoptedSite(smooth);
        bool quiet = !anything, smoothOk = true, moveOk = true, stepOk = true, limiterOk = true;
        bool keptPath1 = false, pagesFailed = false;   // pagesFailed: nothing was tried, and no thread was ever in the way
        char why[600] = "";
        if (anything) {
            constexpr size_t kRanges = 96;   // the DLL, 4 step spans, movement, loop, every smooth site, entity path 1
            truefps::CodeRange ranges[kRanges];
            size_t n = 0;
            const auto add = [&](uintptr_t lo, size_t len) { if (lo && n < kRanges) ranges[n++] = truefps::CodeRange{lo, lo + len}; };
            uintptr_t lo = 0, hi = 0;
            if (selfImage(lo, hi)) add(lo, hi - lo);
            else ranges[n++] = truefps::CodeRange{0, UINTPTR_MAX};   // the DLL's span is unknown: treat every thread as busy
            for (const auto& site : stepSites) { add(site.at, 5); add(site.pad, 5); }
            if (moveSite.patched || moveSite.dirty) add(moveSite.at, 5);
            if (codeInstalled) add(loopSite, 6);
            n += truefps::unloadSiteRanges(smooth, ranges + n, kRanges - n);
            truefps::CodeRange span;
            if (n < kRanges && truefps::entityPath1Span(smooth, span)) ranges[n++] = span;
            const DWORD writers[2] = {fileLog.threadId(), disk.threadId()};
            const uintptr_t moveTarget = reinterpret_cast<uintptr_t>(&truefps::moveStub);
            // Open only spans written below. Read-only thread-check ranges must not become writable.
            truefps::CodeRange writes[kRanges];
            size_t w = 0;
            const auto addWrite = [&](uintptr_t at, size_t len) { if (at && w < kRanges) writes[w++] = truefps::CodeRange{at, at + len}; };
            for (const auto& site : stepSites) addWrite(site.at, 5);   // the head only: the act swaps that back (removeJumps) and never writes the pad
            if (moveSite.patched || moveSite.dirty) addWrite(moveSite.at, 5);
            if (codeInstalled) addWrite(loopSite, 6);
            w += truefps::unloadSiteRanges(smooth, writes + w, kRanges - w);
            truefps::OpenPages pages(writes, w);
            if (!pages.ok()) {
                pagesFailed = true;
                _snprintf_s(why, sizeof why, _TRUNCATE, "the pages holding the patched code could not be opened for writing (error %lu)",
                            static_cast<unsigned long>(pages.error()));
            } else {
                quiet = truefps::whenNoThreadIn(ranges, n, writers, 2, 1000, [&] {
                    smoothOk = truefps::removeAllSitesFrozen(smooth, truefps::writeCode, &keptPath1);
                    moveOk = truefps::removeCall(moveSite, moveTarget);
                    // Retained effect readers still need the step accessors to populate cached values.
                    if (truefps::effectsReadersPatched(smooth)) stepOk = false;
                    else stepOk = !truefps::anyJumpLeft(stepSites, 2) || truefps::removeJumps(stepSites, 2, stubAddress()) == truefps::PatchResult::Done;
                    if (codeInstalled && truefps::retargetSleepSlot(loopSite, uint32_t(reinterpret_cast<uintptr_t>(&g.sleepTarget)), originalSleepImm)) codeInstalled = false;
                    // Check before restoreTimer clears g.timer.
                    noteOrphanedTimer();
                    limiterOk = !codeInstalled && truefps::restoreTimer(currentTimer());
                    // Last, with every other thread held and the limiter's own patch out: no frame can put a paced
                    // value back into the cells after this (allocation- and log-free, as this act requires).
                    truefps::restorePersistentCells();
                }, why, sizeof why, &truefps::g_truefpsStepInflight);
            }
        }
        const bool othersOk = smoothOk && moveOk && stepOk && limiterOk;
        const bool restored = quiet && othersOk && !keptPath1;
        // A site another tool took over is not stuck and needs no pin - its cell outlives this module and holds the
        // client's own constant - but a value is left as that tool set it, so the report must not say everything is
        // undone. One or more: `anyAdoptedSite` does not count them, and the wording does not either. Read after the
        // removal, which restored every adopted site the other tool had handed back.
        const bool adoptedSlot = truefps::anyAdoptedSite(smooth);
        if (!quiet) {
            logLine(Ashita::LogLevel::Error, pagesFailed
                        ? "unload: the pages holding the patched code could not be made writable, so nothing was taken out; TrueFPS's changes stay in, running the game's own arithmetic, until the game closes"
                        : "unload: no moment without a game thread in TrueFPS's code or a patched site came within a second; TrueFPS's changes stay in, running the game's own arithmetic, until the game closes");
            logLine(Ashita::LogLevel::Error, (std::string(pagesFailed ? "unload: reason: " : "unload: last try: ") + why).c_str());
        } else {
            if (keptPath1)
                logLine(Ashita::LogLevel::Warn, "unload: remote-entity path 1 stays in (its routine ran on another thread, so a thread may be part-way through it); with the clock off it runs the game's own arithmetic, and TrueFPS stays in memory until the game closes");
            if (!othersOk) {
                // The outcome only: what each smooth site left in holds, and why, is on its own line below.
                char line[200];
                _snprintf_s(line, sizeof line, _TRUNCATE, "unload: some of TrueFPS's changes could not be taken back out (smooth sites %s, movement %s, step %s, limiter %s); it stays in memory until the game closes",
                            smoothOk ? "ok" : "left", moveOk ? "ok" : "left", stepOk ? "ok" : "left", limiterOk ? "ok" : "left");
                logLine(Ashita::LogLevel::Error, line);
                // Each smooth site left in: its name, what it holds now (and where an operand there points) against what
                // TrueFPS wrote and what it found.
                for (const auto& site : smooth.sites) {
                    if (!site.spec || !site.patched) continue;
                    const uint8_t n = site.spec->length < 16 ? site.spec->length : uint8_t(16);
                    uint8_t now[16] = {};
                    const bool read = truefps::readRaw(site.at, now, n);
                    const char* pointsAt = read ? truefps::operandWhere(site, now) : nullptr;
                    const std::string held = read ? hexBytes(now, n) : std::string("unreadable ");
                    const std::string wrote = hexBytes(site.patch, n), found = hexBytes(site.original, n);
                    char detail[400];
                    _snprintf_s(detail, sizeof detail, _TRUNCATE, "unload: left in: %s at RVA 0x%06X%s%s; holds %s%s%s%s| TrueFPS wrote %s| it found %s",
                                site.spec->name, unsigned(site.at - site.imageLo), site.kept ? " (kept)" : "",
                                site.unsafeOriginal ? " (the address it found no longer holds the game's value)" : "", held.c_str(), pointsAt ? "(" : "", pointsAt ? pointsAt : "",
                                pointsAt ? ") " : "", wrote.c_str(), found.c_str());
                    logLine(Ashita::LogLevel::Error, detail);
                }
            }
            if (restored && anything && !adoptedSlot) logLine(Ashita::LogLevel::Info, "unload: every change TrueFPS made to the game is undone");
            if (restored && adoptedSlot)
                logLine(Ashita::LogLevel::Info, "unload: every change TrueFPS made to the game is undone except a value another tool now owns; the pointer that tool saved is a cell in a page that outlives TrueFPS, and it reads the game's own value");
        }
        if (orphanedTimer) logLine(Ashita::LogLevel::Warn, "unload: the client had replaced its frame timer; the old one may still use TrueFPS's copy, which stays valid (TrueFPS stays mapped)");
        logSiteChanges();
        logRetryPacing();
        // Off-thread unload must retain the DLL: a draw callback may still be inside a Windows call.
        const DWORD releaseThread = GetCurrentThreadId();
        const bool offThread = drawThread && releaseThread != drawThread;
        if (offThread) {
            char line[160];
            _snprintf_s(line, sizeof line, _TRUNCATE, "unload: called on thread %lu, not the drawing thread %lu; TrueFPS stays in memory until the game closes", releaseThread, drawThread);
            logLine(Ashita::LogLevel::Warn, line);
        }
        // Pin if the client may still enter this module. An adopted value is not a reason (its cell outlives us).
        const bool mustStay = truefps::unloadMustStay(restored, orphanedTimer, offThread);
        if (mustStay && !pinned) pinSelf();
        logPinFailure();
        // Give each writer two seconds to drain; pin the DLL if either outlives unload.
        if (presentFailures) logLine(Ashita::LogLevel::Warn, ("frame callbacks that failed this session: " + std::to_string(presentFailures)).c_str());
        if (packetFailures) logLine(Ashita::LogLevel::Warn, ("packet callbacks that failed this session: " + std::to_string(packetFailures)).c_str());
        const bool diskStopped = disk.stop();
        showDiskNotices();   // what that last flush wrote or could not write: no frame follows to show it
        logLine(diskStopped ? Ashita::LogLevel::Info : Ashita::LogLevel::Warn,
                (std::string(diskStopped ? "unloaded" : "unloaded; the settings writer was still busy and finishes by itself") + plog::runSuffix(run)).c_str());
        const bool logStopped = fileLog.stop();
        const bool writerAlive = !diskStopped || !logStopped;
        if (writerAlive && !pinned && !pinSelf())   // the log is closed by now, so the player hears it here
            chat("a file was still being written and TrueFPS could not keep itself in memory for it; do not load TrueFPS again until the game restarts.", 0x44);
        if (anything && !restored) chat("could not fully undo its changes on unload; they stay in until you close the game (the game runs normally).", 0x44);
        else if (pinned) chat(orphanedTimer ? "its changes are undone, but a frame timer it switched may still use its code, so it stays in memory until the game closes."
                              : offThread   ? (fontLeft ? "its changes are undone; it was unloaded from an unexpected thread, so it stays in memory until the game closes and the fps counter stays on screen."
                                                        : "its changes are undone; it was unloaded from an unexpected thread, so it stays in memory until the game closes.")
                                            : "its changes are undone; a file was still being written, so it stays in memory until the game closes.", 0x6A);
        // Said whatever else was said above: a pin for an unrelated reason must not hide it.
        if (restored && adoptedSlot) chat("a value another tool set is left as that tool set it.", 0x6A);
        // Retain the timer while pinned code can still run.
        if (!pinned) truefps::closeHrTimer();
        if (periodSet) { timeEndPeriod(1); periodSet = false; }
        // Keep the instance mutex while patches, writers or a pin survive; refuse reloading over them.
        if (soleInstance && !mustStay && !pinned && !writerAlive) { ReleaseMutex(soleInstance); CloseHandle(soleInstance); soleInstance = nullptr; }
        core = nullptr;
    }

    bool handleCommand(const char* command) {
        if (!command) return false;
        noteCallbackThread("command");
        std::vector<std::string> args;
        Ashita::Commands::GetCommandArgs(command, &args);
        if (args.empty() || !Ashita::Commands::CommandCheck(args[0], {"/truefps", "/tf"})) return false;
        commandOurs = true;
        const std::string sub = args.size() >= 2 ? lower(args[1]) : "";
        const std::string val = args.size() >= 3 ? lower(args[2]) : "";
        const auto usage = [](const char* text) { chat(text, 0x68); };
        if (sub.empty()) {
            panelOpen = !panelOpen;   // showing or hiding the settings says nothing in chat
        } else if (sub == "status") {
            printStatus();
        } else if (sub == "limit" || sub == "fps" || sub == "rate") {
            int fps = 0;
            if (args.size() == 2) { chat(("frame rate " + rateReportText(frameRate) + ".").c_str()); chat(kRateUsage, 0x68); }
            else if (args.size() == 3 && (val == "off" || val == "uncapped")) toChat(opRate(truefps::kRateUncapped));
            else if (args.size() == 3 && val == "max") toChat(opRate(truefps::kRateMax));
            else if (args.size() == 3 && truefps::parseIntArg(val, truefps::kRateMinFps, truefps::kRateMaxFps, fps)) toChat(opRate(fps));
            else usage(kRateUsage);
        } else if (sub == "background" || sub == "bg") {
            int fps = 0;
            if (args.size() == 2) { chat(backgroundText().c_str()); chat(kBackgroundUsage, 0x68); }
            else if (args.size() == 3 && val == "off") toChat(opBackground(truefps::kBackgroundOff));
            else if (args.size() == 3 && (val == "uncapped" || val == "unlimited")) toChat(opBackground(truefps::kBackgroundUncapped));
            else if (args.size() == 3 && val == "max") toChat(opBackground(truefps::kRateMax));
            else if (args.size() == 3 && truefps::parseIntArg(val, truefps::kBackgroundMinFps, truefps::kBackgroundMaxFps, fps)) toChat(opBackground(fps));
            else usage(kBackgroundUsage);
        } else if (sub == "ui") {
            if (args.size() == 2) panelOpen = !panelOpen;
            else usage("usage: /truefps ui  (shows or hides the settings).");
        } else if (sub == "overlay") {
            const std::string val2 = args.size() >= 4 ? lower(args[3]) : "";
            if (args.size() == 2) { chat(overlayText().c_str()); chat(kOverlayUsage, 0x68); }
            else if (args.size() == 3 && (val == "on" || val == "off")) toChat(opOverlay(val == "on"));
            else if (args.size() == 3 && val == "lock") { chat((std::string("the fps counter is ") + (overlayLocked ? "locked" : "unlocked") + ".").c_str()); chat(kOverlayUsage, 0x68); }
            else if (args.size() == 4 && val == "lock" && (val2 == "on" || val2 == "off")) toChat(opOverlayLock(val2 == "on"));
            else if (args.size() == 3 && val == "detail") { chat((std::string("the fps counter's longest frame is ") + (overlayDetail ? "on" : "off") + ".").c_str()); chat(kOverlayUsage, 0x68); }
            else if (args.size() == 4 && val == "detail" && (val2 == "on" || val2 == "off")) toChat(opOverlayDetail(val2 == "on"));
            else if (args.size() == 3 && val == "bands") { chat((std::string("fps counter colour threshold ") + (overlayBands ? "on" : "off") + ".").c_str()); chat(kOverlayUsage, 0x68); }
            else if (args.size() == 4 && val == "bands" && (val2 == "on" || val2 == "off")) toChat(opOverlayBands(val2 == "on"));
            else usage(kOverlayUsage);
        } else if (sub == "cutscene" || sub == "cs") {
            int speed = 0;
            if (args.size() == 2) {
                OpResult r;
                r.add(cutsceneSpeedText());
                addCutsceneCaveats(r);   // the same caveats the value form gives: the setting alone does not say whether it applies
                toChat(r, false);   // a report, not a change: no inactive note, as every other no-value arm
                chat(kCutsceneUsage, 0x68);
            }
            else if (args.size() == 3 && val == "off") toChat(opCutsceneSpeed(1));
            else if (args.size() == 3 && truefps::parseIntArg(val, 1, 6, speed)) toChat(opCutsceneSpeed(speed));
            else usage(kCutsceneUsage);
        } else if (sub == "smooth") {
            if (args.size() == 2) { chat(smoothStatus().c_str(), smoothOn && !smoothUnavailable().empty() ? 0x68 : 0x6A); chat(kSmoothUsage, 0x68); }
            else if (args.size() == 3 && (val == "on" || val == "off")) toChat(opSmoothOn(val == "on"));
            else usage(kSmoothUsage);
        } else if (sub == "frames") {
            if (args.size() == 2) printFrames();
            else if (args.size() == 3 && val == "save") toChat(opSaveFrames(), false);   // a capture works on any client build
            else usage("usage: /truefps frames [save].");
        } else if (sub == "help") {
            printHelp();
        } else if (sub == "diag") {
            if (args.size() == 2) runDiag();
            else usage("usage: /truefps diag.");
        } else {
            chat("unknown command. /truefps help lists them.", 0x68);
        }
        return true;
    }
    // Catch C++ exceptions only. Under /EHa, catch (...) would also swallow access violations.
    bool HandleCommand(int32_t, const char* command, bool) override {
        commandOurs = false;
        try {
            return handleCommand(command);
        } catch (const std::exception& e) {
            commandFailed(e.what());
        }
        return commandOurs;   // a throw before the command was recognised as ours leaves it to whoever owns it
    }
    // Reporting must not throw from an exception handler.
    void commandFailed(const char* what) {
        if (!commandOurs) return;
        try {
            char line[200];
            _snprintf_s(line, sizeof line, _TRUNCATE, "the command failed (%s); it may not have been applied or saved.", what);
            chat(line, 0x44);
            logLine(Ashita::LogLevel::Error, line);
        } catch (...) {}
    }

    // Report C++ exceptions once and pass the packet through. Let access violations propagate.
    bool HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t*, uint32_t, const uint8_t*, bool injected, bool) override {
        try {
            incomingPacket(id, size, data, injected);
        } catch (const std::exception& e) {
            packetFailed(e.what());
        }
        return false;
    }
    void incomingPacket(uint16_t id, uint32_t size, const uint8_t* data, bool injected) {
        if (injected) return;
        if (id == 0x000A || id == 0x000B) noteCallbackThread("packet");
        if (id == 0x000A) {
            zoning = false;  // zone-in finished
            if (_InterlockedExchange(&packetForget, 0)) packetIdentity = truefps::Identity{};
            truefps::Identity who;
            if (truefps::identityFromZoneIn(data, size, who) && who != packetIdentity) {   // a login or a switch; a plain zone-in repeats the same character
                packetIdentity = who;
                std::memset(packetName, 0, sizeof packetName);
                std::memcpy(packetName, who.name.c_str(), (std::min)(who.name.size(), size_t(16)));
                packetServerId = who.serverId;
                _InterlockedExchange(&packetIdentityPending, 1);
            }
        } else if (id == 0x000B && truefps::logoutFromZoneOut(data, size)) {
            zoning = false;  // the zone was left for good: no zone-in follows
            packetIdentity = truefps::Identity{};
            _InterlockedExchange(&packetIdentityPending, 2);
        }
    }
    bool HandleOutgoingPacket(uint16_t id, uint32_t, const uint8_t*, uint8_t*, uint32_t, const uint8_t*, bool, bool) override {
        if (id == 0x000D) zoning = true;  // leaving the zone
        return false;
    }
    // Report once and count repeats; do not throw from the handler.
    void packetFailed(const char* what) {
        if (_InterlockedIncrement(&packetFailures) != 1) return;
        try {
            char line[200];
            _snprintf_s(line, sizeof line, _TRUNCATE, "a packet callback failed (%s); TrueFPS carries on.", what);
            logLine(Ashita::LogLevel::Error, line);
        } catch (...) {}
    }

    bool Direct3DInitialize(IDirect3DDevice8* device) override { return device != nullptr; }

    // The first scene starts draw timing; the last scene ends it.
    void Direct3DBeginScene(bool) override {
        const int64_t now = truefps::qpcNow();
        if (sceneFirstBegin <= truefps::g.lastReset) { sceneFirstBegin = now; sceneCount = 0; }
        if (sceneCount < 0xFFFF) ++sceneCount;
    }
    void Direct3DEndScene(bool) override { sceneLastEnd = truefps::qpcNow(); }

    // Let access violations propagate; swallowing one could repeat the fault with patches still installed.
    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override {
        try {
            present();
        } catch (const std::exception& e) {
            presentFailed(e.what());
        }
    }
    // Rate-limit reports; do not throw from the handler.
    void presentFailed(const char* what) {
        const unsigned n = ++presentFailures;
        if (n > 10 && n % 1000 != 0) return;
        try {
            char line[200];
            _snprintf_s(line, sizeof line, _TRUNCATE, "a frame callback failed (%s); TrueFPS carries on (failure %u).", what, n);
            if (n == 1) chat(line, 0x44);
            logLine(Ashita::LogLevel::Error, line);
        } catch (...) {}
    }
    void present() {
        auto& g = truefps::g;
        drawThread = GetCurrentThreadId();
        recordFrame();
        if (!pendingNotice.empty()) {   // taken first: a chat that throws must not say it again every frame
            const std::string notice = std::move(pendingNotice);
            pendingNotice.clear();
            chat(notice.c_str(), pendingColor);
        }
        showDiskNotices();
        if (sampledFrame) {   // two user32 calls, every 16th frame: a background switch is seen within 0.1 s at 175 fps
            const HWND wnd = core && core->GetProperties() ? core->GetProperties()->GetFinalFantasyHwnd() : nullptr;
            const bool behind = wnd && (GetForegroundWindow() != wnd || IsIconic(wnd));
            if (behind != inBackground && backgroundFps != truefps::kBackgroundOff)
                logLine(Ashita::LogLevel::Info, behind ? ("background: another window is in front, now " + backgroundNowText()).c_str() : "background: the game window is in front again");
            inBackground = behind;
        }

        static int64_t oneNumberLastQpc = 0;
        const int64_t oneNumberQpc = truefps::qpcNow();
        const double lastFrameSeconds = (oneNumberLastQpc && g.frequency) ? double(oneNumberQpc - oneNumberLastQpc) / double(g.frequency) : 0.0;
        oneNumberLastQpc = oneNumberQpc;

        // Pace the caret only after validating its window class. Without smooth mode, retain native counting.
        if (caretResolved && truefps::g.frameSmooth) {
            uint32_t obj = 0, vtable = 0, draw = 0;
            int32_t counter = 0;
            if (truefps::readValue(caretSite.slot, obj) && obj && truefps::readValue(obj, vtable) && vtable &&
                truefps::readValue(uintptr_t(vtable) + 0x10, draw) && caretSite.timer >= draw && caretSite.timer - draw < truefps::kCaretDrawReach &&
                truefps::readValue(uintptr_t(obj) + truefps::kCaretCounterOffset, counter)) {
                const int32_t phase = truefps::caretAdvance(caretClock, counter, lastFrameSeconds * truefps::kNativeFramesPerSecond);   // native 30 fps frames
                truefps::writeRaw(uintptr_t(obj) + truefps::kCaretCounterOffset, &phase, sizeof phase);
            }
        } else if (caretClock.started) {
            caretClock = truefps::CaretClock{};   // smooth mode off: the client counts again
        }
        // Validate each cursor's window class before writing.
        for (size_t i = 0; i < truefps::kMenuCursorCount; i++) {
            const truefps::MenuCursorSite& site = menuCursorSites[i];
            truefps::MenuCursorClock& mc = menuCursorClocks[i];
            uint32_t obj = 0, vtable = 0, counter = 0;
            const bool live = truefps::g.frameSmooth && site.ok() && truefps::readValue(site.slot, obj) && obj && truefps::readValue(obj, vtable) &&
                              vtable == site.vtable && truefps::readRaw(uintptr_t(obj) + site.offset, &counter, site.width);
            if (!live) { mc = truefps::MenuCursorClock{}; continue; }
            if (mc.object != obj) mc = truefps::MenuCursorClock{obj, {}};
            const uint32_t phase = uint32_t(truefps::phaseAdvance(mc.clock, counter, lastFrameSeconds * truefps::kNativeFramesPerSecond, truefps::kMenuCursorPeriod));
            truefps::writeRaw(uintptr_t(obj) + site.offset, &phase, site.width);   // little-endian: a WORD takes the low half
        }

        // Only extend countdowns while in the shutdown state.
        {
            uint32_t app = 0, state = 0;
            int32_t count = 0, held = 0;
            if (shutdownWaitResolved && truefps::g.appSlot && truefps::readValue(truefps::g.appSlot, app) && app &&
                truefps::readValue(uintptr_t(app) + truefps::kAppState, state) && state == truefps::kAppStateShutdown &&
                truefps::readValue(uintptr_t(app) + truefps::kAppShutdownCount, count)) {
                if (truefps::pacedCountdown(shutdownWait, count, lastFrameSeconds, truefps::kSafetyPollsPerSecond, &held))
                    truefps::writeRaw(uintptr_t(app) + truefps::kAppShutdownCount, &held, sizeof held);
            } else {
                shutdownWait = truefps::PacedCountdown{};
            }
        }
        if (cameraGraceSlot && truefps::g.frameSmooth) {
            int32_t grace = 0, held = 0;
            if (truefps::readValue(cameraGraceSlot, grace) && truefps::pacedCountdown(cameraGrace, grace, lastFrameSeconds, truefps::kNativeFramesPerSecond, &held))
                truefps::writeRaw(cameraGraceSlot, &held, sizeof held);
        } else {
            cameraGrace = truefps::PacedCountdown{};
        }

        if (watchdogResolved) {
            uint32_t counter = 0, corrected = 0;
            const bool wasWaiting = watchdogWaiting;
            if (truefps::readValue(watchdogSite.counter, counter)) {
                if (truefps::watchdogCorrection(watchdogClock, counter, lastFrameSeconds, watchdogSite.limit, &corrected) &&
                    truefps::writeRaw(watchdogSite.counter, &corrected, sizeof corrected))
                    counter = corrected;
                watchdogWaiting = watchdogClock.waiting;
                if (wasWaiting && !watchdogWaiting) {
                    watchdogLastWait = watchdogClock.longest;
                    watchdogLastWaitAt = truefps::qpcNow();
                    char line[200];
                    _snprintf_s(line, sizeof line, _TRUNCATE, "the client waited %.1f s for the server (counter %u of %u; kept on a clock)",
                                watchdogLastWait, unsigned(counter), watchdogSite.limit);
                    logLine(Ashita::LogLevel::Info, line);
                    watchdogClock.longest = 0.0;
                }
            }
        }

        // Keep diagnostics available even when client routines do not resolve.
        if (!resolveFailed) {
            if (!refusedForGood) installLimiter();
            governSmooth();
            updateCutscene();
            {   // the cutscene move help (smooth.h, eventMoveValue): said in the log, at most once a second
                static long loggedBoosts = 0;
                static ULONGLONG loggedAt = 0;
                const long boosts = truefps::g_truefpsEventBoosts;
                if (boosts != loggedBoosts && GetTickCount64() - loggedAt >= 1000) {
                    char line[200];
                    _snprintf_s(line, sizeof line, _TRUNCATE, "cutscene: an actor's move made no progress for half a second and got a bigger step (%ld helped moves this session)", boosts);
                    logLine(Ashita::LogLevel::Info, line);
                    loggedBoosts = boosts;
                    loggedAt = GetTickCount64();
                }
            }
            // Keep step patches throughout the cutscene; menus pause only the multiplier.
            const bool cutsceneStep = inCutscene && !zoning && cutsceneSpeed > 1;
            const bool speedUp = cutsceneStep && !menuPaused;
            const bool needStep = cutsceneStep || smoothActive;
            if (needStep && !installStep()) {
                smoothActive = false;
                static uint64_t waitingLogs = 0;
                if (waitingLogs++ < 3) logLine(Ashita::LogLevel::Warn, "the game step patch is not in; smooth mode and the cutscene speed-up are off");
            }
            const uintptr_t moveTarget = reinterpret_cast<uintptr_t>(&truefps::moveStub);
            if (smoothActive && stepInstalled()) {
                // Verify movement patches on sampled frames, detecting failed removal within 16 frames.
                if (truefps::installCall(moveSite, moveTarget, truefps::writeCode, sampledFrame)) moveWriteFails = 0;
                else {
                    smoothActive = false;
                    // Foreign bytes are permanent failures. Bound write retries to avoid
                    // installing and removing step patches every frame indefinitely.
                    const bool foreign = !moveSite.dirty && !truefps::bytesAre(moveSite.at, moveSite.original, 5);   // bytes a failed write left there are truefps's own
                    moveStuck = foreign || ++moveWriteFails >= kMoveWriteTries;
                    if (moveStuck && !moveLogged) {
                        moveLogged = true;
                        if (foreign) logLine(Ashita::LogLevel::Error, "smooth mode: the player movement site holds code TrueFPS did not write; smooth mode is off this session");
                        else {
                            char line[200];
                            _snprintf_s(line, sizeof line, _TRUNCATE, "smooth mode: the player movement patch could not be written on %d frames in a row; smooth mode is off this session", kMoveWriteTries);
                            logLine(Ashita::LogLevel::Error, line);
                        }
                    }
                }
            }
            const bool smoothRunning = smoothActive && stepInstalled() && moveSite.patched;
            if (!smoothRunning) {
                // Stop the clock before removing routine patches, then movement.
                if (g.smoothStep || g.frameSmooth || smooth.live) truefps::smoothClockOff();
                if (smooth.live || truefps::anySitePatched(smooth)) removeSmoothPatches();
                if ((moveSite.patched || moveSite.dirty) && !truefps::removeCall(moveSite, moveTarget) && !moveRemoveLogged) {
                    moveRemoveLogged = true;
                    const char* why = "smooth mode: the player movement site holds code TrueFPS did not write";
                    if (moveSite.dirty && truefps::bytesAre(moveSite.at, moveSite.left, 5))
                        why = "smooth mode: a part-written player movement patch could not be taken back out; TrueFPS tries again every frame";
                    else if (truefps::callSiteHolds(moveSite, moveTarget))
                        why = "smooth mode: the player movement patch could not be taken back out (the write failed); TrueFPS tries again every frame";
                    logLine(Ashita::LogLevel::Error, why);
                }
            } else {
                g.smoothStep = true;
                const StuckBefore stuckBefore;
                truefps::runSmoothPatches(smooth, &logGroup);
                stuckBefore.sayCleared("goes back in");
                logSiteChanges();
            }
            truefps::g_truefpsSmoothMove = (g.smoothStep && g.lastFrameTicks > 0.0f) ? 1 : 0;
            g.speed = (speedUp && stepInstalled()) ? float(cutsceneSpeed) : 1.0f;
            // Retained effect readers require the step patch to keep filling their cache.
            if (!truefps::stepPatchWanted(cutsceneStep, smoothActive, smooth) && !stepStuck && truefps::anyJumpLeft(stepSites, 2)) uninstallStep();
            applyLimit();
            if (g.offThreadSteps > 0 && (g.lastOffThreadRet != lastLoggedOffThreadRet || g.lastOffThreadTid != lastLoggedOffThreadTid) && offThreadLogs < 10) {
                lastLoggedOffThreadRet = g.lastOffThreadRet;
                lastLoggedOffThreadTid = g.lastOffThreadTid;
                ++offThreadLogs;
                char line[200];
                _snprintf_s(line, sizeof line, _TRUNCATE, "the game step was read from thread %lu, not the game's, returning to RVA 0x%06X (%ld such reads so far)",
                            static_cast<unsigned long>(lastLoggedOffThreadTid), unsigned(lastLoggedOffThreadRet - client.base), long(g.offThreadSteps));
                logLine(Ashita::LogLevel::Warn, line);
            }

        }

        const int64_t now = truefps::qpcNow();
        ++windowFrames;
        bool newSecond = false;
        if (!windowStart) { windowStart = now; spinStartCounts = g.spinCounts; }
        if (now - windowStart >= g.frequency) {
            const double seconds = double(now - windowStart) / double(g.frequency);
            lastFps = double(windowFrames) / seconds;
            lastSpinMs = windowFrames ? double(g.spinCounts - spinStartCounts) * 1000.0 / double(g.frequency) / double(windowFrames) : 0.0;
            windowStart = now;
            windowFrames = 0;
            spinStartCounts = g.spinCounts;
            lastWorstMs = secondWorstMs;
            secondWorstMs = 0.0;
            newSecond = true;
        }
        // Packets drive identity changes; polling handles mid-session loads and debounces empty reads.
        if (const long pending = _InterlockedExchange(&packetIdentityPending, 0)) {
            // Defer login until the zone is loaded; process logout immediately.
            if (pending == 1 && !loggedIn()) _InterlockedCompareExchange(&packetIdentityPending, 1, 0);   // put back unless a logout arrived meanwhile
            else {
                const truefps::Identity who = pending == 1 ? truefps::Identity{packetName, packetServerId} : truefps::Identity{};
                packetKnownOnGame = who.known();
                emptyPolls = 0;
                if (who != identity) onIdentityChanged(who);
            }
        } else if (newSecond) {
            const auto who = readIdentity();
            emptyPolls = who.known() ? 0 : (std::min)(emptyPolls + 1, truefps::kEmptyPollsForDisconnect);
            if (identity.known() && !who.known() && packetKnownOnGame && truefps::connectionLost(emptyPolls)) {
                packetKnownOnGame = false;
                _InterlockedExchange(&packetForget, 1);   // the callback drops its record, so the next login packet counts
            }
            if (who.known() ? who != identity : (identity.known() && !packetKnownOnGame && truefps::identityGone(emptyPolls))) onIdentityChanged(who);
        }
        applySettingsWhenReady();
        if (newSecond && fileLog.takeWriteWarning()) chat(("the log cannot be written (" + logPathText() + "); TrueFPS runs on without it.").c_str(), 0x68);
        if (newSecond && fileLog.takeTrimWarning()) chat(("its log is over 1.5 MB and cannot be trimmed (" + logPathText() + "): is another program holding it open?").c_str(), 0x68);
        updateOverlay(newSecond);   // pure measurement: it runs even when the client code was not found
        if (newSecond) noteSecond(lastFps);
        renderPanel();
        framePrevPaceEnd = truefps::qpcNow();   // the end of truefps's callback: the real Present follows
        ULONG64 paceEndCycles = 0;
        framePrevPaceEndCycles = (cyclesPerMs > 0.0 && QueryThreadCycleTime(GetCurrentThread(), &paceEndCycles)) ? paceEndCycles : 0;
    }

private:

    void recordFrame() {
        auto& g = truefps::g;
        const int64_t at = truefps::qpcNow();
        if (framePrevAt && g.frequency) {
            truefps::FrameRecord r;
            r.at = at;
            r.ms = float(double(at - framePrevAt) * 1000.0 / double(g.frequency));
            r.waitMs = float(double(g.waitCounts - framePrevWait) * 1000.0 / double(g.frequency));
            r.lateMs = float(double(g.lateCounts - framePrevLate) * 1000.0 / double(g.frequency));
            r.limiterWaitMs = float(double(g.limiterWaitCounts - framePrevLimiterWait) * 1000.0 / double(g.frequency));
            if (g.frameSmooth) r.ticks = float(g.frame.s);
            else {
                uintptr_t app = 0;
                float step = 0.0f;
                if (g.appSlot && truefps::readValue(g.appSlot, app) && app && truefps::readValue(app + truefps::kAppStep, step)) r.ticks = truefps::stepValue(step, g.speed);
            }
            truefps::splitFrame(r, framePrevAt, framePrevPaceEnd, g.lastReset, at, g.frequency);
            truefps::splitScenes(r, g.lastReset, sceneFirstBegin, sceneLastEnd, at, g.frequency);
            r.scenes = sceneCount;
            const int paced = smoothActive ? truefps::heldFps(pacedFps, backgroundNowFps(), inBackground) : 0;
            r.smoothFps = uint16_t(!smoothActive ? 0 : paced > 0 ? paced : 0xFFFF);   // 65535: smooth mode uncapped
            r.speed = uint8_t(g.speed >= 1.0f && g.speed <= 255.0f ? g.speed : 1.0f);
            r.flags = uint8_t((inBackground ? truefps::kFrameBackground : 0) | (menuPaused ? truefps::kFrameMenuPaused : 0));
            ULONG64 cycles = 0;
            if (cyclesPerMs > 0.0 && QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
                if (framePrevCycles && cycles >= framePrevCycles) r.cpuMs = float(double(cycles - framePrevCycles) / cyclesPerMs);
                if (framePrevPaceEndCycles && cycles >= framePrevPaceEndCycles) r.outsideCpuMs = float(double(cycles - framePrevPaceEndCycles) / cyclesPerMs);
                framePrevCycles = cycles;
            }
            // Sample the read counter to limit syscalls; unsampled frames report zero.
            sampledFrame = truefps::sampleThisFrame(frameIndex, r.ms);
            IO_COUNTERS io{};
            if (sampledFrame && GetProcessIoCounters(GetCurrentProcess(), &io)) {
                if (framePrevIo && io.ReadTransferCount >= framePrevIo) r.ioKb = uint32_t((io.ReadTransferCount - framePrevIo) / 1024);   // never counts backwards, as the CPU counter above
                framePrevIo = io.ReadTransferCount;
            }
            frameLog.add(r);
            const double f = double(g.frequency);
            const double sinceQuiet = quietFrom ? double(at - quietFrom) / f : 1e9, sinceLast = lastLongFrameAt ? double(at - lastLongFrameAt) / f : 1e9;
            if (truefps::longFrameWorthLogging(r, zoning, prevFrameBackground, sinceQuiet, sinceLast)) {
                char head[64];
                _snprintf_s(head, sizeof head, _TRUNCATE, "long frame: %.0f ms", double(r.ms));
                std::string line = std::string(head) + (longFramesUnsaid ? " (" + std::to_string(longFramesUnsaid) + " more since the last one)" : std::string()) +
                                   "; " + stateText(lastFps) + "; " + truefps::framePartsText(r);
                logLine(Ashita::LogLevel::Warn, line.c_str());
                lastLongFrameAt = at;
                longFramesUnsaid = 0;
            } else if (truefps::longFrameWorthLogging(r, zoning, prevFrameBackground, sinceQuiet, 1e9)) {
                ++longFramesUnsaid;   // the same rule without the once-a-minute term
            }
            prevFrameBackground = inBackground || (r.flags & truefps::kFrameBackground);
            if (double(r.ms) > secondWorstMs) secondWorstMs = r.ms;
        }
        frameIndex++;
        sceneFirstBegin = sceneLastEnd = 0;
        sceneCount = 0;
        framePrevAt = at;
        framePrevWait = g.waitCounts;
        framePrevLate = g.lateCounts;
        framePrevLimiterWait = g.limiterWaitCounts;
    }

    bool refused = false;
    bool periodSet = false;
    unsigned presentFailures = 0;   // frame callbacks that threw: said once, counted for the unload line
    volatile long packetFailures = 0;   // packet callbacks that threw: said once, counted for the unload line
    bool commandOurs = false;       // the command being handled was recognised as /truefps
    HANDLE soleInstance = nullptr;
};

extern "C" {
__declspec(dllexport) IPlugin* __stdcall expCreatePlugin(const char*) {
    try { return new TrueFps(); } catch (...) { return nullptr; }
}
__declspec(dllexport) void __stdcall expDestroyPlugin(void* instance) { delete static_cast<TrueFps*>(instance); }
__declspec(dllexport) double __stdcall expGetInterfaceVersion() { return ASHITA_INTERFACE_VERSION; }
}
