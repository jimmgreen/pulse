// ops_manager.cpp — See ops_manager.h for the contract.
#include "ops_manager.h"
#include "../ipc/shell_client.h"
#include "../common/json_utils.h"
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <system_error>
#include <winioctl.h>

namespace pulse::ops {

namespace {

std::wstring FileName(const std::wstring& path) {
    std::wstring_view v = path;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    if (pos != std::wstring_view::npos) return std::wstring(v.substr(pos + 1));
    return std::wstring(v);
}

std::wstring ParentOf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();

    std::wstring prefix;
    std::wstring_view core = p;
    if (p.starts_with(L"\\\\?\\UNC\\")) {
        prefix = L"\\\\?\\UNC\\";
        core.remove_prefix(8);
    } else if (p.starts_with(L"\\\\?\\")) {
        prefix = L"\\\\?\\";
        core.remove_prefix(4);
    } else if (p.starts_with(L"\\\\")) {
        prefix = L"\\\\";
        core.remove_prefix(2);
    }

    std::wstring temp(core);
    const auto pos = temp.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos == 0) return path;
    temp.resize(pos);
    if (temp.size() == 2 && temp[1] == L':') temp += L'\\';
    return prefix + temp;
}

bool IsVolumeRoot(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    if (view.starts_with(L"\\\\?\\UNC\\")) {
        view.remove_prefix(8);
        const auto slash = view.find(L'\\');
        if (slash == std::wstring_view::npos) return true;
        return view.find(L'\\', slash + 1) == std::wstring_view::npos;
    }
    if (view.starts_with(L"\\\\?\\")) view.remove_prefix(4);
    else if (view.starts_with(L"\\\\")) {
        view.remove_prefix(2);
        const auto slash = view.find(L'\\');
        if (slash == std::wstring_view::npos) return true;
        return view.find(L'\\', slash + 1) == std::wstring_view::npos;
    }
    return view.size() == 2 && view[1] == L':';
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

std::wstring DisplayPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

const wchar_t* OpVerb(OpType t) {
    switch (t) {
    case OpType::Copy: return L"复制";
    case OpType::Move: return L"移动";
    case OpType::RecycleDelete: return L"删除";
    case OpType::RealDelete: return L"永久删除";
    case OpType::Rename: return L"重命名";
    case OpType::CreateFolder: return L"新建文件夹";
    case OpType::CreateTextFile: return L"新建文本文档";
    case OpType::RestoreRecycle: return L"还原";
    }
    return L"操作";
}

std::wstring Describe(const OpRequest& r) {
    std::wstring s = OpVerb(r.type);
    s += L" ";
    if (!r.sources.empty()) s += FileName(r.sources.front());
    if (r.sources.size() > 1) {
        wchar_t buf[32];
        swprintf_s(buf, L" 等 %zu 项", r.sources.size());
        s += buf;
    }
    if (r.type == OpType::Copy || r.type == OpType::Move) {
        s += L" → " + DisplayPath(r.dest_dir);
    } else if (r.type == OpType::Rename) {
        s += L" → " + r.new_name;
    }
    return s;
}

struct TransferEntry {
    std::wstring source;
    std::wstring destination;
    bool directory = false;
    bool reparse = false;
    uint64_t bytes = 0;
    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    FILETIME created{};
    FILETIME accessed{};
    FILETIME modified{};
};

struct CopyProgressContext {
    std::atomic<bool>* cancel = nullptr;
    std::atomic<bool>* pause = nullptr;
    std::function<void(uint64_t, uint64_t)> report;
    bool pause_sent = false;
    ULONGLONG last_report = 0;
};

class TransferRateEstimator {
public:
    void Reset(ULONGLONG tick, uint64_t bytes) {
        samples_.clear();
        samples_.push_back({ tick, bytes });
        smoothed_speed_ = 0.0;
        eta_seconds_ = 0;
        last_rate_tick_ = tick;
        last_eta_tick_ = tick;
    }

    void Observe(ULONGLONG tick, uint64_t bytes, uint64_t total_bytes) {
        if (samples_.empty() || bytes < samples_.back().bytes) {
            Reset(tick, bytes);
            return;
        }

        constexpr ULONGLONG kMinimumSampleMs = 100;
        constexpr ULONGLONG kWindowMs = 4000;
        constexpr ULONGLONG kWarmupMs = 750;
        if (tick - samples_.back().tick < kMinimumSampleMs && bytes < total_bytes)
            return;

        if (tick == samples_.back().tick) {
            samples_.back().bytes = bytes;
        } else {
            samples_.push_back({ tick, bytes });
        }
        while (samples_.size() > 2 && samples_[1].tick + kWindowMs <= tick)
            samples_.pop_front();

        const ULONGLONG span = tick - samples_.front().tick;
        const uint64_t byte_delta = bytes - samples_.front().bytes;
        if (span < kWarmupMs || byte_delta == 0) return;

        const double window_speed = static_cast<double>(byte_delta) * 1000.0
            / static_cast<double>(span);
        if (smoothed_speed_ <= 0.0) {
            smoothed_speed_ = window_speed;
        } else {
            const double seconds = static_cast<double>(tick - last_rate_tick_) / 1000.0;
            const double alpha = 1.0 - std::exp(-seconds);
            smoothed_speed_ += alpha * (window_speed - smoothed_speed_);
        }
        last_rate_tick_ = tick;

        if (bytes >= total_bytes || smoothed_speed_ <= 1.0) {
            eta_seconds_ = 0;
            return;
        }
        if (eta_seconds_ != 0 && tick - last_eta_tick_ < 1000) return;

        const double raw_eta = static_cast<double>(total_bytes - bytes) / smoothed_speed_;
        if (eta_seconds_ == 0) {
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::ceil(raw_eta)));
        } else {
            // React faster to a slowdown than to a transient speed-up.
            const double alpha = raw_eta > static_cast<double>(eta_seconds_) ? 0.45 : 0.20;
            const double blended = static_cast<double>(eta_seconds_)
                + alpha * (raw_eta - static_cast<double>(eta_seconds_));
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::llround(blended)));
        }
        last_eta_tick_ = tick;
    }

    double speed() const { return smoothed_speed_; }
    uint64_t eta_seconds() const { return eta_seconds_; }

private:
    struct Sample {
        ULONGLONG tick = 0;
        uint64_t bytes = 0;
    };
    std::deque<Sample> samples_;
    double smoothed_speed_ = 0.0;
    uint64_t eta_seconds_ = 0;
    ULONGLONG last_rate_tick_ = 0;
    ULONGLONG last_eta_tick_ = 0;
};

