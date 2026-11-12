#pragma once
#include "../../../../sdk/native/include/pulse/settings_pages.h"
#include "ui_adapter.h"
namespace pulse::extensions::native {
class DiagnosticsSink;
struct SettingsPageDescriptor { uint64_t id = 0; std::wstring title; };
class SettingsPagesAdapter {
public:
    SettingsPagesAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, float scale, std::function<bool()> alive);
    ~SettingsPagesAdapter();
    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    std::optional<SettingsPageDescriptor> Page() const;
    bool Show(uint64_t page, RECT bounds, bool dark, D2D1_COLOR_F accent, float scale);
    void Hide();
    void Close();
    bool InCallback() const;
    UiAdapter& Ui();
    // The page's own state, read or edited by the host for the user (store views).
    std::optional<std::string> State(const std::string& key) const;
    bool CommitState(const std::string& key, const std::string& value);
    bool Click(const std::string& node);
private:
    struct AdapterState;
    std::shared_ptr<AdapterState> state_;
};
}
