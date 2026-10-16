#include "app_internal.h"
#include "app_input.h"
#include <filesystem>
#include <fstream>
#include <shlobj.h>

#ifdef PULSE_WITH_SELFTEST
using namespace pulse;
void Render(pulse::AppState&);

namespace pulse::ui {
struct SidebarFolderIconTest {
    static bool WriteIcon(const std::filesystem::path& path, DWORD color) {
        std::ofstream file(path, std::ios::binary);
        const WORD directory[] = {0, 1, 1};
        const BYTE dimensions[] = {32, 32, 0, 0};
        const WORD format[] = {1, 32};
        const DWORD entry[] = {40 + 32 * 32 * 4 + 32 * 4, 22};
        BITMAPINFOHEADER bitmap{};
        bitmap.biSize = sizeof(bitmap);
        bitmap.biWidth = 32;
        bitmap.biHeight = 64;
        bitmap.biPlanes = 1;
        bitmap.biBitCount = 32;
        file.write(reinterpret_cast<const char*>(directory), sizeof(directory));
        file.write(reinterpret_cast<const char*>(dimensions), sizeof(dimensions));
        file.write(reinterpret_cast<const char*>(format), sizeof(format));
        file.write(reinterpret_cast<const char*>(entry), sizeof(entry));
        file.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap));
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            const DWORD pixel = x >= 4 && x < 28 && y >= 4 && y < 28 ? color : 0;
            file.write(reinterpret_cast<const char*>(&pixel), sizeof(pixel));
        }
        const DWORD mask[32]{};
        file.write(reinterpret_cast<const char*>(mask), sizeof(mask));
        return file.good();
    }

    static bool Customize(const std::filesystem::path& folder, const std::filesystem::path& icon) {
        SHFOLDERCUSTOMSETTINGS settings{};
        settings.dwSize = sizeof(settings);
        settings.dwMask = FCSM_ICONFILE;
        auto resource = icon.wstring();
        settings.pszIconFile = resource.data();
        settings.iIconIndex = 0;
        const bool ok = SUCCEEDED(SHGetSetFolderCustomSettings(&settings, folder.c_str(), FCS_FORCEWRITE));
        SetFileAttributesW(folder.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY);
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, folder.c_str(), nullptr);
        return ok;
    }

    static int Run(AppState& s, const wchar_t* output) {
        const auto root = std::filesystem::path(output).parent_path();
        std::ofstream log{std::filesystem::path(output)};
        int failures = 0;
        auto check = [&](bool ok, const char* label) {
            log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
            if (!ok) ++failures;
        };
        const auto red = root / L"Custom red";
        const auto blue = root / L"自定义蓝色";
        const auto plain = root / L"Ordinary folder";
        const auto broken = root / L"Unavailable icon";
        const auto missing = root / L"Missing folder";
        for (const auto& folder : {red, blue, plain, broken}) std::filesystem::create_directories(folder);
        check(WriteIcon(root / L"red.ico", 0xffe03040) && WriteIcon(root / L"blue.ico", 0xff2060e0),
              "create isolated custom ICO files");
        check(Customize(red, root / L"red.ico") && Customize(blue, root / L"blue.ico") &&
              Customize(broken, root / L"missing.ico"), "configure Windows folder icons only in fixtures");
        s.places.quick_access_paths = {red.wstring(), blue.wstring(), plain.wstring(), broken.wstring(), missing.wstring()};
        s.appPrefs.persist = false;
        auto vm = BuildVm(s, false);
        auto& renderer = s.renderer;
        auto& cache = renderer.icon_cache_;
        auto find = [&](const std::wstring& path) {
            for (const auto& group : vm.sidebar)
                for (const auto& item : group.items)
                    if (item.path == path) return item;
            return SidebarItem{};
        };
        const auto red_item = find(red.wstring());
        const auto blue_item = find(blue.wstring());
        check(red_item.shell_folder_icon && blue_item.shell_folder_icon && find(plain.wstring()).shell_folder_icon,
              "quick access opts filesystem folders into native icons");
        bool virtual_safe = true;
        for (const auto& group : vm.sidebar) for (const auto& item : group.items)
            if (fs::IsVirtualPath(item.path)) virtual_safe &= !item.shell_folder_icon;
        check(virtual_safe, "virtual items retain their Fluent icons");

        auto wait = [&](auto condition) {
            const ULONGLONG end = GetTickCount64() + 10000;
            while (GetTickCount64() < end) {
                if (condition()) return true;
                Sleep(10);
            }
            return condition();
        };
        auto native_index = [&](const std::filesystem::path& folder) {
            SHFILEINFOW info{};
            return SHGetFileInfoW(folder.c_str(), 0, &info, sizeof(info),
                SHGFI_SYSICONINDEX | SHGFI_SMALLICON) ? info.iIcon : -1;
        };
        const int red_index = native_index(red);
        const int blue_index = native_index(blue);
        const int plain_index = native_index(plain);
        check(red_index >= 0 && blue_index >= 0 && plain_index >= 0 &&
              red_index != blue_index && red_index != plain_index && blue_index != plain_index,
              "Windows recognizes distinct custom and ordinary folder icons");
        auto ready = [&](const SidebarItem& item, int index, float size) {
            auto* bitmap = renderer.SidebarFolderIcon(item, D2D1::RectF(0, 0, size, size));
            if (!bitmap) return false;
            std::lock_guard lock(cache.mutex_);
            const auto found = cache.exact_index_.find(item.path);
            if (found == cache.exact_index_.end() || found->second != index) return false;
            const auto key = (static_cast<uint64_t>(static_cast<uint32_t>(cache.ImageListId(size))) << 32) |
                             static_cast<uint32_t>(index);
            const auto converted = cache.bitmaps_.find(key);
            return converted != cache.bitmaps_.end() && converted->second.get() == bitmap;
        };
        for (float scale : {1.0f, 1.5f, 2.0f}) {
            cache.SetScale(scale);
            check(wait([&] { return ready(red_item, red_index, 20 * scale) && ready(blue_item, blue_index, 20 * scale); }),
                  "sidebar receives exact custom artwork asynchronously at each DPI");
        }
        for (const auto& folder : {plain, broken, missing}) {
            const auto item = find(folder.wstring());
            check(wait([&] { return renderer.SidebarFolderIcon(item, D2D1::RectF(0, 0, 20, 20)) != nullptr; }),
                  "ordinary, broken-icon and missing folders have fallback artwork");
        }
        cache.SetScale(s.scale);
        const auto original = red_index;
        check(Customize(red, root / L"blue.ico"), "change an existing pinned folder's Windows icon");
        const int changed = native_index(red);
        check(changed >= 0 && changed != original, "Windows exposes the replacement icon");
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, VK_F5, 0);
        check(wait([&] { return ready(red_item, changed, 20 * s.scale); }),
              "F5 discards old icons and loads the new native icon");
        cache.InvalidatePaths({red.wstring()});
        cache.InvalidatePaths({red.wstring()});
        check(wait([&] { return ready(red_item, changed, 20 * s.scale); }),
              "repeated refresh recovers without waiting for old Shell work");
        // Recreate just the cache in a second instance, as on an app restart.
        ShellIconCache restarted;
        restarted.SetDeviceContext(s.compositor.Dc());
        check(wait([&] {
            restarted.Prefetch(red.wstring(), L"Custom red", true, FILE_ATTRIBUTE_DIRECTORY, 20);
            if (!restarted.CachedBitmapFor(red.wstring(), L"Custom red", true, FILE_ATTRIBUTE_DIRECTORY, 20)) return false;
            std::lock_guard lock(restarted.mutex_);
            return restarted.exact_index_.contains(red.wstring()) && restarted.exact_index_.at(red.wstring()) == changed;
        }), "a new cache resolves the saved Windows customization");

        check(Customize(red, root / L"red.ico"), "restore contrasting fixture icons for visual inspection");
        renderer.RefreshSidebarFolderIcons(BuildVm(s, false));
        check(wait([&] { return ready(red_item, native_index(red), 20 * s.scale) &&
                                ready(blue_item, blue_index, 20 * s.scale); }),
              "both pinned folders retain independent artwork after refresh");

        auto snapshot = [&](const wchar_t* name) {
            for (int frame = 0; frame < 3; ++frame) { Render(s); Sleep(30); }
            check(s.compositor.SaveSnapshot((root / name).c_str()), "save sidebar visual verification image");
        };
        snapshot(L"sidebar-light.png");
        s.darkMode = true;
        s.themeOverride = ThemeMode::Dark;
        snapshot(L"sidebar-dark.png");
        s.appPrefs.sidebar_collapsed = true;
        renderer.SetSidebarCollapsed(true);
        snapshot(L"sidebar-rail.png");
        check(renderer.EffectiveSidebarWidth(static_cast<float>(s.compositor.Width())) < 80 * s.scale,
              "collapsed sidebar screenshot exercises the icon rail");
        return failures;
    }
};
}

int RunSidebarFolderIconTest(AppState& s, const wchar_t* output) {
    return ui::SidebarFolderIconTest::Run(s, output);
}
#endif
