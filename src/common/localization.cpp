#include "localization.h"

#include <array>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <unordered_map>

namespace pulse::l10n {
namespace {

HINSTANCE g_module = nullptr;
std::atomic<Language> g_preference{Language::System};
std::atomic<Language> g_effective{Language::EnUS};
std::mutex g_mutex;
// Published strings stay immutable when the UI language changes. Workers may
// still hold references to the previous language while finishing an operation.
constexpr size_t kLocaleCount = 3;
// Keyed by id, so a newly added string never falls outside a fixed range
// (that once left every new label blank). Entries are never erased, and an
// unordered_map keeps element references valid across inserts.
std::array<std::unordered_map<UINT, std::wstring>, kLocaleCount> g_cache;
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
    return GetForLanguage(id, g_effective.load());
}

const std::wstring& GetForLanguage(StringId id, Language language) {
    const UINT value = static_cast<UINT>(id);
    if (value == 0) return g_empty;
    std::lock_guard lock(g_mutex);
    if (!g_module) return g_empty;
    if (language == Language::System) language = SystemLanguage();
    auto& cache = g_cache[LocaleSlot(language)];
    if (const auto found = cache.find(value); found != cache.end()) return found->second;
    std::wstring text = LoadStringResource(value, ResourceLanguage(language));
    // Traditional readers prefer Simplified over English for a missing entry.
    if (text.empty() && language == Language::ZhTW)
        text = LoadStringResource(value, ResourceLanguage(Language::ZhCN));
    if (text.empty() && language != Language::EnUS)
        text = LoadStringResource(value, ResourceLanguage(Language::EnUS));
    return cache.emplace(value, std::move(text)).first->second;
}

} // namespace pulse::l10n
