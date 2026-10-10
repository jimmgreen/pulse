#pragma once

#include "network_agent_protocol.h"
#include "network_agent_security.h"
#include "network_index.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <map>

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
    friend struct NetworkAgentClientTestAccess;
    std::wstring pipe_name_ = agent::PipeName();
    bool EnsureAgent();
    bool OpenPipe(HANDLE& pipe);
    bool Request(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload,
                 uint32_t& response_type, std::vector<uint8_t>& response);
    void SearchRequest(Query query, uint32_t id);
    void SearchLoop();
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
    std::map<uint32_t,SearchResult> results_;
    std::mutex request_mu_;
    std::mutex pipe_mu_;
    HANDLE active_pipe_ = INVALID_HANDLE_VALUE;
    std::mutex status_mu_;
    std::condition_variable status_cv_;
    std::thread status_thread_;
    std::mutex search_mu_;
    std::condition_variable search_cv_;
    Query pending_query_;
    uint32_t pending_search_id_ = 0;
    bool have_pending_search_ = false;
    std::map<uint64_t,std::pair<uint32_t,Query>> pending_searches_;
    std::map<uint64_t,uint32_t> session_requests_;
    std::thread search_thread_;
    HANDLE agent_process_ = nullptr;
    ULONGLONG last_spawn_tick_ = 0;  // guarded by request_mu_
    static constexpr ULONGLONG kRespawnBackoffMs = 5000;
    // A freshly started agent creates its pipe only after loading its roots,
    // and a busy agent briefly has no free instance; wait this long for either.
    static constexpr ULONGLONG kOpenPipeWaitMs = 3000;
    // Must match the mutex created by RunAgent() in network_agent_main.cpp.
};

} // namespace pulse::index