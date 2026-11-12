#include "jobs.h"
#include "engine.h"
#include "text.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

using pulse::sdk::View;
using T = text::Id;
namespace fs = std::filesystem;

struct Jobs::Shared {
    PulseStagingV1 staging;
    pulse::sdk::DispatchQueue& dispatch;
    Dialogs& dialogs;
    // Passwords that opened an archive this session, newest first (memory only).
    std::mutex lock;
    std::vector<std::wstring> remembered;
    std::atomic<bool> remember{true};
    Shared(const PulseStagingV1& api, pulse::sdk::DispatchQueue& queue, Dialogs& dialog) : staging(api), dispatch(queue), dialogs(dialog) {}
    std::vector<std::wstring> Remembered() { std::lock_guard guard(lock); return remember ? remembered : std::vector<std::wstring>{}; }
    void Keep(const std::wstring& password) {
        if (!remember || password.empty()) return;
        std::lock_guard guard(lock);
        std::erase(remembered, password);
        remembered.insert(remembered.begin(), password);
        if (remembered.size() > 20) remembered.resize(20);
    }
};

namespace {
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}
// "a.tar.gz" -> "a", "a.7z.001" -> "a", "a.part1.rar" -> "a".
std::wstring ArchiveStem(const std::wstring& file_name) {
    std::wstring stem = file_name;
    for (int round = 0; round < 3; ++round) {
        const auto dot = stem.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0) break;
        std::wstring ext = stem.substr(dot + 1);
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        const bool numeric = !ext.empty() && std::all_of(ext.begin(), ext.end(), iswdigit);
        const bool part = ext.rfind(L"part", 0) == 0 && ext.size() > 4 && std::all_of(ext.begin() + 4, ext.end(), iswdigit);
        const bool known = ext == L"tar" || ext == L"7z" || ext == L"zip" || ext == L"rar" || ext == L"gz" || ext == L"xz" ||
                           ext == L"bz2" || ext == L"tgz" || ext == L"txz" || ext == L"tbz2" || ext == L"iso" || ext == L"cab" ||
                           ext == L"wim" || ext == L"lzma" || ext == L"z" || ext == L"zipx" || ext == L"arj" || ext == L"lzh";
        if (!numeric && !part && !known) break;
        stem.resize(dot);
        if (!numeric && !part && ext != L"tar" && ext != L"gz" && ext != L"xz" && ext != L"bz2") break;
    }
    return stem.empty() ? file_name : stem;
}
uint32_t Percent(uint64_t done, uint64_t total, uint32_t span) {
    return total ? static_cast<uint32_t>(std::min<uint64_t>(done, total) * span / total) : UINT32_MAX;
}
std::string LimitErrorText(const std::string& detail) {
    const LANGID id = GetUserDefaultUILanguage();
    const int lang = PRIMARYLANGID(id) != LANG_CHINESE ? 2
        : (SUBLANGID(id) == SUBLANG_CHINESE_TRADITIONAL || SUBLANGID(id) == SUBLANG_CHINESE_HONGKONG ||
           SUBLANGID(id) == SUBLANG_CHINESE_MACAU) ? 1 : 0;
    if (detail.find("disk space") != std::string::npos) {
        return lang == 0 ? "磁盘剩余空间不足，已中止操作。"
             : lang == 1 ? "磁碟剩餘空間不足，已中止作業。"
             : "Not enough free disk space to complete the operation.";
    }
    if (detail.find("timed out") != std::string::npos) {
        return lang == 0 ? "压缩包处理超时，已中止操作。"
             : lang == 1 ? "壓縮檔處理逾時，已中止作業。"
             : "The archive took too long to process and was stopped.";
    }
    return lang == 0 ? "压缩包体积或条目数超出安全上限，已中止操作。"
         : lang == 1 ? "壓縮檔大小或項目數超出安全上限，已中止作業。"
         : "The archive exceeds the safety limit for size or item count.";
}
std::string ErrorText(engine::Status status, const std::string& detail) {
    switch (status) {
    case engine::Status::WrongPassword: return text::Get(T::ErrWrongPassword);
    case engine::Status::NeedPassword: return text::Get(T::ErrNoPassword);
    case engine::Status::Unsupported: return text::Get(T::ErrUnsupported);
    case engine::Status::LimitReached: return LimitErrorText(detail);
    case engine::Status::WorkerCrashed: return text::Get(T::ErrWorkerCrashed);
    case engine::Status::WorkerTimedOut: return text::Get(T::ErrWorkerTimedOut);
    case engine::Status::WorkerMemoryLimit: return text::Get(T::ErrWorkerMemory);
    case engine::Status::IoError: return text::Get(T::ErrIo) + detail;
    default: return text::Get(T::ErrDamaged) + detail;
    }
}

