#include "../common/virtual_disk_eject.h"
#include "../ipc/elevated_transfer_transport.h"
#include "../ipc/elevated_transfer_messages.h"
#include "../common/path_utils.h"
#include "../ops/elevated_delete.h"
#include <shellapi.h>
#include <objbase.h>
#include <atomic>
#include <filesystem>
#include <thread>

namespace {
using namespace pulse;
using namespace pulse::elevated;
int Run(DWORD parent_id, const Nonce& nonce) {
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_id));
    if (!parent || UserSid(parent.Get()).empty() || WaitForSingleObject(parent.Get(), 0) != WAIT_TIMEOUT) return 3;
    DWORD parent_session = 0, own_session = 0;
    if (!ProcessIdToSessionId(parent_id, &parent_session) || !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) ||
        parent_session != own_session) return 3;
    const auto own_image = ProcessImage(GetCurrentProcess());
    const auto parent_image = ProcessImage(parent.Get());
#ifdef PULSE_ELEVATED_TEST_HOST
    const auto expected = std::filesystem::path(own_image).parent_path() / L"pulse_elevated_session_test.exe";
    const auto audit_expected = std::filesystem::path(own_image).parent_path() / L"pulse_transfer_audit_test.exe";
#else
    if (!IsElevated(GetCurrentProcess())) return 3;
    const auto expected = std::filesystem::path(own_image).parent_path() / L"pulse.exe";
