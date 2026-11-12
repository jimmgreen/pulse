#pragma once
#include "../../../../sdk/native/include/pulse/workbench.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::extensions::native {

class DiagnosticsSink;

struct WorkbenchActionSpec {
    std::string id;
    std::wstring label;
    uint64_t token = 0;
};

struct SnackPresentationSpec {
    uint64_t notification_id = 0;
    uint32_t flags = 0;
    uint32_t kind = PULSE_NOTICE_INFO;
    uint32_t duration_ms = 5000;
    std::wstring text;
    std::wstring detail;
    std::vector<WorkbenchActionSpec> actions;
};

struct NoticePresentationSpec {
    uint64_t notification_id = 0;
    uint32_t kind = PULSE_NOTICE_INFO;
    std::wstring title;
    std::wstring message;
    std::vector<WorkbenchActionSpec> actions;
};

struct WorkbenchBackend {
    std::function<bool()> valid_owner;
    std::function<bool(SnackPresentationSpec)> show_snack;
    std::function<bool(NoticePresentationSpec)> show_notice;
    std::function<PulseStatus(const std::wstring& path, uint32_t mode)> reveal_path;
    std::function<PulseStatus(const std::wstring& left_path, const std::wstring& right_path)> show_text_diff;
    std::function<PulseStatus(const std::wstring& text)> set_clipboard_text;
};

class WorkbenchAdapter {
public:
    WorkbenchAdapter(WorkbenchBackend backend, std::function<bool()> alive,
                     std::shared_ptr<DiagnosticsSink> diagnostics = nullptr);
    WorkbenchAdapter(HWND owner, std::function<bool()> alive,
                     std::shared_ptr<DiagnosticsSink> diagnostics = nullptr);
    ~WorkbenchAdapter();
    WorkbenchAdapter(const WorkbenchAdapter&) = delete;
    WorkbenchAdapter& operator=(const WorkbenchAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    bool InvokeActionToken(uint64_t token) noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

    // Global UI-thread dispatch for host window messages carrying a notification action token.
    static bool DispatchHostActionToken(uint64_t token) noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native
