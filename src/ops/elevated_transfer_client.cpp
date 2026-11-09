#include "../common/rename_identity.h"
#include "elevated_transfer_client.h"
#include "../ipc/elevated_transfer_transport.h"
#include "../ipc/elevated_transfer_messages.h"
#include "../common/path_utils.h"
#include <shellapi.h>
#include <bcrypt.h>
#include <filesystem>
#include <mutex>

namespace pulse::ops {
namespace {
using namespace elevated;
struct Session {
    std::mutex mutex;
    Handle pipe;
    Handle process;
    Nonce nonce{};
    uint64_t sequence = 0;
    void Reset() { pipe.Reset(); process.Reset(); }
};
Session& Shared() { static Session session; return session; }
HRESULT LastFailure() { const DWORD error = GetLastError(); return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE); }
HRESULT StartSession(Session& session, HWND owner, const std::function<void(bool)>& authorization) {
    if (session.pipe && session.process && WaitForSingleObject(session.process.Get(), 0) == WAIT_TIMEOUT) return S_OK;
    session.Reset();
    if (BCryptGenRandom(nullptr, session.nonce.data(), static_cast<ULONG>(session.nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return E_FAIL;
    const DWORD parent = GetCurrentProcessId();
    const auto parent_image = ProcessImage(GetCurrentProcess());
    if (parent_image.empty()) return E_FAIL;
#ifdef PULSE_ELEVATED_TEST_CLIENT
    const auto helper = std::filesystem::path(parent_image).parent_path() / L"pulse_elevated_test_host.exe";
#else
    if (!path::EqualInsensitive(std::filesystem::path(parent_image).filename().wstring(), L"pulse.exe")) return E_ACCESSDENIED;
    const auto helper = std::filesystem::path(parent_image).parent_path() / L"pulse_elevated.exe";
#endif
    session.pipe = CreateSessionPipe(PipeName(parent, session.nonce));
    if (!session.pipe) return LastFailure();
    struct AuthorizationScope {
        const std::function<void(bool)>& callback;
        ~AuthorizationScope() { try { if (callback) callback(false); } catch (...) {} }
    } authorization_scope{authorization};
    try { if (authorization) authorization(true); }
    catch (...) { session.Reset(); return E_FAIL; }
    const std::wstring arguments = L"--parent " + std::to_wstring(parent) + L" --nonce " + NonceText(session.nonce);
#ifdef PULSE_ELEVATED_TEST_CLIENT
    std::wstring command = L"\"" + helper.wstring() + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        helper.parent_path().c_str(), &startup, &information)) { const auto error = LastFailure(); session.Reset(); return error; }
    CloseHandle(information.hThread); session.process.Reset(information.hProcess);
    (void)owner;
#else
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner; execute.lpVerb = L"runas"; execute.lpFile = helper.c_str();
    execute.lpParameters = arguments.c_str(); execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) { const auto error = LastFailure(); session.Reset(); return error; }
    session.process.Reset(execute.hProcess);
#endif
    if (!session.process || !WaitPipeConnection(session.pipe.Get(), session.process.Get(), 30000)) {
        const auto error = LastFailure(); session.Reset(); return error;
    }
    ULONG peer = 0;
    const auto actual_image = ProcessImage(session.process.Get());
    if (!GetNamedPipeClientProcessId(session.pipe.Get(), &peer) || peer != GetProcessId(session.process.Get()) ||
        !path::EqualInsensitive(actual_image, helper.wstring()) || UserSid(session.process.Get()).empty()
#ifndef PULSE_ELEVATED_TEST_CLIENT
        || !IsElevated(session.process.Get())
#endif
        ) { session.Reset(); return E_ACCESSDENIED; }
    Header header; std::vector<unsigned char> payload;
    if (!ReadFrame(session.pipe.Get(), session.process.Get(), session.nonce, header, payload, {}, 10000) ||
        header.kind != Kind::Hello || header.request != 0 || !payload.empty()) { session.Reset(); return E_ACCESSDENIED; }
    return S_OK;
}
}
static ShellTransferResult RunAuthorizedRequest(elevated::Kind kind, const elevated::Writer& request,
    HWND owner, std::atomic<bool>& cancel, ElevatedTransferCallbacks callbacks) {
    using namespace elevated;
    ShellTransferResult result;
    auto& session = Shared();
    std::lock_guard lock(session.mutex);
    if (cancel.load()) { result.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); result.cancelled = true; return result; }
    result.hr = StartSession(session, owner, callbacks.authorization);
    if (FAILED(result.hr)) { result.cancelled = result.hr == HRESULT_FROM_WIN32(ERROR_CANCELLED); return result; }
    if (cancel.load()) { result.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); result.cancelled = true; return result; }
    const uint64_t sequence = ++session.sequence;
    if (!WriteFrame(session.pipe.Get(), session.process.Get(), session.nonce, kind, sequence, request)) {
        result.hr = LastFailure();
        // A lost acknowledgement cannot prove that the helper did not receive
        // the request. Do not offer replay of a possibly completed operation.
        result.mutated = true;
        result.error = L"The connection ended while submitting the operation. Check the affected files before starting it again.";
        session.Reset(); return result;
    }
    bool sent_cancel = false, was_paused = false;
    auto tick = [&] {
        if (cancel.load() && !sent_cancel) {
            sent_cancel = true;
            if (!WriteFrame(session.pipe.Get(), session.process.Get(), session.nonce, Kind::Cancel, sequence, {})) return false;
        }
        // Cancel already wakes the worker. Do not send a redundant Resume or
        // a conflict reply after the request's one terminal control message.
        if (sent_cancel) return true;
        const bool paused = callbacks.paused && callbacks.paused();
        if (paused != was_paused) {
            was_paused = paused;
            if (!WriteFrame(session.pipe.Get(), session.process.Get(), session.nonce, paused ? Kind::Pause : Kind::Resume, sequence, {})) return false;
        }
        return true;
    };
    try {
        std::vector<unsigned char> payload;
        for (;;) {
            Header header;
            if (!tick() || !ReadFrame(session.pipe.Get(), session.process.Get(), session.nonce, header, payload, tick) || header.request != sequence) break;
            Reader reader{payload};
            if (header.kind == Kind::Progress) {
                auto status = ReadStatus(reader);
                if (!reader.Done()) break;
                if (callbacks.progress) callbacks.progress(status);
            } else if (header.kind == Kind::Conflict) {
                const auto conflict = ReadConflict(reader);
                if (!reader.Done()) break;
                const auto answer = cancel.load() || !callbacks.conflict ? ElevatedConflictAnswer{} : callbacks.conflict(conflict);
                // A conflict cancel and the operation Cancel button use one
                // protocol entry point. The host may clear PendingConflict as
                // soon as Cancel is read, so a later reply must not follow it.
                if (answer.choice == ConflictChoice::Cancel) cancel.store(true);
                // Apply a newly requested pause before releasing the worker.
                if (!tick()) break;
                if (sent_cancel) continue;
                Writer response; response.Number(conflict.token); response.Number(static_cast<uint32_t>(answer.choice));
                response.Number<uint32_t>(answer.apply_to_all ? 1 : 0);
                if (!WriteFrame(session.pipe.Get(), session.process.Get(), session.nonce, Kind::ConflictReply, sequence, response)) break;
            } else if (header.kind == Kind::CompletedItem && (kind == Kind::Transfer || kind == Kind::CreateFolder)) {
                auto from = reader.Text(), to = reader.Text();
                if (!reader.Done() || !SafePath(from) || !SafePath(to) || result.sources.size() >= 100000) break;
                result.sources.push_back(std::move(from)); result.destinations.push_back(std::move(to)); result.mutated = true;
            } else if (header.kind == Kind::DeletedItem && (kind == Kind::RecycleDelete || kind == Kind::PermanentDelete)) {
                auto from = reader.Text();
                if (!reader.Done() || !SafePath(from) || result.sources.size() >= kMaxPaths) break;
                result.sources.push_back(std::move(from)); result.mutated = true;
            } else if (header.kind == Kind::Result) {
                result.hr = reader.Number<HRESULT>(); result.cancelled = reader.Number<uint32_t>() != 0;
                result.mutated = reader.Number<uint32_t>() != 0 || result.mutated;
                result.error = reader.Text();
                if (!reader.Done()) break;
                return result;
            } else break;
        }
    } catch (...) { result.hr = E_FAIL; }
    result.hr = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
    result.error = L"The authorized file-operation session ended before completion.";
    result.cancelled = cancel.load(); result.mutated = true;
    session.Reset(); // No replay: the helper may have already changed files.
    return result;
}
ShellTransferResult TransferWithElevatedHelper(const std::vector<std::wstring>& sources,
    const std::wstring& destination, bool move, HWND owner, std::atomic<bool>& cancel,
    ShellCollisionPolicy policy, ElevatedTransferCallbacks callbacks, const std::wstring& target_name, bool rename_only) {
    using namespace elevated;
    ShellTransferResult result;
    if (cancel.load()) { result.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); result.cancelled = true; return result; }
    if (sources.empty() || !SafePath(destination)) { result.hr = E_INVALIDARG; result.error = L"The destination is not a supported absolute filesystem path."; return result; }
    if (sources.size() > kMaxPaths) { result.hr = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); result.error = L"An authorized transfer supports at most 4096 selected items per request."; return result; }
    for (const auto& source : sources) if (!SafePath(source)) { result.hr = E_INVALIDARG; result.error = L"A source is not a supported absolute filesystem path."; return result; }
    if ((rename_only && (!move || target_name.empty())) || !SafeTransferName(target_name, sources.size(), move)) {
        result.hr = E_INVALIDARG; result.error = L"An explicit move destination must be one safe leaf name for one source."; return result;
    }
    Writer request;
    request.Number<uint32_t>(rename_only ? 2 : move ? 1 : 0); request.Number(static_cast<uint32_t>(policy));
    request.Number<uint32_t>(callbacks.verify_copies ? 1 : 0);
    request.Text(destination); request.Number(static_cast<uint32_t>(sources.size()));
    for (const auto& source : sources) request.Text(source);
    request.Text(target_name);
    if (rename_only) {
        ops::RenameIdentity identity;
        const DWORD error = ops::CaptureRenameIdentity(sources.front(), identity);
        if (error) { result.hr = HRESULT_FROM_WIN32(error); return result; }
        for (const auto& object : {identity.source, identity.parent}) {
            request.Number(object.volume); request.Number(object.high); request.Number(object.low);
        }
    }
    if (!request.good) { result.hr = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); result.error = L"The selected paths exceed the authorized transfer request limit of 1 MiB."; return result; }
    return RunAuthorizedRequest(Kind::Transfer, request, owner, cancel, std::move(callbacks));
}
ShellTransferResult DeleteWithElevatedHelper(const std::vector<std::wstring>& sources,
    bool permanent, HWND owner, std::atomic<bool>& cancel, ElevatedTransferCallbacks callbacks) {
    using namespace elevated;
    ShellTransferResult result;
    if (cancel.load()) { result.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); result.cancelled = true; return result; }
    if (sources.empty() || sources.size() > kMaxPaths) { result.hr = E_INVALIDARG; result.error = L"An authorized deletion requires 1 to 4096 selected items."; return result; }
    Writer request; request.Number(static_cast<uint32_t>(sources.size()));
    for (const auto& source : sources) {
        if (!SafePath(source)) { result.hr = E_INVALIDARG; result.error = L"A source is not a supported absolute filesystem path."; return result; }
        request.Text(source);
    }
    if (!request.good) { result.hr = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); result.error = L"The selected paths exceed the authorized request limit of 1 MiB."; return result; }
    callbacks.paused = {}; // Shell deletion does not expose reliable pause.
    callbacks.conflict = {};
    return RunAuthorizedRequest(permanent ? Kind::PermanentDelete : Kind::RecycleDelete, request, owner, cancel, std::move(callbacks));
}

