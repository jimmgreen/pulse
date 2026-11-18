#include "../index/index_engine.h"
#include "../index/index_volume_release.h"
#include <filesystem>
#include <iostream>
#include <chrono>
namespace ipc = pulse::ipc;
namespace pulse::index {
struct VolumeReleaseTestAccess {
    static DWORD Acquire(VolumeReleaseLease& lease, const std::wstring& name, DWORD timeout) {
        return lease.AcquireNames({name}, {L"Z:\\"}, timeout);
    }
};
struct EngineTestAccess {
    static void Start(Engine& engine, const std::vector<std::wstring>& paths) { engine.StartWalkWatches(paths); }
    static void Apply(Engine& engine) { engine.ApplyVolumePauses(); }
    static HANDLE Watch(Engine& engine, size_t index) { return engine.watches_.at(index).dir; }
    static bool Paused(Engine& engine, const std::wstring& id) { return engine.VolumePaused(id); }
    static void Stop(Engine& engine) { engine.StopWalkWatches(); }
};
}
int main() {
    using namespace pulse::index;
    using namespace std::chrono_literals;
    const auto base = std::filesystem::temp_directory_path() / (L"pulse-index-pause-" + std::to_wstring(GetCurrentProcessId()));
    const auto target = base / L"target", other = base / L"other";
    std::filesystem::create_directories(target); std::filesystem::create_directories(other);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; if (!ok) ++failures; };
    const auto wire = [&](int mode) {
        const bool reply = mode == 1;
        const auto name = L"\\\\.\\pipe\\PulseIndex.LeaseTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(mode);
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, nullptr);
        std::atomic<bool> disconnected{false};
        std::thread server([&] {
            if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
            ipc::MsgHeader header{};
            if (!ipc::PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header))) return;
            std::vector<uint8_t> payload(header.payload_size);
            if (!ipc::PipeRead(pipe, payload.data(), static_cast<DWORD>(payload.size()))) return;
            if (mode != 0) {
                ipc::PayloadWriter status;
                status.PutU32(1); status.PutU32(0); status.PutString(L"fixture"); status.PutU32(0); status.PutU64(1); status.PutU32(1);
                if (reply) status.PutU32(kVolumePauseCapability);
                auto response = MakeIndexHdr(RSP_IDX_STATUS, header.request_id, static_cast<uint32_t>(status.data().size()));
                ipc::PipeWrite(pipe, reinterpret_cast<uint8_t*>(&response), sizeof(response));
                ipc::PipeWrite(pipe, status.data().data(), static_cast<DWORD>(status.data().size()));
            }
            if (reply) {
                if (!ipc::PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header))) return;
                payload.resize(header.payload_size);
                if (!ipc::PipeRead(pipe, payload.data(), static_cast<DWORD>(payload.size()))) return;
                auto response = MakeIndexHdr(RSP_IDX_VOLUME_PAUSE, header.request_id, sizeof(DWORD));
                DWORD status = ERROR_SUCCESS;
                ipc::PipeWrite(pipe, reinterpret_cast<uint8_t*>(&response), sizeof(response));
                ipc::PipeWrite(pipe, reinterpret_cast<uint8_t*>(&status), sizeof(status));
            }
            uint8_t byte = 0; DWORD received = 0;
            disconnected = !ReadFile(pipe, &byte, 1, &received, nullptr);
        });
        VolumeReleaseLease lease;
        const auto result = VolumeReleaseTestAccess::Acquire(lease, name, mode != 0 ? 5000 : 100);
        if (reply) {
            check(result == ERROR_SUCCESS && !disconnected, "IPC lease stays connected after acquire");
            lease.Release();
        } else if (mode == 2) check(result == ERROR_NOT_SUPPORTED, "legacy status immediately reports unsupported lease");
        else check(result == ERROR_TIMEOUT, "unresponsive service times out safely");
        server.join(); CloseHandle(pipe);
        check(disconnected, reply ? "Release disconnects lease pipe" : "failed negotiation disconnects pipe without pause request");
    };
    wire(1); wire(0); wire(2);
    VolumeReleaseLease missing;
    check(VolumeReleaseTestAccess::Acquire(missing, L"\\\\.\\pipe\\PulseIndex.LeaseTest.Missing." + std::to_wstring(GetCurrentProcessId()), 100) == ERROR_SUCCESS,
        "absent helper pipe requires no lease");
    Engine engine;
    EngineTestAccess::Start(engine, {target.wstring(), other.wstring()});
    const HANDLE unrelated = EngineTestAccess::Watch(engine, 1);
    check(EngineTestAccess::Watch(engine, 0) != INVALID_HANDLE_VALUE && unrelated != INVALID_HANDLE_VALUE, "isolated watches open");
    auto lease = engine.PauseVolumes({L"test-volume"}, {target.wstring()});
    check(lease->ready.wait_for(0ms) != std::future_status::ready, "no ack before worker safe point");
    EngineTestAccess::Apply(engine);
    bool completed = lease->ready.wait_for(0ms) == std::future_status::ready;
    try { if (completed) lease->ready.get(); } catch (...) { completed = false; }
    check(completed && EngineTestAccess::Watch(engine, 0) == INVALID_HANDLE_VALUE, "ack follows target handle close");
    check(EngineTestAccess::Watch(engine, 1) == unrelated, "unrelated watch remains open");
    auto second = engine.PauseVolumes({L"test-volume"}, {target.wstring()});
    EngineTestAccess::Apply(engine);
    engine.ResumeVolumes(lease); EngineTestAccess::Apply(engine);
    check(EngineTestAccess::Paused(engine, L"test-volume") && EngineTestAccess::Watch(engine, 0) == INVALID_HANDLE_VALUE,
        "one client cannot revoke another lease");
    engine.ResumeVolumes(second); EngineTestAccess::Apply(engine);
    check(!EngineTestAccess::Paused(engine, L"test-volume") && EngineTestAccess::Watch(engine, 0) != INVALID_HANDLE_VALUE,
        "last disconnect restores target watch");
    auto cancelled = engine.PauseVolumes({L"test-volume"}, {target.wstring()});
    const HANDLE restored = EngineTestAccess::Watch(engine, 0);
    engine.ResumeVolumes(cancelled); EngineTestAccess::Apply(engine);
    check(EngineTestAccess::Watch(engine, 0) == restored, "cancelled queued request does not close resources");
    EngineTestAccess::Stop(engine);
    std::error_code error; std::filesystem::remove_all(base, error);
    check(!error && !std::filesystem::exists(base), "isolated fixture cleanup");
    return failures ? 1 : 0;
}
