#include "jobs.h"
#include "engine.h"
#include "text.h"
#include <windows.h>
#include <atomic>
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
std::string ErrorText(engine::Status status, const std::string& detail) {
    switch (status) {
    case engine::Status::WrongPassword: return text::Get(T::ErrWrongPassword);
    case engine::Status::NeedPassword: return text::Get(T::ErrNoPassword);
    case engine::Status::Unsupported: return text::Get(T::ErrUnsupported);
    case engine::Status::IoError: return text::Get(T::ErrIo) + detail;
    default: return text::Get(T::ErrDamaged) + detail;
    }
}

// Reports progress; keeps percent and counters monotonic as the host requires.
class Reporter {
    const PulseTaskContext& task_;
    PulseTaskProgress last_{};
    std::string item_, error_;
public:
    explicit Reporter(const PulseTaskContext& task) : task_(task) { last_.size = sizeof(last_); last_.percent = UINT32_MAX; }
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
        task_.report(task_.context, &next);
        last_ = next;
        last_.current_item = {};
    }
    void Note(const std::string& item, uint32_t percent = UINT32_MAX) {
        PulseTaskProgress next = last_;
        if (percent != UINT32_MAX) next.percent = std::max(percent, last_.percent == UINT32_MAX ? 0u : last_.percent);
        item_ = item;
        next.current_item = View(item_);
        task_.report(task_.context, &next);
        last_.percent = next.percent;
    }
    void Fail(const std::string& error) {
        PulseTaskProgress next = last_;
        error_ = error;
        next.error = View(error_);
        task_.report(task_.context, &next);
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
        static void PULSE_CALL Read(void* user, const PulseStageSnapshot* value) {
            auto& self = *static_cast<Snapshot*>(user);
            self.state = value->state;
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
    PulseTaskState Commit(const std::function<bool()>& cancelled, Reporter& report) {
        if (api_.commit(api_.context, id_) != PULSE_OK) { report.Fail(text::Get(T::ErrCommit)); return PULSE_TASK_FAILED; }
        committed_ = true;
        report.Note(text::Get(T::WaitingHost), 90);
        for (;;) {
            if (cancelled()) api_.cancel(api_.context, id_);
            Snapshot snapshot;
            if (api_.get(api_.context, id_, Snapshot::Read, &snapshot) != PULSE_OK) { report.Fail(text::Get(T::ErrCommit)); return PULSE_TASK_FAILED; }
            if (snapshot.state >= PULSE_STAGE_COMPLETED) {
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
    bool user_wrong = false, from_user = false;
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
        struct Answer { HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr); std::mutex lock; bool ok = false; std::wstring password;
                        ~Answer() { if (event) CloseHandle(event); } };
        auto answer = std::make_shared<Answer>();
        if (!answer->event) return false;
        const bool again = session.user_wrong;
        const auto posted = shared.dispatch.Post([&shared, archive, again, answer] {
            shared.dialogs.AskPassword(archive, again, shared.remember.load(), [answer](bool ok, std::wstring password) {
                { std::lock_guard lock(answer->lock); answer->ok = ok; answer->password = std::move(password); }
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
    return tasks_.Start(text::Format(T::TaskExtract, Utf8(name)), text::Get(T::TaskType),
        [shared, cancel, archive, destination, mode, name](const PulseTaskContext& task) -> PulseTaskState {
            Reporter report(task);
            const std::function<bool()> cancelled = [&] { return cancel->load() || task.cancelled(task.context) != 0; };
            Stage stage(shared->staging);
            if (!stage.Create(destination)) { report.Fail(text::Get(T::ErrStage)); return PULSE_TASK_FAILED; }
            const fs::path root(stage.Path());
            const auto stem = ArchiveStem(name);
            const fs::path target = mode == ExtractMode::Folder ? root / stem : root;
            PasswordSession passwords{shared->Remembered()};
            const auto ask = PasswordPrompt(*shared, name, cancelled, passwords);
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
            if (result.status != engine::Status::Ok) { report.Fail(ErrorText(result.status, result.error)); return PULSE_TASK_FAILED; }
            if (passwords.from_user) shared->Keep(passwords.last);
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
            auto outcome = stage.Commit(cancelled, report);
            if (outcome == PULSE_TASK_COMPLETED && result.skipped_links) {
                report.Fail(text::Format(T::ErrLinks, std::to_string(result.skipped_links)));
                outcome = PULSE_TASK_PARTIAL;
            }
            if (outcome == PULSE_TASK_COMPLETED) report.Note({}, 100);
            return outcome;
        },
        [cancel](PulseTaskId) { cancel->store(true); }, &id);
}

PulseStatus Jobs::Test(const std::wstring& archive) {
    tasks_.Collect();
    auto shared = shared_;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    const auto name = fs::path(archive).filename().wstring();
    PulseTaskId id = 0;
    return tasks_.Start(text::Format(T::TaskTest, Utf8(name)), text::Get(T::TaskType),
        [shared, cancel, archive, name](const PulseTaskContext& task) -> PulseTaskState {
            Reporter report(task);
            const std::function<bool()> cancelled = [&] { return cancel->load() || task.cancelled(task.context) != 0; };
            PasswordSession passwords{shared->Remembered()};
            const auto ask = PasswordPrompt(*shared, name, cancelled, passwords);
            engine::ExtractResult result;
            for (size_t attempt = 0; attempt < passwords.Attempts(); ++attempt) {
                result = engine::Extract(archive, {}, true, cancelled, [&](const engine::Progress& value) { report.Progress(value, 99); }, ask);
                if (result.status != engine::Status::WrongPassword) break;
                passwords.Failed();
            }
            if (result.status == engine::Status::Cancelled || result.status == engine::Status::NeedPassword || cancelled()) return PULSE_TASK_CANCELLED;
            if (result.status != engine::Status::Ok) { report.Fail(ErrorText(result.status, result.error)); return PULSE_TASK_FAILED; }
            report.Note(text::Get(T::TestOk), 100);
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
    return tasks_.Start(text::Format(T::TaskCompress, Utf8(choice.name)), text::Get(T::TaskType),
        [shared, cancel, sources, destination, choice](const PulseTaskContext& task) -> PulseTaskState {
            Reporter report(task);
            const std::function<bool()> cancelled = [&] { return cancel->load() || task.cancelled(task.context) != 0; };
            Stage stage(shared->staging);
            if (!stage.Create(destination)) { report.Fail(text::Get(T::ErrStage)); return PULSE_TASK_FAILED; }
            const auto result = engine::Create(sources, stage.Path(), choice.name, choice.options, cancelled,
                                               [&](const engine::Progress& value) { report.Progress(value, 88); });
            if (result.status == engine::Status::Cancelled || cancelled()) return PULSE_TASK_CANCELLED;
            if (result.status != engine::Status::Ok) { report.Fail(ErrorText(result.status, result.error)); return PULSE_TASK_FAILED; }
            auto outcome = stage.Commit(cancelled, report);
            if (outcome == PULSE_TASK_COMPLETED && result.skipped) {
                report.Fail(text::Format(T::ErrSkipped, std::to_string(result.skipped)));
                outcome = PULSE_TASK_PARTIAL;
            }
            if (outcome == PULSE_TASK_COMPLETED) report.Note({}, 100);
            return outcome;
        },
        [cancel](PulseTaskId) { cancel->store(true); }, &id);
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
