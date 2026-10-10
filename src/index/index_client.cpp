// index_client.cpp — Connect to Pulse.Index (service or spawned helper).
#include "../common/command_line.h"
#include "index_client.h"
#include "index_config.h"
#include "index_executable.h"
#include "search_trace.h"
#include <chrono>
#include <algorithm>
#include <shellapi.h>
#include <winsvc.h>

namespace pulse::index {

namespace {

bool IsFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Index location changes (Pulse.Index.exe --set-index-path, elevated) stop the
// service while they copy the index and start it again themselves. The mutex
// exists exactly while one runs; an elevated owner may deny us SYNCHRONIZE.
bool IndexConfigurationRunning() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\PulseIndexConfiguration");
    if (mutex) {
        CloseHandle(mutex);
        return true;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

} // namespace

std::wstring IndexClient::ExePath() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    wchar_t* slash = wcsrchr(exe, L'\\');
    const std::wstring sibling = slash ? std::wstring(exe, slash + 1) + L"Pulse.Index.exe" : L"Pulse.Index.exe";
    if (IsFile(sibling)) return sibling;  // common case: no service query
    return ResolveIndexExecutable(sibling, InstalledServiceCommand(kServiceName), IsFile);
}

void IndexClient::Start(HWND notify, UINT status_msg, UINT search_msg, std::wstring pipe_name) {
    Stop();
    pipe_name_ = std::move(pipe_name);
    notify_ = notify;
    status_msg_ = status_msg;
    search_msg_ = search_msg;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = L"索引未连接";
        scope_ready_ = false;
    }
    worker_ = std::thread([this] { Worker(); });
    writer_ = std::thread([this] { Writer(); });
}

void IndexClient::Stop() {
    running_ = false;
    pinyin_ready_ = false;
    pending_cv_.notify_all();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        pipe = pipe_;
    }
    if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
    if (worker_.joinable()) worker_.join();
    if (writer_.joinable()) writer_.join();
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (child_proc_) {
        CloseHandle(child_proc_);
        child_proc_ = nullptr;
    }
    if (child_thread_) {
        CloseHandle(child_thread_);
        child_thread_ = nullptr;
    }
    connected_ = false;
}

// The service is installed but its host is gone: it crashed, setup ended it,
// or it quit with an error. SCM restarts crashed hosts only after a minute and
// never restarts one that stopped "cleanly", so start it from here, at most
// every 30 s. Interactive users have SERVICE_START (InstallService); with an
// older service DACL this is denied and Pulse keeps waiting as before.
void IndexClient::StartStoppedService() {
    const ULONGLONG now = GetTickCount64();
    if (service_start_tick_ && now - service_start_tick_ < 30000) return;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    if (SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS | SERVICE_START)) {
        SERVICE_STATUS status{};
        if (QueryServiceStatus(svc, &status) && status.dwCurrentState == SERVICE_STOPPED &&
            !IndexConfigurationRunning()) {
            service_start_tick_ = now;
            StartServiceW(svc, 0, nullptr);   // the pipe shows up once it runs
        }
        CloseServiceHandle(svc);
    } else {
        service_start_tick_ = now;   // denied (older DACL) or gone: do not ask every second
    }
    CloseServiceHandle(scm);
}

bool IndexClient::SpawnHelper() {
    if (pipe_name_ != kPipeName) return false;
    if (ServiceInstalled()) {
        StartStoppedService();
        {
            std::lock_guard<std::mutex> lock(mu_);
            status_ = L"正在等待索引服务…";
        }
        if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
        return false;
    }
    const std::wstring exe = ExePath();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = L"找不到 Pulse.Index.exe";
        return false;
    }
    std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_FORCEOFFFEEDBACK; // background helper: no AppStarting cursor
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    if (child_proc_) CloseHandle(child_proc_);
    if (child_thread_) CloseHandle(child_thread_);
    child_proc_ = pi.hProcess;
    child_thread_ = pi.hThread;
    return true;
}

