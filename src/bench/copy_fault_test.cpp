// Test-executable-only CopyFile2 import replacement. Never linked into Pulse.
#include "../ops/transfer_engine.h"
#include "../ops/owned_transfer_output.h"
#include <objbase.h>
#include <filesystem>
#include <cstdio>
#include <array>
#include <vector>
#include <cstring>
using namespace pulse::ops;
namespace fs = std::filesystem;
namespace {
using CopyApi = HRESULT (WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);
CopyApi real_copy = nullptr;
enum class Fault { Retry, DiskFull, AccessDenied, Replaced, CleanupBlocked, RetryAfterStop };
Fault fault = Fault::Retry;
std::wstring isolated_prefix;
unsigned calls = 0;
std::array<DWORD, 2> call_flags{};
bool partial_seen = false;
bool output_retained = false;
DWORD output_probe_error = ERROR_SUCCESS;
HRESULT partial_api_result = S_OK;
DWORD setup_error = ERROR_SUCCESS;
HANDLE held_output = INVALID_HANDLE_VALUE;
int failures = 0;
void Check(bool pass, const char* name) { printf("[%s] %s\n", pass ? "PASS" : "FAIL", name); if (!pass) ++failures; }
bool WriteBytes(const std::wstring& path, const void* data, DWORD count) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0; const bool ok = WriteFile(file, data, count, &wrote, nullptr) && wrote == count;
    CloseHandle(file); return ok;
}
constexpr char replacement[] = "external writer sentinel";
struct CallbackProxy { PCOPYFILE2_PROGRESS_ROUTINE callback; void* context; };
COPYFILE2_MESSAGE_ACTION CALLBACK InterruptAfterPartial(const COPYFILE2_MESSAGE* message, PVOID opaque) {
    const auto& proxy = *static_cast<CallbackProxy*>(opaque);
    const auto action = proxy.callback ? proxy.callback(message, proxy.context) : COPYFILE2_PROGRESS_CONTINUE;
    if (action != COPYFILE2_PROGRESS_CONTINUE) return action;
    if (message->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED) {
        const auto& chunk = message->Info.ChunkFinished;
        if (chunk.uliTotalBytesTransferred.QuadPart > 0 && chunk.uliTotalBytesTransferred.QuadPart < chunk.uliTotalFileSize.QuadPart) {
            partial_seen = true;
            // PAUSE preserves resumable partial output; STOP exercises API cleanup.
            return fault == Fault::RetryAfterStop ? COPYFILE2_PROGRESS_STOP : COPYFILE2_PROGRESS_PAUSE;
        }
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}
HRESULT WINAPI InjectedCopy(PCWSTR source, PCWSTR destination, COPYFILE2_EXTENDED_PARAMETERS* parameters) {
    if (!parameters || !std::wstring(destination).starts_with(isolated_prefix)) return real_copy(source, destination, parameters);
    const unsigned call = calls++;
    if (call < call_flags.size()) call_flags[call] = parameters->dwCopyFlags;
    if (call != 0) return real_copy(source, destination, parameters);
    auto actual = *parameters;
    CallbackProxy proxy{actual.pProgressRoutine, actual.pvCallbackContext};
    actual.pProgressRoutine = InterruptAfterPartial; actual.pvCallbackContext = &proxy;
    // Inject unsupported NB after real cached partial I/O, independently of device support.
    actual.dwCopyFlags &= ~COPY_FILE_NO_BUFFERING;
    partial_api_result = real_copy(source, destination, &actual);
    output_retained = GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES;
    output_probe_error = output_retained ? ERROR_SUCCESS : GetLastError();
    const bool absent = !output_retained &&
        (output_probe_error == ERROR_FILE_NOT_FOUND || output_probe_error == ERROR_PATH_NOT_FOUND);
    const bool expected_state = fault == Fault::RetryAfterStop ? absent : output_retained;
    if (!partial_seen || SUCCEEDED(partial_api_result) || !expected_state) {
        setup_error = ERROR_INVALID_DATA; return E_UNEXPECTED;
    }
    if (fault == Fault::Replaced) {
        const std::wstring previous = std::wstring(destination) + L".original";
        if (!MoveFileExW(destination, previous.c_str(), 0) || !WriteBytes(destination, replacement, sizeof(replacement)))
            setup_error = GetLastError();
    } else if (fault == Fault::CleanupBlocked) {
        held_output = CreateFileW(destination, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (held_output == INVALID_HANDLE_VALUE) setup_error = GetLastError();
    }
    if (setup_error) return E_UNEXPECTED;
    if (fault == Fault::DiskFull) return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    if (fault == Fault::AccessDenied) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
}
}
// MSVC x64 engine references this imported-function pointer. The real export is
// resolved dynamically above, so this symbol cannot recurse through its own hook.
extern "C" { CopyApi __imp_CopyFile2 = InjectedCopy; }
int wmain() {
    real_copy = reinterpret_cast<CopyApi>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CopyFile2"));
    if (!real_copy) { Check(false, "resolve real CopyFile2"); return 1; }
    GUID guid{}; wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&guid))) return 1;
    StringFromGUID2(guid, name, 40);
    const auto base = fs::absolute(L"bench_data").lexically_normal();
    const auto root = base / (std::wstring(L"copy_fault_") + name);
    std::error_code error;
    const bool created = fs::create_directories(root, error);
    Check(created && !error, "create exclusive GUID fault fixture");
    if (!created || error) return 1;
    isolated_prefix = root.wstring() + L"\\";
    const auto source = (root / L"source.bin").wstring(), reference = (root / L"reference.bin").wstring();
    constexpr uint64_t file_bytes = 64ull << 20;
    std::vector<unsigned char> data(static_cast<size_t>(file_bytes));
    uint32_t state = 0x91a72e4u;
    for (auto& byte : data) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; byte = static_cast<unsigned char>(state); }
    Check(WriteBytes(source, data.data(), static_cast<DWORD>(data.size())) && WriteBytes(reference, data.data(), static_cast<DWORD>(data.size())), "real non-sparse source and independent reference");
    if (failures) return 1;
    data.clear(); data.shrink_to_fit();
    SHA256VerificationWorker verifier;
    std::atomic<bool> never_cancel{false};
    unsigned scenario = 0;
    for (const Fault mode : {Fault::Retry, Fault::DiskFull, Fault::AccessDenied, Fault::Replaced, Fault::CleanupBlocked, Fault::RetryAfterStop}) {
        fault = mode; calls = 0; call_flags = {}; partial_seen = false; setup_error = 0;
        output_retained = false; output_probe_error = ERROR_SUCCESS; partial_api_result = S_OK;
        const auto staged = (root / (L"stage" + std::to_wstring(scenario))).wstring();
        const auto final = (root / (L"final" + std::to_wstring(scenario++))).wstring();
        std::atomic<bool> cancel{false}, pause{false}; std::mutex mutex; std::condition_variable cv;
        std::vector<SmallCopyResult> results;
        {
            SmallCopyPool pool(1, cancel, pause, mutex, cv);
            SmallCopyJob job{0, source, staged, FILE_ATTRIBUTE_NORMAL, false, file_bytes};
            job.verify = true; job.staged = true;
            job.unbuffered = mode == Fault::Retry || mode == Fault::RetryAfterStop || mode == Fault::Replaced || mode == Fault::CleanupBlocked;
            pool.Submit(std::move(job));
            const auto deadline = GetTickCount64() + 30000;
            while (pool.Outstanding() && GetTickCount64() < deadline) {
                auto batch = pool.Take(true); results.insert(results.end(), batch.begin(), batch.end());
            }
            Check(pool.Outstanding() == 0, "fault case terminates within bound");
            if (pool.Outstanding()) pool.Stop();
        }
        printf("[SIMULATED] case=%u calls=%u partial=%d action=%s api_hr=0x%08lx retained=%d probe_error=%lu setup_error=%lu\n",
            scenario, calls, partial_seen ? 1 : 0, mode == Fault::RetryAfterStop ? "STOP" : "PAUSE",
            static_cast<unsigned long>(partial_api_result), output_retained ? 1 : 0, output_probe_error, setup_error);
        if (mode == Fault::RetryAfterStop) {
            Check(partial_seen && FAILED(partial_api_result) && !output_retained &&
                (output_probe_error == ERROR_FILE_NOT_FOUND || output_probe_error == ERROR_PATH_NOT_FOUND),
                "STOP automatically removes partial output before INVALID_PARAMETER fallback");
        } else {
            Check(partial_seen && FAILED(partial_api_result) && output_retained && !output_probe_error,
                "PAUSE retains actual partial output before fault injection and cleanup");
        }
        Check(results.size() == 1 && partial_seen && !setup_error, "real callback reached partial write before simulated error");
        if (results.size() != 1) { if (held_output != INVALID_HANDLE_VALUE) { CloseHandle(held_output); held_output = INVALID_HANDLE_VALUE; } continue; }
        const auto& result = results.front();
        if (mode == Fault::Retry || mode == Fault::RetryAfterStop) {
            Check(calls == 2 && result.buffered_retry && result.buffering_fallback_error == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) &&
                (call_flags[0] & COPY_FILE_NO_BUFFERING) && call_flags[1] == (call_flags[0] & ~COPY_FILE_NO_BUFFERING) &&
                (call_flags[1] & COPY_FILE_COPY_SYMLINK), "NB fallback removes only NO_BUFFERING and retains symlink flag");
            Check(SUCCEEDED(result.hr) && result.copy_completed && result.bytes == file_bytes && result.verification &&
                result.verification->Success() && result.owned_output, "retry completes actual copy and SHA256 verification");
            DWORD commit_error = 0;
            const bool committed = SUCCEEDED(result.hr) && result.owned_output && CommitVerifiedOutput(staged, final, *result.owned_output, commit_error);
            Check(committed && !commit_error && verifier.Verify(reference, final, never_cancel, {}).Success(), "only successful retry commits complete verified output");
        } else {
            Check(FAILED(result.hr) && !result.copy_completed && calls == 1 && !result.buffered_retry &&
                GetFileAttributesW(final.c_str()) == INVALID_FILE_ATTRIBUTES, "fault does not retry or publish final output");
            if (mode == Fault::DiskFull || mode == Fault::AccessDenied) {
                const DWORD expected = mode == Fault::DiskFull ? ERROR_DISK_FULL : ERROR_ACCESS_DENIED;
                Check(result.hr == HRESULT_FROM_WIN32(expected) && result.owned_output &&
                    GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES, "simulated I/O failure preserves HRESULT and cleans owned temporary output");
            } else if (mode == Fault::Replaced) {
                std::array<char, sizeof(replacement)> bytes{};
                HANDLE file = CreateFileW(staged.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                DWORD read = 0;
                const bool preserved = file != INVALID_HANDLE_VALUE && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                    read == bytes.size() && memcmp(bytes.data(), replacement, bytes.size()) == 0;
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                Check(result.buffering_cleanup_error == ERROR_FILE_INVALID && preserved, "replacement identity blocks cleanup and retry; external content survives");
            } else {
                Check(result.buffering_cleanup_error != 0 && held_output != INVALID_HANDLE_VALUE &&
                    GetFileAttributesW(staged.c_str()) != INVALID_FILE_ATTRIBUTES, "blocked cleanup preserves temporary and prevents retry");
            }
        }
        if (held_output != INVALID_HANDLE_VALUE) { CloseHandle(held_output); held_output = INVALID_HANDLE_VALUE; }
        Check(verifier.Verify(source, reference, never_cancel, {}).Success(), "source remains intact SHA256 after simulated fault");
    }
    if (!failures && root.parent_path() == base && root.filename().wstring().starts_with(L"copy_fault_")) {
        fs::remove_all(root, error); Check(!error, "isolated fault fixture cleanup");
    } else printf("[DIAG] retained fixture=%ls\n", root.c_str());
    return failures ? 1 : 0;
}