bool IsWorkerFault(engine::Status status) noexcept {
    return status == engine::Status::WorkerCrashed ||
           status == engine::Status::WorkerTimedOut ||
           status == engine::Status::WorkerMemoryLimit;
}

// The failure's one-line headline: the engine's detail goes to the diagnostics block instead.
std::string Headline(engine::Status status, const std::string& detail) {
    if (status != engine::Status::InvalidArchive && status != engine::Status::IoError) return ErrorText(status, detail);
    std::string text = ErrorText(status, {});
    for (const char* tail : {"\xEF\xBC\x9A", ": ", ":"})  // full-width colon, then ASCII
        if (text.size() >= strlen(tail) && text.compare(text.size() - strlen(tail), strlen(tail), tail) == 0) {
            text.resize(text.size() - strlen(tail));
            break;
        }
    return text;
}
// "engine detail\n2026-11-11 10:42:07", or nothing without a detail.
std::string Diagnostics(const std::string& detail) {
    if (detail.empty()) return {};
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return detail.substr(0, 1900) + "\n" + stamp;
}

std::string SizeText(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes); int unit = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }
    char buffer[32];
    snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : value < 10 ? "%.2f %s" : value < 100 ? "%.1f %s" : "%.0f %s", value, units[unit]);
    return buffer;
}
// "智能解压 · 312 个项目 · 248 MB · 用时 9 秒" (the lead is optional).
std::string ResultLine(const std::string& lead, uint64_t items, uint64_t bytes, ULONGLONG started) {
    const char* dot = " \xC2\xB7 ";
    std::string line = lead;
    if (items) line += (line.empty() ? "" : dot) + text::Format(T::ResultItems, {}, std::to_string(items));
    if (bytes) line += (line.empty() ? "" : dot) + SizeText(bytes);
    const auto seconds = std::max<ULONGLONG>(1, (GetTickCount64() - started + 500) / 1000);
    return line + (line.empty() ? "" : dot) + text::Format(T::ResultTook, {}, std::to_string(seconds));
}

