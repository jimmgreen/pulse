#include "content_search_client.h"
#include "content_search_protocol.h"
#include "content_config_storage.h"

#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace pulse::index {
namespace {

std::atomic<ContentAgentLocator> g_content_agent_locator{nullptr};

std::wstring NewToken() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64());
    }
    wchar_t text[40]{};
    StringFromGUID2(guid, text, ARRAYSIZE(text));
    std::wstring token(text);
    token.erase(std::remove_if(token.begin(), token.end(), [](wchar_t value) {
        return value == L'{' || value == L'}';
    }), token.end());
    return token;
}

bool Transfer(HANDLE pipe, void* buffer, DWORD size, bool writing, HANDLE cancel) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    bool ok = true;
    while (size && ok) {
        OVERLAPPED ov{}; ov.hEvent = event; ResetEvent(event);
        DWORD done = 0;
        const BOOL immediate = writing ? WriteFile(pipe, bytes, size, &done, &ov) : ReadFile(pipe, bytes, size, &done, &ov);
        if (!immediate) {
            if (GetLastError() != ERROR_IO_PENDING) { ok = false; break; }
            HANDLE events[]{event, cancel};
            const DWORD wait = WaitForMultipleObjects(cancel ? 2 : 1, events, FALSE, cancel ? INFINITE : 5000);
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &done, TRUE);
                ok = false; break;
            }
            if (!GetOverlappedResult(pipe, &ov, &done, FALSE)) { ok = false; break; }
        }
        if (!done) { ok = false; break; }
        bytes += done; size -= done;
    }
    CloseHandle(event); return ok;
}
bool ReadAll(HANDLE pipe, void* data, DWORD size, HANDLE cancel = nullptr) { return Transfer(pipe, data, size, false, cancel); }
bool WriteAll(HANDLE pipe, const void* data, DWORD size, HANDLE cancel = nullptr) { return Transfer(pipe, const_cast<void*>(data), size, true, cancel); }
HANDLE Connect(const std::wstring& token, HANDLE process, HANDLE cancel) {
    const auto name = content::PipeName(token);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if ((cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) || (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0)) break;
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        WaitNamedPipeW(name.c_str(), 25);
        if (cancel) WaitForSingleObject(cancel, 10); else Sleep(10);
    }
    return INVALID_HANDLE_VALUE;
}
std::vector<uint8_t> RequestPayload(const ContentSearchRequest& request) {
    ipc::PayloadWriter writer;
    writer.PutU64(request.generation);
    writer.PutU32(static_cast<uint32_t>(request.mode));
    uint32_t flags = request.recursive ? 1u : 0u;
    if (request.case_sensitive) flags |= 2u;
    if (request.skip_system_locations) flags |= 4u;
    if (request.whole_word) flags |= 8u;
    // 16: a multi-folder content search with no single scope. The wire carries
    // roots.front() in the root field; without this bit the agent would read
    // it back as a scope and drop the other folders.
    if (request.mode == ContentSearchMode::Content && request.root.empty() && !request.roots.empty()) flags |= 16u;
    if (request.allow_uncovered_root) flags |= 32u;
    writer.PutU32(flags);
    writer.PutU64(request.maximum_file_bytes);
    writer.PutU32(static_cast<uint32_t>(request.maximum_hits));
    std::vector<std::wstring> roots = request.roots;
    if (roots.empty() && !request.root.empty()) roots.push_back(request.root);
    writer.PutString(roots.empty() ? std::wstring{} : roots.front());
    writer.PutString(request.needle);
    writer.PutU64(request.minimum_file_bytes);
    const uint32_t extra = roots.size() > 1 ? static_cast<uint32_t>(roots.size() - 1) : 0;
    writer.PutU32(extra);
    for (uint32_t i = 0; i < extra; ++i) writer.PutString(roots[i + 1]);
    writer.PutU32(static_cast<uint32_t>(request.match_mode));
    writer.PutU32(static_cast<uint32_t>(request.needles.size()));
    for (const auto& needle : request.needles) writer.PutString(needle);
    writer.PutU32(static_cast<uint32_t>(request.excluded_needles.size()));
    for (const auto& needle : request.excluded_needles) writer.PutString(needle);
    writer.PutU32(static_cast<uint32_t>(request.candidate_paths.size()));
    for (const auto& path : request.candidate_paths) writer.PutString(path);
    if (request.mode == ContentSearchMode::Content) {
        writer.PutString(request.filename_query);
        writer.PutU32(static_cast<uint32_t>(request.sort)); writer.PutU32(request.sort_desc ? 1u : 0u);
        writer.PutU32(1); writer.PutU64(request.session_id);
        writer.PutU64(request.maximum_document_bytes);
        writer.PutU32(request.task_scan ? 1u : 0u);
    }
    return writer.data();
}

