#include "setup_strings.h"

#include <windows.h>

#include "setup_common.h"

namespace pulse::setup {
namespace {

constexpr const wchar_t* kTable[][3] = {
#include "setup_strings.generated.inc"
};
static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(Str::Count));

}  // namespace

Lang DetectLanguage(const wchar_t* inno_lang) {
    if (inno_lang && *inno_lang) {
        const std::wstring name = inno_lang;
        if (EqualsNoCase(name, L"chinesesimp")) return Lang::ZhHans;
        if (EqualsNoCase(name, L"chinesetrad")) return Lang::ZhHant;
        if (EqualsNoCase(name, L"english")) return Lang::En;
    }
    const LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) != LANG_CHINESE) return Lang::En;
    switch (SUBLANGID(id)) {
        case SUBLANG_CHINESE_TRADITIONAL: case SUBLANG_CHINESE_HONGKONG: case SUBLANG_CHINESE_MACAU:
            return Lang::ZhHant;
        default:
            return Lang::ZhHans;
    }
}

const wchar_t* Text(Lang lang, Str id) { return kTable[static_cast<int>(id)][static_cast<int>(lang)]; }

}  // namespace pulse::setup