// Reports progress; keeps percent and counters monotonic as the host requires.
class Reporter {
    const PulseTaskContext& task_;
    PulseTaskProgress last_{};
    std::string item_, error_, note_, detail_, assurance_, log_, result_, result_path_;
    std::vector<std::pair<std::string, std::string>> action_text_;  // id, text
    std::vector<PulseTaskAction> actions_;
    void Send(PulseTaskProgress& next) {
        next.note = View(note_);
        next.detail = View(detail_);
        next.assurance = View(assurance_);
        next.log = View(log_);
        next.result = View(result_);
        next.result_path = View(result_path_);
        actions_.clear();
        for (size_t i = 0; i < action_text_.size(); ++i)
            actions_.push_back({sizeof(PulseTaskAction), i + 1 == action_text_.size() ? PULSE_TASK_ACTION_PRIMARY : 0u,
                                View(action_text_[i].first), View(action_text_[i].second)});
        next.actions = actions_.empty() ? nullptr : actions_.data();
        next.action_count = static_cast<uint32_t>(actions_.size());
        task_.report(task_.context, &next);
    }
public:
    explicit Reporter(const PulseTaskContext& task) : task_(task) { last_.size = sizeof(last_); last_.percent = UINT32_MAX; }
    // The step shown (1-based), the operation the host shows as this task, the footer hint.
    void Step(uint32_t step, const std::string& note) { last_.step = step; note_ = note; }
    void Link(uint64_t operation) { last_.linked_operation = operation; }
    // How a failure reads: why, what stayed safe, and the buttons (the last is the default).
    void Explain(const std::string& detail, const std::string& assurance,
                 std::vector<std::pair<std::string, std::string>> actions) {
        detail_ = detail; assurance_ = assurance; action_text_ = std::move(actions);
    }
    // Diagnostics for the failure view's monospace block (and 复制详情).
    void Log(const std::string& text) { log_ = text; }
    // On success: the notice ("已解压到 X"), its second line and what 打开文件夹 shows.
    void Result(const std::string& text, const std::string& detail, const std::string& path) {
        result_ = text; detail_ = detail; result_path_ = path;
    }
    uint64_t Bytes() const { return last_.total_bytes; }
    uint64_t Items() const { return last_.total_items; }
    // Commit progress (the host writing the staged output) mapped into [from, 99].
    void Writing(uint32_t percent, uint64_t done_items, uint64_t total_items, uint64_t done_bytes, uint64_t total_bytes) {
        PulseTaskProgress next = last_;
        if (percent != UINT32_MAX) next.percent = std::max(last_.percent == UINT32_MAX ? 0u : last_.percent, 90u + percent * 9u / 100u);
        // Counters restart for the write: report them only when they keep the host's order.
        if (total_items >= last_.total_items && done_items >= last_.completed_items && done_items <= total_items) {
            next.total_items = total_items; next.completed_items = done_items;
        }
        if (total_bytes >= last_.total_bytes && done_bytes >= last_.completed_bytes && done_bytes <= total_bytes) {
            next.total_bytes = total_bytes; next.completed_bytes = done_bytes;
        }
        next.current_item = {};
        Send(next);
        last_ = next;
    }
    void Progress(const engine::Progress& value, uint32_t span) {
        PulseTaskProgress next = last_;
        const uint32_t percent = Percent(value.completed_bytes, value.total_bytes, span);
        if (percent != UINT32_MAX && (last_.percent == UINT32_MAX || percent >= last_.percent)) next.percent = percent;
        next.total_bytes = std::max(last_.total_bytes, value.total_bytes);
        next.completed_bytes = std::min(std::max(last_.completed_bytes, value.completed_bytes), next.total_bytes);
        next.total_items = std::max(last_.total_items, value.total_items);
        next.completed_items = std::min(std::max(last_.completed_items, value.completed_items), next.total_items);
        item_ = Utf8(value.current);
        next.current_item = View(item_);
        next.error = {};
        Send(next);
        last_ = next;
        last_.current_item = {};
    }
    void Note(const std::string& item, uint32_t percent = UINT32_MAX) {
        PulseTaskProgress next = last_;
        if (percent != UINT32_MAX) next.percent = std::max(percent, last_.percent == UINT32_MAX ? 0u : last_.percent);
        item_ = item;
        next.current_item = View(item_);
        Send(next);
        last_.percent = next.percent;
    }
    void Fail(const std::string& error) {
        PulseTaskProgress next = last_;
        error_ = error;
        next.error = View(error_);
        next.linked_operation = 0;
        Send(next);
    }
};

