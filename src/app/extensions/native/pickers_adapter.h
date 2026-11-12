#pragma once
#include "../../../../sdk/native/include/pulse/pickers.h"
#include <windows.h>
#include <d2d1.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::extensions::native {

class DiagnosticsSink;

enum class PickerRequestKind : uint32_t {
    OpenFiles = 0,
    SaveFile = 1,
    PickFolders = 2,
    QuickPick = 3,
};

struct PickerFilterSpec {
    std::wstring label;
    std::wstring pattern;
};

struct QuickPickItemSpec {
    std::string id;
    std::wstring title;
    std::wstring detail;
    std::wstring glyph;
    std::wstring shortcut;
    bool selected = false;
    bool disabled = false;
};

struct SharedEventHandle {
    HANDLE handle = nullptr;
    explicit SharedEventHandle(HANDLE h = nullptr) noexcept : handle(h) {}
    ~SharedEventHandle() { if (handle) CloseHandle(handle); }
    SharedEventHandle(const SharedEventHandle&) = delete;
    SharedEventHandle& operator=(const SharedEventHandle&) = delete;
};

struct PickerLaunchSpec {
    uint64_t request_id = 0;
    PickerRequestKind kind = PickerRequestKind::OpenFiles;
    uint32_t flags = 0;
    uint32_t default_filter_index = 0;
    std::wstring title;
    std::wstring ok_label;
    std::wstring initial_path;
    std::wstring default_name;
    std::wstring default_extension;
    std::vector<PickerFilterSpec> filters;
    std::wstring placeholder;
    std::wstring initial_query;
    std::vector<QuickPickItemSpec> items;
    std::shared_ptr<SharedEventHandle> cancel_event;
};

struct PickerBackendCompletion {
    uint32_t outcome = PULSE_PICKER_FAILED;
    uint32_t filter_index = 0;
    std::vector<std::string> paths;
    std::string selected_id;
    std::string error;
};

struct PickersBackend {
    std::function<bool()> valid_owner;
    std::function<bool(PickerLaunchSpec, std::function<void(PickerBackendCompletion)>)> launch;
    std::function<void(uint64_t request_id)> cancel;
};

class PickersAdapter {
public:
    PickersAdapter(PickersBackend backend, std::function<bool()> alive,
                   std::shared_ptr<DiagnosticsSink> diagnostics = nullptr);
    PickersAdapter(HWND owner, std::function<HWND()> active_modal_owner, std::function<bool()> alive,
                   std::shared_ptr<DiagnosticsSink> diagnostics = nullptr);
    ~PickersAdapter();
    PickersAdapter(const PickersAdapter&) = delete;
    PickersAdapter& operator=(const PickersAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native
