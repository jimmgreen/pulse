#include "localization.h"

#include <array>
#include <atomic>
#include <cwchar>
#include <mutex>

namespace pulse::l10n {
namespace {

constexpr UINT kFirstString = IDS_SETTINGS;
// Must stay on the highest allocated string id, otherwise Get() returns empty.
constexpr UINT kLastString = IDS_SETTINGS_CLOSE_TAB_DOUBLE_CLICK_DESC;
static_assert(static_cast<UINT>(StringId::HelpContextMenu) <= kLastString);
static_assert(static_cast<UINT>(StringId::UiFontLarger) <= kLastString);
static_assert(static_cast<UINT>(StringId::OpAuthorizationNotGranted) <= kLastString);
static_assert(static_cast<UINT>(StringId::ListThumbnailBadgesDesc) <= kLastString);
static_assert(static_cast<UINT>(StringId::SystemGroupNodeModules) <= kLastString);
static_assert(static_cast<UINT>(StringId::UpdateWaitingOperations) <= kLastString);
static_assert(static_cast<UINT>(StringId::SidebarShowHidden) <= kLastString);
static_assert(static_cast<UINT>(StringId::ApplyGroupNoneMessage) <= kLastString);
static_assert(static_cast<UINT>(StringId::ExitPulse) <= kLastString);
static_assert(static_cast<UINT>(StringId::RecycleConfirmManyFormat) <= kLastString);
static_assert(static_cast<UINT>(StringId::NetworkLiveAddFailed) <= kLastString);
static_assert(static_cast<UINT>(StringId::IntegrationReapply) <= kLastString);
static_assert(static_cast<UINT>(StringId::UpdateDescManual) <= kLastString);
static_assert(static_cast<UINT>(StringId::Downloads) <= kLastString);
static_assert(static_cast<UINT>(StringId::OpItemsPerSecond) <= kLastString);
static_assert(static_cast<UINT>(StringId::SettingsChangeTracking) >= kFirstString &&
              static_cast<UINT>(StringId::ChangeDisabled) <= kLastString &&
              static_cast<UINT>(StringId::FolderSortMixed) <= kLastString &&
              static_cast<UINT>(StringId::WallpaperBlurStrong) <= kLastString &&
              static_cast<UINT>(StringId::TextRenderSmooth) <= kLastString &&
              static_cast<UINT>(StringId::TipTrayReleaseMove) <= kLastString &&
              static_cast<UINT>(StringId::CompareDiffers) <= kLastString &&
              static_cast<UINT>(StringId::CompareChipDiffers) <= kLastString &&
              static_cast<UINT>(StringId::AdvSumKeywordNeeded) <= kLastString &&
              static_cast<UINT>(StringId::HintsResetDone) <= kLastString &&
              static_cast<UINT>(StringId::ApplyViewAllConfirm) <= kLastString &&
              static_cast<UINT>(StringId::NewTabOpenCurrent) <= kLastString &&
              static_cast<UINT>(StringId::HelpMoveFocus) <= kLastString &&
              static_cast<UINT>(StringId::ContextRowMore) <= kLastString &&
              static_cast<UINT>(StringId::GlobalSearchTruncatedShort) <= kLastString);
static_assert(static_cast<UINT>(StringId::SettingsCloseLastTabDesc) <= kLastString);
static_assert(static_cast<UINT>(StringId::SettingsCloseTabDoubleClickDesc) <= kLastString);
static_assert(static_cast<UINT>(StringId::LockedItemTitle) <= kLastString &&
              static_cast<UINT>(StringId::LockedItemRetry) <= kLastString);
static_assert(static_cast<UINT>(StringId::LanguageZhTW) <= kLastString &&
              static_cast<UINT>(StringId::SettingsAutoUpdate) <= kLastString &&
              static_cast<UINT>(StringId::UpdateDescManual) <= kLastString);

HINSTANCE g_module = nullptr;
std::atomic<Language> g_preference{Language::System};
std::atomic<Language> g_effective{Language::EnUS};
std::mutex g_mutex;
// Published strings stay immutable when the UI language changes. Workers may
// still hold references to the previous language while finishing an operation.
constexpr size_t kLocaleCount = 3;
std::array<std::array<std::wstring, kLastString - kFirstString + 1>, kLocaleCount> g_cache;
std::array<std::array<bool, kLastString - kFirstString + 1>, kLocaleCount> g_loaded{};
const std::wstring g_empty;

Language SystemLanguage() noexcept {
    return LanguageFromLangId(GetUserDefaultUILanguage());
}

LANGID ResourceLanguage(Language language) noexcept {
    switch (language) {
    case Language::ZhCN: return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
    case Language::ZhTW: return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
    default: return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    }
}

size_t LocaleSlot(Language language) noexcept {
    switch (language) {
    case Language::ZhCN: return 0;
    case Language::ZhTW: return 2;
    default: return 1;
    }
}

std::wstring LoadStringResource(UINT id, LANGID language) {
    if (!g_module) return {};
    const UINT block = id / 16 + 1;
    const UINT index = id % 16;
    HRSRC resource = FindResourceExW(g_module, RT_STRING, MAKEINTRESOURCEW(block), language);
    if (!resource) return {};
    HGLOBAL data = LoadResource(g_module, resource);
    const auto* cursor = static_cast<const wchar_t*>(LockResource(data));
    if (!cursor) return {};
    for (UINT i = 0; i < index; ++i) cursor += 1 + static_cast<UINT>(*cursor);
    const UINT length = static_cast<UINT>(*cursor++);
    return std::wstring(cursor, length);
}

void ApplyLanguage() {
    const Language selected = g_preference.load();
    g_effective = selected == Language::System ? SystemLanguage() : selected;
    SetThreadUILanguage(ResourceLanguage(g_effective));
}

} // namespace

bool IsLanguageId(std::wstring_view id) noexcept {
    return id == L"system" || id == L"zh-CN" || id == L"zh-TW" || id == L"en-US";
}

Language LanguageFromLangId(LANGID language) noexcept {
    if (PRIMARYLANGID(language) != LANG_CHINESE) return Language::EnUS;
    switch (SUBLANGID(language)) {
    case SUBLANG_CHINESE_TRADITIONAL:  // zh-TW
    case SUBLANG_CHINESE_HONGKONG:     // zh-HK
    case SUBLANG_CHINESE_MACAU:        // zh-MO
    case 0x1F:                         // zh-Hant (LANG_CHINESE_TRADITIONAL)
        return Language::ZhTW;
    default:
        return Language::ZhCN;
    }
}

Language LanguageFromId(std::wstring_view id) noexcept {
    if (id == L"zh-CN") return Language::ZhCN;
    if (id == L"zh-TW") return Language::ZhTW;
    if (id == L"en-US") return Language::EnUS;
    return Language::System;
}

const wchar_t* LanguageId(Language language) noexcept {
    switch (language) {
    case Language::ZhCN: return L"zh-CN";
    case Language::ZhTW: return L"zh-TW";
    case Language::EnUS: return L"en-US";
    default: return L"system";
    }
}

void Initialize(HINSTANCE module, std::wstring_view preference) {
    std::lock_guard lock(g_mutex);
    g_module = module;
    g_preference = LanguageFromId(preference);
    ApplyLanguage();
}

void SetLanguage(std::wstring_view preference) {
    const Language next = LanguageFromId(preference);
    if (next == g_preference) return;
    g_preference = next;
    ApplyLanguage();
}

Language preference() noexcept { return g_preference; }
Language effective_language() noexcept { return g_effective; }

const wchar_t* LocaleName() noexcept {
    switch (g_effective.load()) {
    case Language::ZhCN: return L"zh-CN";
    case Language::ZhTW: return L"zh-TW";
    default: return L"en-US";
    }
}

const std::wstring& Get(StringId id) {
    const UINT value = static_cast<UINT>(id);
    if (value < kFirstString || value > kLastString) return g_empty;
    const size_t index = value - kFirstString;
    std::lock_guard lock(g_mutex);
    if (!g_module) return g_empty;
    const Language language = g_effective.load();
    const size_t locale = LocaleSlot(language);
    if (!g_loaded[locale][index]) {
        auto& text = g_cache[locale][index];
        text = LoadStringResource(value, ResourceLanguage(language));
        // Traditional readers prefer Simplified over English for a missing entry.
        if (text.empty() && language == Language::ZhTW)
            text = LoadStringResource(value, ResourceLanguage(Language::ZhCN));
        if (text.empty() && language != Language::EnUS)
            text = LoadStringResource(value, ResourceLanguage(Language::EnUS));
        g_loaded[locale][index] = true;
    }
    return g_cache[locale][index];
}

} // namespace pulse::l10n