// A host stage: created on the destination volume, committed through the host.
class Stage {
    PulseStagingV1 api_;
    std::atomic<bool> created_{false};
    PulseStatus create_status_ = PULSE_ERROR_STATE;
    PulseStageId id_ = 0;
    bool requested_ = false, committed_ = false, terminal_ = false;
    std::wstring path_;
    static void PULSE_CALL Created(void* user, PulseStatus status, PulseStageId id, PulseStringView path) {
        auto& self = *static_cast<Stage*>(user);
        self.id_ = id;
        self.create_status_ = status;
        try { if (path.data && path.length) self.path_ = Wide(std::string(path.data, path.length)); } catch (...) { self.create_status_ = PULSE_ERROR_STATE; }
        self.created_.store(true, std::memory_order_release);
    }
    struct Snapshot {
        PulseStageState state = PULSE_STAGE_PREPARING;
        std::string error;
        uint64_t task = 0, done_items = 0, total_items = 0, done_bytes = 0, total_bytes = 0;
        uint32_t percent = UINT32_MAX;
        static void PULSE_CALL Read(void* user, const PulseStageSnapshot* value) {
            auto& self = *static_cast<Snapshot*>(user);
            self.state = value->state;
            self.task = value->task_id; self.percent = value->percent;
            self.done_items = value->completed_items; self.total_items = value->total_items;
            self.done_bytes = value->completed_bytes; self.total_bytes = value->total_bytes;
            try { if (value->error.data && value->error.length) self.error.assign(value->error.data, value->error.length); } catch (...) {}
        }
    };
public:
    explicit Stage(const PulseStagingV1& api) : api_(api) {}
    ~Stage() { Release(); }
    const std::wstring& Path() const { return path_; }
    bool Create(const std::string& destination) {
        if (api_.create(api_.context, View(destination), Created, this) != PULSE_OK) return false;
        requested_ = true;
        while (!created_.load(std::memory_order_acquire)) Sleep(10);  // an accepted create always calls back
        return create_status_ == PULSE_OK && id_ && !path_.empty();
    }
    // write_step: the task's step for the host's write (0 when it has no steps).
    PulseTaskState Commit(const std::function<bool()>& cancelled, Reporter& report, uint32_t write_step, const std::string& note) {
        if (api_.commit(api_.context, id_) != PULSE_OK) { report.Fail(text::Get(T::ErrCommit)); return PULSE_TASK_FAILED; }
        committed_ = true;
        report.Step(write_step, note);
        report.Note(text::Get(T::WaitingHost), 90);
        for (;;) {
            if (cancelled()) api_.cancel(api_.context, id_);
            Snapshot snapshot;
            if (api_.get(api_.context, id_, Snapshot::Read, &snapshot) != PULSE_OK) { report.Fail(text::Get(T::ErrCommit)); return PULSE_TASK_FAILED; }
            if (snapshot.state < PULSE_STAGE_COMPLETED && snapshot.task) {
                // The host's file operation now writes our output: it shows as this task's second step.
                report.Link(snapshot.task);
                report.Writing(snapshot.percent, snapshot.done_items, snapshot.total_items, snapshot.done_bytes, snapshot.total_bytes);
            }
            if (snapshot.state >= PULSE_STAGE_COMPLETED) {
                report.Link(0);
                terminal_ = true;
                if (!snapshot.error.empty()) report.Fail(snapshot.error);
                return snapshot.state == PULSE_STAGE_COMPLETED ? PULSE_TASK_COMPLETED : snapshot.state == PULSE_STAGE_PARTIAL ? PULSE_TASK_PARTIAL
                     : snapshot.state == PULSE_STAGE_CANCELLED ? PULSE_TASK_CANCELLED : PULSE_TASK_FAILED;
            }
            Sleep(20);
        }
    }
    void Release() {
        if (!requested_) return;
        requested_ = false;
        while (!created_.load(std::memory_order_acquire)) Sleep(10);
        if (!id_) return;
        if (committed_ && !terminal_) {
            api_.cancel(api_.context, id_);
            for (;;) {  // never release a directory a running file operation may still read
                Snapshot snapshot;
                if (api_.get(api_.context, id_, Snapshot::Read, &snapshot) != PULSE_OK || snapshot.state >= PULSE_STAGE_COMPLETED) break;
                Sleep(20);
            }
        }
        api_.release(api_.context, id_);
    }
};

void ClearFolder(const fs::path& folder) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) fs::remove_all(entry.path(), ec);
}

// Asks on the UI thread and waits; returns false when cancelled or refused.
// One archive's password attempts: remembered passwords first, then the user.
struct PasswordSession {
    std::vector<std::wstring> candidates;
    size_t tried = 0;
    bool user_wrong = false, from_user = false, keep = true;  // keep: the dialog's 记住 switch
    std::wstring last;
    void Failed() { if (from_user) user_wrong = true; }
    size_t Attempts() const { return candidates.size() + 5; }
};

