#pragma once

#include "network_agent_protocol.h"
#include "network_index.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::index {

// UI-side client for Pulse.Index.exe --network-agent. It deliberately keeps
// the same surface as NetworkIndex so navigation/search code does not know
// whether the network index is in-process or hosted in the user agent.
class NetworkAgentClient {
public:
    NetworkAgentClient() = default;
    ~NetworkAgentClient() { Stop(); }
    NetworkAgentClient(const NetworkAgentClient&) = delete;
    NetworkAgentClient& operator=(const NetworkAgentClient&) = delete;

    void Start(HWND notify, UINT status_msg, UINT search_msg);
    void Stop();
    void SearchAsync(const Query& query, uint32_t id);
    bool TakeResult(uint32_t id, SearchResult& result);
    std::vector<NetworkRootInfo> Roots() const;
    bool AddRoot(const std::wstring& path, std::wstring* error = nullptr);
    bool RemoveRoot(const std::wstring& path, std::wstring* error = nullptr);
    void Rebuild(const std::wstring& path = {});

private:
    bool EnsureAgent();
    bool OpenPipe(HANDLE& pipe);
    bool Request(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload,
                 uint32_t& response_type, std::vector<uint8_t>& response);
    void SearchRequest(Query query, uint32_t id);
    void RefreshRoots();
    static std::wstring ExePath();

    HWND notify_ = nullptr;
    UINT status_msg_ = 0;
    UINT search_msg_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> latest_search_id_{0};
    mutable std::mutex mu_;
    std::vector<NetworkRootInfo> roots_;
    uint32_t result_id_ = 0;
    SearchResult result_;
    std::mutex request_mu_;
    std::mutex status_mu_;
    std::condition_variable status_cv_;
    std::thread status_thread_;
    HANDLE agent_process_ = nullptr;
};

} // namespace pulse::index
