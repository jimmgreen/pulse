// preview_host_client.h - test-side client of Pulse.Preview.exe: starts the
// host next to the running executable and speaks the preview pipe protocol
// (shared by pulse_preview_test's self test, --probe and --coverage).
#pragma once
#include "../ipc/preview_protocol.h"
#include "../common/decode_memory_budget.h"
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace pulse_test {

struct Result {
    pulse::ipc::PreviewResponse response{};
    std::wstring text;
    std::wstring error;
    std::vector<std::pair<std::wstring, std::wstring>> properties;
    // Bitmaps are read back so a preview that renders nothing can be told apart
    // from one that renders a blank page.
    std::vector<unsigned char> pixels;
};

class Host {
public:
    void SetBudgeted(bool enabled) { budgeted_ = enabled; }
    void SetBudgetGrantHook(void (*hook)(HANDLE)) { budget_grant_hook_ = hook; }
    DWORD ProcessId() const { return process_.dwProcessId; }
    bool Start() {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (!slash) return false;
        *(slash + 1) = 0;
        const DWORD owner = GetCurrentProcessId();
        std::wstring command = L"\"" + std::wstring(exe) + L"Pulse.Preview.exe\" " +
                               std::to_wstring(owner);
        STARTUPINFOW si{sizeof(si)};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &process_)) return false;
        const std::wstring pipeName = pulse::ipc::PreviewPipeName(owner);
        const ULONGLONG deadline = GetTickCount64() + 3000;
        do {
            pipe_ = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) return true;
            Sleep(10);
        } while (GetTickCount64() < deadline);
        Stop();
        return false;
    }

    void Stop() {
        if (pipe_ != INVALID_HANDLE_VALUE) { CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
        if (process_.hProcess) {
            if (WaitForSingleObject(process_.hProcess, 500) == WAIT_TIMEOUT)
                TerminateProcess(process_.hProcess, 0);
            CloseHandle(process_.hProcess);
            CloseHandle(process_.hThread);
            process_ = {};
        }
    }
    ~Host() { Stop(); }

    bool Request(const std::wstring& path, Result& result, DWORD attrs_override = MAXDWORD,
                 uint32_t pixel_size = pulse::ipc::kPreviewDefaultPixelSize,
                 pulse::ipc::PreviewRequestKind kind = pulse::ipc::PreviewRequestKind::Content,
                 uint32_t flags = 0, uint32_t frame_index = 0) {
        pulse::ipc::PreviewRequest request{};
        request.flags = flags;
        pulse::preview::DecodeMemoryBudget::Lease lease;
        if (budgeted_) { request.magic = pulse::ipc::kPreviewBudgetRequestMagic; request.flags |= pulse::ipc::kPreviewRequestFlagGrid; }
        request.frame_index = frame_index;
        request.request_id = next_++;
        request.generation = request.request_id;
        request.kind = kind;
        request.pixel_size = pixel_size;
        request.attrs = attrs_override == MAXDWORD ? GetFileAttributesW(path.c_str())
                                                   : attrs_override;
        request.path_chars = static_cast<uint32_t>(path.size());
        if (!pulse::ipc::WriteAll(pipe_, &request, sizeof(request)) ||
            !pulse::ipc::WriteAll(pipe_, path.data(), request.path_chars * sizeof(wchar_t)) ||
            !pulse::ipc::ReadAll(pipe_, &result.response, sizeof(result.response))) return false;
        unsigned proposals = 0;
        while (result.response.magic == pulse::ipc::kPreviewBudgetProposalMagic) {
            if (!budgeted_ || ++proposals > 16 || result.response.request_id != request.request_id ||
                result.response.generation != request.generation) return false;
            const uint64_t bytes = uint64_t(result.response.width) | (uint64_t(result.response.height) << 32);
            const bool acquired = pulse::preview::SharedDecodeMemoryBudget().Reserve(lease, bytes, (result.response.flags & 1) != 0);
            const uint32_t grant = acquired ? pulse::ipc::kPreviewBudgetGrant : 0;
            if (!pulse::ipc::WriteAll(pipe_, &grant, sizeof(grant))) return false;
            if (acquired && budget_grant_hook_) budget_grant_hook_(process_.hProcess);
            if (!pulse::ipc::ReadAll(pipe_, &result.response, sizeof(result.response))) return false;
        }
        if (result.response.magic != pulse::ipc::kPreviewMagic ||
            result.response.request_id != request.request_id) return false;
        std::wstring mapping(result.response.mapping_chars, L'\0');
        if (!mapping.empty() && !pulse::ipc::ReadAll(pipe_, mapping.data(),
            result.response.mapping_chars * sizeof(wchar_t))) return false;
        result.text.assign(result.response.text_chars, L'\0');
        if (!result.text.empty() && !pulse::ipc::ReadAll(pipe_, result.text.data(),
            result.response.text_chars * sizeof(wchar_t))) return false;
        result.error.assign(result.response.error_chars, L'\0');
        if (!result.error.empty() && !pulse::ipc::ReadAll(pipe_, result.error.data(),
            result.response.error_chars * sizeof(wchar_t))) return false;
        result.properties.clear();
        if (result.response.property_count > 6) return false;
        auto read_string = [&](std::wstring& value) {
            uint32_t count = 0;
            if (!pulse::ipc::ReadAll(pipe_, &count, sizeof(count)) || count > 32768) return false;
            value.assign(count, L'\0');
            return count == 0 || pulse::ipc::ReadAll(pipe_, value.data(), count * sizeof(wchar_t));
        };
        for (uint32_t i = 0; i < result.response.property_count; ++i) {
            std::wstring label, value;
            if (!read_string(label) || !read_string(value)) return false;
            result.properties.emplace_back(std::move(label), std::move(value));
        }
        if (!mapping.empty()) {
            // Read the raster before acknowledging, so tests can tell a preview
            // that drew nothing from one that drew a blank page.
            HANDLE file_map = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping.c_str());
            if (file_map) {
                const size_t bytes = static_cast<size_t>(result.response.stride) *
                                     result.response.height;
                if (auto* view = MapViewOfFile(file_map, FILE_MAP_READ, 0, 0, bytes)) {
                    result.pixels.assign(static_cast<const unsigned char*>(view),
                                         static_cast<const unsigned char*>(view) + bytes);
                    UnmapViewOfFile(view);
                }
                CloseHandle(file_map);
            }
            const unsigned char ack = 1;
            if (!pulse::ipc::WriteAll(pipe_, &ack, 1)) return false;
        }
        return true;
    }
private:
    bool budgeted_ = false;
    void (*budget_grant_hook_)(HANDLE) = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process_{};
    uint32_t next_ = 1;
};

} // namespace pulse_test