COPYFILE2_MESSAGE_ACTION CALLBACK CopyProgress(const COPYFILE2_MESSAGE* message,
                                               void* raw) {
    auto* ctx = static_cast<CopyProgressContext*>(raw);
    if (!ctx || !message) return COPYFILE2_PROGRESS_CANCEL;
    uint64_t transferred = 0;
    uint64_t total = 0;
    switch (message->Type) {
    case COPYFILE2_CALLBACK_CHUNK_FINISHED:
        transferred = message->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
        total = message->Info.ChunkFinished.uliTotalFileSize.QuadPart;
        break;
    case COPYFILE2_CALLBACK_STREAM_FINISHED:
        transferred = message->Info.StreamFinished.uliTotalBytesTransferred.QuadPart;
        total = message->Info.StreamFinished.uliTotalFileSize.QuadPart;
        break;
    case COPYFILE2_CALLBACK_ERROR:
        transferred = message->Info.Error.uliTotalBytesTransferred.QuadPart;
        total = message->Info.Error.uliTotalFileSize.QuadPart;
        break;
    default:
        break;
    }
    const ULONGLONG now = GetTickCount64();
    if (ctx->report && (transferred == total || now - ctx->last_report >= 100)) {
        ctx->last_report = now;
        ctx->report(transferred, total);
    }
    if (ctx->cancel && ctx->cancel->load()) return COPYFILE2_PROGRESS_CANCEL;
    if (ctx->pause && ctx->pause->load() && !ctx->pause_sent) {
        ctx->pause_sent = true;
        return COPYFILE2_PROGRESS_PAUSE;
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}

bool ReadEntryMetadata(const std::wstring& path, TransferEntry& entry) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry.attributes = data.dwFileAttributes;
    entry.created = data.ftCreationTime;
    entry.accessed = data.ftLastAccessTime;
    entry.modified = data.ftLastWriteTime;
    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    entry.bytes = entry.directory ? 0 : size.QuadPart;
    return true;
}

bool PathExists(const std::wstring& path, bool* directory = nullptr) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (directory) *directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

std::wstring Win32Message(DWORD code) {
    wchar_t* raw = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring text = raw ? raw : L"文件操作失败";
    if (raw) LocalFree(raw);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return text;
}

bool EnsureDirectories(const std::wstring& dir, std::wstring& failure) {
    if (dir.empty() || IsVolumeRoot(dir)) return true;
    bool is_directory = false;
    if (PathExists(dir, &is_directory)) {
        if (is_directory) return true;
        failure = L"无法创建目标目录：" + dir + L" | 目标已存在且不是文件夹";
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(dir), error);
    if (error) {
        failure = L"无法创建目标目录：" + dir + L" | " + Win32Message(error.value());
        return false;
    }
    return true;
}

std::wstring UniqueCopyPath(const std::wstring& destination, bool directory) {
    if (!PathExists(destination)) return destination;
    const std::wstring parent = ParentOf(destination);
    const std::wstring leaf = FileName(destination);
    std::wstring stem = leaf;
    std::wstring extension;
    if (!directory) {
        const size_t dot = leaf.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            stem = leaf.substr(0, dot);
            extension = leaf.substr(dot);
        }
    }
    for (unsigned index = 1; index < 10000; ++index) {
        std::wstring name = stem + L" - 副本";
        if (index > 1) name += L" (" + std::to_wstring(index) + L")";
        std::wstring candidate = JoinPath(parent, name + extension);
        if (!PathExists(candidate)) return candidate;
    }
    return JoinPath(parent, stem + L" - 副本 " + std::to_wstring(GetTickCount64()) + extension);
}

std::wstring UniqueTemporaryPath(const std::wstring& destination,
                                 const wchar_t* marker,
                                 uint64_t task_id,
                                 size_t index) {
    const std::wstring base = destination + marker + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(task_id) + L"-" + std::to_wstring(index);
    if (!PathExists(base)) return base;
    for (unsigned suffix = 2; suffix < 10000; ++suffix) {
        const std::wstring candidate = base + L"-" + std::to_wstring(suffix);
        if (!PathExists(candidate)) return candidate;
    }
    return base + L"-" + std::to_wstring(GetTickCount64());
}

bool SetCopiedDirectoryMetadata(const TransferEntry& entry) {
    HANDLE handle = CreateFileW(entry.destination.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const BOOL times = SetFileTime(handle, &entry.created, &entry.accessed, &entry.modified);
    CloseHandle(handle);
    DWORD attrs = entry.attributes & ~(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY);
    if (attrs == 0) attrs = FILE_ATTRIBUTE_NORMAL;
    SetFileAttributesW(entry.destination.c_str(), attrs);
    return times != FALSE;
}

bool CopyReparsePoint(const TransferEntry& entry, std::wstring& error) {
    HANDLE source = CreateFileW(entry.source.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        error = Win32Message(GetLastError()) + L" | " + entry.source;
        return false;
    }
    std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD bytes = 0;
    const BOOL read = DeviceIoControl(source, FSCTL_GET_REPARSE_POINT, nullptr, 0,
        buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr);
    CloseHandle(source);
    if (!read) {
        error = Win32Message(GetLastError()) + L" | " + entry.source;
        return false;
    }

    if (entry.directory) {
        if (!CreateDirectoryW(entry.destination.c_str(), nullptr)) {
            error = Win32Message(GetLastError()) + L" | " + entry.destination;
            return false;
        }
    } else {
        HANDLE placeholder = CreateFileW(entry.destination.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (placeholder == INVALID_HANDLE_VALUE) {
            error = Win32Message(GetLastError()) + L" | " + entry.destination;
            return false;
        }
        CloseHandle(placeholder);
    }

    HANDLE destination = CreateFileW(entry.destination.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (destination == INVALID_HANDLE_VALUE) {
        error = Win32Message(GetLastError()) + L" | " + entry.destination;
        if (entry.directory) RemoveDirectoryW(entry.destination.c_str());
        else DeleteFileW(entry.destination.c_str());
        return false;
    }
    DWORD written = 0;
    const BOOL set = DeviceIoControl(destination, FSCTL_SET_REPARSE_POINT,
        buffer.data(), bytes, nullptr, 0, &written, nullptr);
    if (set) SetFileTime(destination, &entry.created, &entry.accessed, &entry.modified);
    CloseHandle(destination);
    if (!set) {
        error = Win32Message(GetLastError()) + L" | " + entry.destination;
        if (entry.directory) RemoveDirectoryW(entry.destination.c_str());
        else DeleteFileW(entry.destination.c_str());
        return false;
    }
    DWORD attributes = entry.attributes & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
    if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
    SetFileAttributesW(entry.destination.c_str(), attributes);
    return true;
}

bool StartsWithPath(const std::wstring& path, const std::wstring& prefix) {
    if (path.size() < prefix.size() || _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0)
        return false;
    return path.size() == prefix.size() || path[prefix.size()] == L'\\' || path[prefix.size()] == L'/';
}

} // namespace

OpsManager::~OpsManager() {
    Stop();
}

void OpsManager::Start(std::function<void()> notify) {
    if (running_) return;
    notify_ = std::move(notify);
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        menu_running_ = true;
    }
    thread_ = std::thread([this] { WorkerThread(); });
    menu_thread_ = std::thread([this] { MenuThread(); });
}

void OpsManager::Stop() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        menu_running_ = false;
    }
    menu_cv_.notify_all();
    if (menu_thread_.joinable()) menu_thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    CancelCurrent(); // unblock an in-flight RunShellOp via DONE(cancelled)
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