ShellTransferResult CreateFolderWithElevatedHelper(const std::wstring& path, HWND owner,
    std::atomic<bool>& cancel, ElevatedTransferCallbacks callbacks) {
    using namespace elevated;
    ShellTransferResult result;
    if (cancel.load()) { result.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED); result.cancelled = true; return result; }
    if (!SafePath(path)) { result.hr = E_INVALIDARG; return result; }
    Writer request;
    request.Text(path);
    if (!request.good) { result.hr = E_INVALIDARG; return result; }
    callbacks.paused = {};
    callbacks.conflict = {};
    return RunAuthorizedRequest(Kind::CreateFolder, request, owner, cancel, std::move(callbacks));
}

ShellTransferResult EjectWithElevatedHelper(const storage::VirtualDiskIdentity& identity, HWND owner,
    std::atomic<bool>& cancel) {
    elevated::Writer request;
    if (!storage::ValidVolumeName(identity.volume_name) || identity.image_path.empty()) {
        ShellTransferResult result; result.hr = E_INVALIDARG; return result;
    }
    request.Text(identity.volume_name); request.Text(identity.image_path);
    request.Number(identity.serial); request.Number(identity.index_high); request.Number(identity.index_low);
    if (!request.good) { ShellTransferResult result; result.hr = E_INVALIDARG; return result; }
    return RunAuthorizedRequest(elevated::Kind::EjectVirtualDisk, request, owner, cancel, {});
}

void ShutdownElevatedTransferHelper() {
    auto& session = Shared(); std::lock_guard lock(session.mutex);
    if (session.pipe) elevated::WriteFrame(session.pipe.Get(), session.process.Get(), session.nonce, elevated::Kind::Shutdown, 0, {}, 1000);
    // Called after the operation worker stops. Release the helper executable
    // before an updater can replace the installation immediately after exit.
    session.pipe.Reset();
    if (session.process) WaitForSingleObject(session.process.Get(), 5000);
    session.Reset();
}
#ifdef PULSE_ELEVATED_TEST_CLIENT
DWORD ElevatedHelperProcessIdForTesting() {
    auto& session = Shared(); std::lock_guard lock(session.mutex); return GetProcessId(session.process.Get());
}
#endif
}
