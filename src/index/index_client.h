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
#include <map>
#include <windows.h>

namespace pulse::index {

class IndexClient {
public:
    IndexClient() = default;
    ~IndexClient() { Stop(); }
    IndexClient(const IndexClient&) = delete;
    IndexClient& operator=(const IndexClient&) = delete;

    void Start(HWND notify, UINT status_msg, UINT search_msg, std::wstring pipe_name = kPipeName);
    void Stop();

    std::wstring Status() const;
    std::wstring IndexPath() const;
    bool ServiceMode() const;
    bool Connected() const { return connected_.load(); }
    bool PinyinReady() const { return connected_.load() && pinyin_ready_.load(); }
    uint64_t Revision() const { return revision_.load(); }
    std::vector<VolumeInfo> Volumes() const;
    std::vector<std::wstring> ExcludedPaths() const;
    // excluded also lists the folders hidden by the system folder switch.
    bool GetScope(std::vector<VolumeInfo>& volumes, std::vector<std::wstring>& excluded) const;
    // False until a service that reports the system folder switch answers.
    bool SystemExclusion(bool& enabled, std::vector<std::wstring>& groups) const;
    void RefreshVolumesAsync();
    bool RequestConfigureVolume(const std::wstring& volume_id, bool enabled);
    bool RequestRebuild();
    static bool ConfigureVolumeElevated(const std::wstring& volume_id, bool enabled);
    static bool RebuildElevated();
    static bool InstallServiceElevated(DWORD* error = nullptr);
    static bool ConfigureIndexPathElevated(const std::wstring& path, std::wstring* error = nullptr);
    static bool ConfigureExcludePathElevated(const std::wstring& path, bool enabled);
    static bool ConfigureSystemExclusionElevated(const std::wstring& group, bool enabled);
    static bool ExportDiagnosticsElevated(const std::wstring& empty_directory, bool include_dumps = false);

    // Fire-and-forget. Reply arrives as search_msg (wParam = request id).
    void SearchAsync(const Query& q, uint32_t id);
    void CancelSession(uint64_t session_id);
    bool TakeResult(uint32_t id, SearchResult& out);

    bool ServiceInstalled() const;
    bool RequestInstallService(); // one UAC via runas --install

    static std::wstring ExePath();

private:
    void Worker();
    void Writer();
    bool EnsureConnected();
    bool SpawnHelper();
    void StartStoppedService();
    bool WriteMsg(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload);
    bool ReadMsg(ipc::MsgHeader& hdr, std::vector<uint8_t>& payload);
    void HandleStatus(const uint8_t* p, size_t n);
    void HandleSearch(uint32_t id, const uint8_t* p, size_t n);
    void HandleVolumes(const uint8_t* p, size_t n);
    void FlushPendingSearch();

    HWND notify_ = nullptr;
    UINT status_msg_ = 0;
    UINT search_msg_ = 0;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::wstring pipe_name_ = kPipeName;
    HANDLE child_proc_ = nullptr;
    HANDLE child_thread_ = nullptr;
    ULONGLONG service_start_tick_ = 0;   // last StartStoppedService attempt (Worker thread only)
    std::thread worker_;
    std::thread writer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> pinyin_ready_{false};
    std::atomic<size_t> count_{0};
    std::atomic<uint64_t> revision_{0};
    std::atomic<uint32_t> latest_search_id_{0};
    mutable std::mutex mu_;
    std::wstring status_ = L"索引未连接";
    std::wstring index_path_;
    bool service_mode_ = false;
    std::vector<VolumeInfo> volumes_;
    bool scope_ready_ = false;
    std::vector<std::wstring> excluded_paths_;
    bool system_known_ = false;
    bool exclude_system_ = false;
    std::vector<std::wstring> system_groups_;
    std::vector<std::wstring> system_paths_;
    uint32_t result_id_ = 0;
    SearchResult result_;
    std::mutex pipe_mu_;
    std::mutex write_mu_;
    Query pending_q_;
    uint32_t pending_id_ = 0;
    bool have_pending_ = false;
    std::map<uint64_t, std::pair<uint32_t, Query>> pending_searches_;
    std::map<uint64_t, std::pair<uint32_t, Query>> subscribed_searches_;
    std::vector<uint64_t> cancelled_sessions_;
    std::map<uint64_t, uint32_t> session_requests_;
    std::map<uint32_t, SearchResult> results_;
    bool volume_refresh_requested_ = true;
    std::condition_variable pending_cv_;
};

} // namespace pulse::index