bool IndexClient::EnsureConnected() {
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (pipe_ != INVALID_HANDLE_VALUE) return true;
    }
    auto try_open = [&]() -> HANDLE {
        HANDLE h = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
        return h;
    };
    HANDLE h = try_open();
    if (h == INVALID_HANDLE_VALUE) {
        SpawnHelper();
        const ULONGLONG deadline = GetTickCount64() + 8000;
        while (running_ && h == INVALID_HANDLE_VALUE && GetTickCount64() < deadline) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) WaitNamedPipeW(pipe_name_.c_str(), 200);
            else Sleep(50);
            h = try_open();
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        connected_ = false;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (!running_) {
            CloseHandle(h);
            return false;
        }
        pipe_ = h;
    }
    { std::lock_guard<std::mutex> lock(mu_); scope_ready_ = false; pending_searches_=subscribed_searches_; have_pending_=!pending_searches_.empty(); }
    connected_ = true;
    pending_cv_.notify_one();
    return true;
}

bool IndexClient::WriteMsg(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(write_mu_);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
        pipe = pipe_;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    auto hdr = MakeIndexHdr(type, id, static_cast<uint32_t>(payload.size()));
    if (!ipc::PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)))
        return false;
    if (!payload.empty() &&
        !ipc::PipeWrite(pipe, payload.data(), static_cast<DWORD>(payload.size())))
        return false;
    return true;
}

bool IndexClient::ReadMsg(ipc::MsgHeader& hdr, std::vector<uint8_t>& payload) {
    HANDLE pipe;
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        pipe = pipe_;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    if (!ipc::PipeRead(pipe, reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr))) return false;
    if (hdr.magic != kIndexMagic || hdr.payload_size > kIndexMaxPayload) return false;
    payload.resize(hdr.payload_size);
    if (hdr.payload_size &&
        !ipc::PipeRead(pipe, payload.data(), hdr.payload_size))
        return false;
    return true;
}

void IndexClient::HandleStatus(const uint8_t* p, size_t n) {
    ipc::PayloadReader r(p, n);
    uint32_t ready = 0, count = 0;
    std::wstring text;
    if (!r.GetU32(ready) || !r.GetU32(count) || !r.GetString(text)) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = std::move(text);
    }
    uint32_t pinyin_ready = 0;
    // Optional trailing status field: older hosts remain usable and report unavailable.
    if (!r.GetU32(pinyin_ready)) pinyin_ready = 0;
    pinyin_ready_.store(pinyin_ready == 1);
    ready_.store(ready != 0);
    count_.store(count);
    uint64_t revision = 0; uint32_t version = 0;
    if (r.GetU64(revision) && r.GetU32(version) && version == 1) revision_ = revision;
    else { std::lock_guard lock(mu_); status_ = L"索引进程版本过旧，请重启 Pulse 和索引服务以启用实时搜索"; }
    if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
}

void IndexClient::HandleSearch(uint32_t id, const uint8_t* p, size_t n) {
    { std::lock_guard lock(mu_); if (std::none_of(session_requests_.begin(), session_requests_.end(), [id](const auto& value) { return value.second == id; })) return; }
    ipc::PayloadReader r(p, n);
    uint32_t total = 0, nh = 0;
    if (!r.GetU32(total) || !r.GetU32(nh)) return;
    SearchResult sr;
    sr.total = total;
    sr.hits.reserve(nh);
    for (uint32_t i = 0; i < nh; ++i) {
        Hit h;
        uint32_t flags = 0, size_lo = 0, size_hi = 0, mt_lo = 0, mt_hi = 0;
        if (!r.GetString(h.path) || !r.GetString(h.name) || !r.GetU32(flags) ||
            !r.GetU32(size_lo) || !r.GetU32(size_hi) ||
            !r.GetU32(mt_lo) || !r.GetU32(mt_hi))
            return;
        h.is_dir = (flags & 1) != 0;
        h.size = (static_cast<uint64_t>(size_hi) << 32) | size_lo;
        h.mtime = (static_cast<uint64_t>(mt_hi) << 32) | mt_lo;
        sr.hits.push_back(std::move(h));
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        r.GetU64(sr.revision);
        TraceSearch("filename_received", sr.revision);
        results_[id] = std::move(sr);
    }
    if (notify_ && search_msg_) PostMessageW(notify_, search_msg_, id, 0);
}

