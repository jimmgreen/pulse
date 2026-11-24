#pragma once
#include "../../../../sdk/native/include/pulse/tray.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::extensions::native {

class DiagnosticsSink;

struct TrayEntryData {
    uint64_t item_id = 0;
    uint64_t batch_id = 0;
    uint64_t size_bytes = 0;
    uint32_t flags = PULSE_TRAY_ITEM_NONE;
    std::string path;
    std::string name;
};

struct TraySnapshotData {
    uint64_t revision = 0;
    uint64_t total_bytes = 0;
    std::vector<TrayEntryData> entries;
};

struct TrayBackend {
    std::function<bool()> valid_owner;
    std::function<PulseStatus(TraySnapshotData& out)> snapshot;
    std::function<PulseStatus(const std::vector<std::wstring>& paths, bool move_intent)> collect;
    std::function<PulseStatus(uint64_t item_id)> remove_item;
    std::function<PulseStatus()> clear;
    std::function<PulseStatus(const std::wstring& destination_dir, uint32_t release_mode)> release_to_directory;
};

class TrayAdapter {
public:
    TrayAdapter(TrayBackend backend, std::function<bool()> alive,
                std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                std::function<void()> wake = nullptr);
    TrayAdapter(HWND owner, std::function<bool()> alive,
                std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                std::function<void()> wake = nullptr);
    ~TrayAdapter();
    TrayAdapter(const TrayAdapter&) = delete;
    TrayAdapter& operator=(const TrayAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    void SetWake(std::function<void()> wake);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Sync() noexcept;
    bool HasSubscriptions() const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native
