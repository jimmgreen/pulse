#pragma once

#include "content_search.h"

#include <deque>
#include <mutex>
#include <thread>

namespace pulse::index {

struct ContentSearchUpdate {
    ContentSearchProgress progress;
    std::vector<ContentHit> hits;
};

// Starts one short-lived, current-user content agent per request. Cancelling a
// generation terminates that agent, which also interrupts blocked file reads.
class ContentSearchClient {
public:
    ContentSearchClient() = default;
    ~ContentSearchClient() { Stop(); }
    ContentSearchClient(const ContentSearchClient&) = delete;
    ContentSearchClient& operator=(const ContentSearchClient&) = delete;

    void Start(HWND notify, UINT update_message);
    void Stop();
    void SearchAsync(ContentSearchRequest request);
    void Cancel();
    bool TakeUpdate(ContentSearchUpdate& update);

private:
    void Run(ContentSearchRequest request);
    void Publish(ContentSearchUpdate update);
    static std::wstring ExePath();

    HWND notify_ = nullptr;
    UINT update_message_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
    std::mutex process_mu_;
    HANDLE process_ = nullptr;
    std::thread worker_;
    std::mutex updates_mu_;
    std::deque<ContentSearchUpdate> updates_;
};

} // namespace pulse::index
