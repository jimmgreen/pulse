#pragma once

#include <windows.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::picker_save {

struct Stamp {
    bool exists = false;
    uint64_t volume = 0;
    std::array<BYTE, 16> file_id{};
    uint64_t size = 0;
    uint64_t write_time = 0;
    DWORD attributes = 0;
};

struct Request {
    std::wstring path;
    std::wstring app_name;
    Stamp before;
    DWORD source_pid = 0;
    uint64_t request_id = 0;
    bool notify_if_no_tab = false;
    bool copy_path = false;
    bool stage_to_tray = false;
};

struct Completion {
    Request request;
    Stamp after;
};

// Worker-only filesystem call. Missing paths return success with exists=false;
// access failures and excluded file types return their actual error instead.
DWORD Capture(const std::wstring& path, Stamp& stamp);

class Observer {
public:
    struct Config {
        std::chrono::milliseconds quiet{1500};
        std::chrono::milliseconds deadline{120000};
        std::chrono::milliseconds poll{100};
        size_t capacity = 64;
    };

    Observer();
    explicit Observer(Config config);
    ~Observer();
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;

    bool Submit(Request request);
    void Cancel(uint64_t request_id);
    std::vector<Completion> Drain();

private:
    struct State;
    static DWORD WINAPI Worker(void* argument);
    std::shared_ptr<State> state_;
};

} // namespace pulse::picker_save