uint64_t OpsManager::Submit(OpRequest req) {
    QueueItem item;
    item.req = std::move(req);
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = next_seq_++;
        item.seq = seq;
        queue_.push_back(std::move(item));
    }
    if (notify_) notify_();
    cv_.notify_one();
    return seq;
}

void OpsManager::OpenWith(const std::wstring& path) {
    QueueItem item;
    item.open_path = path;
    item.open_verb = L"open";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item.seq = next_seq_++;
        queue_.push_back(std::move(item));
    }
    cv_.notify_one();
}

void OpsManager::ShowProperties(const std::wstring& path) {
    ExecuteVerb(path, L"properties");
}

void OpsManager::ExecuteVerb(const std::wstring& path, const std::wstring& verb) {
    QueueItem item;
    item.open_path = path;
    item.open_verb = verb.empty() ? L"open" : verb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item.seq = next_seq_++;
        queue_.push_back(std::move(item));
    }
    cv_.notify_one();
}

void OpsManager::OpenWithApp(const std::wstring& app_exe, const std::wstring& file) {
    QueueItem item;
    item.open_path = ParentOf(file);      // lpDirectory
    item.open_file = app_exe;
    item.open_verb = L"open";
    item.open_args = L"\"" + file + L"\"";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item.seq = next_seq_++;
        queue_.push_back(std::move(item));
    }
    cv_.notify_one();
}

std::wstring TerminalCommandLine(const std::wstring& dir) {
    // wt.exe -d "<dir>" — never built with a trailing backslash before the
    // closing quote (would escape it).
    std::wstring d = dir;
    while (d.size() > 3 && d.back() == L'\\') d.pop_back();
    return L"-d \"" + d + L"\"";
}

void OpsManager::OpenTerminal(const std::wstring& dir) {
    QueueItem item;
    item.open_path = dir;
    item.open_file = L"wt.exe";
    item.open_verb = L"open";
    item.open_args = TerminalCommandLine(dir);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item.seq = next_seq_++;
        queue_.push_back(std::move(item));
    }
    cv_.notify_one();
}

void OpsManager::CancelCurrent() {
    if (transfer_active_.load()) {
        transfer_cancel_.store(true);
        transfer_pause_.store(false);
        transfer_control_cv_.notify_all();
        SetStatus([](OpStatus& status) {
            if (status.active) status.phase = OpPhase::Cancelling;
        });
    }
    uint32_t id = current_req_id_.load();
    if (id != 0) ipc::ShellClient::Instance().Cancel(id);
}

void OpsManager::PauseCurrent() {
    if (!transfer_active_.load() || transfer_cancel_.load()) return;
    transfer_pause_.store(true);
}

void OpsManager::ResumeCurrent() {
    if (!transfer_active_.load()) return;
    transfer_pause_.store(false);
    transfer_control_cv_.notify_all();
}

std::optional<ConflictItemInfo> OpsManager::PendingConflict() const {
    std::lock_guard<std::mutex> lock(transfer_control_mutex_);
    return pending_conflict_;
}

void OpsManager::ResolveConflict(uint64_t token, ConflictChoice choice, bool apply_to_all) {
    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        if (!pending_conflict_ || pending_conflict_->token != token) return;
        resolved_conflict_token_ = token;
        resolved_conflict_choice_ = choice;
        resolved_conflict_apply_all_ = apply_to_all;
    }
    transfer_control_cv_.notify_all();
}

OpStatus OpsManager::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void OpsManager::SetStatus(const std::function<void(OpStatus&)>& fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(status_);
    }
    if (notify_) notify_();
}

bool OpsManager::CanUndo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !undo_.empty() && undo_.back().supported;
}

std::wstring OpsManager::UndoLabel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (undo_.empty()) return L"";
    const UndoEntry& e = undo_.back();
    if (!e.supported) return std::wstring(L"撤销") + OpVerb(e.type) + L"（不支持）";
    OpRequest r;
    r.type = e.type;
    r.sources = e.sources;
    r.dest_dir = e.dest_dir;
    r.new_name = e.new_name;
    return std::wstring(L"撤销") + Describe(r);
}

void OpsManager::PushUndo(const OpRequest& req,
                          const std::vector<std::wstring>* actual_destinations) {
    CompletedOperation completed;
    completed.type = req.type;
    completed.sources = req.sources;
    if (actual_destinations) {
        completed.destinations = *actual_destinations;
    } else if (req.type == OpType::Rename && !req.sources.empty()) {
        completed.destinations.push_back(JoinPath(ParentOf(req.sources.front()), req.new_name));
    } else if (req.type == OpType::Copy || req.type == OpType::Move) {
        for (const auto& source : req.sources)
            completed.destinations.push_back(JoinPath(req.dest_dir, FileName(source)));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions_.push_back(std::move(completed));
    }
    if (req.is_undo) return;
    UndoEntry e;
    e.type = req.type;
    e.sources = req.sources;
    if (actual_destinations) e.destinations = *actual_destinations;
    e.dest_dir = req.dest_dir;
    e.new_name = req.new_name;
    if (req.type == OpType::RealDelete) return;              // never undoable, not recorded
    if (req.type == OpType::RestoreRecycle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.push_back(std::move(e));
}

std::vector<CompletedOperation> OpsManager::DrainCompletions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CompletedOperation> out;
    out.reserve(completions_.size());
    while (!completions_.empty()) {
        out.push_back(std::move(completions_.front()));
        completions_.pop_front();
    }
    return out;
}