bool ParseUpdate(const std::vector<uint8_t>& payload, ContentSearchUpdate& update) {
    ipc::PayloadReader reader(payload.data(), payload.size());
    uint32_t flags = 0;
    uint32_t error = 0;
    uint32_t phase = 0;
    uint32_t count = 0;
    if (!reader.GetU64(update.progress.generation) ||
        !reader.GetU64(update.progress.scanned_files) ||
        !reader.GetU64(update.progress.scanned_bytes) ||
        !reader.GetU32(flags) || !reader.GetU32(error) ||
        !reader.GetU32(phase) || !reader.GetU64(update.progress.total_files) ||
        !reader.GetString(update.progress.current_root) || !reader.GetU32(count) ||
        count > 10000) return false;
    update.progress.done = (flags & 1u) != 0;
    update.progress.truncated = (flags & 2u) != 0;
    update.progress.live = (flags & 4u) != 0;
    update.progress.delta = (flags & 8u) != 0;
    update.progress.error = error;
    update.progress.phase = phase <= static_cast<uint32_t>(ContentSearchPhase::Hashing)
        ? static_cast<ContentSearchPhase>(phase) : ContentSearchPhase::Enumerating;
    update.hits.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ContentHit hit;
        if (!reader.GetString(hit.path) || !reader.GetString(hit.name) ||
            !reader.GetString(hit.snippet) || !reader.GetU64(hit.size) ||
            !reader.GetU64(hit.modified) || !reader.GetU32(hit.line) ||
            !reader.GetU32(hit.group)) return false;
        hit.removed = (hit.group & 0x80000000u) != 0; hit.group &= 0x7fffffffu;
        update.hits.push_back(std::move(hit));
    }
    if (reader.remaining() && !reader.GetU64(update.progress.index_revision)) return false;
    if(reader.remaining()) {
        uint32_t identities=0;if(!reader.GetU32(identities)||identities!=update.hits.size()) return false;
        for(auto& hit:update.hits) if(!reader.GetU64(hit.file_id)) return false;
    }
    return content::GetSubscriptionStatus(reader, update.progress);
}

} // namespace

void SetContentAgentLocator(ContentAgentLocator locator) noexcept {
    g_content_agent_locator.store(locator);
}

// Pulse.Content.exe beside the running exe (development builds, tests), else
// wherever the registered locator finds a verified pack; empty when neither.
std::wstring ContentSearchClient::ExePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length && length < path.size()) {
        path.resize(length);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring sibling = path.substr(0, slash + 1) + L"Pulse.Content.exe";
            const DWORD attributes = GetFileAttributesW(sibling.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return sibling;
        }
    }
    const auto locate = g_content_agent_locator.load();
    return locate ? locate() : std::wstring{};
}

