// Per-character append-only logging with a 1 MB target cap.
#pragma once
#include <windows.h>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace plog {

inline constexpr uint64_t kCapBytes = 1024 * 1024;          // a log is trimmed before it passes this
inline constexpr uint64_t kKeepBytes = 768 * 1024;          // a trim keeps the newest this much
inline constexpr uint64_t kTrimWarnBytes = 1536 * 1024;     // past this, a log that cannot be trimmed is reported
inline constexpr ULONGLONG kStartupKeepMs = 24ull * 60 * 60 * 1000;   // a left-behind startup file is kept this long
inline constexpr ULONGLONG kFailWarnMs = 10000;             // writes failing this long are reported in chat
inline constexpr size_t kQueueMax = 2000;                   // lines waiting past this are dropped and counted

// Names.

// Sanitize path components and run-tag fields.
inline std::string cleanName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out += (std::isalnum(c) || c == '_' || c == '-') ? char(c) : '_';
    return out;
}

// Character directory key: <Name>_<server id>.
inline std::string characterKey(const std::string& name, uint32_t serverId) { return cleanName(name) + "_" + std::to_string(serverId); }

// Run identity.

// Host, PID and process start time distinguish runs across clients.
struct Run {
    std::string computer;
    DWORD pid = 0;
    uint64_t start = 0;   // FILETIME ticks; 0 = not known (a startup file from an older version)
    bool operator==(const Run& o) const { return computer == o.computer && pid == o.pid && start == o.start; }
    // "<computer>/<pid>/<start as 16 hex digits>"
    std::string tag() const {
        char tail[48];
        _snprintf_s(tail, sizeof tail, _TRUNCATE, "/%lu/%016llX", static_cast<unsigned long>(pid), static_cast<unsigned long long>(start));
        return computer + tail;
    }
};

inline std::string computerName() {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof name;
    return GetComputerNameA(name, &size) ? cleanName(name) : std::string("unknown");
}