engine::AskPassword PasswordPrompt(Jobs::Shared& shared, const std::wstring& archive, const std::function<bool()>& cancelled, PasswordSession& session) {
    return [&shared, archive, &cancelled, &session](bool, std::wstring& out) -> bool {
        if (session.tried < session.candidates.size()) {  // no dialog while a remembered password may fit
            out = session.last = session.candidates[session.tried++];
            session.from_user = false;
            return true;
        }
        struct Answer { HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr); std::mutex lock; bool ok = false, keep = true; std::wstring password;
                        ~Answer() { if (event) CloseHandle(event); } };
        auto answer = std::make_shared<Answer>();
        if (!answer->event) return false;
        const bool again = session.user_wrong;
        const auto posted = shared.dispatch.Post([&shared, archive, again, answer] {
            shared.dialogs.AskPassword(archive, again, shared.remember.load(), [answer](bool ok, std::wstring password, bool keep) {
                { std::lock_guard lock(answer->lock); answer->ok = ok; answer->keep = keep; answer->password = std::move(password); }
                SetEvent(answer->event);
            });
        });
        if (posted != PULSE_OK) return false;
        while (WaitForSingleObject(answer->event, 100) == WAIT_TIMEOUT)
            if (cancelled()) return false;
        std::lock_guard lock(answer->lock);
        if (!answer->ok) return false;
        out = session.last = answer->password;
        session.from_user = true;
        session.keep = answer->keep;
        return true;
    };
}
} // namespace

Jobs::Jobs(const PulseTasksV1& tasks, const PulseStagingV1& staging, pulse::sdk::DispatchQueue& dispatch, Dialogs& dialogs)
    : tasks_(tasks), shared_(std::make_shared<Shared>(staging, dispatch, dialogs)) {}
Jobs::~Jobs() = default;  // the host drains task workers before plugin destroy
void Jobs::CancelAll() { tasks_.CancelAll(); }
void Jobs::RememberPasswords(bool on) {
    shared_->remember = on;
    if (!on) { std::lock_guard guard(shared_->lock); shared_->remembered.clear(); }
}

