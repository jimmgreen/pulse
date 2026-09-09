// index_client.cpp — Connect to Pulse.Index (service or spawned helper).
#include "../common/command_line.h"
#include "index_client.h"
#include <chrono>
#include <shellapi.h>
#include <winsvc.h>

namespace pulse::index {

std::wstring IndexClient::ExePath() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return L"Pulse.Index.exe";
    return std::wstring(exe, slash + 1) + L"Pulse.Index.exe";
}

void IndexClient::Start(HWND notify, UINT status_msg, UINT search_msg) {
    Stop();
    notify_ = notify;
    status_msg_ = status_msg;
    search_msg_ = search_msg;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = L"索引未连接";
    }
    worker_ = std::thread([this] { Worker(); });
    writer_ = std::thread([this] { Writer(); });
}

void IndexClient::Stop() {
    running_ = false;
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

bool IndexClient::SpawnHelper() {
    if (ServiceInstalled()) {
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
        HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
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
            if (err == ERROR_PIPE_BUSY) WaitNamedPipeW(kPipeName, 200);
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
    ready_.store(ready != 0);
    count_.store(count);
    if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
}

void IndexClient::HandleSearch(uint32_t id, const uint8_t* p, size_t n) {
    if (id != latest_search_id_.load()) return;
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
        result_id_ = id;
        result_ = std::move(sr);
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
    {
        std::lock_guard<std::mutex> lock(mu_);
        service_mode_ = service != 0;
        index_path_ = std::move(path);
        volumes_ = std::move(volumes);
        excluded_paths_ = std::move(excluded_paths);
    }
    if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
}

void IndexClient::FlushPendingSearch() {
    Query q;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!have_pending_) return;
        q = pending_q_;
        id = pending_id_;
    }
    ipc::PayloadWriter w;
    uint32_t flags = 0;
    if (q.rank) flags |= 1;
    if (q.folders_only) flags |= 2;
    if (q.sort_desc) flags |= 4;
    w.PutU32(flags);
    w.PutU32(static_cast<uint32_t>(q.sort));
    w.PutU32(static_cast<uint32_t>(q.limit));
    w.PutU32(static_cast<uint32_t>(q.offset));
    w.PutString(q.needle);
    w.PutString(q.path_prefix);
    if (WriteMsg(REQ_IDX_SEARCH, id, w.data())) {
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_id_ == id) have_pending_ = false;
    }
}

void IndexClient::Writer() {
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            pending_cv_.wait_for(lock, std::chrono::milliseconds(100),
                [this] { return !running_ || have_pending_ || volume_refresh_requested_; });
            if (!running_) return;
            if (!have_pending_ && !volume_refresh_requested_) continue;
        }
        if (connected_) {
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
        pending_q_ = q;
        pending_id_ = id;
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

void IndexClient::RefreshVolumesAsync() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        volume_refresh_requested_ = true;
    }
    pending_cv_.notify_one();
}

bool IndexClient::TakeResult(uint32_t id, SearchResult& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (result_id_ != id) return false;
    out = std::move(result_);
    result_id_ = 0;
    return true;
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
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
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

bool IndexClient::InstallServiceElevated() {
    return RunElevatedIndexCommand(ExePath(), L"--install");
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

bool IndexClient::ExportDiagnosticsElevated(const std::wstring& empty_directory) {
    return RunElevatedIndexCommand(ExePath(),
        L"--export-diagnostics " + QuoteCommandArg(empty_directory));
}

} // namespace pulse::index