void IndexClient::HandleVolumes(const uint8_t* p, size_t n) {
    ipc::PayloadReader r(p, n);
    uint32_t service = 0, count = 0;
    std::wstring path;
    if (!r.GetU32(service) || !r.GetString(path) || !r.GetU32(count)) return;
    std::vector<VolumeInfo> volumes;
    volumes.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        VolumeInfo volume;
        uint32_t flags = 0, kind = 0, progress = 0, lo = 0, hi = 0;
        if (!r.GetString(volume.id) || !r.GetString(volume.label) ||
            !r.GetString(volume.mount_point) || !r.GetString(volume.file_system) ||
            !r.GetString(volume.state) || !r.GetString(volume.error) ||
            !r.GetU32(flags) || !r.GetU32(kind) || !r.GetU32(progress) ||
            !r.GetU32(lo) || !r.GetU32(hi)) return;
        volume.online = (flags & 1u) != 0;
        volume.supported = (flags & 2u) != 0;
        volume.enabled = (flags & 4u) != 0;
        volume.kind = static_cast<VolumeKind>(kind);
        volume.progress = progress;
        volume.indexed_items = (static_cast<uint64_t>(hi) << 32) | lo;
        volumes.push_back(std::move(volume));
    }
    uint32_t excluded_count = 0;
    std::vector<std::wstring> excluded_paths;
    if (r.GetU32(excluded_count)) {
        excluded_paths.reserve(excluded_count);
        for (uint32_t i = 0; i < excluded_count; ++i) {
            std::wstring path_value;
            if (!r.GetString(path_value)) break;
            excluded_paths.push_back(std::move(path_value));
        }
    }
    uint32_t system_flags = 0, group_count = 0, system_count = 0;
    std::vector<std::wstring> groups, system_paths;
    bool system_known = r.GetU32(system_flags) && r.GetU32(group_count);
    for (uint32_t i = 0; system_known && i < group_count; ++i) {
        std::wstring group;
        system_known = r.GetString(group);
        if (system_known) groups.push_back(std::move(group));
    }
    system_known = system_known && r.GetU32(system_count);
    for (uint32_t i = 0; system_known && i < system_count; ++i) {
        std::wstring path_value;
        system_known = r.GetString(path_value);
        if (system_known) system_paths.push_back(std::move(path_value));
    }
    if (!system_known) { groups.clear(); system_paths.clear(); }
    {
        std::lock_guard<std::mutex> lock(mu_);
        service_mode_ = service != 0;
        index_path_ = std::move(path);
        volumes_ = std::move(volumes);
        excluded_paths_ = std::move(excluded_paths);
        system_known_ = system_known;
        exclude_system_ = system_known && (system_flags & 1u) != 0;
        system_groups_ = std::move(groups);
        system_paths_ = std::move(system_paths);
        scope_ready_ = true;
    }
    if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
}

void IndexClient::FlushPendingSearch() {
    Query q;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_searches_.empty()) return;
        id = pending_searches_.begin()->second.first;
        q = pending_searches_.begin()->second.second;
    }
    ipc::PayloadWriter w;
    uint32_t flags = 0;
    if (q.rank) flags |= 1;
    if (q.folders_only) flags |= 2;
    if (q.sort_desc) flags |= 4;
    if (q.subscribe) flags |= 8;
    w.PutU32(flags);
    w.PutU32(static_cast<uint32_t>(q.sort));
    w.PutU32(static_cast<uint32_t>(q.limit));
    w.PutU32(static_cast<uint32_t>(q.offset));
    w.PutString(q.needle);
    w.PutString(q.path_prefix);
    w.PutU64(q.session_id);
    if (WriteMsg(REQ_IDX_SEARCH, id, w.data())) {
        std::lock_guard<std::mutex> lock(mu_);
        auto found = pending_searches_.find(q.session_id);
        if (found != pending_searches_.end() && found->second.first == id) pending_searches_.erase(found);
        have_pending_ = !pending_searches_.empty();
    }
}

void IndexClient::Writer() {
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            pending_cv_.wait_for(lock, std::chrono::milliseconds(100),
                [this] { return !running_ || (connected_ &&
                    (have_pending_ || !cancelled_sessions_.empty() || volume_refresh_requested_)); });
            if (!running_) return;
            // Queued work must survive an outage without keeping this thread
            // runnable. Worker notifies us after reconnecting.
            if (!connected_) continue;
            if (!have_pending_ && cancelled_sessions_.empty() && !volume_refresh_requested_) continue;
        }
        if (connected_) {
            std::vector<uint64_t> cancelled;
            {std::lock_guard lock(mu_);cancelled.swap(cancelled_sessions_);}
            for(auto session:cancelled) {ipc::PayloadWriter payload;payload.PutU64(session);WriteMsg(8,0,payload.data());}
            bool refresh = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                refresh = volume_refresh_requested_;
                volume_refresh_requested_ = false;
            }
            if (refresh && !WriteMsg(REQ_IDX_VOLUMES, 0, {})) {
                std::lock_guard<std::mutex> lock(mu_);
                volume_refresh_requested_ = true;
            }
            FlushPendingSearch();
        }
    }
}