PulseStatus Jobs::Extract(const std::wstring& archive, const std::string& destination, ExtractMode mode) {
    tasks_.Collect();
    auto shared = shared_;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    const auto name = fs::path(archive).filename().wstring();
    PulseTaskId id = 0;
    pulse::sdk::TaskGroup::TaskOptions options;
    options.source = Utf8(name);
    // The route names the folder, not its whole path ("下载\设计稿-v3").
    const auto shown = fs::path(Wide(destination)).filename().wstring();
    options.destination = Utf8(shown.empty() ? Wide(destination) : shown);
    if (mode == ExtractMode::Folder) options.destination += "\\" + Utf8(ArchiveStem(name));
    options.steps = {text::Get(T::StepUnpack), text::Get(T::StepWrite)};
    const auto status = tasks_.Start(text::Format(T::TaskExtract, Utf8(name)), text::Get(T::TaskType), options,
        [shared, cancel, archive, destination, mode, name](const PulseTaskContext& task) -> PulseTaskState {
            Reporter report(task);
            const ULONGLONG started = GetTickCount64();
            report.Step(1, text::Get(T::NoteUnpack));
            const std::function<bool()> cancelled = [&] { return cancel->load() || task.cancelled(task.context) != 0; };
            const std::vector<std::pair<std::string, std::string>> retry{{"retry", text::Get(T::ActionRetry)}};
            Stage stage(shared->staging);
            if (!stage.Create(destination)) {
                report.Explain({}, text::Get(T::SafeUnchanged), retry);
                report.Fail(text::Get(T::ErrStage));
                return PULSE_TASK_FAILED;
            }
            const fs::path root(stage.Path());
            const auto stem = ArchiveStem(name);
            const fs::path target = mode == ExtractMode::Folder ? root / stem : root;
            PasswordSession passwords{shared->Remembered()};
            const auto ask = PasswordPrompt(*shared, archive, cancelled, passwords);
            engine::ExtractResult result;
            for (size_t attempt = 0; attempt < passwords.Attempts(); ++attempt) {
                ClearFolder(root);
                std::error_code ec;
                if (mode == ExtractMode::Folder) fs::create_directory(target, ec);
                result = engine::Extract(archive, target.wstring(), false, cancelled,
                                         [&](const engine::Progress& value) { report.Progress(value, 88); }, ask);
                if (result.status != engine::Status::WrongPassword) break;
                passwords.Failed();
            }
            // Closing the password box is the user's cancel, not a failure to report.
            if (result.status == engine::Status::Cancelled || result.status == engine::Status::NeedPassword || cancelled())
                return PULSE_TASK_CANCELLED;
            if (result.status != engine::Status::Ok) {
                ClearFolder(root);
                const bool password = result.status == engine::Status::WrongPassword;
                const bool damaged = result.status == engine::Status::InvalidArchive;
                report.Explain(password ? text::Get(T::DetailWrongPassword)
                               : damaged ? text::Get(T::DetailDamaged)
                               : IsWorkerFault(result.status) ? text::Get(T::DetailWorkerIsolated) : std::string{},
                               text::Get(T::SafeUnchanged),
                               password ? std::vector<std::pair<std::string, std::string>>{{"password", text::Get(T::ActionPassword)}}
                               : damaged ? std::vector<std::pair<std::string, std::string>>{{"test", text::Get(T::CmdTest)}, {"retry", text::Get(T::ActionRetry)}}
                               : retry);
                report.Log(Diagnostics(result.error));
                report.Fail(Headline(result.status, result.error));
                return PULSE_TASK_FAILED;
            }
            if (passwords.from_user && passwords.keep) shared->Keep(passwords.last);
            if (mode == ExtractMode::Smart) {
                // One top-level item: in place. Several: into a folder named after the archive.
                std::vector<fs::path> items;
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(root, ec)) items.push_back(entry.path());
                if (items.size() > 1) {
                    std::wstring folder = stem;
                    for (int n = 2; fs::exists(root / folder); ++n) folder = stem + L" (" + std::to_wstring(n) + L")";
                    fs::create_directory(root / folder, ec);
                    for (const auto& item : items)
                        if (!MoveFileExW(item.c_str(), (root / folder / item.filename()).c_str(), 0)) {
                            report.Fail(text::Get(T::ErrIo) + Utf8(item.filename().wstring()));
                            return PULSE_TASK_FAILED;
                        }
                }
            }
            if (fs::is_empty(root)) { report.Note(text::Get(T::TestOk), 100); return PULSE_TASK_COMPLETED; }
            {
                // What lands in the destination: one top-level folder is named, anything else is
                // the destination folder itself (a conflict rename may still change the name).
                std::vector<fs::path> top;
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(root, ec)) top.push_back(entry.path());
                const bool one_folder = top.size() == 1 && fs::is_directory(top.front(), ec);
                const auto shown = one_folder ? top.front().filename().wstring() : fs::path(Wide(destination)).filename().wstring();
                const auto path = one_folder ? destination + "\\" + Utf8(top.front().filename().wstring()) : destination;
                report.Result(text::Format(T::ResultExtracted, Utf8(shown)),
                              ResultLine(mode == ExtractMode::Smart ? text::Get(T::CmdSmart) : std::string{},
                                         result.files, result.bytes, started), path);
            }
            auto outcome = stage.Commit(cancelled, report, 2, text::Get(T::NoteWrite));
            if (outcome == PULSE_TASK_COMPLETED && result.skipped_links) {
                report.Explain({}, {}, {});
                report.Fail(text::Format(T::ErrLinks, std::to_string(result.skipped_links)));
                outcome = PULSE_TASK_PARTIAL;
            }
            if (outcome == PULSE_TASK_COMPLETED) report.Note({}, 100);
            return outcome;
        },
        [cancel](PulseTaskId) { cancel->store(true); }, &id);
    if (status == PULSE_OK)
        Offer(id, {{"retry", [this, archive, destination, mode] { Extract(archive, destination, mode); }},
                   {"password", [this, archive, destination, mode] { Extract(archive, destination, mode); }},
                   {"test", [this, archive, destination, mode] { Test(archive, [this, archive, destination, mode] { Extract(archive, destination, mode); }); }}});
    return status;
}