void OpsManager::Undo() {
    UndoEntry e;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (undo_.empty()) return;
        e = undo_.back();
        if (!e.supported) {
            // Leave the entry; report why it cannot be undone.
            status_.last_error = L"回收站删除暂不支持撤销（1B-2 恢复方案：枚举 $Recycle.Bin 还原）";
        } else {
            undo_.pop_back();
        }
    }
    if (!e.supported) {
        if (notify_) notify_();
        return;
    }

    switch (e.type) {
    case OpType::Move: {
        // Move each dest copy back to its original parent directory.
        for (size_t i = 0; i < e.sources.size(); ++i) {
            const auto& src = e.sources[i];
            OpRequest inv;
            inv.type = OpType::Move;
            inv.is_undo = true;
            inv.sources.push_back(i < e.destinations.size()
                ? e.destinations[i] : JoinPath(e.dest_dir, FileName(src)));
            inv.dest_dir = ParentOf(src);
            Submit(std::move(inv));
        }
        break;
    }
    case OpType::Rename: {
        OpRequest inv;
        inv.type = OpType::Rename;
        inv.is_undo = true;
        std::wstring new_path = JoinPath(ParentOf(e.sources.front()), e.new_name);
        inv.sources.push_back(new_path);
        inv.new_name = FileName(e.sources.front());
        Submit(std::move(inv));
        break;
    }
    case OpType::Copy: {
        // Explorer semantics: undo copy = delete the produced copies (to bin).
        OpRequest inv;
        inv.type = OpType::RecycleDelete;
        inv.is_undo = true;
        if (!e.destinations.empty()) inv.sources = e.destinations;
        else for (const auto& src : e.sources)
            inv.sources.push_back(JoinPath(e.dest_dir, FileName(src)));
        Submit(std::move(inv));
        break;
    }
    case OpType::CreateFolder:
    case OpType::CreateTextFile: {
        // Undo create = recycle-delete the created item.
        OpRequest inv;
        inv.type = OpType::RecycleDelete;
        inv.is_undo = true;
        inv.sources = e.sources;
        Submit(std::move(inv));
        break;
    }
    case OpType::RecycleDelete: {
        OpRequest inv;
        inv.type = OpType::RestoreRecycle;
        inv.is_undo = true;
        inv.sources = e.sources;
        Submit(std::move(inv));
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Worker thread: serialized queue -> ShellClient -> event wait.
// ---------------------------------------------------------------------------
void OpsManager::WorkerThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    ipc::ShellClient::Callbacks cb;
    cb.progress = [this](uint32_t, float pct, std::wstring item,
                         uint32_t items_done, uint32_t total_items) {
        SetStatus([&](OpStatus& st) {
            st.percent = pct;
            st.current_item = item;
            if (total_items > 0) st.total_items = total_items;
            st.completed_items = (std::min)(static_cast<uint64_t>(items_done),
                                             st.total_items);
            if (!item.empty()) {
                std::wstring base = st.summary;
                auto sep = base.find(L"  (");
                if (sep != std::wstring::npos) base.erase(sep);
                wchar_t buf[32];
                swprintf_s(buf, L"  (%.0f%%)", pct);
                st.summary = base + buf;
            }
        });
    };
    // Done results land here (reader thread) and RunShellOp waits on them.
    // Context-menu invoke completions share RSP_DONE; route them away first so
    // they can never overwrite the file-op completion a RunShellOp is waiting on.
    cb.done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error) {
        if (ConsumeCtxInvokeDone(id)) {
            ctx_invoke_done_.fetch_add(1, std::memory_order_acq_rel);
            if (notify_) notify_();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_id_ = id;
            done_hr_ = hr;
            done_cancelled_ = cancelled;
            done_error_ = std::move(error);
            done_ready_ = true;
        }
        done_cv_.notify_one();
    };
    cb.ctx_items = [this](uint32_t id, std::vector<ipc::CtxMenuItem> items) {
        std::vector<ShellMenuItem> out;
        out.reserve(items.size());
        for (auto& it : items) {
            ShellMenuItem m;
            m.id = it.id;
            m.enabled = it.enabled;
            m.separator_after = it.separator_after;
            m.has_children = it.has_children;
            m.child = it.child;
            m.verb = std::move(it.verb);
            m.text = std::move(it.text);
            out.push_back(std::move(m));
        }
        OnCtxItems(id, std::move(out));
    };
    ipc::ShellClient::Instance().Start(cb);

    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            if (!running_ && queue_.empty()) break;
            item = std::move(queue_.front());
            queue_.pop_front();
        }

        if (!item.open_path.empty()) {
            // Plan §6.2: ShellExecuteEx lives on the ops pool, never the UI thread.
            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.lpVerb = item.open_verb.empty() ? L"open" : item.open_verb.c_str();
            sei.lpFile = item.open_file.empty() ? item.open_path.c_str()
                                                : item.open_file.c_str();
            sei.lpParameters = item.open_args.empty() ? nullptr : item.open_args.c_str();
            sei.lpDirectory = item.open_file.empty() ? nullptr : item.open_path.c_str();
            sei.nShow = SW_SHOWNORMAL;
            sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            ShellExecuteExW(&sei); // best effort; errors surface via the OS association UI
            continue;
        }

        if (item.req.type == OpType::Copy || item.req.type == OpType::Move)
            RunTransfer(item.req, item.seq);
        else
            RunShellOp(item.req, item.seq);
    }

    ipc::ShellClient::Instance().Stop();
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// Context-menu forwarding thread: keeps ShellClient::Submit (which may spawn
// or reconnect to pulse_shell) off the UI thread and out of the transfer queue.
// ---------------------------------------------------------------------------
void OpsManager::SetShellMenuCallback(ShellMenuCallback cb) {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    menu_cb_ = std::move(cb);
}

uint32_t OpsManager::QueryShellMenu(std::vector<std::wstring> paths, void* owner_hwnd,
                                    bool background, bool extended) {
    MenuJob job;
    job.kind = MenuJob::Kind::Query;
    job.owner_hwnd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(owner_hwnd));
    job.background = background;
    job.extended = extended;
    job.paths = std::move(paths);
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        if (!menu_running_) return 0;
        token = next_menu_token_++;
        job.token = token;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
    return token;
}

void OpsManager::InvokeShellMenu(uint32_t token, uint32_t item_id) {
    MenuJob job;
    job.kind = MenuJob::Kind::Invoke;
    job.token = token;
    job.item_id = item_id;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        if (!menu_running_) return;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
}

void OpsManager::CloseShellMenu(uint32_t token) {
    MenuJob job;
    job.kind = MenuJob::Kind::Close;
    job.token = token;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        if (!menu_running_) return;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
}

bool OpsManager::ConsumeCtxInvokeDone(uint32_t id) {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    return ctx_invoke_ids_.erase(id) != 0;
}

bool OpsManager::TakeCtxInvokeDone() {
    return ctx_invoke_done_.exchange(0, std::memory_order_acq_rel) != 0;
}

void OpsManager::OnCtxItems(uint32_t client_id, std::vector<ShellMenuItem> items) {
    ShellMenuCallback cb;
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        auto it = menu_token_by_session_.find(client_id);
        if (it == menu_token_by_session_.end()) return; // session already closed
        token = it->second;
        cb = menu_cb_;
    }
    if (cb) cb(token, std::move(items));
}

void OpsManager::MenuThread() {
    for (;;) {
        MenuJob job;
        {
            std::unique_lock<std::mutex> lock(menu_mutex_);
            menu_cv_.wait(lock, [this] { return !menu_queue_.empty() || !menu_running_; });
            if (!menu_running_) break; // drop queued jobs; sessions die with the host
            job = std::move(menu_queue_.front());
            menu_queue_.pop_front();
        }
        auto& client = ipc::ShellClient::Instance();
        switch (job.kind) {
        case MenuJob::Kind::Query: {
            const uint32_t id = client.QueryContextMenu(
                job.paths, job.owner_hwnd, job.background, job.extended);
            std::lock_guard<std::mutex> lock(menu_mutex_);
            if (id != 0) {
                menu_session_by_token_[job.token] = id;
                menu_token_by_session_[id] = job.token;
            }
            break;
        }
        case MenuJob::Kind::Invoke: {
            uint32_t session = 0;
            {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                auto it = menu_session_by_token_.find(job.token);
                if (it == menu_session_by_token_.end()) break;
                session = it->second;
                menu_session_by_token_.erase(it);
                menu_token_by_session_.erase(session);
            }
            const uint32_t id = client.InvokeContextMenu(session, job.item_id);
            if (id != 0) {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                ctx_invoke_ids_.insert(id);
            }
            break;
        }
        case MenuJob::Kind::Close: {
            uint32_t session = 0;
            {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                auto it = menu_session_by_token_.find(job.token);
                if (it == menu_session_by_token_.end()) break;
                session = it->second;
                menu_session_by_token_.erase(it);
                menu_token_by_session_.erase(session);
            }
            client.CloseContextMenu(session);
            break;
        }
        }
    }
}