void IndexClient::Worker() {
    while (running_) {
        if (!EnsureConnected()) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (status_ != L"找不到 Pulse.Index.exe")
                    status_ = L"索引未连接";
            }
            for (int i = 0; i < 10 && running_; ++i) Sleep(100);
            continue;
        }
        WriteMsg(REQ_IDX_STATUS, 0, {});
        pending_cv_.notify_one();
        while (running_) {
            ipc::MsgHeader hdr{};
            std::vector<uint8_t> payload;
            if (!ReadMsg(hdr, payload)) {
                std::lock_guard<std::mutex> write_lock(write_mu_);
                std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
                if (pipe_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(pipe_);
                    pipe_ = INVALID_HANDLE_VALUE;
                }
                connected_ = false;
                pinyin_ready_ = false;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    status_ = L"索引连接已断开，正在重新连接…";
                }
                if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
                break;
            }
            if (hdr.type == RSP_IDX_STATUS)
                HandleStatus(payload.data(), payload.size());
            else if (hdr.type == RSP_IDX_SEARCH)
                HandleSearch(hdr.request_id, payload.data(), payload.size());
            else if (hdr.type == RSP_IDX_VOLUMES)
                HandleVolumes(payload.data(), payload.size());
        }
    }
}

std::wstring IndexClient::Status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void IndexClient::SearchAsync(const Query& q, uint32_t id) {
    if (!running_) return;
    latest_search_id_.store(id);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto previous = session_requests_.find(q.session_id);
        if (previous != session_requests_.end()) results_.erase(previous->second);
        session_requests_[q.session_id] = id;
        pending_searches_[q.session_id] = {id, q};
        subscribed_searches_[q.session_id] = {id,q};
        have_pending_ = true;
    }
    pending_cv_.notify_one();
}

std::wstring IndexClient::IndexPath() const {
    std::lock_guard<std::mutex> lock(mu_);
    return index_path_;
}

bool IndexClient::ServiceMode() const {
    std::lock_guard<std::mutex> lock(mu_);
    return service_mode_;
}

std::vector<VolumeInfo> IndexClient::Volumes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return volumes_;
}

std::vector<std::wstring> IndexClient::ExcludedPaths() const {
    std::lock_guard<std::mutex> lock(mu_);
    return excluded_paths_;
}
bool IndexClient::GetScope(std::vector<VolumeInfo>& volumes, std::vector<std::wstring>& excluded) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!connected_ || !scope_ready_) return false;
    volumes = volumes_; excluded = excluded_paths_;
    excluded.insert(excluded.end(), system_paths_.begin(), system_paths_.end());
    return true;
}

bool IndexClient::SystemExclusion(bool& enabled, std::vector<std::wstring>& groups) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!system_known_) return false;
    enabled = exclude_system_;
    groups = system_groups_;
    return true;
}

void IndexClient::RefreshVolumesAsync() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        volume_refresh_requested_ = true;
    }
    pending_cv_.notify_one();
}

bool IndexClient::TakeResult(uint32_t id, SearchResult& out) {
    std::lock_guard<std::mutex> lock(mu_);
    auto found = results_.find(id);
    if (found == results_.end()) return false;
    out = std::move(found->second);
    results_.erase(found);
    return true;
}
void IndexClient::CancelSession(uint64_t session_id) {
    if(!session_id) return;
    {std::lock_guard lock(mu_);
     if(auto it=session_requests_.find(session_id);it!=session_requests_.end()) {results_.erase(it->second);session_requests_.erase(it);}
     pending_searches_.erase(session_id);subscribed_searches_.erase(session_id);cancelled_sessions_.push_back(session_id);
     have_pending_=!pending_searches_.empty();}
    pending_cv_.notify_all();
}

