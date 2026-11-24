#pragma once
// Coalescing background atomic UTF-8 file writer for prefs/session/alias saves.
// Keeps FlushFileBuffers + MOVEFILE_WRITE_THROUGH off the UI thread during
// active interaction while allowing exit/update paths to Flush() synchronously.
#include "utf8_file.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <windows.h>

namespace pulse {

class AsyncUtf8FileWriter {
public:
    AsyncUtf8FileWriter() = default;
    ~AsyncUtf8FileWriter() { Stop(); }
    AsyncUtf8FileWriter(const AsyncUtf8FileWriter&) = delete;
    AsyncUtf8FileWriter& operator=(const AsyncUtf8FileWriter&) = delete;

    bool Submit(std::wstring path, std::wstring content) {
        if (path.empty()) return false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            auto it = pending_.find(path);
            if (it != pending_.end()) {
                it->second = std::move(content);
            } else {
                order_.push_back(path);
                pending_.emplace(std::move(path), std::move(content));
            }
            if (!thread_.joinable()) {
                stopped_ = false;
                thread_ = std::thread([this] { Run(); });
            }
        }
        wake_.notify_all();
        return true;
    }

    void Flush() {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return (order_.empty() && !in_flight_) || stopped_; });
    }

    void Stop() {
        std::thread thread;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            thread = std::move(thread_);
        }
        wake_.notify_all();
        if (thread.joinable()) thread.join();
    }

private:
    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return stopping_ || !order_.empty(); });
            if (order_.empty()) {
                stopped_ = true;
                wake_.notify_all();
                lock.unlock();
                SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
                return;
            }
            std::wstring path = std::move(order_.front());
            order_.pop_front();
            auto it = pending_.find(path);
            std::wstring content;
            if (it != pending_.end()) {
                content = std::move(it->second);
                pending_.erase(it);
            }
            in_flight_ = true;
            lock.unlock();
            try {
                WriteUtf8FileAtomic(path, content);
            } catch (...) {
            }
            lock.lock();
            in_flight_ = false;
            if (order_.empty()) wake_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::deque<std::wstring> order_;
    std::unordered_map<std::wstring, std::wstring> pending_;
    bool in_flight_ = false;
    bool stopping_ = false;
    bool stopped_ = false;
};

inline AsyncUtf8FileWriter& GlobalAsyncFileWriter() {
    static AsyncUtf8FileWriter writer;
    return writer;
}

} // namespace pulse