inline uint64_t processStart(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

inline Run thisRun() {
    Run r;
    r.computer = computerName();
    r.pid = GetCurrentProcessId();
    r.start = processStart(GetCurrentProcess());
    return r;
}

inline bool parseTag(const std::string& tag, Run& out) {
    const size_t a = tag.find('/');
    const size_t b = a == std::string::npos ? std::string::npos : tag.find('/', a + 1);
    if (a == 0 || b == std::string::npos || b + 1 >= tag.size()) return false;
    char* end = nullptr;
    const unsigned long pid = std::strtoul(tag.c_str() + a + 1, &end, 10);
    if (end != tag.c_str() + b || pid == 0) return false;
    const unsigned long long start = _strtoui64(tag.c_str() + b + 1, &end, 16);
    if (*end != '\0') return false;
    out.computer = tag.substr(0, a);
    out.pid = pid;
    out.start = start;
    return true;
}

// Read the run tag; older log lines may omit it.
inline bool runInLine(const std::string& line, Run& out) {
    const size_t at = line.rfind(", run ");
    if (at == std::string::npos) return false;
    size_t end = at + 6;
    while (end < line.size() && line[end] != ' ' && line[end] != '\r' && line[end] != '\n' && line[end] != ')') ++end;
    return parseTag(line.substr(at + 6, end - (at + 6)), out);
}

// Check local runs only. Treat access denied as still running; callers filter out remote hosts.
inline bool runAlive(const Run& r) {
    if (r.computer != computerName() || r.pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    const uint64_t start = processStart(process);
    CloseHandle(process);
    return running && (r.start == 0 || start == r.start);
}

// Paths.

// Resolve the Ashita root above the plugins directory.
inline std::string rootOfDll(const std::string& dllPath) {
    const size_t slash = dllPath.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    const std::string dir = dllPath.substr(0, slash + 1);
    const size_t up = dir.size() > 1 ? dir.find_last_of("\\/", dir.size() - 2) : std::string::npos;
    return up == std::string::npos ? dir : dir.substr(0, up + 1);
}
// Resolve the root from a module address before Ashita core is available.
inline std::string ashitaRoot(const void* anyAddressInside) {
    HMODULE self = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(anyAddressInside), &self))
        return std::string();
    const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    return (n == 0 || n >= MAX_PATH) ? std::string() : rootOfDll(path);
}
inline std::string logsDir(const std::string& root, const std::string& plugin) { return root + "logs\\" + plugin + "\\"; }
inline std::string characterLogPath(const std::string& root, const std::string& plugin, const std::string& key) {
    return logsDir(root, plugin) + key + "\\" + plugin + ".log";
}
inline std::string startupLogPath(const std::string& root, const std::string& plugin, const Run& r) {
    char tail[48];
    _snprintf_s(tail, sizeof tail, _TRUNCATE, "_%lu_%016llX.log", static_cast<unsigned long>(r.pid), static_cast<unsigned long long>(r.start));
    return logsDir(root, plugin) + "startup_" + r.computer + tail;
}
// Accept startup_<host>_<pid>_<start>.log and legacy startup_<pid>.log names.
inline bool parseStartupName(const std::string& fileName, Run& out) {
    const std::string head = "startup_", tail = ".log";
    if (fileName.size() <= head.size() + tail.size() || fileName.compare(0, head.size(), head) != 0 ||
        fileName.compare(fileName.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    const std::string mid = fileName.substr(head.size(), fileName.size() - head.size() - tail.size());
    const size_t b = mid.rfind('_');
    if (b == std::string::npos) {
        char* end = nullptr;
        const unsigned long pid = std::strtoul(mid.c_str(), &end, 10);
        if (*end != '\0' || pid == 0) return false;
        out.computer = computerName();
        out.pid = pid;
        out.start = 0;
        return true;
    }
    const size_t a = b == 0 ? std::string::npos : mid.rfind('_', b - 1);
    if (a == std::string::npos || a == 0) return false;
    return parseTag(mid.substr(0, a) + "/" + mid.substr(a + 1, b - a - 1) + "/" + mid.substr(b + 1), out);
}
// Use a relative path inside the Ashita root; otherwise retain the full path.
inline std::string underRoot(const std::string& root, const std::string& path) {
    if (!root.empty() && path.size() > root.size() && _strnicmp(path.c_str(), root.c_str(), root.size()) == 0) return path.substr(root.size());
    return path;
}
inline void ensureDirs(const std::string& root, const std::string& path) {
    for (size_t i = root.size(); i < path.size(); i++)
        if (path[i] == '\\' || path[i] == '/') CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
}

// Log records.

inline std::string stampLine(const char* level, const std::string& text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[48];
    _snprintf_s(stamp, sizeof stamp, _TRUNCATE, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds, level);
    return stamp + text + "\n";
}

// Read the module's PE timestamp; return 0 for an unreadable image.
inline uint32_t imageStamp(HMODULE module) {
    if (!module) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}
inline uint32_t ownImageStamp(const void* anyAddressInside) {
    HMODULE self = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(anyAddressInside), &self)
               ? imageStamp(self)
               : 0;
}

inline std::string sessionText(const std::string& plugin, const std::string& version, uint32_t build, uint32_t clientBuild, const std::string& ashitaInterface,
                               const Run& run) {
    char mid[96];
    _snprintf_s(mid, sizeof mid, _TRUNCATE, " loading, build %08X, client build %08X, Ashita interface ", build, clientBuild);
    return plugin + " " + version + mid + ashitaInterface + ", run " + run.tag();
}
// Pair unload records with their session by run tag.
inline std::string runSuffix(const Run& run) { return ", run " + run.tag(); }

// File I/O.

// Append in one shared write without overwriting another client's data.
inline bool appendText(const std::string& path, const std::string& text) {
    if (text.empty()) return true;
    HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(file);
    return ok;
}

inline uint64_t fileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) return UINT64_MAX;
    return (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

// Read the last maxBytes from a line boundary. A missing file is a successful empty read.
inline bool readText(const std::string& path, std::string& out, uint64_t maxBytes = 4 * kCapBytes) {
    out.clear();
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE;
    const uint64_t total = ok ? uint64_t(size.QuadPart) : 0;
    const uint64_t take = total > maxBytes ? maxBytes : total;
    if (ok && take > 0) {
        LARGE_INTEGER at{};
        at.QuadPart = LONGLONG(total - take);
        out.resize(size_t(take));
        DWORD got = 0;
        ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, out.data(), DWORD(take), &got, nullptr) && got == take;
        if (ok && take < total) {
            const size_t nl = out.find('\n');
            out.erase(0, nl == std::string::npos ? out.size() : nl + 1);
        }
    }
    CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

enum class Trim { NotNeeded, Trimmed, Busy, Failed };

// Trim to the newest keep bytes at a line boundary, allowing readers but excluding writers.
// If another writer holds the file, append now and retry trimming next batch.
inline Trim trimIfNeeded(const std::string& path, uint64_t incoming, uint64_t cap = kCapBytes, uint64_t keep = kKeepBytes) {
    const uint64_t before = fileSize(path);
    if (before == UINT64_MAX || before + incoming <= cap) return Trim::NotNeeded;
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION ? Trim::Busy : Trim::Failed;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return Trim::Failed;
    }
    const uint64_t now = uint64_t(size.QuadPart);
    if (now + incoming <= cap || now <= keep) {
        CloseHandle(file);
        return Trim::NotNeeded;
    }
    std::string tail(size_t(keep), '\0');
    LARGE_INTEGER at{};
    at.QuadPart = LONGLONG(now - keep);
    DWORD got = 0;
    bool ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, tail.data(), DWORD(keep), &got, nullptr) && got == keep;
    if (ok) {
        const size_t nl = tail.find('\n');
        const size_t from = nl == std::string::npos ? 0 : nl + 1;
        const DWORD count = DWORD(tail.size() - from);
        LARGE_INTEGER zero{};
        DWORD written = 0;
        ok = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) && WriteFile(file, tail.data() + from, count, &written, nullptr) && written == count &&
             SetEndOfFile(file);
    }
    CloseHandle(file);
    return ok ? Trim::Trimmed : Trim::Failed;
}

