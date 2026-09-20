// Queue settings and capture snapshots for a single disk writer.
#pragma once
#include <windows.h>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace truefps {

struct IniValue { std::string section, key, value; bool remove = false; };   // remove: the key is deleted instead
using IniSnapshot = std::vector<IniValue>;
struct DiskNotice { std::string text; unsigned char color = 0x6A; };

class DiskWriter {
public:
    using IniWrite = bool (*)(const char* section, const char* key, const char* value, const char* path);
    using FileWrite = bool (*)(const std::string& path, const std::string& text);

    // Do not lock or join under the loader lock. Release stops the writer.
    ~DiskWriter() { if (writer_.joinable()) writer_.detach(); }

    void setWriters(IniWrite ini, FileWrite file) {
        std::lock_guard<std::mutex> lock(mutex_);
        ini_ = ini ? ini : &iniWrite;
        file_ = file ? file : &fileWrite;
    }

    void start() {
        stop();
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
        running_ = true;
        const uint64_t generation = ++generation_;
        writer_ = std::thread([this, generation] { run(generation); });
    }

    // Queue file work in FIFO order.
    bool runJob(std::function<void()> job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return false;
        jobs_.push_back(std::move(job));
        wake_.notify_all();
        return true;
    }

    // Coalesce pending settings snapshots. `shown` overrides the path in notices.
    void saveSettings(std::string path, IniSnapshot values, std::string shown = std::string()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        settingsShown_ = shown.empty() ? path : std::move(shown);
        settingsPath_ = std::move(path);
        settings_ = std::move(values);
        settingsPending_ = true;
        wake_.notify_all();
    }

    // Build file contents on the writer thread. Reject while another file is pending.
    // `shown` overrides the path in notices.
    bool saveFile(std::string path, std::function<std::string()> make, std::string doneText, std::string shown = std::string()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || fileBusy_) return false;
        fileBusy_ = true;
        fileShown_ = shown.empty() ? path : std::move(shown);
        filePath_ = std::move(path);
        fileMake_ = std::move(make);
        fileDone_ = std::move(doneText);
        wake_.notify_all();
        return true;
    }

    // Drain completion notices in FIFO order.
    std::vector<DiskNotice> takeNotices() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<DiskNotice> out(notices_.begin(), notices_.end());
        notices_.clear();
        return out;
    }

    DWORD threadId() { return writer_.joinable() ? GetThreadId(static_cast<HANDLE>(writer_.native_handle())) : 0; }

    bool fileBusy() {
        std::lock_guard<std::mutex> lock(mutex_);
        return fileBusy_;
    }
    bool settingsBusy() {
        std::lock_guard<std::mutex> lock(mutex_);
        return settingsPending_ || settingsWriting_;
    }

    // Drain and stop within timeoutMs. On timeout, detach; the caller must retain this object and pin the DLL.
    bool stop(DWORD timeoutMs = 2000) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return true;
            running_ = false;   // nothing more is queued
            stopping_ = true;
            wake_.notify_all();
        }
        if (!writer_.joinable()) return true;   // start() threw before the thread existed
        if (WaitForSingleObject(static_cast<HANDLE>(writer_.native_handle()), timeoutMs) == WAIT_OBJECT_0) {
            writer_.join();
            return true;
        }
        writer_.detach();
        return false;
    }

private:
    static bool iniWrite(const char* section, const char* key, const char* value, const char* path) {
        return WritePrivateProfileStringA(section, key, value, path) != 0;
    }
    static bool fileWrite(const std::string& path, const std::string& text) {
        FILE* out = nullptr;
        if (fopen_s(&out, path.c_str(), "wb") != 0 || !out) return false;
        const bool wrote = fwrite(text.data(), 1, text.size(), out) == text.size();
        const bool closed = fclose(out) == 0;
        return wrote && closed;
    }

    void notice(std::string text, unsigned char color) {   // mutex held
        if (notices_.size() < 32) notices_.push_back(DiskNotice{std::move(text), color});
    }

    // A detached writer must not consume a newer generation's queue.
    void run(uint64_t generation) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return stopping_ || generation_ != generation || settingsPending_ || (fileBusy_ && fileMake_) || !jobs_.empty(); });
            if (generation_ != generation) return;
            if (!jobs_.empty()) {
                std::function<void()> job = std::move(jobs_.front());
                jobs_.pop_front();
                lock.unlock();
                try { job(); } catch (...) {}
                lock.lock();
                continue;
            }
            // Catch each job's exceptions to avoid std::terminate.
            if (settingsPending_) {
                bool ok = false;
                DWORD error = 0;
                std::string path, shown;
                settingsWriting_ = true;
                settingsPending_ = false;
                try {
                    path = settingsPath_;
                    shown = settingsShown_;
                    const IniSnapshot values = std::move(settings_);
                    const IniWrite write = ini_;
                    settings_.clear();
                    lock.unlock();
                    // Try every key; preserve the first error.
                    bool allWritten = true;
                    for (const auto& v : values)
                        if (!write(v.section.c_str(), v.key.c_str(), v.remove ? nullptr : v.value.c_str(), path.c_str())) {
                            if (allWritten) error = GetLastError();
                            allWritten = false;
                        }
                    ok = allWritten;   // set once the run is through: a throw before this leaves it false
                    lock.lock();
                } catch (...) {
                    if (!lock.owns_lock()) lock.lock();
                    // Preserve successful writes if the following lock throws.
                    if (!ok && !error) error = ERROR_NOT_ENOUGH_MEMORY;   // the same rule as the loop: the first failure's reason is kept
                }
                settingsWriting_ = false;
                // Report once per consecutive run of failed saves.
                try {
                    if (!ok && settingsOk_) notice("could not save the settings to " + shown + " (error " + std::to_string(error) + "); they apply for this session.", 0x44);
                    if (ok && !settingsOk_) notice("settings saved again.", 0x6A);
                } catch (...) {}
                settingsOk_ = ok;
                continue;   // a newer snapshot may have arrived meanwhile
            }
            if (fileBusy_ && fileMake_) {
                bool ok = false;
                DWORD error = 0;
                std::string path, done, shown;
                try {
                    path = filePath_;
                    shown = fileShown_;
                    done = fileDone_;
                    const auto make = std::move(fileMake_);
                    const FileWrite write = file_;
                    fileMake_ = nullptr;
                    lock.unlock();
                    ok = write(path, make());
                    error = ok ? 0 : GetLastError();
                    lock.lock();
                } catch (...) {
                    if (!lock.owns_lock()) lock.lock();
                    fileMake_ = nullptr;
                    // Preserve a completed write if the following lock throws.
                    if (!ok && !error) error = ERROR_NOT_ENOUGH_MEMORY;   // the text could not be built; a reason already recorded is kept
                }
                fileBusy_ = false;
                try { notice(ok ? done : "could not write " + shown + " (error " + std::to_string(error) + ").", ok ? 0x6A : 0x44); } catch (...) {}
                continue;
            }
            if (stopping_) return;
        }
    }

    IniWrite ini_ = &iniWrite;
    FileWrite file_ = &fileWrite;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::string settingsPath_, settingsShown_, filePath_, fileShown_, fileDone_;
    IniSnapshot settings_;
    std::function<std::string()> fileMake_;
    std::deque<std::function<void()>> jobs_;
    std::deque<DiskNotice> notices_;
    uint64_t generation_ = 0;
    bool settingsPending_ = false, settingsWriting_ = false, settingsOk_ = true, fileBusy_ = false;
    bool stopping_ = false, running_ = false;
    std::thread writer_;
};

}  // namespace truefps
