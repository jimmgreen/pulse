#pragma once
#include "change_tracking.h"
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <deque>
#include <unordered_map>

namespace pulse::index {
class ChangeTrackingClient {
public:
    ~ChangeTrackingClient() { Stop(); }
    void Start(HWND notify, UINT message);
    void Stop();
    void SetEnabled(bool enabled);
    void QuerySummaryAsync(std::vector<std::wstring> paths, uint64_t since, uint32_t id);
    void QueryDetailsAsync(std::wstring path, uint64_t since, uint64_t before,
                          uint32_t limit, uint32_t id, uint32_t kind_filter = UINT32_MAX);
    bool TakeSummary(uint32_t id, ChangeResponse& result);
    bool TakeDetails(uint32_t id, ChangeResponse& result);
private:
    struct Request { uint32_t id = 0; std::vector<uint8_t> payload; bool pending = false; };
    void Worker();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    HANDLE cancel_ = nullptr;
    HWND notify_ = nullptr;
    UINT message_ = 0;
    bool running_ = false, enabled_ = false, changed_ = false;
    Request summary_;
    std::deque<Request> details_queue_;
    std::unordered_map<uint32_t, ChangeResponse> details_results_;
    uint32_t summary_id_ = 0;
    bool have_summary_ = false;
    ChangeResponse summary_result_;
};
}