// Sessions.

// Detect an unclosed session from a terminated local run. Ignore missing tags and remote hosts.
inline bool endedUncleanly(const std::string& path) {
    std::string text;
    if (!readText(path, text, kTrimWarnBytes) || text.empty()) return false;
    const size_t mark = text.rfind(" loading, build ");
    if (mark == std::string::npos) return false;
    const size_t nl = text.rfind('\n', mark);
    const size_t lineStart = nl == std::string::npos ? 0 : nl + 1;
    size_t lineEnd = text.find('\n', mark);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    Run run;
    if (!runInLine(text.substr(lineStart, lineEnd - lineStart), run) || run.computer != computerName()) return false;
    const std::string tag = ", run " + run.tag();
    for (size_t at = lineEnd; at < text.size();) {
        size_t end = text.find('\n', at + 1);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        if ((line.find("] unloaded") != std::string::npos || line.find("] log continues in ") != std::string::npos) && line.find(tag) != std::string::npos)
            return false;
        at = end;
    }
    return !runAlive(run);
}

// Remove stale local startup logs, excluding active runs and this run.
inline void cleanupStartupFiles(const std::string& root, const std::string& plugin, const Run& self, ULONGLONG keepMs = kStartupKeepMs) {
    const std::string dir = logsDir(root, plugin);
    WIN32_FIND_DATAA found{};
    HANDLE find = FindFirstFileA((dir + "startup_*.log").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    const uint64_t now = (uint64_t(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
    do {
        Run run;
        if (!parseStartupName(found.cFileName, run) || run.computer != self.computer || run == self) continue;
        const uint64_t written = (uint64_t(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
        if (now < written || (now - written) / 10000 < keepMs) continue;   // FILETIME ticks are 100 ns
        if (runAlive(run)) continue;
        DeleteFileA((dir + found.cFileName).c_str());
    } while (FindNextFileA(find, &found));
    FindClose(find);
}

inline void deleteFiles(const std::string& root, const std::vector<std::string>& relativePaths) {
    for (const auto& rel : relativePaths) DeleteFileA((root + rel).c_str());
}

inline void deleteInCharacterFolders(const std::string& root, const std::string& plugin, const std::string& fileName) {
    const std::string dir = logsDir(root, plugin);
    WIN32_FIND_DATAA found{};
    HANDLE find = FindFirstFileA((dir + "*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.') continue;
        DeleteFileA((dir + found.cFileName + "\\" + fileName).c_str());
    } while (FindNextFileA(find, &found));
    FindClose(find);
}

// Report each warning cause once per session; count repeats.
class Repeats {
public:
    bool first(const std::string& cause) { return ++counts_[cause] == 1; }
    // Flush repeat counts at unload.
    std::vector<std::pair<std::string, unsigned>> take() {
        std::vector<std::pair<std::string, unsigned>> out;
        for (const auto& c : counts_)
            if (c.second > 1) out.emplace_back(c.first, c.second);
        counts_.clear();
        return out;
    }

private:
    std::map<std::string, unsigned> counts_;
};

// Writer.

// Serialize all file I/O on the writer thread to avoid stalling frames on a slow share.
class FileLog {
public:
    using Appender = bool (*)(const std::string& path, const std::string& text);

    // Do not lock or join at process exit: the writer may have died holding the mutex.
    ~FileLog() {
        if (writer_.joinable()) writer_.detach();
    }

    // Buffer startup lines until start().
    void open(const std::string& root, const std::string& startupPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        root_ = root;
        path_ = shown_ = startup_ = startupPath;
        merged_ = false;
        closedOld_ = false;
    }
    // Reinsert the session header after trimming or switching without a startup log.
    void setSession(const std::string& text, const Run& run) {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = text;
        run_ = run;
    }
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return;
        running_ = true;
        writerActive_ = true;
        everStarted_ = true;
        const uint64_t generation = ++generation_;
        writer_ = std::thread([this, generation] { run(generation); });
    }
    // After stop(), write synchronously so unload messages are preserved.
    void write(const char* level, const std::string& text) { queue(stampLine(level, text)); }
    // Write the diagnostic report as one block.
    void writeDiag(const std::string& who, const std::string& body) {
        queue(stampLine("info", "===== diag " + who + " =====") + body + stampLine("info", "===== end diag ====="));
    }
    // Queue a switch to the character log.
    void moveToCharacter(const std::string& path, const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path == shown_) return;
        Entry e;
        e.kind = Entry::Move;
        e.text = path;
        e.name = name;
        entries_.push_back(std::move(e));
        shown_ = path;
        wake_.notify_all();
    }
    // Queue file work after pending entries.
    void post(std::function<void()> job) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry e;
        e.kind = Entry::Job;
        e.job = std::move(job);
        entries_.push_back(std::move(e));
        wake_.notify_all();
    }
    // Report the new path as soon as its switch is queued.
    std::string path() {
        std::lock_guard<std::mutex> lock(mutex_);
        return shown_;
    }
    bool atStartupFile() {
        std::lock_guard<std::mutex> lock(mutex_);
        return shown_ == startup_;
    }
    // Latch after 10 seconds of failed writes; reset after a successful write.
    bool takeWriteWarning() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failSince_ == 0 || failWarned_ || GetTickCount64() - failSince_ < failWarnMs_) return false;
        failWarned_ = true;
        return true;
    }
    // Warn once if trimming fails and size exceeds 1.5 times the cap.
    bool takeTrimWarning() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!trimStuck_ || trimWarned_) return false;
        trimWarned_ = true;
        return true;
    }
    // Writer thread ID, excluded from patch freezes.
    DWORD threadId() {
        std::lock_guard<std::mutex> lock(mutex_);
        return writer_.joinable() ? GetThreadId(writer_.native_handle()) : 0;
    }
    // Drain and stop within timeoutMs, dropping failed writes after one final attempt.
    // On timeout, detach; the caller must keep the DLL mapped.
    bool stop(DWORD timeoutMs = 2000) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return true;
            running_ = false;
            wake_.notify_all();
        }
        if (!writer_.joinable()) {   // start() threw before the thread existed: nothing to wait for, nothing to detach
            std::lock_guard<std::mutex> lock(mutex_);
            writerActive_ = false;   // so a line queued after this goes straight to the file, not into a queue nobody drains
            return true;
        }
        if (WaitForSingleObject(writer_.native_handle(), timeoutMs) == WAIT_OBJECT_0) {
            writer_.join();
            return true;
        }
        writer_.detach();
        return false;
    }

    // Tests only.
    void setAppender(Appender a) {
        std::lock_guard<std::mutex> lock(mutex_);
        append_ = a ? a : &appendText;
    }
    void setLimits(uint64_t cap, uint64_t keep, ULONGLONG failWarnMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        cap_ = cap;
        keep_ = keep;
        failWarnMs_ = failWarnMs;
    }

