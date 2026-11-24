#pragma once
#include "../../../../sdk/native/include/pulse/shell.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::extensions::native {

class DiagnosticsSink;

struct ColorTagInfoData {
    std::string id;
    std::string name;
    uint32_t argb = 0;
    int32_t favorite_slot = 0;
};

struct ShellMenuItemData {
    uint32_t item_id = 0;
    uint32_t flags = 0;
    uint32_t depth = 0;
    uint32_t mnemonic_codepoint = 0;
    std::string verb;
    std::string label;
    std::string handler_name;
};

struct ShellMenuBackendReply {
    PulseStatus status = PULSE_OK;
    bool partial = false;
    std::vector<ShellMenuItemData> items;
};

struct ShellPreviewData {
    PulseStatus status = PULSE_OK;
    std::string path;
    std::string display_type;
    std::string text_excerpt;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    uint32_t thumb_width = 0;
    uint32_t thumb_height = 0;
    uint32_t thumb_stride = 0;
    std::vector<uint8_t> thumb_bgra;
};

struct ShellBackend {
    std::function<bool()> valid_owner;
    std::function<PulseStatus(std::vector<ColorTagInfoData>& out)> get_tag_catalog;
    std::function<PulseStatus(const std::wstring& path, std::vector<ColorTagInfoData>& out)> get_path_tags;
    std::function<PulseStatus(const std::wstring& path, const std::wstring& tag_id, bool tagged)> set_path_tag;
    std::function<PulseStatus(uint64_t session_id, const std::vector<std::wstring>& paths, uint32_t flags,
                              std::function<void(ShellMenuBackendReply)> deliver)> query_context_menu;
    std::function<PulseStatus(uint64_t session_id, uint32_t item_id, const std::wstring& verb)> invoke_context_menu;
    std::function<PulseStatus(uint64_t session_id)> close_context_menu;
    std::function<PulseStatus(uint64_t request_id, const std::wstring& path, uint32_t max_thumb_px,
                              std::function<void(ShellPreviewData)> complete)> read_preview;
    std::function<void(uint64_t request_id)> cancel_preview;
};

class ShellAdapter {
public:
    ShellAdapter(ShellBackend backend, std::function<bool()> alive,
                 std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                 std::function<void()> wake = nullptr);
    ShellAdapter(HWND owner, std::function<bool()> alive,
                 std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                 std::function<void()> wake = nullptr);
    ~ShellAdapter();
    ShellAdapter(const ShellAdapter&) = delete;
    ShellAdapter& operator=(const ShellAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    void SetWake(std::function<void()> wake);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Poll() noexcept;
    bool HasPendingWork() const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native
