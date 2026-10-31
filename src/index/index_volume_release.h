#pragma once
#include "index_protocol.h"
#include <algorithm>

namespace pulse::index {
// Keep this object alive through the actual eject operation. Closing its pipes
// revokes only these connections' leases; destruction never waits for the service.
class VolumeReleaseLease {
public:
    ~VolumeReleaseLease() { Release(); }
    VolumeReleaseLease() = default;
    VolumeReleaseLease(const VolumeReleaseLease&) = delete;
    VolumeReleaseLease& operator=(const VolumeReleaseLease&) = delete;
    void Release() {
        for (HANDLE pipe : pipes_) CloseHandle(pipe);
        pipes_.clear();
    }
    // NOT_SUPPORTED means at least one old host has no lease capability. Leases
    // from other supported hosts remain held; the caller may attempt OS-checked eject.
    DWORD Acquire(const std::wstring& pipe_name, const std::vector<std::wstring>& roots, DWORD timeout_ms = 10000) {
        std::vector<std::wstring> names{kPipeName};
        if (!pipe_name.empty() && pipe_name != kPipeName) names.push_back(pipe_name);
        return AcquireNames(names, roots, timeout_ms);
    }
private:
    friend struct VolumeReleaseTestAccess;
    DWORD AcquireNames(const std::vector<std::wstring>& names, const std::vector<std::wstring>& roots, DWORD timeout_ms) {
        Release();
        if (roots.empty() || roots.size() > 64) return ERROR_INVALID_PARAMETER;
        const auto deadline = GetTickCount64() + timeout_ms;
        bool unsupported = false;
        for (const auto& name : names) {
            HANDLE pipe = INVALID_HANDLE_VALUE;
            for (;;) {
                pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
                if (pipe != INVALID_HANDLE_VALUE) break;
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND) {
                    if (name == kPipeName && ServiceRunning()) { Release(); return ERROR_NOT_READY; }
                    break;
                }
                if (error != ERROR_PIPE_BUSY || GetTickCount64() >= deadline) { Release(); return error; }
                WaitNamedPipeW(name.c_str(), 25);
            }
            if (pipe == INVALID_HANDLE_VALUE) continue;
            pipes_.push_back(pipe);
            ipc::PayloadWriter payload; payload.PutU32(static_cast<uint32_t>(roots.size()));
            for (const auto& root : roots) payload.PutString(root);
            auto header = MakeIndexHdr(REQ_IDX_STATUS, 1, 0);
            DWORD error = Transfer(pipe, &header, sizeof(header), true, deadline);
            bool requested_pause = false;
            while (!error) {
                error = Transfer(pipe, &header, sizeof(header), false, deadline);
                if (error) break;
                if (header.magic != kIndexMagic || header.payload_size > kIndexMaxPayload) { error = ERROR_INVALID_DATA; break; }
                std::vector<uint8_t> bytes(header.payload_size);
                error = Transfer(pipe, bytes.data(), header.payload_size, false, deadline);
                if (error) break;
                if (header.type == RSP_IDX_STATUS) {
                    if (requested_pause) continue;
                    if (!SupportsPause(bytes)) {
                        unsupported = true;
                        CloseHandle(pipe); pipes_.pop_back();
                        break; // Old service is optional; actual eject still checks all volume locks.
                    }
                    header = MakeIndexHdr(REQ_IDX_VOLUME_PAUSE, 2, static_cast<uint32_t>(payload.data().size()));
                    error = Transfer(pipe, &header, sizeof(header), true, deadline);
                    if (!error) error = Transfer(pipe, const_cast<uint8_t*>(payload.data().data()), header.payload_size, true, deadline);
                    requested_pause = true;
                    continue;
                }
                if (header.type == RSP_IDX_VOLUMES) continue;
                ipc::PayloadReader reader(bytes.data(), bytes.size()); uint32_t status = 0;
                if (!requested_pause || header.type != RSP_IDX_VOLUME_PAUSE || header.request_id != 2 || !reader.GetU32(status) || reader.remaining())
                    error = ERROR_INVALID_DATA;
                else error = status;
                break;
            }
            if (error) { Release(); return error; }
        }
        return unsupported ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
    }
    static bool SupportsPause(const std::vector<uint8_t>& bytes) {
        ipc::PayloadReader reader(bytes.data(), bytes.size());
        uint32_t ready = 0, count = 0, pinyin = 0, version = 0, capability = 0;
        uint64_t revision = 0; std::wstring status;
        return reader.GetU32(ready) && reader.GetU32(count) && reader.GetString(status) &&
            reader.GetU32(pinyin) && reader.GetU64(revision) && reader.GetU32(version) && version == 1 &&
            reader.GetU32(capability) && capability == kVolumePauseCapability;
    }
    static bool ServiceRunning() {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) return true; // Unknown service state cannot certify absence.
        SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
        if (!service) { const auto error = GetLastError(); CloseServiceHandle(manager); return error != ERROR_SERVICE_DOES_NOT_EXIST; }
        SERVICE_STATUS_PROCESS status{}; DWORD bytes = 0;
        const bool active = !QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) ||
            status.dwCurrentState != SERVICE_STOPPED;
        CloseServiceHandle(service); CloseServiceHandle(manager); return active;
    }
    static DWORD Transfer(HANDLE pipe, void* data, DWORD size, bool write, ULONGLONG deadline) {
        if (!size) return ERROR_SUCCESS;
        OVERLAPPED operation{}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return GetLastError();
        DWORD done = 0;
        const BOOL ok = write ? WriteFile(pipe, data, size, &done, &operation) : ReadFile(pipe, data, size, &done, &operation);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error == ERROR_IO_PENDING) {
            const auto now = GetTickCount64();
            if (WaitForSingleObject(operation.hEvent, static_cast<DWORD>(deadline > now ? deadline - now : 0)) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &done, TRUE); error = ERROR_TIMEOUT;
            } else if (!GetOverlappedResult(pipe, &operation, &done, FALSE)) error = GetLastError();
            else error = ERROR_SUCCESS;
        }
        CloseHandle(operation.hEvent);
        return error ? error : (done == size ? ERROR_SUCCESS : ERROR_INVALID_DATA);
    }
    std::vector<HANDLE> pipes_;
};
}