private:
    struct Entry {
        enum Kind { Line, Move, Job } kind = Line;
        std::string text, name;
        std::function<void()> job;
    };

    void queue(std::string line) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (everStarted_ && !running_ && !writerActive_) {   // after stop: straight to the file
            const std::string path = path_;
            const Appender append = append_;
            lock.unlock();
            append(path, line);
            return;
        }
        if (entries_.size() >= kQueueMax) {
            ++dropped_;
            return;
        }
        Entry e;
        e.text = std::move(line);
        entries_.push_back(std::move(e));
        wake_.notify_all();
    }

    void noteFailure() {
        if (failSince_ == 0) failSince_ = GetTickCount64();
    }
    void noteSuccess() {
        failSince_ = 0;
        failWarned_ = false;
    }

    // Reinsert the session header after trimming.
    bool writeBatch(const std::string& path, const std::string& batch) {
        uint64_t cap, keep;
        std::string session;
        Appender append;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cap = cap_;
            keep = keep_;
            session = session_;
            append = append_;
        }
        std::string text;
        const Trim trim = trimIfNeeded(path, batch.size(), cap, keep);
        if (trim == Trim::Trimmed && !session.empty()) text = stampLine("info", session + " (log trimmed)");
        if ((trim == Trim::Busy || trim == Trim::Failed) && fileSize(path) > cap + cap / 2) {
            std::lock_guard<std::mutex> lock(mutex_);
            trimStuck_ = true;
        }
        text += batch;
        return append(path, text);
    }

    // Close the old character log and merge startup records into the new one.
    // Retry failed switches without duplicating the closing record.
    bool move(const std::string& to, const std::string& name) {
        std::string from, startup, root, session, tag;
        bool merged, closed;
        uint64_t cap, keep;
        Appender append;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            from = path_;
            startup = startup_;
            root = root_;
            session = session_;
            tag = run_.tag();
            merged = merged_;
            closed = closedOld_;
            cap = cap_;
            keep = keep_;
            append = append_;
        }
        if (from == to) return true;
        const bool fromStartup = from == startup && !merged;
        if (!fromStartup && !closed) {
            if (!append(from, stampLine("info", "log continues in " + name + "'s log, run " + tag))) return false;
            std::lock_guard<std::mutex> lock(mutex_);
            closedOld_ = true;
        }
        ensureDirs(root, to);
        std::string text;
        if (endedUncleanly(to)) text += stampLine("warn", "the previous session ended without unloading (the game closed or crashed)");
        if (fromStartup) {
            std::string earlier;
            if (!readText(startup, earlier)) return false;
            text += earlier;
        } else if (!session.empty()) {
            text += stampLine("info", session);
        }
        text += stampLine("info", "character: " + name);
        trimIfNeeded(to, text.size(), cap, keep);
        if (!append(to, text)) return false;
        if (fromStartup) DeleteFileA(startup.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = to;
        closedOld_ = false;
        if (fromStartup) merged_ = true;
        return true;
    }

    // On failure, retain the failed entry and all later entries for retry.
    bool drain(std::unique_lock<std::mutex>& lock) {
        while (!entries_.empty()) {
            if (entries_.front().kind == Entry::Job) {
                std::function<void()> job = std::move(entries_.front().job);
                entries_.pop_front();
                lock.unlock();
                try {
                    job();
                } catch (...) {
                }
                lock.lock();
                continue;
            }
            if (entries_.front().kind == Entry::Move) {
                const std::string to = entries_.front().text, name = entries_.front().name;
                lock.unlock();
                const bool ok = move(to, name);
                lock.lock();
                if (!ok) {
                    noteFailure();
                    return false;
                }
                entries_.pop_front();
                noteSuccess();
                continue;
            }
            std::string batch;
            size_t lines = 0;
            for (const Entry& e : entries_) {
                if (e.kind != Entry::Line) break;
                batch += e.text;
                ++lines;
            }
            const size_t dropped = dropped_;
            if (dropped) batch += stampLine("warn", "(" + std::to_string(dropped) + " log lines dropped: too many at once)");
            const std::string path = path_;
            lock.unlock();
            const bool ok = writeBatch(path, batch);
            lock.lock();
            if (!ok) {
                noteFailure();
                return false;
            }
            entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(lines));
            dropped_ -= dropped;
            noteSuccess();
        }
        return true;
    }

    // A detached writer must not consume a newer generation's queue.
    void run(uint64_t generation) {
        std::unique_lock<std::mutex> lock(mutex_);
        {
            const std::string root = root_, first = path_;
            lock.unlock();
            ensureDirs(root, first);
            lock.lock();
        }
        bool backoff = false;   // the last pass failed: wait 250 ms before the next try
        for (;;) {
            try {
                if (backoff)
                    wake_.wait_for(lock, std::chrono::milliseconds(250), [&] { return !running_ || generation_ != generation; });
                else
                    wake_.wait_for(lock, std::chrono::milliseconds(250), [&] { return !running_ || generation_ != generation || !entries_.empty(); });
                if (generation_ != generation) return;
                const bool last = !running_;   // stop() asked: this pass is the last try
                backoff = !drain(lock);
                if (last) {
                    if (backoff) entries_.clear();   // the last try failed: what is left is dropped
                    if (entries_.empty()) {
                        writerActive_ = false;
                        return;
                    }
                }
            } catch (...) {
                if (!lock.owns_lock()) lock.lock();
                if (!entries_.empty()) entries_.pop_front();   // out of memory in a 32-bit client: the entry in hand is lost
                backoff = true;
            }
        }
    }

    std::string root_, path_, shown_, startup_, session_;
    Run run_;
    bool merged_ = false, closedOld_ = false;
    Appender append_ = &appendText;
    uint64_t cap_ = kCapBytes, keep_ = kKeepBytes;
    ULONGLONG failWarnMs_ = kFailWarnMs, failSince_ = 0;
    bool failWarned_ = false, trimStuck_ = false, trimWarned_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Entry> entries_;
    size_t dropped_ = 0;
    uint64_t generation_ = 0;
    bool running_ = false, writerActive_ = false, everStarted_ = false;
    std::thread writer_;
};

}  // namespace plog
