#include "staging_adapter.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace pulse::extensions::native {
namespace {
std::atomic<uint64_t> next_stage{0};
bool Terminal(PulseStageState state) { return state >= PULSE_STAGE_COMPLETED && state <= PULSE_STAGE_CANCELLED; }
std::wstring Wide(PulseStringView value) {
    if (!value.data || !value.length || value.length > 131072 || std::memchr(value.data, 0, value.length)) throw std::runtime_error("Invalid destination");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 destination");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), result.data(), count); return result;
}
std::string Utf8(const std::wstring& value) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count && !value.empty()) throw std::runtime_error("Invalid path encoding");
    std::string result(static_cast<size_t>(count), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
struct DirectoryLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DirectoryLock() = default;
    DirectoryLock(const DirectoryLock&) = delete;
    DirectoryLock& operator=(const DirectoryLock&) = delete;
    DirectoryLock(DirectoryLock&& other) noexcept : handle(other.handle) { other.handle = INVALID_HANDLE_VALUE; }
    ~DirectoryLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
DirectoryLock LockDirectory(const std::filesystem::path& path, bool deleting = false) {
    DirectoryLock result;
    result.handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (deleting ? DELETE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (result.handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(result.handle, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Destination/staging directory rejected (Win32 " + std::to_string(GetLastError()) + "): " + Utf8(path.wstring()));
    return result;
}
std::filesystem::path Destination(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    if (value.size() < 3 || !((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) ||
        value[1] != L':' || value[2] != L'\\' || value.find(L':', 2) != std::wstring::npos)
        throw std::runtime_error("Staging requires an absolute local drive destination");
    std::filesystem::path path(value);
    for (const auto& part : path.relative_path()) {
        auto name = part.wstring();
        if (name == L"." || name == L".." || name.empty() || name.back() == L'.' || name.back() == L' ')
            throw std::runtime_error("Destination path aliases are not accepted");
    }
    const auto drive = path.root_path().wstring(); const auto kind = GetDriveTypeW(drive.c_str());
    if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE && kind != DRIVE_RAMDISK) throw std::runtime_error("Destination must be a local volume");
    return std::filesystem::path(L"\\\\?\\" + path.wstring());
}
void Inspect(const std::filesystem::path& root, std::vector<std::wstring>* sources) {
    size_t count = 0, bytes = 0, top = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (++top > 256) throw std::runtime_error("Too many top-level staging items");
        if (sources) sources->push_back(entry.path().wstring());
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto path = entry.path(); const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Staging tree contains reparse or inaccessible entries");
        bytes += Utf8(path.wstring()).size();
        if (++count > 8192 || bytes > 4 * 1024 * 1024) throw std::runtime_error("Staging tree budget exceeded");
    }
    if (sources && sources->empty()) throw std::runtime_error("Cannot commit an empty stage");
}
void DeleteChildren(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        DirectoryLock file;
        file.handle = CreateFileW(entry.path().c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (file.handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.handle, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Cleanup entry changed or is in use");
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteChildren(entry.path());
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file.handle, FileDispositionInfo, &disposition, sizeof(disposition)))
            throw std::runtime_error("Cannot remove staging entry");
    }
}

}
struct StagingAdapter::State : std::enable_shared_from_this<State> {
    struct Stage {
        uint64_t id = 0, task = 0;
        HANDLE cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Stage() { if (cancel_event) CloseHandle(cancel_event); }
        std::filesystem::path destination, path;
        std::string path_utf8;
        StageOperation operation{PULSE_STAGE_PREPARING};
        std::vector<DirectoryLock> ancestors;
        std::unique_ptr<DirectoryLock> directory;
        std::vector<std::wstring> sources;
        bool busy = true, cancel = false, prepared = false, submitted = false, cleaning = false, released = false, cancel_sent = false, cleanup_attempted = false;
    };
    HWND owner;
    DWORD thread = GetCurrentThreadId();
    std::function<bool()> live;
    StagingBackend backend;
    mutable std::mutex mutex;
    bool closed = false, cleanup = false, polling = false;
    std::atomic<unsigned> workers{0}, callbacks{0};
    std::map<uint64_t, std::shared_ptr<Stage>> stages;
    State(HWND window, std::function<bool()> check, StagingBackend functions) : owner(window), live(std::move(check)), backend(std::move(functions)) {}
    template<class Function> bool Work(Function function) {
        auto self = shared_from_this(); ++workers;
        try { std::thread([self, function = std::move(function)]() mutable { try { function(); } catch (...) {} --self->workers; }).detach(); return true; }
        catch (...) { --workers; return false; }
    }
    static PulseStatus PULSE_CALL Choose(void* context, PulseStringView initial, PulseStageChooseFn callback, void* user) {
        try {
            auto* raw = static_cast<State*>(context); if (!raw || !callback) return PULSE_ERROR_ARGUMENT;
            auto self = raw->shared_from_this(); if (GetCurrentThreadId() != self->thread) return PULSE_ERROR_STATE;
            { std::lock_guard lock(self->mutex); if (self->closed) return PULSE_ERROR_STATE; }
            const auto text = initial.length ? Wide(initial) : std::wstring{};
            ++self->callbacks;
            try {
                auto result = self->backend.choose_destination ? self->backend.choose_destination(text) : std::pair<PulseStatus, std::wstring>{PULSE_ERROR_UNAVAILABLE, {}};
                auto path = result.first == PULSE_OK ? Utf8(result.second) : std::string{};
                callback(user, result.first, {path.data(), static_cast<uint32_t>(path.size())});
            } catch (...) { --self->callbacks; return PULSE_ERROR_STATE; }
            --self->callbacks; return PULSE_OK;
        } catch (...) { return PULSE_ERROR_ARGUMENT; }
    }
    static PulseStatus PULSE_CALL Create(void* context, PulseStringView destination, PulseStageCreateFn callback, void* user) {
        try {
            auto* raw = static_cast<State*>(context); if (!raw || !callback) return PULSE_ERROR_ARGUMENT;
            auto self = raw->shared_from_this(); auto text = Wide(destination); auto stage = std::make_shared<Stage>(); if (!stage->cancel_event) return PULSE_ERROR_INITIALIZATION;
            { std::lock_guard lock(self->mutex); if (self->closed) return PULSE_ERROR_STATE; if (self->stages.size() >= 16) return PULSE_ERROR_UNAVAILABLE;
              auto id = next_stage.load(); do { if (id == UINT64_MAX) return PULSE_ERROR_UNAVAILABLE; } while (!next_stage.compare_exchange_weak(id, id + 1));
              stage->id = id + 1; self->stages.emplace(stage->id, stage); }
            if (!self->Work([self, stage, text = std::move(text), callback, user] {
                PulseStatus status = PULSE_OK;
                try {
                    stage->destination = Destination(text);
                    for (auto parent = stage->destination; !parent.empty(); parent = parent.parent_path()) {
                        // MSVC parent_path reaches \\?\C: without the root separator.
                        auto pinned = parent;
                        if (pinned.native().size() == 6 && pinned.native().back() == L':') pinned += L"\\";
                        stage->ancestors.push_back(LockDirectory(pinned));
                        if (pinned.native().size() <= 7) break;
                    }
                    for (unsigned attempt = 0; attempt < 32; ++attempt) {
                        auto candidate = stage->destination / (L".pulse-stage-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(stage->id) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
                        if (CreateDirectoryW(candidate.c_str(), nullptr)) { stage->path = candidate; break; }
                        if (GetLastError() != ERROR_ALREADY_EXISTS) throw std::runtime_error("Cannot create staging directory");
                    }
                    if (stage->path.empty()) throw std::runtime_error("Cannot allocate unique staging directory");
                    if (!SetFileAttributesW(stage->path.c_str(), FILE_ATTRIBUTE_HIDDEN)) throw std::runtime_error("Cannot hide staging directory");
                    stage->directory = std::make_unique<DirectoryLock>(LockDirectory(stage->path, true));
                    stage->path_utf8 = Utf8(stage->path.wstring());
                } catch (const std::exception& error) { status = PULSE_ERROR_STATE; std::lock_guard lock(self->mutex); stage->operation.error = error.what(); }
                { std::lock_guard lock(self->mutex); stage->operation.state = status == PULSE_OK ? (stage->cancel || self->closed ? PULSE_STAGE_CANCELLED : PULSE_STAGE_READY) : PULSE_STAGE_FAILED; stage->busy = false;
                  if (self->closed) status = PULSE_ERROR_STATE; }
                const auto callback_path = stage->path_utf8;
                ++self->callbacks;
                try { callback(user, status, stage->id, status == PULSE_OK ? PulseStringView{callback_path.data(), static_cast<uint32_t>(callback_path.size())} : PulseStringView{}); } catch (...) {}
                --self->callbacks;
            })) { std::lock_guard lock(self->mutex); self->stages.erase(stage->id); return PULSE_ERROR_INITIALIZATION; }
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_ARGUMENT; }
    }
    static PulseStatus PULSE_CALL Commit(void* context, uint64_t id) {
        try {
            auto* raw = static_cast<State*>(context); if (!raw || !id) return PULSE_ERROR_ARGUMENT;
            auto self = raw->shared_from_this(); std::shared_ptr<Stage> stage;
            { std::lock_guard lock(self->mutex); auto found = self->stages.find(id);
              if (self->closed || found == self->stages.end()) return PULSE_ERROR_STATE; stage = found->second;
              if (stage->busy || stage->cancel || stage->operation.state != PULSE_STAGE_READY) return PULSE_ERROR_STATE;
              stage->busy = true; stage->operation.state = PULSE_STAGE_COMMITTING; }
            if (!self->Work([self, stage] {
                std::vector<std::wstring> sources; std::string error;
                try { Inspect(stage->path, &sources); }
                catch (const std::exception& exception) { error = exception.what(); }
                std::lock_guard lock(self->mutex); stage->busy = false;
                if (stage->cancel || self->closed) stage->operation.state = PULSE_STAGE_CANCELLED;
                else if (!error.empty()) { stage->operation.state = PULSE_STAGE_FAILED; stage->operation.error = std::move(error); }
                else { stage->sources = std::move(sources); stage->prepared = true; }
            })) { std::lock_guard lock(self->mutex); stage->busy = false; stage->operation.state = PULSE_STAGE_FAILED; return PULSE_ERROR_INITIALIZATION; }
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL Get(void* context, uint64_t id, PulseStageSnapshotFn callback, void* user) {
        try {
            auto* raw = static_cast<State*>(context); if (!raw || !id || !callback) return PULSE_ERROR_ARGUMENT;
            auto self = raw->shared_from_this(); StageOperation value; uint64_t task = 0;
            { std::lock_guard lock(self->mutex); auto found = self->stages.find(id); if (found == self->stages.end()) return PULSE_ERROR_STATE; value = found->second->operation; task = found->second->task; }
            PulseStageSnapshot snapshot{sizeof(snapshot), id, task, value.state, value.percent, value.completed_items, value.total_items,
                value.completed_bytes, value.total_bytes, {value.error.data(), static_cast<uint32_t>(value.error.size())}};
            ++self->callbacks; try { callback(user, &snapshot); } catch (...) { --self->callbacks; return PULSE_ERROR_STATE; } --self->callbacks; return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL Cancel(void* context, uint64_t id) {
        try { auto* self = static_cast<State*>(context); if (!self || !id) return PULSE_ERROR_ARGUMENT;
            std::lock_guard lock(self->mutex); auto found = self->stages.find(id); if (found == self->stages.end()) return PULSE_ERROR_STATE;
            auto& stage = *found->second; if (Terminal(stage.operation.state)) return PULSE_ERROR_STATE;
            stage.cancel = true; SetEvent(stage.cancel_event); if (!stage.busy && !stage.submitted) stage.operation.state = PULSE_STAGE_CANCELLED; return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    bool Clean(const std::shared_ptr<Stage>& stage) {
        { std::lock_guard lock(mutex); if (stage->busy || stage->cleaning || stage->released || (stage->submitted && !Terminal(stage->operation.state))) return false;
          stage->cleaning = stage->busy = stage->cleanup_attempted = true; }
        auto self = shared_from_this();
        if (Work([self, stage] {
            std::string error;
            try {
                if (!stage->path.empty()) { Inspect(stage->path, nullptr);
                    if (!stage->directory) stage->directory = std::make_unique<DirectoryLock>(LockDirectory(stage->path, true));
                    DeleteChildren(stage->path);
                    FILE_DISPOSITION_INFO disposition{TRUE};
                    if (!SetFileInformationByHandle(stage->directory->handle, FileDispositionInfo, &disposition, sizeof(disposition))) throw std::runtime_error("Cannot remove staging directory");
                    stage->directory.reset(); }
                stage->ancestors.clear();
            } catch (const std::exception& exception) { error = exception.what(); }
            std::lock_guard lock(self->mutex); stage->busy = stage->cleaning = false;
            if (error.empty()) { stage->released = true; self->stages.erase(stage->id); }
            else { stage->operation.state = PULSE_STAGE_FAILED; stage->operation.error = std::move(error); }
        })) return true;
        std::lock_guard lock(mutex); stage->busy = stage->cleaning = false; return false;
    }
    static PulseStatus PULSE_CALL Release(void* context, uint64_t id) {
        try { auto* raw = static_cast<State*>(context); if (!raw || !id) return PULSE_ERROR_ARGUMENT; auto self = raw->shared_from_this(); std::shared_ptr<Stage> stage;
            { std::lock_guard lock(self->mutex); auto found = self->stages.find(id); if (found == self->stages.end()) return PULSE_ERROR_STATE;
              stage = found->second; if (stage->operation.state != PULSE_STAGE_READY && !Terminal(stage->operation.state)) return PULSE_ERROR_STATE; }
            return self->Clean(stage) ? PULSE_OK : PULSE_ERROR_STATE;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    void Poll() {
        if (GetCurrentThreadId() != thread || polling) return;
        polling = true;
        bool is_closed = false; { std::lock_guard lock(mutex); is_closed = closed; }
        if (!is_closed && (!owner || !live || !live())) Close();
        std::vector<std::shared_ptr<Stage>> copy;
        { std::lock_guard lock(mutex); for (const auto& [id, stage] : stages) { (void)id; copy.push_back(stage); } }
        for (const auto& stage : copy) {
            bool submit = false, cancel = false; uint64_t task = 0; StageSubmission submission;
            { std::lock_guard lock(mutex);
              if (stage->prepared && !stage->submitted && !stage->cancel && !closed) { stage->submitted = true; stage->prepared = false; submit = true; submission = {stage->id, stage->destination.wstring(), stage->sources, stage->cancel_event}; }
              task = Terminal(stage->operation.state) ? 0 : stage->task;
              if (task && (stage->cancel || closed) && !stage->cancel_sent && !Terminal(stage->operation.state)) { stage->cancel_sent = true; cancel = true; } }
            if (submit) {
                StageSubmitResult result{PULSE_ERROR_INITIALIZATION, 0};
                try { if (backend.submit) result = backend.submit(submission); } catch (...) {}
                std::lock_guard lock(mutex); stage->task = result.task;
                if (result.status == PULSE_OK && result.task) stage->operation.state = PULSE_STAGE_RUNNING;
                else { stage->submitted = false; stage->operation.state = result.status == PULSE_ERROR_STATE ? PULSE_STAGE_CANCELLED : PULSE_STAGE_FAILED; }
            }
            if (cancel && backend.cancel) { try { backend.cancel(task); } catch (...) {} }
            if (task && backend.query) {
                StageOperation value{PULSE_STAGE_FAILED};
                try { value = backend.query(task); } catch (...) { value.error = "Operation query failed"; } if (value.state < PULSE_STAGE_RUNNING || value.state > PULSE_STAGE_CANCELLED) value.state = PULSE_STAGE_FAILED;
                std::lock_guard lock(mutex); if (!Terminal(stage->operation.state)) stage->operation = std::move(value);
            }
            bool clean = false; { std::lock_guard lock(mutex); clean = cleanup && !stage->cleanup_attempted && !stage->busy && (!stage->submitted || Terminal(stage->operation.state)); }
            if (clean) Clean(stage);
        }
        polling = false;
    }
    void Close() { std::lock_guard lock(mutex); closed = true; for (auto& [id, stage] : stages) { (void)id; stage->cancel = true; SetEvent(stage->cancel_event);
        if (!stage->busy && !stage->submitted && !Terminal(stage->operation.state)) stage->operation.state = PULSE_STAGE_CANCELLED; } }
};
StagingAdapter::StagingAdapter(HWND owner, std::function<bool()> live, StagingBackend backend) : state_(std::make_shared<State>(owner, std::move(live), std::move(backend))) {}
StagingAdapter::~StagingAdapter() { Close(); }
PulseStatus StagingAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    if (version != PULSE_STAGING_VERSION_1) return PULSE_ERROR_ABI;
    if (!out || size < sizeof(PulseStagingV1)) return PULSE_ERROR_ARGUMENT;
    const PulseStagingV1 table{sizeof(table), PULSE_STAGING_VERSION_1, state_.get(), State::Create, State::Commit, State::Get, State::Cancel, State::Release, State::Choose};
    std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
}
void StagingAdapter::Poll() noexcept { auto self = state_; try { self->Poll(); } catch (...) { self->polling = false; try { self->Close(); } catch (...) {} } }
void StagingAdapter::Close() noexcept { try { state_->Close(); } catch (...) {} }
void StagingAdapter::CleanupRemaining() noexcept { auto self = state_; try { self->Close(); { std::lock_guard lock(self->mutex); self->cleanup = true; } self->Poll(); } catch (...) {} }
bool StagingAdapter::CallbackActive() const noexcept {
    if (state_->workers || state_->callbacks) return true;
    std::lock_guard lock(state_->mutex);
    for (const auto& [id, stage] : state_->stages) { (void)id; if (stage->submitted && !Terminal(stage->operation.state)) return true; }
    return false;
}
}
