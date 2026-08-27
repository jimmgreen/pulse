#pragma once

#include <initializer_list>
#include <string_view>

namespace pulse::preview {

inline bool IsOneOf(std::wstring_view extension,
                    std::initializer_list<std::wstring_view> values) {
    for (const auto value : values) {
        if (extension == value) return true;
    }
    return false;
}

inline bool IsTextExtension(std::wstring_view extension) {
    return IsOneOf(extension, {
        L".txt", L".md", L".log", L".json", L".xml", L".yaml", L".yml",
        L".ini", L".cfg", L".conf", L".csv", L".tsv", L".cpp", L".c",
        L".h", L".hpp", L".cc", L".cxx", L".cs", L".java", L".js",
        L".jsx", L".ts", L".tsx", L".py", L".rs", L".go", L".php",
        L".html", L".htm", L".css", L".scss", L".sql", L".ps1", L".bat",
        L".cmd", L".sh", L".qml", L".cmake", L".toml", L".properties",
        L".lpm"
    });
}

inline bool IsImageExtension(std::wstring_view extension) {
    return IsOneOf(extension, {
        L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff",
        L".webp", L".heic", L".ico"
    });
}

inline bool IsNativeExtension(std::wstring_view extension) {
    return IsTextExtension(extension) || IsImageExtension(extension);
}

} // namespace pulse::preview
