#include "win_e_broker_client.h"
#include "../ipc/win_e_broker_protocol.h"
#include "../ipc/elevated_transfer_transport.h"
#include "../common/runtime_log.h"
#include "../common/path_utils.h"
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <filesystem>
#include <thread>
#include <cstring>
#include <objbase.h>
#include <new>

namespace pulse::app {
namespace protocol = win_e_broker;
namespace log = diagnostics::runtime;
using Slot = protocol::SlotState;
using Delivery = SingleInstanceCoordinator::OpenDeliveryState;
struct WinEBrokerClient::Session {
    elevated::Handle mapping, stop, child;
    protocol::Shared* shared = nullptr;
    std::atomic<bool> stopping{false}, verified{false}, finished{false};
    ~Session() { if (shared) UnmapViewOfFile(shared); }
    void Stop() noexcept {
        stopping = true;
        if (shared) InterlockedExchange(&shared->owner, static_cast<LONG>(protocol::Owner::Medium));
        if (stop) SetEvent(stop.Get());
    }
};
WinEBrokerClient::WinEBrokerClient(HWND owner) : session_(std::make_shared<Session>()) {
    auto state = session_;
    // Resource creation completes before publishing the session to the hook
    // thread. Only the potentially interactive launch runs in a detached worker.
    protocol::Nonce nonce{};
    DWORD session = 0;
    const DWORD parent = GetCurrentProcessId();
    const auto creation = protocol::CreationTime(GetCurrentProcess());
    auto fail = [&](DWORD error) {
        state->finished = true;
        log::Event("win_e_broker_start", {{"success", 0}, {"error", error}}, log::Level::Warning);
    };
    if (!creation || !ProcessIdToSessionId(parent, &session) ||
        BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        fail(ERROR_GEN_FAILURE); return;
    }
    const auto sid = elevated::UserSid(GetCurrentProcess());
    LPWSTR sid_text = nullptr;
    if (sid.empty() || !ConvertSidToStringSidW(const_cast<unsigned char*>(sid.data()), &sid_text)) {
        fail(ERROR_ACCESS_DENIED); return;
    }
    const std::wstring acl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid_text) + L")";
    LocalFree(sid_text);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        fail(GetLastError()); return;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    state->mapping.Reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
        sizeof(protocol::Shared), protocol::MappingName(parent, nonce).c_str()));
    const DWORD mapping_error = GetLastError();
    state->stop.Reset(CreateEventW(&security, TRUE, FALSE, protocol::StopEventName(parent, nonce).c_str()));
    const DWORD stop_error = GetLastError();
    LocalFree(descriptor);
    if (!state->mapping || !state->stop || mapping_error == ERROR_ALREADY_EXISTS || stop_error == ERROR_ALREADY_EXISTS) {
        fail(!state->mapping ? mapping_error : !state->stop ? stop_error : ERROR_ALREADY_EXISTS); return;
    }
    state->shared = static_cast<protocol::Shared*>(MapViewOfFile(state->mapping.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(protocol::Shared)));
    if (!state->shared) { fail(GetLastError()); return; }
    protocol::Initialize(*state->shared, parent, session, creation);
    const auto helper = std::filesystem::path(elevated::ProcessImage(GetCurrentProcess())).parent_path() / L"pulse_hotkey_broker.exe";
    const auto arguments = L"--parent " + std::to_wstring(parent) + L" --nonce " + protocol::NonceText(nonce);
    try {
        std::thread([state, helper, arguments, owner, session] {
            try {
                SHELLEXECUTEINFOW execute{sizeof(execute)};
                execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
                execute.hwnd = owner; execute.lpVerb = L"runas"; execute.lpFile = helper.c_str();
                execute.lpParameters = arguments.c_str(); execute.nShow = SW_HIDE;
                if (state->stopping) { state->finished = true; return; }
                if (!ShellExecuteExW(&execute)) {
                    log::Event("win_e_broker_start", {{"success", 0}, {"error", GetLastError()}}, log::Level::Warning);
                    state->finished = true; return;
                }
                state->child.Reset(execute.hProcess);
                DWORD child_session = 0;
                const bool verified = state->child && elevated::SameUser(GetCurrentProcess(), state->child.Get()) &&
                    elevated::IsElevated(state->child.Get()) &&
                    ProcessIdToSessionId(GetProcessId(state->child.Get()), &child_session) && child_session == session &&
                    path::EqualInsensitive(elevated::ProcessImage(state->child.Get()), helper.wstring());
                if (!verified || state->stopping) {
                    state->Stop(); state->finished = true;
                    log::Event("win_e_broker_start", {{"success", 0}, {"error", ERROR_ACCESS_DENIED}}, log::Level::Warning);
                    return;
                }
                state->verified.store(true, std::memory_order_release);
                log::Event("win_e_broker_start", {{"success", 1}, {"error", 0}, {"broker_pid", GetProcessId(state->child.Get())}});
            } catch (...) {
                state->Stop();
                state->finished = true;
                log::Event("win_e_broker_start", {{"success", 0}, {"error", ERROR_NOT_ENOUGH_MEMORY}}, log::Level::Warning);
            }
        }).detach();
    } catch (...) { fail(ERROR_NOT_ENOUGH_MEMORY); }
}
#if defined(PULSE_WIN_E_BROKER_TEST)
WinEBrokerClient::WinEBrokerClient(HANDLE child, HANDLE mapping) : session_(std::make_shared<Session>()) {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        throw std::bad_alloc();
    session_->child.Reset(duplicate);
    if (!DuplicateHandle(GetCurrentProcess(), mapping, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        throw std::bad_alloc();
    session_->mapping.Reset(duplicate);
    session_->shared = static_cast<protocol::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(protocol::Shared)));
    if (!session_->shared) throw std::bad_alloc();
    session_->verified = true;
}
#endif
WinEBrokerClient::~WinEBrokerClient() {
    auto state = session_;
    if (!state) return;
    if (state->shared) InterlockedExchange(&state->shared->owner, static_cast<LONG>(protocol::Owner::Medium));
    if (!delivery_) { state->Stop(); return; }
    const auto terminal = delivery_->load();
    if (terminal != Delivery::Queued && terminal != Delivery::Pending) {
        if (state->shared) protocol::Transition(*state->shared, Slot::Admitted,
            terminal == Delivery::Dispatched ? Slot::Dispatched : Slot::Rejected);
        state->Stop(); return;
    }
    // An admitted request may already be executing on the UI thread. Do not
    // cancel it and race a native fallback. Keep only its lifetime-safe receipt.
    const auto receipt = delivery_;
    try {
        std::thread([state, receipt] {
            while (receipt->load() == Delivery::Queued || receipt->load() == Delivery::Pending) Sleep(25);
            if (state->shared) protocol::Transition(*state->shared, Slot::Admitted,
                receipt->load() == Delivery::Dispatched ? Slot::Dispatched : Slot::Rejected);
            state->Stop();
        }).detach();
    } catch (...) {
        // Keep owner disabled. The broker also observes this process lifetime;
        // uncertainty cannot authorize a second opening.
        log::Event("win_e_broker_drain", {{"error", ERROR_NOT_ENOUGH_MEMORY}}, log::Level::Error);
    }
}
bool WinEBrokerClient::Active() const noexcept {
    const auto& s = session_;
    return s && s->shared && s->verified.load(std::memory_order_acquire) && !s->finished &&
        protocol::LoadOwner(*s->shared) == protocol::Owner::Broker;
}
unsigned WinEBrokerClient::HeldFlags() const noexcept {
    return session_ && session_->shared ? static_cast<unsigned>(InterlockedCompareExchange(&session_->shared->chord_flags, 0, 0)) : 0;
}
void WinEBrokerClient::Poll(SingleInstanceCoordinator& coordinator, HWND host, UINT wake, bool can_activate) {
    auto& s = *session_;
    if (!s.shared) return;
    InterlockedExchange64(&s.shared->parent_heartbeat, static_cast<LONG64>(GetTickCount64()));
    if (!s.verified.load(std::memory_order_acquire)) return;
    const DWORD child_wait = WaitForSingleObject(s.child.Get(), 0);
    if (child_wait != WAIT_TIMEOUT && child_wait != WAIT_OBJECT_0) return;
    const bool child_exited = child_wait == WAIT_OBJECT_0;
    if (child_exited && !s.finished.exchange(true)) {
        InterlockedExchange(&s.shared->owner, static_cast<LONG>(protocol::Owner::Medium));
        log::Event("win_e_broker_exit", {{"broker_pid", GetProcessId(s.child.Get())}, {"held", HeldFlags()}});
    }
    // A committed offer belongs to the consumed key even if its producer dies
    // before this poll. Claim it once; never replay an admitted or terminal slot.
    if (!child_exited) {
        if (s.finished) return;
        const auto ready = static_cast<DWORD>(InterlockedCompareExchange(&s.shared->ready_pid, 0, 0));
        if (ready != GetProcessId(s.child.Get())) return;
        if (can_activate && protocol::LoadOwner(*s.shared) == protocol::Owner::Medium) {
            InterlockedExchange(&s.shared->owner, static_cast<LONG>(protocol::Owner::Broker));
            log::Event("win_e_broker_owner", {{"owner", 1}, {"broker_pid", ready}});
        }
    }
    if (delivery_) {
        const auto status = delivery_->load();
        if (status != Delivery::Queued && status != Delivery::Pending) {
            const bool dispatched = status == Delivery::Dispatched;
            protocol::Transition(*s.shared, Slot::Admitted, dispatched ? Slot::Dispatched : Slot::Rejected);
            log::Event("win_e_broker_delivery", {{"sequence", sequence_}, {"dispatched", dispatched},
                {"id_high", request_high_}, {"id_low", request_low_}});
            delivery_.reset();
        }
        return;
    }
    if (!protocol::Transition(*s.shared, Slot::Offered, Slot::Claimed)) return;
    sequence_ = protocol::LoadSequence(*s.shared);
    try {
        SingleInstanceCoordinator::OpenRequest request;
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) throw std::bad_alloc();
        std::memcpy(request.id.data(), &id, sizeof(id));
        std::memcpy(&request_high_, request.id.data(), sizeof(request_high_));
        std::memcpy(&request_low_, request.id.data() + sizeof(request_high_), sizeof(request_low_));
        request.path = L"pulse:win-e";
        auto receipt = std::make_shared<std::atomic<Delivery>>(Delivery::Pending);
        request.delivery = receipt;
        const bool accepted = coordinator.TrySubmitKeyboardOpen(request, GetTickCount64(), host, wake);
        protocol::Transition(*s.shared, Slot::Claimed, accepted ? Slot::Admitted : Slot::Rejected);
        if (accepted) delivery_ = std::move(receipt);
        log::Event("win_e_broker_request", {{"sequence", sequence_}, {"accepted", accepted},
            {"id_high", request_high_}, {"id_low", request_low_}});
    } catch (...) { protocol::Transition(*s.shared, Slot::Claimed, Slot::Rejected); }
}
}