#endif
    if (!path::EqualInsensitive(parent_image, expected.wstring())
#ifdef PULSE_ELEVATED_TEST_HOST
        && !path::EqualInsensitive(parent_image, audit_expected.wstring())
#endif
        ) return 3;
    const auto name = PipeName(parent_id, nonce);
    Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!pipe) return 4;
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(pipe.Get(), &server) || server != parent_id) return 3;
    if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::Hello, 0, {})) return 4;
    Handle changed(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!changed) return 4;
    Handle receiver_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!receiver_done) return 4;
    Handle finished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!finished) return 4;
    // Parent death must never leave an authorized process waiting on a slow
    // filesystem/provider forever. Allow normal cancellation a bounded grace.
    std::thread lifetime([&] {
        HANDLE waits[]{finished.Get(), parent.Get()};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1 &&
            WaitForSingleObject(finished.Get(), 5000) != WAIT_OBJECT_0)
            TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
    });
    ops::OpsManager manager;
    std::atomic<bool> stopping{false}, cancelled{false};
    std::atomic<uint64_t> active_request{0}, active_task{0};
    std::atomic<bool> deleting{false}, delete_done{false};
    std::mutex delete_mutex;
    ops::OpStatus delete_status;
    ops::ShellTransferResult delete_result;
    std::thread delete_worker;
    const bool same_user = SameUser(GetCurrentProcess(), parent.Get());
    manager.Start([&] { SetEvent(changed.Get()); });
    std::thread receiver([&] {
        uint64_t last_request = 0;
        std::vector<unsigned char> payload;
        while (!stopping.load()) {
            Header header;
            if (!ReadFrame(pipe.Get(), parent.Get(), nonce, header, payload)) break;
            Reader reader{payload};
            if (header.kind == Kind::Shutdown && header.request == 0 && reader.Done()) break;
            if (header.kind == Kind::Transfer) {
                if (active_request.load() || header.request <= last_request) break;
                const auto move = reader.Number<uint32_t>();
                const auto policy = reader.Number<uint32_t>();
                const auto verify = reader.Number<uint32_t>();
                ops::OpRequest request;
                request.type = move ? ops::OpType::Move : ops::OpType::Copy;
                request.collision_policy = static_cast<ops::CollisionPolicy>(policy);
                request.dest_dir = reader.Text();
                const auto count = reader.Number<uint32_t>();
                if (!reader.good || move > 2 || policy > 2 || verify > 1 || count == 0 || count > kMaxPaths || !SafePath(request.dest_dir)) break;
                bool valid = true;
                for (uint32_t i = 0; i < count; ++i) {
                    auto source = reader.Text(); if (!SafePath(source)) valid = false;
                    request.sources.push_back(std::move(source));
                }
                request.new_name = reader.Text();
                if (move == 2) {
                    ops::RenameIdentity identity;
                    for (auto* object : {&identity.source, &identity.parent}) {
                        object->volume = reader.Number<DWORD>();
                        object->high = reader.Number<DWORD>();
                        object->low = reader.Number<DWORD>();
                    }
                    request.authorized_rename_identity = identity;
                }
                if (!valid || !SafeTransferName(request.new_name, count, move != 0) || !reader.Done()) break;
                if (move == 2) {
                    if (request.new_name.empty() || count != 1) break;
                    const auto parent_path = std::filesystem::path(request.sources.front()).parent_path().lexically_normal().wstring();
                    const auto destination = std::filesystem::path(request.dest_dir).lexically_normal().wstring();
                    if (CompareStringOrdinal(parent_path.c_str(), -1, destination.c_str(), -1, TRUE) != CSTR_EQUAL) break;
                    // Dedicated no-overwrite rename; never enter the move engine.
                    request.type = ops::OpType::Rename;
                }
                request.is_undo = true;
                manager.SetVerifyCopies(verify != 0);
                cancelled.store(false);
                deleting.store(false);
                active_task.store(0);
                active_request.store(header.request);
                last_request = header.request;
                active_task.store(manager.Submit(std::move(request)));
                SetEvent(changed.Get());
            } else if (header.kind == Kind::CreateFolder) {
                if (active_request.load() || header.request <= last_request) break;
                auto path = reader.Text();
                if (!reader.Done() || !SafePath(path)) break;
                ops::OpRequest request;
                request.type = ops::OpType::CreateFolder;
                request.sources.push_back(std::move(path));
                request.is_undo = true;
                cancelled.store(false);
                deleting.store(false);
                active_task.store(0);
                active_request.store(header.request);
                last_request = header.request;
                active_task.store(manager.Submit(std::move(request)));
                SetEvent(changed.Get());
            } else if (header.kind == Kind::EjectVirtualDisk) {
                if (active_request.load() || header.request <= last_request) break;
                storage::VirtualDiskIdentity identity;
                identity.volume_name = reader.Text(); identity.image_path = reader.Text();
                identity.serial = reader.Number<DWORD>(); identity.index_high = reader.Number<DWORD>();
                identity.index_low = reader.Number<DWORD>();
                if (!reader.Done() || !storage::ValidVolumeName(identity.volume_name) || identity.image_path.empty()) break;
                last_request = header.request;
                // The shared engine verifies the image and exact virtual device, then
                // negotiates safe removal; it never force-removes or climbs parents.
                std::wstring veto_name;
                const DWORD error = storage::DetachVirtualDiskSafely(identity, &veto_name);
                Writer result;
                result.Number<HRESULT>(HRESULT_FROM_WIN32(error));
                result.Number<uint32_t>(0); result.Number<uint32_t>(error == ERROR_SUCCESS ? 1 : 0);
                result.Text(veto_name);
                if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::Result, header.request, result)) break;
            } else if (header.kind == Kind::RecycleDelete || header.kind == Kind::PermanentDelete) {
                if (active_request.load() || header.request <= last_request) break;
                const auto count = reader.Number<uint32_t>();
                if (!reader.good || count == 0 || count > kMaxPaths) break;
                std::vector<std::wstring> sources;
                bool valid = true;
                for (uint32_t i = 0; i < count; ++i) {
                    auto source = reader.Text(); if (!SafePath(source)) valid = false;
                    sources.push_back(std::move(source));
                }
                if (!valid || !reader.Done()) break;
                if (delete_worker.joinable()) delete_worker.join();
                const bool permanent = header.kind == Kind::PermanentDelete;
                {
                    std::lock_guard lock(delete_mutex);
                    delete_result = {};
                    delete_status = {};
                    delete_status.active = true; delete_status.can_pause = false;
                    delete_status.phase = ops::OpPhase::Running;
                    delete_status.total_items = sources.size();
                }
                cancelled.store(false); delete_done.store(false); deleting.store(true);
                active_task.store(0); active_request.store(header.request); last_request = header.request;
                delete_worker = std::thread([&, sources = std::move(sources), permanent] {
                    ops::ShellTransferResult result;
                    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                    if (!permanent && !same_user) {
                        result.hr = E_ACCESSDENIED;
                        result.error = L"Recycling with a different administrator account cannot preserve the original user's Recycle Bin. The files were not deleted.";
                    } else if (FAILED(initialized)) result.hr = initialized;
                    else {
                        result = ops::DeleteWithCurrentToken(sources, permanent, nullptr, cancelled,
                            [&](const std::wstring& item, float percent) {
                                std::lock_guard lock(delete_mutex);
                                delete_status.current_item = item; delete_status.percent = percent;
                                SetEvent(changed.Get());
                            });
                    }
                    if (SUCCEEDED(initialized)) CoUninitialize();
                    {
                        std::lock_guard lock(delete_mutex);
                        delete_status.active = false;
                        delete_status.phase = FAILED(result.hr) || result.cancelled ? ops::OpPhase::Failed : ops::OpPhase::Completed;
                        delete_status.completed_items = result.sources.size();
                        delete_status.last_error = result.error;
                        delete_result = std::move(result);
                    }
                    delete_done.store(true); SetEvent(changed.Get());
                });
                SetEvent(changed.Get());
            } else {
                if (!active_request.load() && header.request == last_request &&
                    (header.kind == Kind::Cancel || header.kind == Kind::Pause || header.kind == Kind::Resume) && reader.Done()) continue;
                if (!active_request.load() || header.request != active_request.load()) break;
                if (header.kind == Kind::ConflictReply) {
                    if (deleting.load()) break;
                    const auto token = reader.Number<uint64_t>();
                    const auto choice = reader.Number<uint32_t>();
                    const auto apply = reader.Number<uint32_t>();
                    const auto pending = manager.PendingConflict();
                    if (!reader.Done() || choice > static_cast<uint32_t>(ops::ConflictChoice::KeepBoth) || apply > 1 || !pending || pending->token != token) break;
                    if (choice == static_cast<uint32_t>(ops::ConflictChoice::Cancel)) cancelled.store(true);
                    manager.ResolveConflict(token, static_cast<ops::ConflictChoice>(choice), apply != 0);
                } else {
                    if (!reader.Done()) break;
                    if (header.kind == Kind::Cancel) { cancelled.store(true); manager.CancelCurrent(); }
                    else if (header.kind == Kind::Pause) manager.PauseCurrent();
                    else if (header.kind == Kind::Resume) manager.ResumeCurrent();
                    else break;
                }
            }
        }
        stopping.store(true); cancelled.store(true); manager.CancelCurrent(); SetEvent(changed.Get()); SetEvent(receiver_done.Get());
    });
    uint64_t sent_conflict = 0;
    ULONGLONG last_progress = 0;
    ops::OpPhase last_phase = ops::OpPhase::Failed;
    while (!stopping.load()) {
        HANDLE waits[]{changed.Get(), parent.Get()};
        const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (waited != WAIT_OBJECT_0) break;
        (void)manager.DrainFinishedTasks();
        const uint64_t request = active_request.load(), task = active_task.load();
        if (!request) continue;
        if (deleting.load()) {
            ops::OpStatus status;
            ops::ShellTransferResult result;
            const bool done = delete_done.load();
            {
                std::lock_guard lock(delete_mutex);
                status = delete_status;
                if (done) result = delete_result;
            }
            const auto now = GetTickCount64();
            if (done || now - last_progress >= 100 || status.phase != last_phase) {
                Writer progress; WriteStatus(progress, status);
                if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::Progress, request, progress)) break;
                last_progress = now; last_phase = status.phase;
            }
            if (done) {
                bool sent = true;
                for (const auto& source : result.sources) {
                    Writer item; item.Text(source);
                    if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::DeletedItem, request, item)) { sent = false; break; }
                }
                if (!sent) break;
                Writer completed;
                completed.Number(result.hr); completed.Number<uint32_t>(result.cancelled ? 1 : 0);
                completed.Number<uint32_t>(result.mutated ? 1 : 0); completed.Text(result.error);
                deleting.store(false); active_request.store(0);
                if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::Result, request, completed)) break;
            }
            continue;
        }
        if (!task) continue;
        const auto status = manager.Status();
        if (status.task_id != task) continue;
        const auto conflict = manager.PendingConflict();
        if (conflict && conflict->token != sent_conflict) {
            Writer message; WriteConflict(message, *conflict);
            if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::Conflict, request, message)) break;
            sent_conflict = conflict->token;
        }
        const auto now = GetTickCount64();
        if (now - last_progress >= 100 || status.phase != last_phase || !status.active || conflict) {
            Writer message; WriteStatus(message, status);
            if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::Progress, request, message)) break;
            last_progress = now; last_phase = status.phase;
        }
        if (!status.active && (status.phase == ops::OpPhase::Completed || status.phase == ops::OpPhase::Failed)) {
            bool sent = true;
            size_t items = 0;
            const auto completions = manager.DrainCompletions();
            for (const auto& completion : completions) {
                if (completion.refresh_only) continue;
                if (completion.sources.size() != completion.destinations.size()) { sent = false; break; }
                for (size_t i = 0; i < completion.sources.size(); ++i) {
                    if (++items > 100000) { sent = false; break; }
                    Writer message; message.Text(completion.sources[i]); message.Text(completion.destinations[i]);
                    if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::CompletedItem, request, message)) { sent = false; break; }
                }
                if (!sent) break;
            }
            Writer result;
            result.Number<HRESULT>(!sent ? HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW) :
                cancelled.load() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) :
                status.phase == ops::OpPhase::Failed ? HRESULT_FROM_WIN32(ERROR_GEN_FAILURE) : S_OK);
            result.Number<uint32_t>(cancelled.load() ? 1 : 0);
            result.Number<uint32_t>(1); // Conservative refresh even after partial directory mutations.
            result.Text(sent ? status.last_error : L"The operation completed too many items to report safely.");
            // Clear before publishing Result, allowing the next authenticated
            // request immediately after the client receives completion.
            active_task.store(0); active_request.store(0); sent_conflict = 0;
            if (!WriteFrame(pipe.Get(), parent.Get(), nonce, Kind::Result, request, result)) break;
        }
    }
    stopping.store(true); cancelled.store(true);
    manager.CancelCurrent();
    // Cancel each pending read until the reader has left; this also covers a
    // read starting between the stop flag and the first CancelIoEx call.
    while (WaitForSingleObject(receiver_done.Get(), 0) != WAIT_OBJECT_0) {
        CancelIoEx(pipe.Get(), nullptr);
        WaitForSingleObject(receiver_done.Get(), 100);
    }
    // Broken/closed parent means no further commands are accepted. The existing
    // engine handles cancellation/rollback before this process exits.
    if (receiver.joinable()) receiver.join();
    manager.Stop();
    if (delete_worker.joinable()) delete_worker.join();
    SetEvent(finished.Get());
    lifetime.join();
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 2;
    DWORD parent = 0; pulse::elevated::Nonce nonce{}; bool valid = false;
    if (count == 5 && std::wstring(args[1]) == L"--parent" && std::wstring(args[3]) == L"--nonce") {
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(args[2], &end, 10);
        parent = static_cast<DWORD>(parsed);
        valid = parent != 0 && end && *end == L'\0' && pulse::elevated::ParseNonce(args[4], nonce);
    }
    LocalFree(args);
    if (!valid) return 2;
    try { return Run(parent, nonce); } catch (...) { return 5; }
}
