// Default Quick Look routing. System preview remains an explicit user choice.
#pragma once
#include <string_view>
#include <initializer_list>
namespace pulse::preview {
inline bool OfficeExtensionEquals(std::wstring_view value, std::wstring_view lower) {
    if (value.size() != lower.size()) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        wchar_t c = value[i];
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (c != lower[i]) return false;
    }
    return true;
}
inline bool IsLegacyWordPreviewExtension(std::wstring_view extension) {
    for (const auto candidate : {L".doc", L".dot", L".rtf", L".wps"})
        if (OfficeExtensionEquals(extension, candidate)) return true;
    return false;
}
inline bool IsPresentationPreviewExtension(std::wstring_view extension) {
    for (const auto candidate : {L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".ppsm",
                                 L".pot", L".potx", L".potm", L".dps"})
        if (OfficeExtensionEquals(extension, candidate)) return true;
    return false;
}
inline bool IsNativeOfficeDocumentExtension(std::wstring_view extension) {
    if (IsLegacyWordPreviewExtension(extension) || IsPresentationPreviewExtension(extension)) return true;
    for (const auto candidate : {L".docx", L".docm", L".dotx", L".dotm"})
        if (OfficeExtensionEquals(extension, candidate)) return true;
    return false;
}
} // namespace pulse::preview