void OpsManager::RunTransfer(const OpRequest& req, uint64_t task_id) {
    transfer_active_.store(true);
    transfer_cancel_.store(false);
    transfer_pause_.store(false);
    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        pending_conflict_.reset();
        resolved_conflict_token_ = 0;
    }

    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Scanning;
        st.percent = -1.0f;
        st.summary = std::wstring(L"正在准备") + OpVerb(req.type) + L"…";
        st.last_error.clear();
        st.source_label = req.sources.empty() ? L"" : FileName(req.sources.front());
        st.destination_label = FileName(req.dest_dir);
        if (st.destination_label.empty()) st.destination_label = req.dest_dir;
        st.current_item.clear();
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.eta_seconds = 0;
    });

    std::wstring failure;
    bool cancelled = false;
    std::vector<TransferEntry> entries;
    std::vector<bool> root_destination_preexisting;
    std::vector<std::wstring> completed_sources;
    std::vector<std::wstring> completed_destinations;
    struct ReplacementBackup {
        std::wstring original;
        std::wstring backup;
    };
    std::vector<ReplacementBackup> replacement_backups;

    if (req.sources.empty() || req.dest_dir.empty()) {
        failure = L"复制或移动请求缺少来源/目标";
    }

    // A single unobstructed same-volume move is an atomic rename and should
    // not be expanded into per-file work.
    if (failure.empty() && req.type == OpType::Move && req.sources.size() == 1) {
        const std::wstring destination = JoinPath(req.dest_dir, FileName(req.sources.front()));
        if (!PathExists(destination)) {
            if (MoveFileExW(req.sources.front().c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                completed_sources = req.sources;
                completed_destinations.push_back(destination);
                PushUndo(req, &completed_destinations);
                SetStatus([&](OpStatus& st) {
                    st.active = false;
                    st.phase = OpPhase::Completed;
                    st.percent = 100.0f;
                    st.total_items = st.completed_items = 1;
                    st.summary = Describe(req) + L" 完成";
                    st.completed_ops++;
                });
                transfer_active_.store(false);
                return;
            }
            const DWORD move_error = GetLastError();
            if (move_error != ERROR_NOT_SAME_DEVICE)
                failure = Win32Message(move_error) + L" | " + req.sources.front();
        }
    }

    namespace fsys = std::filesystem;
    if (failure.empty()) {
        for (const auto& source : req.sources) {
            TransferEntry root;
            root.source = source;
            root.destination = JoinPath(req.dest_dir, FileName(source));
            if (!ReadEntryMetadata(source, root)) {
                failure = Win32Message(GetLastError()) + L" | " + source;
                break;
            }
            const bool same_location =
                _wcsicmp(root.source.c_str(), root.destination.c_str()) == 0;
            if (same_location && req.type == OpType::Move) {
                // Dropping an item onto its current folder is a no-op, like Explorer.
                continue;
            }
            if (same_location && req.type == OpType::Copy) {
                root.destination = UniqueCopyPath(root.destination, root.directory);
            }
            if (root.directory && StartsWithPath(req.dest_dir, root.source)) {
                failure = L"不能将目录复制或移动到其自身内部：" + source;
                break;
            }
            root_destination_preexisting.push_back(PathExists(root.destination));
            entries.push_back(root);
            if (!root.directory || root.reparse) continue;

            std::error_code ec;
            fsys::recursive_directory_iterator it(fsys::path(source),
                fsys::directory_options::skip_permission_denied, ec);
            fsys::recursive_directory_iterator end;
            if (ec) {
                failure = L"无法读取来源目录：" + source + L" | " + Win32Message(ec.value());
                break;
            }
            for (; it != end; it.increment(ec)) {
                if (ec) {
                    failure = L"扫描来源目录失败：" + source + L" | " + Win32Message(ec.value());
                    break;
                }
                TransferEntry child;
                child.source = it->path().wstring();
                child.destination = (fsys::path(root.destination) /
                    it->path().lexically_relative(fsys::path(source))).wstring();
                if (!ReadEntryMetadata(child.source, child)) {
                    failure = Win32Message(GetLastError()) + L" | " + child.source;
                    break;
                }
                entries.push_back(std::move(child));
                if (entries.back().reparse && entries.back().directory) it.disable_recursion_pending();
            }
            if (!failure.empty()) break;
        }
    }

    uint64_t total_bytes = 0;
    for (const auto& entry : entries) total_bytes += entry.bytes;
    SetStatus([&](OpStatus& st) {
        st.total_bytes = total_bytes;
        st.total_items = entries.size();
        if (failure.empty()) {
            st.phase = OpPhase::Running;
            st.percent = 0.0f;
            st.summary = L"正在" + std::wstring(OpVerb(req.type)) + L" "
                + std::to_wstring(entries.size()) + L" 个项目";
        }
    });

    bool apply_all = req.collision_policy != CollisionPolicy::System;
    ConflictChoice repeated = req.collision_policy == CollisionPolicy::KeepBoth
        ? ConflictChoice::KeepBoth : ConflictChoice::Replace;
    std::vector<std::wstring> skipped_prefixes;
    uint64_t committed_bytes = 0;
    uint64_t published_bytes = 0;
    uint64_t processed_items = 0;
    TransferRateEstimator rate;
    rate.Reset(GetTickCount64(), 0);

    auto publish_progress = [&](const TransferEntry& entry, uint64_t file_done,
                                uint64_t file_total, uint64_t completed_items) {
        const uint64_t bounded_file_done = file_total > 0
            ? (std::min)(file_done, file_total) : file_done;
        uint64_t overall = committed_bytes + bounded_file_done;
        if (overall < committed_bytes) overall = UINT64_MAX;
        overall = (std::max)(published_bytes, overall);
        if (total_bytes > 0) overall = (std::min)(overall, total_bytes);
        published_bytes = overall;
        const ULONGLONG now = GetTickCount64();
        rate.Observe(now, overall, total_bytes);
        SetStatus([&](OpStatus& st) {
            st.current_item = FileName(entry.source);
            st.transferred_bytes = overall;
            st.completed_items = completed_items;
            st.bytes_per_second = rate.speed();
            st.peak_bytes_per_second = (std::max)(st.peak_bytes_per_second, rate.speed());
            if (st.total_bytes > 0) {
                st.percent = static_cast<float>((std::min)(100.0,
                    static_cast<double>(overall) * 100.0 / st.total_bytes));
                st.eta_seconds = rate.eta_seconds();
            } else if (st.total_items > 0) {
                st.percent = static_cast<float>(completed_items * 100.0 / st.total_items);
            }
        });
    };

    auto reset_rate = [&] {
        rate.Reset(GetTickCount64(), published_bytes);
        SetStatus([&](OpStatus& st) {
            st.bytes_per_second = 0.0;
            st.eta_seconds = 0;
        });
    };

    auto mark_skipped = [&](const TransferEntry& entry) {
        total_bytes -= (std::min)(total_bytes, entry.bytes);
        ++processed_items;
        rate.Observe(GetTickCount64(), published_bytes, total_bytes);
        SetStatus([&](OpStatus& st) {
            st.total_bytes = total_bytes;
            st.transferred_bytes = published_bytes;
            st.completed_items = processed_items;
            st.current_item = FileName(entry.source);
            st.eta_seconds = rate.eta_seconds();
            if (st.total_bytes > 0) {
                st.percent = static_cast<float>((std::min)(100.0,
                    static_cast<double>(published_bytes) * 100.0 / st.total_bytes));
            } else if (st.total_items > 0) {
                st.percent = static_cast<float>(processed_items * 100.0 / st.total_items);
            }
        });
    };

    auto remaining_conflicts = [&](size_t start) {
        size_t count = 0;
        for (size_t j = start; j < entries.size(); ++j) {
            bool dest_dir = false;
            if (PathExists(entries[j].destination, &dest_dir) &&
                !(entries[j].directory && !entries[j].reparse && dest_dir)) ++count;
        }
        return count;
    };

    for (size_t index = 0; failure.empty() && index < entries.size(); ++index) {
        auto& entry = entries[index];
        if (transfer_cancel_.load()) { cancelled = true; break; }
        bool skipped = false;
        for (const auto& prefix : skipped_prefixes) {
            if (StartsWithPath(entry.source, prefix)) { skipped = true; break; }
        }
        if (skipped) {
            mark_skipped(entry);
            continue;
        }

        bool destination_is_directory = false;
        bool destination_exists = PathExists(entry.destination, &destination_is_directory);
        ConflictChoice choice = repeated;
        const bool conflict = destination_exists &&
            !(entry.directory && !entry.reparse && destination_is_directory);
        bool selected_apply_all = false;
        if (conflict && !apply_all) {
            ConflictItemInfo info;
            info.task_id = task_id;
            info.source = entry.source;
            info.destination = entry.destination;
            info.source_size = entry.bytes;
            info.source_modified = entry.modified;
            info.source_is_directory = entry.directory;
            TransferEntry destination_entry;
            if (ReadEntryMetadata(entry.destination, destination_entry)) {
                info.destination_size = destination_entry.bytes;
                info.destination_modified = destination_entry.modified;
                info.destination_is_directory = destination_entry.directory;
            }
            info.remaining = remaining_conflicts(index);
            {
                std::lock_guard<std::mutex> lock(transfer_control_mutex_);
                info.token = next_conflict_token_++;
                pending_conflict_ = info;
                resolved_conflict_token_ = 0;
            }
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::WaitingForConflict;
                st.current_item = FileName(entry.source);
                st.summary = L"正在等待处理文件冲突";
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || resolved_conflict_token_ == info.token;
            });
            if (transfer_cancel_.load()) {
                pending_conflict_.reset();
                cancelled = true;
                break;
            }
            choice = resolved_conflict_choice_;
            selected_apply_all = resolved_conflict_apply_all_;
            pending_conflict_.reset();
            lock.unlock();
            if (choice == ConflictChoice::Cancel) {
                cancelled = true;
                transfer_cancel_.store(true);
                break;
            }
            if (selected_apply_all) {
                apply_all = true;
                repeated = choice;
            }
            rate.Reset(GetTickCount64(), published_bytes);
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = L"正在" + std::wstring(OpVerb(req.type)) + L" "
                    + std::to_wstring(entries.size()) + L" 个项目";
            });
        }

        if (conflict && choice == ConflictChoice::Skip) {
            if (entry.directory) skipped_prefixes.push_back(entry.source);
            mark_skipped(entry);
            continue;
        }
        if (conflict && choice == ConflictChoice::KeepBoth) {
            const std::wstring old_destination = entry.destination;
            const std::wstring unique = UniqueCopyPath(entry.destination, entry.directory);
            entry.destination = unique;
            if (entry.directory) {
                for (size_t j = index + 1; j < entries.size(); ++j) {
                    if (StartsWithPath(entries[j].destination, old_destination))
                        entries[j].destination = unique + entries[j].destination.substr(old_destination.size());
                }
            }
            destination_exists = false;
            destination_is_directory = false;
        }
        if (conflict && choice == ConflictChoice::Replace &&
            (entry.directory != destination_is_directory || entry.reparse)) {
            if (req.type == OpType::Move) {
                failure = L"移动时无法原子替换不同类型或重解析目标：" + entry.destination;
                break;
            }
            const std::wstring backup = UniqueTemporaryPath(
                entry.destination, L".pulse-backup-", task_id, index);
            if (!MoveFileExW(entry.destination.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
                failure = L"无法安全备份要替换的目标项目：" + entry.destination
                    + L" | " + Win32Message(GetLastError());
                break;
            }
            replacement_backups.push_back({ entry.destination, backup });
            destination_exists = false;
            destination_is_directory = false;
        }

        if (entry.reparse) {
            if (!EnsureDirectories(ParentOf(entry.destination), failure)) break;
            if (!CopyReparsePoint(entry, failure)) break;
            if (req.type == OpType::Move) {
                const BOOL removed = entry.directory ? RemoveDirectoryW(entry.source.c_str())
                                                     : DeleteFileW(entry.source.c_str());
                if (!removed) {
                    failure = Win32Message(GetLastError()) + L" | " + entry.source;
                    break;
                }
            }
            completed_sources.push_back(entry.source);
            completed_destinations.push_back(entry.destination);
            ++processed_items;
            publish_progress(entry, entry.bytes, entry.bytes, processed_items);
            committed_bytes += entry.bytes;
            continue;
        }

        if (entry.directory) {
            if (!EnsureDirectories(entry.destination, failure)) break;
            ++processed_items;
            SetStatus([&](OpStatus& st) {
                st.current_item = FileName(entry.source);
                st.completed_items = processed_items;
                if (st.total_bytes == 0 && st.total_items > 0)
                    st.percent = static_cast<float>(st.completed_items * 100.0 / st.total_items);
            });
            continue;
        }

        if (!EnsureDirectories(ParentOf(entry.destination), failure)) break;

        std::wstring copy_destination = entry.destination;
        const bool replacing = destination_exists && choice == ConflictChoice::Replace;
        if (replacing) {
            copy_destination = UniqueTemporaryPath(
                entry.destination, L".pulse-copy-", task_id, index);
        }

        uint64_t known_file_total = entry.bytes;
        CopyProgressContext context;
        context.cancel = &transfer_cancel_;
        context.pause = &transfer_pause_;
        context.report = [&](uint64_t done, uint64_t actual_total) {
            if (actual_total > 0 && actual_total != known_file_total) {
                if (actual_total > known_file_total) total_bytes += actual_total - known_file_total;
                else total_bytes -= (std::min)(total_bytes, known_file_total - actual_total);
                known_file_total = actual_total;
                entry.bytes = actual_total;
                SetStatus([&](OpStatus& st) { st.total_bytes = total_bytes; });
            }
            publish_progress(entry, done, actual_total, processed_items);
        };

        HRESULT copy_result = E_FAIL;
        bool resume = false;
        DWORD extra_flags = COPY_FILE_COPY_SYMLINK;
        for (;;) {
            context.pause_sent = false;
            COPYFILE2_EXTENDED_PARAMETERS parameters{};
            parameters.dwSize = sizeof(parameters);
            parameters.dwCopyFlags = extra_flags;
            if (!resume && !replacing && !destination_exists)
                parameters.dwCopyFlags |= COPY_FILE_FAIL_IF_EXISTS;
            if (resume) parameters.dwCopyFlags |= COPY_FILE_RESUME_FROM_PAUSE;
            parameters.pfCancel = nullptr;
            parameters.pProgressRoutine = CopyProgress;
            parameters.pvCallbackContext = &context;
            copy_result = CopyFile2(entry.source.c_str(), copy_destination.c_str(), &parameters);
            if (copy_result == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) && extra_flags != 0 && !resume) {
                extra_flags = 0;
                continue;
            }
            if (copy_result != HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED)) break;

            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Paused;
                st.summary = std::wstring(OpVerb(req.type)) + L"已暂停";
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || !transfer_pause_.load();
            });
            if (transfer_cancel_.load()) {
                cancelled = true;
                break;
            }
            resume = true;
            reset_rate();
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = L"正在" + std::wstring(OpVerb(req.type)) + L" "
                    + std::to_wstring(entries.size()) + L" 个项目";
            });
        }
        if (cancelled || transfer_cancel_.load()) {
            cancelled = true;
            DeleteFileW(copy_destination.c_str());
            break;
        }
        if (FAILED(copy_result)) {
            DeleteFileW(copy_destination.c_str());
            failure = Win32Message(HRESULT_CODE(copy_result)) + L" | " + entry.source;
            break;
        }

        if (replacing) {
            DWORD old_attributes = GetFileAttributesW(entry.destination.c_str());
            if (old_attributes != INVALID_FILE_ATTRIBUTES &&
                (old_attributes & FILE_ATTRIBUTE_READONLY))
                SetFileAttributesW(entry.destination.c_str(), old_attributes & ~FILE_ATTRIBUTE_READONLY);
            if (!ReplaceFileW(entry.destination.c_str(), copy_destination.c_str(), nullptr,
                              REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) &&
                !MoveFileExW(copy_destination.c_str(), entry.destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const DWORD error = GetLastError();
                DeleteFileW(copy_destination.c_str());
                failure = Win32Message(error) + L" | " + entry.destination;
                break;
            }
        }

        if (req.type == OpType::Move) {
            DWORD source_attributes = GetFileAttributesW(entry.source.c_str());
            if (source_attributes != INVALID_FILE_ATTRIBUTES &&
                (source_attributes & FILE_ATTRIBUTE_READONLY))
                SetFileAttributesW(entry.source.c_str(), source_attributes & ~FILE_ATTRIBUTE_READONLY);
            if (!DeleteFileW(entry.source.c_str())) {
                failure = Win32Message(GetLastError()) + L" | " + entry.source;
                break;
            }
        }
        completed_sources.push_back(entry.source);
        completed_destinations.push_back(entry.destination);
        ++processed_items;
        publish_progress(entry, entry.bytes, entry.bytes, processed_items);
        committed_bytes += entry.bytes;
    }

    if (failure.empty() && !cancelled) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (!it->directory) continue;
            if (req.type == OpType::Move) {
                RemoveDirectoryW(it->source.c_str());
            } else if (!it->reparse) {
                SetCopiedDirectoryMetadata(*it);
            }
        }
    }

    if (failure.empty() && !cancelled) {
        for (const auto& backup : replacement_backups) {
            std::error_code ignored;
            fsys::remove_all(fsys::path(backup.backup), ignored);
        }
    } else {
        for (auto it = replacement_backups.rbegin(); it != replacement_backups.rend(); ++it) {
            std::error_code ignored;
            fsys::remove_all(fsys::path(it->original), ignored);
            MoveFileExW(it->backup.c_str(), it->original.c_str(), MOVEFILE_WRITE_THROUGH);
        }
        for (size_t i = completed_destinations.size(); i-- > 0;) {
            const bool restored = std::any_of(replacement_backups.begin(),
                replacement_backups.end(), [&](const ReplacementBackup& backup) {
                    return StartsWithPath(completed_destinations[i], backup.original);
                });
            if (restored) {
                completed_destinations.erase(completed_destinations.begin() + i);
                completed_sources.erase(completed_sources.begin() + i);
            }
        }
    }

    if (failure.empty() && !cancelled) {
        OpRequest committed = req;
        committed.sources.clear();
        std::vector<std::wstring> committed_destinations;
        for (size_t root_index = 0; root_index < req.sources.size(); ++root_index) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const TransferEntry& entry) {
                return _wcsicmp(entry.source.c_str(), req.sources[root_index].c_str()) == 0;
            });
            if (found == entries.end()) continue;
            const std::wstring original_destination = JoinPath(req.dest_dir,
                                                               FileName(req.sources[root_index]));
            const bool independent_root = root_index >= root_destination_preexisting.size()
                || !root_destination_preexisting[root_index]
                || _wcsicmp(found->destination.c_str(), original_destination.c_str()) != 0;
            if (independent_root) {
                committed.sources.push_back(req.sources[root_index]);
                committed_destinations.push_back(found->destination);
                continue;
            }
            for (size_t item = 0; item < completed_sources.size(); ++item) {
                if (StartsWithPath(completed_sources[item], req.sources[root_index])) {
                    committed.sources.push_back(completed_sources[item]);
                    committed_destinations.push_back(completed_destinations[item]);
                }
            }
        }
        if (!committed_destinations.empty()) PushUndo(committed, &committed_destinations);
    } else if (!completed_destinations.empty()) {
        OpRequest partial = req;
        partial.sources = completed_sources;
        PushUndo(partial, &completed_destinations);
    }

    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        pending_conflict_.reset();
    }
    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.completed_ops++;
        st.bytes_per_second = 0.0;
        st.eta_seconds = 0;
        if (cancelled || transfer_cancel_.load()) {
            st.phase = OpPhase::Failed;
            st.last_error = L"已取消";
            st.summary = std::wstring(OpVerb(req.type)) + L"已取消";
        } else if (!failure.empty()) {
            st.phase = OpPhase::Failed;
            st.last_error = failure;
            st.summary = std::wstring(OpVerb(req.type)) + L"失败";
        } else {
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.transferred_bytes = st.total_bytes;
            st.completed_items = st.total_items;
            st.summary = Describe(req) + L" 完成";
            st.last_error.clear();
        }
    });
    transfer_active_.store(false);
}

