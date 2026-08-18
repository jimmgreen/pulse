// index_client.h — UI-side connection to Pulse.Index.exe (never blocks the UI
// thread on a search: SearchAsync + WM callback).
#pragma once
#include "index_protocol.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace pulse::index {

class IndexClient {
public:
    IndexClient() = default;
    ~IndexClient() { Stop(); }
    IndexClient(const IndexClient&) = delete;
    IndexClient& operator=(const IndexClient&) = delete;

    void Start(HWND notify, UINT status_msg, UINT search_msg);
    void Stop();

    std::wstring Status() const;

    // Fire-and-forget. Reply arrives as search_msg (wParam = request id).
    void SearchAsync(const Query& q, uint32_t id);
    bool TakeResult(uint32_t id, SearchResult& out);

    bool ServiceInstalled() const;
    bool RequestInstallService(); // one UAC via runas --install

    static std::wstring ExePath();

private:
    void Worker();
    void Writer();
    bool EnsureConnected();
    bool SpawnHelper();
    bool WriteMsg(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload);
    bool ReadMsg(ipc::MsgHeader& hdr, std::vector<uint8_t>& payload);
    void HandleStatus(const uint8_t* p, size_t n);
    void HandleSearch(uint32_t id, const uint8_t* p, size_t n);
    void FlushPendingSearch();

    HWND notify_ = nullptr;
    UINT status_msg_ = 0;
    UINT search_msg_ = 0;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE child_proc_ = nullptr;
    HANDLE child_thread_ = nullptr;
    std::thread worker_;
    std::thread writer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    std::atomic<size_t> count_{0};
    std::atomic<uint32_t> latest_search_id_{0};
    mutable std::mutex mu_;
    std::wstring status_ = L"索引未连接";
    uint32_t result_id_ = 0;
    SearchResult result_;
    std::mutex pipe_mu_;
    std::mutex write_mu_;
    Query pending_q_;
    uint32_t pending_id_ = 0;
    bool have_pending_ = false;
    std::condition_variable pending_cv_;
};

} // namespace pulse::index
