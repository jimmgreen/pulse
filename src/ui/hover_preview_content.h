#pragma once

#include "../common/localization.h"
#include "../common/preview_extensions.h"
#include "video_preview.h"
#include "ui_view_model.h"
#include <initializer_list>
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <cwctype>
#include <string>
#include <unordered_map>

namespace pulse::ui::hover_preview_content {

// The shell's name for the file type ("JPEG 图像"), by extension: a registry
// lookup (SHGFI_USEFILEATTRIBUTES never opens the file), remembered per
// extension.
inline std::wstring TypeName(const std::wstring& name) {
    thread_local std::unordered_map<std::wstring, std::wstring> cache;
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? std::wstring() : name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (const auto it = cache.find(ext); it != cache.end()) return it->second;
    std::wstring type;
    SHFILEINFOW info{};
    const std::wstring probe = L"file" + ext;
    if (SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
        type = info.szTypeName;
    if (type.empty()) {
        std::wstring upper = ext.size() > 1 ? ext.substr(1) : std::wstring();
        for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
        type = upper.empty() ? std::wstring(l10n::Get(l10n::StringId::TypeFile).c_str())
                             : upper + l10n::Get(l10n::StringId::File).c_str();
    }
    if (cache.size() > 256) cache.clear();
    cache.emplace(ext, type);
    return type;
}

inline std::wstring LowerExtension(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? std::wstring() : name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

// Plays (silently) in the card: video Media Foundation or the FFmpeg pack
// can open; audio-only files keep their cover art or icon.
inline bool CardVideo(const std::wstring& path) {
    return VideoPreview::Supports(path) && !VideoPreview::IsAudio(path);
}

// Takes the preview host or a player a noticeable moment: these show the
// dwell line on the row and a loading sweep in the card.
inline bool SlowToDecode(const std::wstring& name, uint64_t size, const std::wstring& path) {
    if (size >= 48ull * 1024 * 1024) return true;
    const std::wstring ext = LowerExtension(name);
    if (preview::IsVideoExtension(ext) || CardVideo(path) || preview::IsPdfRasterExtension(ext) ||
        preview::IsArchiveExtension(ext) || preview::IsPsdExtension(ext) ||
        preview::IsFamilyExtension(preview::PreviewFamily::Raw, ext))
        return true;
    return preview::IsOneOf(ext, {L".docx", L".pptx", L".xlsx", L".xlsm", L".epub", L".ipynb", L".dwg",
                                  L".heic", L".heif", L".hif", L".avif"});
}

// The composition bar's kind of a file, by extension.
inline int FolderKind(const std::wstring& name) {
    static const std::unordered_map<std::wstring, int> kinds = [] {
        std::unordered_map<std::wstring, int> map;
        const auto add = [&map](int kind, std::initializer_list<const wchar_t*> list) {
            for (const wchar_t* ext : list) map.emplace(ext, kind);
        };
        add(kHoverKindImage, {L".jpg", L".jpeg", L".jfif", L".png", L".gif", L".bmp", L".webp", L".heic",
                                  L".heif", L".hif", L".avif", L".tif", L".tiff", L".svg", L".ico", L".psd",
                                  L".raw", L".dng", L".cr2", L".cr3", L".nef", L".arw", L".orf", L".rw2"});
        add(kHoverKindVideo, {L".mp4", L".mkv", L".mov", L".avi", L".wmv", L".flv", L".webm", L".m4v",
                                  L".ts", L".mts", L".m2ts", L".mpg", L".mpeg", L".3gp"});
        add(kHoverKindAudio, {L".mp3", L".flac", L".wav", L".aac", L".m4a", L".ogg", L".opus", L".wma",
                                  L".ape", L".aiff"});
        add(kHoverKindDocument, {L".doc", L".docx", L".xls", L".xlsx", L".xlsm", L".ppt", L".pptx", L".pdf",
                                     L".txt", L".md", L".rtf", L".odt", L".ods", L".odp", L".csv", L".wps",
                                     L".et", L".dps", L".epub", L".xps"});
        add(kHoverKindCode, {L".c", L".cc", L".cpp", L".h", L".hpp", L".cs", L".js", L".mjs", L".ts",
                                 L".tsx", L".jsx", L".py", L".java", L".go", L".rs", L".rb", L".php", L".html",
                                 L".css", L".scss", L".json", L".xml", L".yml", L".yaml", L".toml", L".sh",
                                 L".ps1", L".bat", L".cmd", L".lua", L".kt", L".swift", L".sql", L".vue"});
        add(kHoverKindArchive, {L".zip", L".rar", L".7z", L".tar", L".gz", L".tgz", L".bz2", L".xz",
                                    L".zst", L".cab", L".iso"});
        return map;
    }();
    const auto it = kinds.find(LowerExtension(name));
    return it == kinds.end() ? kHoverKindOther : it->second;
}

} // namespace pulse::ui::hover_preview_content