void Jobs::Offer(PulseTaskId id, FollowUps follow_ups) {
    if (!id) return;
    if (follow_ups_.empty())
        tasks_.OnAction([this](PulseTaskId task, std::string_view action) {
            const auto found = follow_ups_.find(task);
            if (found == follow_ups_.end()) return;
            const auto pick = found->second.find(action);
            auto run = pick == found->second.end() ? std::function<void()>{} : pick->second;
            follow_ups_.erase(found);
            if (run) run();
        });
    follow_ups_[id] = std::move(follow_ups);
    while (follow_ups_.size() > 32) follow_ups_.erase(follow_ups_.begin());  // oldest task ids first
}

PulseStatus Jobs::Test(const std::wstring& archive, std::function<void()> extract) {
    tasks_.Collect();
    auto shared = shared_;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    const auto name = fs::path(archive).filename().wstring();
    PulseTaskId id = 0;
    pulse::sdk::TaskGroup::TaskOptions options;
    options.source = Utf8(name);
    return tasks_.Start(text::Format(T::TaskTest, Utf8(name)), text::Get(T::TaskType), options,
        [shared, cancel, archive, name, extract](const PulseTaskContext& task) -> PulseTaskState {
            Reporter report(task);
            const ULONGLONG started = GetTickCount64();
            const std::function<bool()> cancelled = [&] { return cancel->load() || task.cancelled(task.context) != 0; };
            PasswordSession passwords{shared->Remembered()};
            const auto ask = PasswordPrompt(*shared, archive, cancelled, passwords);
            engine::ExtractResult result;
            for (size_t attempt = 0; attempt < passwords.Attempts(); ++attempt) {
                result = engine::Extract(archive, {}, true, cancelled, [&](const engine::Progress& value) { report.Progress(value, 99); }, ask);
                if (result.status != engine::Status::WrongPassword) break;
                passwords.Failed();
            }
            if (result.status == engine::Status::Cancelled || result.status == engine::Status::NeedPassword || cancelled()) return PULSE_TASK_CANCELLED;
            if (result.status != engine::Status::Ok) {
                // A test writes nothing, so there is nothing to reassure about; say why it failed.
                const bool password = result.status == engine::Status::WrongPassword;
                report.Explain(password ? text::Get(T::DetailWrongPassword)
                               : result.status == engine::Status::InvalidArchive ? text::Get(T::DetailDamaged)
                               : IsWorkerFault(result.status) ? text::Get(T::DetailWorkerIsolated) : std::string{}, {}, {});
                report.Log(Diagnostics(result.error));
                report.Fail(Headline(result.status, result.error));
                return PULSE_TASK_FAILED;
            }
            report.Note(text::Get(T::TestOk), 100);
            const uint64_t seconds = (GetTickCount64() - started + 500) / 1000;
            shared->dispatch.Post([shared, archive, extract, files = result.files, bytes = result.bytes, seconds] {
                shared->dialogs.ShowTestResult(archive, files, bytes, seconds, static_cast<bool>(extract),
                    [extract](bool go) { if (go && extract) extract(); });
            });
            return PULSE_TASK_COMPLETED;
        },
        [cancel](PulseTaskId) { cancel->store(true); }, &id);
}

