#pragma once
#include "../../../../sdk/native/include/pulse/settings_pages.h"
#include "ui_adapter.h"
namespace pulse::extensions::native {
struct SettingsPageDescriptor { uint64_t id = 0; std::wstring title; };
class SettingsPagesAdapter {
public:
    SettingsPagesAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, float scale, std::function<bool()> alive);
    ~SettingsPagesAdapter();
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    std::optional<SettingsPageDescriptor> Page() const;
    bool Show(uint64_t page, RECT bounds, bool dark, D2D1_COLOR_F accent, float scale);
    void Hide();
    void Close();
    bool InCallback() const;
    UiAdapter& Ui();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