void OpsManager::RunShellOp(const OpRequest& req, uint64_t task_id) {
    // Status: active.
    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Running;
        st.percent = 0.0f;
        st.summary = Describe(req);
        st.last_error.clear();
        st.source_label = req.sources.empty() ? L"" : FileName(req.sources.front());
        st.destination_label.clear();
        st.current_item = st.source_label;
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = req.sources.size();
        st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.eta_seconds = 0;
    });

    auto& client = ipc::ShellClient::Instance();
    uint32_t id = 0;
    switch (req.type) {
    case OpType::Copy:
    case OpType::Move:
        break;
    case OpType::RecycleDelete: id = client.DeleteRecycle(req.sources); break;
    case OpType::RealDelete: id = client.RealDelete(req.sources); break;
    case OpType::Rename:
        if (!req.sources.empty()) id = client.Rename(req.sources.front(), req.new_name);
        break;
    case OpType::CreateFolder:
        if (!req.sources.empty()) id = client.CreateFolder(req.sources.front());
        break;
    case OpType::CreateTextFile:
        if (!req.sources.empty()) id = client.CreateNewFile(req.sources.front());
        break;
    case OpType::RestoreRecycle:
        id = client.RestoreRecycle(req.sources);
        break;
    }
    current_req_id_.store(id);

    if (id == 0) {
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.phase = OpPhase::Failed;
            st.percent = -1.0f;
            st.last_error = L"操作层未启动";
            st.completed_ops++;
        });
        return;
    }

    // Wait for completion (cancel rides through the sink -> DONE(cancelled)).
    {
        std::unique_lock<std::mutex> lock(done_mutex_);
        done_cv_.wait(lock, [&] { return done_ready_ && done_id_ == id; });
    }
    uint32_t hr = done_hr_;
    bool cancelled = done_cancelled_;
    std::wstring error = std::move(done_error_);
    done_ready_ = false;
    current_req_id_.store(0);

    const bool ok = SUCCEEDED((HRESULT)hr) && !cancelled;
    if (ok) PushUndo(req);

    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.percent = -1.0f;
        st.completed_ops++;
        if (cancelled) {
            st.phase = OpPhase::Failed;
            st.last_error = L"已取消";
        } else if (FAILED((HRESULT)hr)) {
            st.phase = OpPhase::Failed;
            st.last_error = error.empty() ? L"操作失败" : error;
        } else {
            st.phase = OpPhase::Completed;
            st.completed_items = st.total_items;
            st.summary = Describe(req) + L" 完成";
        }
    });
}