PulseStatus Jobs::Compress(const std::vector<std::wstring>& sources, const std::string& destination, CompressChoice choice) {
    if (sources.empty() || destination.empty() || choice.name.empty()) return PULSE_ERROR_ARGUMENT;
    tasks_.Collect();
    auto shared = shared_;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    PulseTaskId id = 0;
    pulse::sdk::TaskGroup::TaskOptions options;
    options.source = sources.size() == 1 ? Utf8(fs::path(sources.front()).filename().wstring())
                                         : text::Format(T::SourceMore, Utf8(fs::path(sources.front()).filename().wstring()),
                                                        std::to_string(sources.size() - 1));
    const auto shown = fs::path(Wide(destination)).filename().wstring();
    options.destination = Utf8(shown.empty() ? Wide(destination) : shown) + "\\" + Utf8(choice.name);
    const auto status = tasks_.Start(text::Format(T::TaskCompress, Utf8(choice.name)), text::Get(T::TaskType), options,
        [shared, cancel, sources, destination, choice](const PulseTaskContext& task) -> PulseTaskState {
            Reporter report(task);
            const ULONGLONG started = GetTickCount64();
            report.Step(0, text::Get(T::NoteCompress));
            const std::function<bool()> cancelled = [&] { return cancel->load() || task.cancelled(task.context) != 0; };
            const std::vector<std::pair<std::string, std::string>> retry{{"retry", text::Get(T::ActionRetry)}};
            Stage stage(shared->staging);
            if (!stage.Create(destination)) {
                report.Explain({}, text::Get(T::SafeNoArchive), retry);
                report.Fail(text::Get(T::ErrStage));
                return PULSE_TASK_FAILED;
            }
            const auto result = engine::Create(sources, stage.Path(), choice.name, choice.options, cancelled,
                                               [&](const engine::Progress& value) { report.Progress(value, 88); });
            if (result.status == engine::Status::Cancelled || cancelled()) return PULSE_TASK_CANCELLED;
            if (result.status != engine::Status::Ok) {
                report.Explain(IsWorkerFault(result.status) ? text::Get(T::DetailWorkerIsolated) : std::string{},
                               text::Get(T::SafeNoArchive), retry);
                report.Log(Diagnostics(result.error));
                report.Fail(Headline(result.status, result.error));
                return PULSE_TASK_FAILED;
            }
            report.Result(text::Format(T::ResultCompressed, Utf8(choice.name)),
                          ResultLine({}, result.files ? result.files : report.Items(), result.bytes ? result.bytes : report.Bytes(), started),
                          destination + "\\" + Utf8(choice.name));
            auto outcome = stage.Commit(cancelled, report, 0, text::Get(T::NoteWrite));
            if (outcome == PULSE_TASK_COMPLETED && result.skipped) {
                report.Explain({}, {}, {});
                report.Fail(text::Format(T::ErrSkipped, std::to_string(result.skipped)));
                outcome = PULSE_TASK_PARTIAL;
            }
            if (outcome == PULSE_TASK_COMPLETED) report.Note({}, 100);
            return outcome;
        },
        [cancel](PulseTaskId) { cancel->store(true); }, &id);
    if (status == PULSE_OK) Offer(id, {{"retry", [this, sources, destination, choice] { Compress(sources, destination, choice); }}});
    return status;
}

PulseStatus Jobs::Hash(const std::vector<std::wstring>& files) {
    tasks_.Collect();
    auto shared = shared_;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    PulseTaskId id = 0;
    return tasks_.Start(text::Get(T::TaskHash), text::Get(T::TaskType),
        [shared, cancel, files](const PulseTaskContext& task) -> PulseTaskState {
            Reporter report(task);
            const std::function<bool()> cancelled = [&] { return cancel->load() || task.cancelled(task.context) != 0; };
            const auto results = engine::Hash(files, cancelled, [&](const engine::Progress& value) { report.Progress(value, 99); });
            if (cancelled()) return PULSE_TASK_CANCELLED;
            // "Copy all": every value of every file, one block per file.
            std::string full;
            for (const auto& item : results) {
                if (!item.error.empty()) continue;
                if (!full.empty()) full += "\r\n";
                full += Utf8(fs::path(item.path).filename().wstring()) + "\r\nCRC32: " + item.crc32 + "\r\nSHA-1: " +
                        item.sha1 + "\r\nSHA-256: " + item.sha256 + "\r\n";
            }
            shared->dispatch.Post([shared, results, full] {
                shared->dialogs.ShowChecksums(results, [full](bool copy) {
                    if (!copy || !OpenClipboard(nullptr)) return;
                    EmptyClipboard();
                    const auto wide = Wide(full);
                    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
                        if (auto* data = static_cast<wchar_t*>(GlobalLock(memory))) {
                            memcpy(data, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
                            GlobalUnlock(memory);
                            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
                        } else GlobalFree(memory);
                    }
                    CloseClipboard();
                });
            });
            report.Note({}, 100);
            return PULSE_TASK_COMPLETED;
        },
        [cancel](PulseTaskId) { cancel->store(true); }, &id);
}