bool IndexClient::ServiceInstalled() const {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    const bool ok = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool IndexClient::RequestInstallService() {
    return InstallServiceElevated();
}

namespace {

std::wstring QuoteCommandArg(const std::wstring& value) {
    return pulse::QuoteWindowsArgument(value);
}

bool RunElevatedIndexCommand(const std::wstring& exe, const std::wstring& parameters,
                            DWORD* result = nullptr) {
    // Never let the shell show its own "cannot find the file" box; callers report it.
    if (!IsFile(exe)) {
        if (result) *result = ERROR_FILE_NOT_FOUND;
        return false;
    }
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        if (result) *result = GetLastError();
        return false;
    }
    DWORD code = ERROR_GEN_FAILURE;
    if (sei.hProcess) {
        if (WaitForSingleObject(sei.hProcess, INFINITE) == WAIT_OBJECT_0)
            GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    if (result) *result = code;
    return code == 0;
}

} // namespace

bool IndexClient::RequestConfigureVolume(const std::wstring& volume_id, bool enabled) {
    const bool ok = ConfigureVolumeElevated(volume_id, enabled);
    if (ok) RefreshVolumesAsync();
    return ok;
}

bool IndexClient::ConfigureVolumeElevated(const std::wstring& volume_id, bool enabled) {
    const std::wstring parameters = L"--configure-volume " + QuoteCommandArg(volume_id) +
                                    (enabled ? L" --enable" : L" --disable");
    return RunElevatedIndexCommand(ExePath(), parameters);
}

bool IndexClient::RequestRebuild() {
    return RebuildElevated();
}

bool IndexClient::RebuildElevated() {
    return RunElevatedIndexCommand(ExePath(), L"--rebuild-index");
}

bool IndexClient::InstallServiceElevated(DWORD* error) {
    return RunElevatedIndexCommand(ExePath(), L"--install", error);
}

bool IndexClient::ConfigureIndexPathElevated(const std::wstring& path, std::wstring* error) {
    DWORD code = 0;
    if (RunElevatedIndexCommand(ExePath(), L"--set-index-path " + QuoteCommandArg(path), &code)) return true;
    if (error) {
        switch (code) {
        case ERROR_CANCELLED: *error = L"已取消索引迁移，原位置保持不变。"; break;
        case ERROR_BUSY: *error = L"另一项索引设置正在处理，请完成后重试。"; break;
        case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL:
            *error = L"目标磁盘空间不足，索引未迁移。请释放空间后重试。"; break;
        case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS:
            *error = L"目标位置已有另一份索引。请选择空目录，避免覆盖已有数据。"; break;
        case ERROR_INVALID_PARAMETER:
            *error = L"请选择独立的索引文件夹，不能与原位置互相包含，也不能使用链接目录。选择磁盘根目录时会自动使用其中的 Index 文件夹。"; break;
        case ERROR_PARTIAL_COPY:
            *error = L"索引已切换到新位置，但部分旧文件被占用，未能清理。新索引可以正常使用。"; break;
        case kIndexDirectoryNotPrivate:
            *error = L"索引位置里有不属于 Pulse 的文件或链接，为保护隐私，索引服务不会使用它。请改选一个空文件夹。"; break;
        case ERROR_SERVICE_NOT_ACTIVE: case ERROR_SERVICE_REQUEST_TIMEOUT:
            *error = L"索引服务未能恢复。原索引文件已保留，请重新启动索引服务后重试。"; break;
        default:
            *error = L"索引迁移未完成，原索引已保留。请检查目标目录的权限和磁盘连接后重试。"; break;
        }
    }
    return false;
}

bool IndexClient::ConfigureExcludePathElevated(const std::wstring& path, bool enabled) {
    const std::wstring parameters = L"--configure-exclude " + QuoteCommandArg(path) +
                                    (enabled ? L" --enable" : L" --disable");
    return RunElevatedIndexCommand(ExePath(), parameters);
}

bool IndexClient::ConfigureSystemExclusionElevated(const std::wstring& group, bool enabled) {
    return RunElevatedIndexCommand(ExePath(), L"--configure-system-exclusion " + QuoteCommandArg(group) +
                                   (enabled ? L" --enable" : L" --disable"));
}

bool IndexClient::ExportDiagnosticsElevated(const std::wstring& empty_directory, bool include_dumps) {
    return RunElevatedIndexCommand(ExePath(),
        L"--export-diagnostics " + QuoteCommandArg(empty_directory) + (include_dumps ? L" --include-dumps" : L""));
}

} // namespace pulse::index
