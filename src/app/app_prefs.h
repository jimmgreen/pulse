// app_prefs.h — General app settings (startup, close-to-tray).
#pragma once
#include <string>

namespace pulse::app {

struct AppPrefs {
    bool persist = true;
    bool launch_on_startup = false;
    bool keep_running_on_close = false;
    // none / acrylic-material / mica / mica-alt  (legacy dwm-blur → acrylic)
    std::wstring window_effect = L"mica-alt";
    std::wstring background_image;
    int row_height = 34; // file-list row height in DIPs (24..48)

    void ResetToDefaults();
    bool Load();
    bool Save() const;
    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);

    // HKCU Run key is the source of truth; call after Load() and on toggle.
    bool ReadLaunchOnStartup() const;
    bool ApplyLaunchOnStartup(bool on);

    bool StoreBackgroundImage(const std::wstring& source_path);
    void ClearBackgroundImage();
};

} // namespace pulse::app