// ---------------------------------------------------------------------------
// Undo stack JSON persistence (minimal parser, same style as StagingTray).
// ---------------------------------------------------------------------------
std::wstring OpsManager::UndoToJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring out = L"[\n";
    for (size_t i = 0; i < undo_.size(); ++i) {
        const auto& e = undo_[i];
        wchar_t head[64];
        swprintf_s(head, L"  {\"type\":%d,\"sup\":%s,", (int)e.type, e.supported ? L"true" : L"false");
        out += head;
        out += L"\"dest\":\"";
        pulse::json::Escape(e.dest_dir, out);
        out += L"\",\"name\":\"";
        pulse::json::Escape(e.new_name, out);
        out += L"\",\"src\":[";
        for (size_t j = 0; j < e.sources.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(e.sources[j], out);
            out += L"\"";
            if (j + 1 < e.sources.size()) out += L",";
        }
        out += L"],\"dst\":[";
        for (size_t j = 0; j < e.destinations.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(e.destinations[j], out);
            out += L"\"";
            if (j + 1 < e.destinations.size()) out += L",";
        }
        out += L"]}";
        if (i + 1 < undo_.size()) out += L",";
        out += L"\n";
    }
    out += L"]";
    return out;
}

bool OpsManager::UndoFromJson(const std::wstring& in) {
    std::deque<UndoEntry> parsed;
    size_t i = in.find(L'[');
    if (i == std::wstring::npos) return false;
    ++i;
    auto skipWs = [&] {
        while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' || in[i] == L'\r' || in[i] == L'\t' || in[i] == L',')) ++i;
    };
    auto skipSpace = [&] {
        while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' ||
               in[i] == L'\r' || in[i] == L'\t')) ++i;
    };
    auto readString = [&](std::wstring& out) -> bool {
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        ++i;
        out.clear();
        while (i < in.size() && in[i] != L'"') {
            if (in[i] == L'\\' && i + 1 < in.size()) {
                ++i;
                if (in[i] == L'n') out += L'\n';
                else if (in[i] == L'r') out += L'\r';
                else if (in[i] == L't') out += L'\t';
                else out += in[i];
            } else {
                out += in[i];
            }
            ++i;
        }
        if (i < in.size()) ++i;
        return true;
    };
    auto readValue = [&](const std::wstring& key, std::wstring& val) -> bool {
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        std::wstring k;
        if (!readString(k)) return false;
        skipWs();
        if (i >= in.size() || in[i] != L':') return false;
        ++i;
        skipWs();
        if (k != key) return false;
        if (i < in.size() && in[i] == L'"') return readString(val);
        size_t start = i;
        while (i < in.size() && in[i] != L',' && in[i] != L'}' && in[i] != L']') ++i;
        val = in.substr(start, i - start);
        while (!val.empty() && (val.back() == L' ')) val.pop_back();
        return true;
    };

    while (true) {
        skipWs();
        if (i >= in.size() || in[i] == L']') break;
        if (in[i] != L'{') return false;
        ++i;
        UndoEntry e;
        std::wstring v;
        if (!readValue(L"type", v)) return false;
        e.type = (OpType)_wtoi(v.c_str());
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"sup", v)) return false;
        e.supported = (v == L"true");
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"dest", e.dest_dir)) return false;
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"name", e.new_name)) return false;
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        // src array
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        std::wstring k;
        if (!readString(k) || k != L"src") return false;
        skipWs();
        if (i >= in.size() || in[i] != L':') return false;
        ++i;
        skipWs();
        if (i >= in.size() || in[i] != L'[') return false;
        ++i;
        while (true) {
            skipWs();
            if (i >= in.size()) return false;
            if (in[i] == L']') { ++i; break; }
            std::wstring s;
            if (!readString(s)) return false;
            e.sources.push_back(std::move(s));
        }
        skipSpace();
        // Version 2 adds actual committed destination paths. Version 1 ended
        // the object after src, so this field must remain optional.
        if (i < in.size() && in[i] == L',') {
            ++i;
            skipSpace();
            std::wstring destination_key;
            if (!readString(destination_key) || destination_key != L"dst") return false;
            skipSpace();
            if (i >= in.size() || in[i] != L':') return false;
            ++i;
            skipSpace();
            if (i >= in.size() || in[i] != L'[') return false;
            ++i;
            while (true) {
                skipWs();
                if (i >= in.size()) return false;
                if (in[i] == L']') { ++i; break; }
                std::wstring destination;
                if (!readString(destination)) return false;
                e.destinations.push_back(std::move(destination));
            }
            skipSpace();
        }
        if (i < in.size() && in[i] == L'}') ++i;
        parsed.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    undo_ = std::move(parsed);
    return true;
}

} // namespace pulse::ops