void ContentSearchClient::Start(HWND notify, UINT update_message, bool persistent, bool read_only) {
    Stop(); persistent_enabled_ = persistent; notify_ = notify; update_message_ = update_message;
    read_only_ = read_only;
    mode_ = read_only ? ContentAgentMode::Observer : ContentAgentMode::LegacyWriter;
    suspended_ = false;
    config_ready_ = false;
    cancel_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { CloseHandle(job_); job_ = nullptr; } }
    running_ = true;
    // A legacy persistent client polls its agent, so it needs the worker now.
    if (persistent) EnsureWorker();
}
// Started by the first command or search, so a client that is never used
// (the duplicate scan until it is opened) keeps no idle thread.
void ContentSearchClient::EnsureWorker() {
    std::lock_guard lock(worker_mu_);
    if (running_ && !worker_.joinable()) worker_ = std::thread([this] { Worker(); });
}
void ContentSearchClient::Start(HWND notify, UINT update_message, ContentAgentMode mode) {
    // The non-persistent worker waits for a command before the mode is published.
    Start(notify, update_message, false, mode == ContentAgentMode::Observer);
    { std::lock_guard lock(state_mu_); mode_ = mode; persistent_enabled_ = true; }
    Enqueue({content::REQ_STATUS, {}});
}
void ContentSearchClient::Suspend() {
    suspended_ = true;
    Cancel();
    if (cancel_event_) SetEvent(cancel_event_);
    wake_.notify_all();
}
void ContentSearchClient::Resume() {
    if (cancel_event_) ResetEvent(cancel_event_);
    suspended_ = false;
}
void ContentSearchClient::Stop() {
    running_ = false;
    if (cancel_event_) SetEvent(cancel_event_);
    Cancel(); wake_.notify_all();
    {
        std::lock_guard lock(worker_mu_);
        if (worker_.joinable()) worker_.join();
    }
    if (cancel_event_) { CloseHandle(cancel_event_); cancel_event_ = nullptr; }
    if (job_) { CloseHandle(job_); job_ = nullptr; }
    std::lock_guard lock(updates_mu_); updates_.clear();
}
void ContentSearchClient::Cancel(uint64_t session_id) {
    {
        std::lock_guard lock(state_mu_);
        if (!session_id) { generation_ = 0; pending_.reset(); }
        if (!session_id) current_sessions_.clear(); else current_sessions_.erase(session_id);
        std::erase_if(pending_requests_, [&](const auto& r) { return !session_id || r.session_id == session_id; });
        for (auto& [id, active] : active_queries_) {
            if (session_id && active->session_id != session_id) continue;
            SetEvent(active->cancel);
            ipc::PayloadWriter writer; writer.PutU64(id);
            commands_.push_back({content::REQ_CANCEL, writer.data()});
        }
    }
    { std::lock_guard lock(process_mu_); if (process_) TerminateProcess(process_, ERROR_CANCELLED); }
    wake_.notify_all();
}
void ContentSearchClient::SearchAsync(ContentSearchRequest request) {
    if (!running_) return;
    Resume();
    Cancel(request.session_id);
    {
        std::lock_guard lock(state_mu_);
        // Instant agents always produce an unordered task stream, including
        // saved searches whose caller did not explicitly request task_scan.
        if (persistent_enabled_ && mode_ == ContentAgentMode::Instant &&
            request.indexed && request.mode == ContentSearchMode::Content) {
            request.task_scan = true;
            request.after_revision = 0;
            request.incremental = false;
        }
        generation_ = request.generation;
        current_sessions_[request.session_id] = request.generation;
        pending_requests_.push_back(std::move(request));
    }
    wake_.notify_all();
    EnsureWorker();
}
void ContentSearchClient::Enqueue(Command command) {
    { std::lock_guard lock(state_mu_); commands_.push_back(std::move(command)); }
    wake_.notify_all();
    EnsureWorker();
}
void ContentSearchClient::Configure(const ContentIndexConfig& config) {
    // Instant configuration needs no agent. A tray suspension must not leave
    // scope updates queued until a search that the stale scope may reject.
    if (mode_ == ContentAgentMode::Instant) Resume();
    ipc::PayloadWriter writer; content::PutConfig(writer, config);
    Enqueue({content::REQ_CONFIG, writer.data()});
}
ContentIndexConfig ContentSearchClient::GetConfig() const { std::lock_guard lock(state_mu_); return config_; }
ContentIndexStatus ContentSearchClient::GetStatus() const { std::lock_guard lock(state_mu_); return status_; }
void ContentSearchClient::Pause(bool paused) { Enqueue({paused ? content::REQ_PAUSE : content::REQ_RESUME, {}}); }
void ContentSearchClient::Rebuild() { Enqueue({content::REQ_REBUILD, {}}); }
void ContentSearchClient::Publish(ContentSearchUpdate update) {
    if (!running_ || !IsCurrent(update.progress.generation)) return;
    bool notify=true;
    {
        std::lock_guard lock(updates_mu_);
        if(update.results) notify=updates_.empty();
        // Disk-backed updates carry counts/state, never an unbounded hit queue.
        if(update.results) std::erase_if(updates_, [&](const auto& old) {
            return old.progress.generation == update.progress.generation;
        });
        updates_.push_back(std::move(update));
    }
    if (notify && notify_ && update_message_) PostMessageW(notify_, update_message_, 0, 0);
}
bool ContentSearchClient::TakeUpdate(ContentSearchUpdate& update) {
    std::lock_guard lock(updates_mu_);
    while (!updates_.empty()) {
        update = std::move(updates_.front()); updates_.pop_front();
        if (IsCurrent(update.progress.generation)) return true;
    }
    return false;
}
bool ContentSearchClient::IsCurrent(uint64_t generation) {
    std::lock_guard lock(state_mu_);
    const auto found = active_queries_.find(generation);
    return found != active_queries_.end() ? WaitForSingleObject(found->second->cancel, 0) != WAIT_OBJECT_0
        : generation != 0 && generation == generation_.load();
}
bool ContentSearchClient::EnsurePersistent() {
    if (suspended_ || !running_) return false;
    if (persistent_process_ && WaitForSingleObject(persistent_process_, 0) == WAIT_TIMEOUT) return true;
    if (persistent_process_) { CloseHandle(persistent_process_); persistent_process_ = nullptr; }
    persistent_token_ = NewToken();
    const auto exe = ExePath();
    if (exe.empty()) {
        std::lock_guard lock(state_mu_); status_.error = ERROR_FILE_NOT_FOUND;
        SetLastError(ERROR_FILE_NOT_FOUND); return false;
    }
    std::wstring command = L"\"" + exe + (mode_ == ContentAgentMode::Instant ? L"\" --content-instant-agent " : read_only_ ? L"\" --content-index-observer " : L"\" --content-index-agent ") + persistent_token_ + L" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_FORCEOFFFEEDBACK; PROCESS_INFORMATION created{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &created)) {
        std::lock_guard lock(state_mu_); status_.error = GetLastError(); return false;
    }
    if (!job_ || !AssignProcessToJobObject(job_, created.hProcess)) {
        const auto error = job_ ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        TerminateProcess(created.hProcess, error); CloseHandle(created.hThread); CloseHandle(created.hProcess);
        SetLastError(error); return false;
    }
    ResumeThread(created.hThread); CloseHandle(created.hThread); persistent_process_ = created.hProcess;
    return true;
}
void ContentSearchClient::Control(const Command& command) {
    if (!persistent_enabled_) return;
    if (mode_ == ContentAgentMode::Instant && (command.type == content::REQ_STATUS || command.type == content::REQ_CONFIG)) {
        // Configuration is independent of the optional body cache and never starts an agent.
        auto config = LoadContentIndexConfig();
        if (command.type == content::REQ_CONFIG) {
            ipc::PayloadReader reader(command.payload.data(), command.payload.size());
            // Acquire the shared configuration lock with cancellation before entering
            // the reentrant storage helper. Closing never waits out another writer.
            HANDLE mutex = config_storage::NamedMutex(config_storage::DatabasePath(), L"config");
            if (!mutex) { std::lock_guard lock(state_mu_); status_.error = GetLastError(); return; }
            HANDLE waits[]{cancel_event_, mutex};
            const auto wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
            if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_ABANDONED_0 + 1) { CloseHandle(mutex); return; }
            struct Release { HANDLE mutex; ~Release() { ReleaseMutex(mutex); CloseHandle(mutex); } } release{mutex};
            if (!content::GetConfig(reader, config) || !SaveInstantContentConfig(config)) {
                std::lock_guard lock(state_mu_); status_.error = ERROR_WRITE_FAULT; return;
            }
        }
        { std::lock_guard lock(state_mu_); config_ = std::move(config); status_ = {}; config_ready_ = true; }
        if (notify_ && update_message_) PostMessageW(notify_, update_message_, 0, 0);
        return;
    }
    std::lock_guard persistent_lock(persistent_mu_);
    if (command.type == content::REQ_CANCEL && !persistent_process_) return;
    if (!EnsurePersistent()) return;
    HANDLE pipe = Connect(persistent_token_, persistent_process_, cancel_event_);
    if (pipe == INVALID_HANDLE_VALUE) { std::lock_guard lock(state_mu_); status_.error = ERROR_TIMEOUT; return; }
    const auto header = content::Header(command.type, static_cast<uint32_t>(command.payload.size()));
    bool ok = WriteAll(pipe, &header, sizeof(header), cancel_event_) && WriteAll(pipe, command.payload.data(), static_cast<DWORD>(command.payload.size()), cancel_event_);
    ipc::MsgHeader response{};
    ok = ok && ReadAll(pipe, &response, sizeof(response), cancel_event_) && response.magic == content::kMagic && response.type == content::RSP_STATUS && response.payload_size <= content::kMaximumPayload;
    std::vector<uint8_t> bytes(ok ? response.payload_size : 0);
    ok = ok && ReadAll(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), cancel_event_);
    if (ok) {
        ipc::PayloadReader reader(bytes.data(), bytes.size()); ContentIndexStatus status; ContentIndexConfig config;
        if (content::GetStatus(reader, status) && content::GetConfig(reader, config)) {
            { std::lock_guard lock(state_mu_); status_ = std::move(status); config_ = std::move(config); }
            config_ready_ = true;
            if (notify_ && update_message_) PostMessageW(notify_, update_message_, 0, 0);
        }
    }
    CloseHandle(pipe);
}
void ContentSearchClient::Worker() {
    while (running_) {
        std::deque<ContentSearchRequest> requests;
        std::deque<Command> commands;
        bool stop_idle_agent = false;
        {
            std::unique_lock lock(state_mu_);
            const auto ready = [&] { return !running_ || suspended_ || !pending_requests_.empty() || !commands_.empty(); };
            if (persistent_enabled_ && mode_ != ContentAgentMode::Instant) wake_.wait_for(lock, std::chrono::milliseconds(750), ready);
            else wake_.wait(lock, ready);
            if (!running_) break;
            if (suspended_) {
                lock.unlock(); ShutdownAgent(); lock.lock();
                wake_.wait(lock, [&] { return !running_ || !suspended_; });
                continue;
            }
            commands.swap(commands_);
            requests.swap(pending_requests_);
            stop_idle_agent = mode_ == ContentAgentMode::Instant && current_sessions_.empty();
        }
        if (stop_idle_agent) ShutdownAgent();
        for (const auto& command : commands) { if (!running_) break; Control(command); }
        std::vector<std::shared_ptr<ActiveQuery>> replaced;
        for (auto& request : requests) {
            if (!running_) break;
            auto active = std::make_shared<ActiveQuery>();
            active->session_id = request.session_id; active->generation = request.generation;
            active->cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            {
                std::lock_guard lock(state_mu_);
                const auto current = current_sessions_.find(request.session_id);
                if (current == current_sessions_.end() || current->second != request.generation) continue;
                if(auto old=active_queries_.find(request.generation);old!=active_queries_.end()) replaced.push_back(old->second);
                active_queries_[request.generation] = active;
                active->thread = std::thread([this, active, request = std::move(request)]() mutable {
                    Run(std::move(request), active); active->done = true;
                });
            }
        }
        if (running_ && mode_ != ContentAgentMode::Instant) Control({content::REQ_STATUS, {}});
        std::vector<std::shared_ptr<ActiveQuery>> retired;
        {
            std::lock_guard lock(state_mu_);
            for (auto it = active_queries_.begin(); it != active_queries_.end();) {
                if (it->second->done && WaitForSingleObject(it->second->cancel, 0) == WAIT_OBJECT_0) {
                    retired.push_back(std::move(it->second)); it = active_queries_.erase(it);
                } else ++it;
            }
        }
    }
    std::map<uint64_t, std::shared_ptr<ActiveQuery>> active;
    { std::lock_guard lock(state_mu_); active.swap(active_queries_); }
    for (auto& [id, query] : active) { SetEvent(query->cancel); if (query->thread.joinable()) query->thread.join(); }
    ShutdownAgent();
    { std::lock_guard lock(state_mu_); commands_.clear(); pending_.reset(); }
}
void ContentSearchClient::ShutdownAgent() {
    std::lock_guard lock(persistent_mu_);
    if (!persistent_process_) return;
    const auto until = GetTickCount64() + 1950;
    // Nested parser job teardown can itself take over a second under load.
    // Give cooperative cancellation a short head start, then reserve the rest
    // of the two-second budget for observing actual descendant process exit.
    // The watchdog also kills the job directly if interrupted pipe I/O cannot unwind promptly.
    constexpr DWORD graceful_ms = 250;
    HANDLE deadline = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE owned_job = job_;
    std::thread watchdog([=] {
        if (WaitForSingleObject(finished, graceful_ms) == WAIT_TIMEOUT) {
            SetEvent(deadline);
            if (owned_job) TerminateJobObject(owned_job, ERROR_CANCELLED);
        }
    });
    HANDLE pipe = Connect(persistent_token_, persistent_process_, deadline);
    if (pipe != INVALID_HANDLE_VALUE) {
        const auto header = content::Header(content::REQ_SHUTDOWN, 0);
        WriteAll(pipe, &header, sizeof(header), deadline); CloseHandle(pipe);
    }
    HANDLE waits[]{persistent_process_, deadline};
    if (WaitForMultipleObjects(2, waits, FALSE, graceful_ms) != WAIT_OBJECT_0 && job_) TerminateJobObject(job_, ERROR_CANCELLED);
    SetEvent(finished); watchdog.join(); CloseHandle(finished); CloseHandle(deadline);
    // TerminateJobObject is asynchronous. Observe process exit before reporting shutdown complete.
    while (job_ && GetTickCount64() < until) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) ||
            !accounting.ActiveProcesses) break;
        Sleep(5);
    }
    CloseHandle(persistent_process_); persistent_process_ = nullptr;
}
void ContentSearchClient::Run(ContentSearchRequest request, const std::shared_ptr<ActiveQuery>& active) {
    const HANDLE cancel_event = active->cancel;
    ContentSearchSession session(request, notify_, update_message_);
    const auto fail = [&](DWORD error) {
        if (auto update = session.Fail(error)) Publish(std::move(*update));
    };
    const bool persistent = persistent_enabled_ && request.indexed && request.mode == ContentSearchMode::Content;
    HANDLE process = nullptr;
    std::wstring token;
    if (persistent) {
        std::lock_guard lock(persistent_mu_);
        if (!EnsurePersistent()) { fail(GetLastError()); return; }
        token = persistent_token_;
        DuplicateHandle(GetCurrentProcess(), persistent_process_, GetCurrentProcess(), &process, 0, FALSE, DUPLICATE_SAME_ACCESS);
    } else {
        const auto exe = ExePath(); token = NewToken();
        if (exe.empty()) { fail(ERROR_FILE_NOT_FOUND); return; }
        std::wstring command = L"\"" + exe + L"\" --content-agent " + token;
        STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_FORCEOFFFEEDBACK; PROCESS_INFORMATION created{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &created)) {
            fail(GetLastError()); return;
        }
        if (!job_ || !AssignProcessToJobObject(job_, created.hProcess)) {
            const auto error = job_ ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
            TerminateProcess(created.hProcess, error); CloseHandle(created.hThread); CloseHandle(created.hProcess); fail(error); return;
        }
        ResumeThread(created.hThread);
        CloseHandle(created.hThread); process = created.hProcess;
    }
    HANDLE pipe = Connect(token, process, cancel_event);
    if (pipe != INVALID_HANDLE_VALUE) {
        const auto payload = RequestPayload(request);
        const auto header = content::Header(persistent && request.subscribe ? content::REQ_SUBSCRIBE : content::REQ_SEARCH, static_cast<uint32_t>(payload.size()));
        bool ok = WriteAll(pipe, &header, sizeof(header), cancel_event) && WriteAll(pipe, payload.data(), static_cast<DWORD>(payload.size()), cancel_event);
        while (ok && running_ && WaitForSingleObject(cancel_event, 0) != WAIT_OBJECT_0) {
            ipc::MsgHeader response{};
            ok = ReadAll(pipe, &response, sizeof(response), cancel_event) && response.magic == content::kMagic && response.type == content::RSP_BATCH && response.payload_size <= content::kMaximumPayload;
            std::vector<uint8_t> bytes(ok ? response.payload_size : 0);
            if (ok) ok = ReadAll(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), cancel_event);
            ContentSearchUpdate update;
            if (!ok || !ParseUpdate(bytes, update) || update.progress.generation != request.generation) break;
            if (update.progress.done && !update.progress.live) request.subscribe = false;
            if (auto accepted = session.Accept(std::move(update), cancel_event))
                Publish(std::move(*accepted));
            if (session.Done() && !request.subscribe) break;
        }
        CloseHandle(pipe);
    }
    if (!session.Done() && running_ && IsCurrent(request.generation)) {
        fail(pipe == INVALID_HANDLE_VALUE ? ERROR_TIMEOUT : ERROR_BROKEN_PIPE);
    }
    else if (mode_ == ContentAgentMode::Instant && request.subscribe && running_ && IsCurrent(request.generation)) {
        ContentSearchUpdate stopped;
        stopped.progress.generation = request.generation;
        stopped.progress.delta = stopped.progress.done = true;
        stopped.progress.subscription_error = ERROR_BROKEN_PIPE;
        stopped.progress.subscription_failure = ContentSubscriptionFailure::Transport;
        if (auto update = session.Accept(std::move(stopped), cancel_event)) Publish(std::move(*update));
    }
    if (!persistent) {
        if (WaitForSingleObject(process, 250) == WAIT_TIMEOUT) TerminateProcess(process, ERROR_CANCELLED);
    }
    if (process) CloseHandle(process);
    if(mode_ != ContentAgentMode::Instant && persistent && request.subscribe && running_ && IsCurrent(request.generation) &&
        WaitForSingleObject(cancel_event,0)!=WAIT_OBJECT_0) {
        request.previous_results=session.Results();
        // Keep the visible snapshot while reconnecting. Restarting the agent
        // obtains a new snapshot before resuming its durable change cursor.
        if(WaitForSingleObject(cancel_event,250)!=WAIT_OBJECT_0) {
            std::lock_guard lock(state_mu_);
            pending_requests_.push_back(std::move(request));wake_.notify_all();
        }
    }
}
} // namespace pulse::index
