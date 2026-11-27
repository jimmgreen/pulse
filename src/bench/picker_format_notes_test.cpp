#include "../ui/picker_format_segments.h"
#include "../common/localization.h"

#include <iostream>

// Keep the same regression source compilable against the pre-change header;
// that implementation must fail to link these new production entry points.
namespace pulse::ui {
std::wstring PickerFormatNote(const PickerFilter& filter);
std::wstring PickerFormatStaticText(const PickerFilter& filter);
}

namespace {
int failures = 0;
int checks = 0;
void Check(bool ok, const char* name) {
    ++checks;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
}

int main() {
    using namespace pulse::ui;
    namespace l10n = pulse::l10n;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    Check(PickerFormatNote({L"PNG (*.PNG)", L"*.PNG"}) ==
        L"Lossless raster image · supports transparency", "PNG capability and case normalization");
    Check(PickerFormatNote({L"JPEG", L"*.jpg; *.JPEG;*.jpe"}) ==
        L"Usually lossy compression · suited to photos · .jpg .jpeg .jpe", "JPEG aliases share accurate description");
    Check(PickerFormatNote({L"WebP", L"*.webp"}) ==
        L"Supports lossy and lossless compression · supports transparency", "WebP does not claim smallest size");
    Check(PickerFormatNote({L"Photoshop", L"*.psd;*.pdd;*.psdt"}) ==
        L"Photoshop document format · supports layers · .psd .pdd .psdt", "PSD capability does not promise caller preservation");
    Check(PickerFormatNote({L"Large document", L"*.psb"}) ==
        L"Photoshop large document format · supports layers", "PSB has no incorrect size threshold");
    Check(PickerFormatNote({L"Text", L"*.txt"}) ==
        L"Plain text · no font or layout formatting", "TXT capability");
    Check(PickerFormatNote({L"Vendor drawing (*.XYZ)", L"*.XYZ"}) ==
        L"Vendor drawing · .xyz", "unknown caller name and extension only");
    Check(PickerFormatNote({L"PNG", L"*.vendor"}) == L"PNG · .vendor", "caller label cannot spoof known extension");
    Check(PickerFormatNote({L"Images", L"*.png;*.jpg"}) == L"Images · .png .jpg", "mixed families do not inherit first capability");
    Check(PickerFormatNote({L"Images", L"*.png;*.vendor"}) == L"Images · .png .vendor", "known and unknown group stays literal");
    Check(PickerFormatNote({L"Images", L"*.png;image?.png"}) == L"Images · .png", "nonliteral mask does not inherit first capability");
    Check(PickerFormatNote({L"", L"*.vendor"}) == L".vendor", "empty caller name has no separator prefix");
    Check(PickerFormatNote({L"Special", L"file?.abc"}) == L"Special", "unknown non-extension mask retains caller name");
    Check(PickerFormatNote({L"", L""}).empty(), "empty filter remains empty");
    Check(PickerFormatStaticText({L"Text", L"*.txt"}) == L"TXT · .txt", "single concrete type static label");
    Check(PickerFormatStaticText({L"JPEG", L"*.jpg;*.jpeg"}) == L"JPEG · .jpg .jpeg", "single type static aliases");
    Check(PickerFormatStaticText({L"All", L"*.*"}) == L"Any format · Uses the extension in the file name", "single wildcard static explanation");
    Check(PickerFormatNote({L"All", L"*.png;*.*"}) == L"Uses the extension in the file name", "wildcard precedence remains unchanged");
    for (const wchar_t* language : {L"zh-CN", L"zh-TW", L"en-US"}) {
        l10n::SetLanguage(language);
        Check(!PickerFormatNote({L"PNG", L"*.png"}).empty() &&
            !PickerFormatNote({L"WebP", L"*.webp"}).empty() &&
            !PickerFormatStaticText({L"All", L"*.*"}).empty(), "real resource strings resolve in shipped language");
        Check(PickerFormatNote({L"用户自定义名稱", L"*.xyz"}) == L"用户自定义名稱 · .xyz", "caller text stays unchanged in every UI language");
    }
    l10n::SetLanguage(L"zh-CN");
    Check(PickerFormatNote({L"PNG", L"*.png"}) == L"无损位图 · 支持透明", "Simplified Chinese resource");
    l10n::SetLanguage(L"zh-TW");
    Check(PickerFormatNote({L"PNG", L"*.png"}) == L"無損點陣圖 · 支援透明", "Traditional Chinese generated resource");
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
