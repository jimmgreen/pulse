#pragma once

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <string_view>

namespace pulse::preview {

inline bool IsOneOf(std::wstring_view extension,
                    std::initializer_list<std::wstring_view> values) {
    for (const auto value : values) {
        if (extension == value) return true;
    }
    return false;
}

// Every format family the preview host decodes itself. Each family has one
// extension list below and one row in kPreviewFormats; the IsXxxExtension
// helpers and IsNativeExtension() all read that table, so adding a format means
// adding its row here plus a decoder entry in preview_host/main.cpp.
enum class PreviewFamily {
    Text,
    Image,
    Vector,
    MetaFile,
    PdfRaster,
    Archive,
    Psd,
    Font,
    Raw,
    Pe,
};

namespace formats {

inline constexpr std::wstring_view kText[] = {
    L".txt", L".md", L".log", L".json", L".xml", L".yaml", L".yml",
    L".ini", L".cfg", L".conf", L".csv", L".tsv", L".cpp", L".c",
    L".h", L".hpp", L".cc", L".cxx", L".cs", L".java", L".js",
    L".jsx", L".ts", L".tsx", L".py", L".rs", L".go", L".php",
    L".html", L".htm", L".css", L".scss", L".sql", L".ps1", L".bat",
    L".cmd", L".sh", L".qml", L".cmake", L".toml", L".properties",
    L".lpm"
};

inline constexpr std::wstring_view kImage[] = {
    L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff",
    L".webp", L".heic", L".heif", L".hif", L".avif", L".ico", L".cur"
};

inline constexpr std::wstring_view kRaw[] = {
    L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".sr2",
    L".dng", L".raf", L".orf", L".rw2", L".pef", L".srw", L".rwl"
};

// Media families describe file categories; decoder availability remains backend-specific.
inline constexpr std::wstring_view kVideo[] = {
    L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv", L".webm",
    L".mpg", L".mpeg", L".m2ts", L".mts", L".3gp"
};
inline constexpr std::wstring_view kAudio[] = {
    L".mp3", L".wav", L".flac", L".m4a", L".aac", L".wma", L".ogg",
    L".oga", L".opus", L".aif", L".aiff"
};

// Vector documents WIC cannot decode but Direct2D renders natively. They are
// deliberately not part of the image list: that list feeds the WIC decoder,
// and sending an SVG there only produced "image-decode-failed".
inline constexpr std::wstring_view kVector[] = { L".svg" };

// Windows metafiles: WIC only decodes them where its codec is installed and the
// shell exposes no thumbnail provider for them, so the host draws them with GDI.
// Kept out of the image list for the same reason as SVG.
inline constexpr std::wstring_view kMetaFile[] = { L".wmf", L".emf" };

// PDF documents, and Illustrator files saved with PDF compatibility (the
// default since CS), render through PDFium in the preview host. Listing them as
// native keeps the details pane and Quick Look on that fast first-page render
// instead of the heavyweight system PDF preview handler.
inline constexpr std::wstring_view kPdfRaster[] = { L".pdf", L".ai" };

// Archives list their contents as a browsable tree: ZIP from its central
// directory, the rest through the system's libarchive (archiveint.dll).
inline constexpr std::wstring_view kArchive[] = {
    L".zip", L".7z", L".rar", L".tar", L".tgz", L".gz", L".txz", L".xz",
    L".tbz2", L".bz2", L".cab", L".iso", L".wim", L".swm", L".esd", L".001", L".zipx", L".vhd", L".vhdx", L".vdi", L".vmdk",
    L".dmg", L".xar", L".rpm", L".deb", L".arj", L".lzh", L".lha", L".chm", L".udf"
};

// Photoshop documents: the merged composite (or the embedded JPEG thumbnail)
// decodes in the preview host, no Adobe software required.
inline constexpr std::wstring_view kPsd[] = { L".psd", L".psb" };

// Font files render a specimen page through DirectWrite without installing.
inline constexpr std::wstring_view kFont[] = { L".ttf", L".otf", L".ttc", L".otc" };

inline constexpr std::wstring_view kPe[] = { L".exe", L".dll", L".sys", L".ocx", L".scr", L".cpl" };

} // namespace formats

struct PreviewFormat {
    PreviewFamily family;
    const std::wstring_view* extensions;
    size_t count;
};

inline constexpr PreviewFormat kPreviewFormats[] = {
    { PreviewFamily::Text,      formats::kText,      std::size(formats::kText) },
    { PreviewFamily::Image,     formats::kImage,     std::size(formats::kImage) },
    { PreviewFamily::Vector,    formats::kVector,    std::size(formats::kVector) },
    { PreviewFamily::MetaFile,  formats::kMetaFile,  std::size(formats::kMetaFile) },
    { PreviewFamily::PdfRaster, formats::kPdfRaster, std::size(formats::kPdfRaster) },
    { PreviewFamily::Archive,   formats::kArchive,   std::size(formats::kArchive) },
    { PreviewFamily::Psd,       formats::kPsd,       std::size(formats::kPsd) },
    { PreviewFamily::Raw,       formats::kRaw,       std::size(formats::kRaw) },
    { PreviewFamily::Font,      formats::kFont,      std::size(formats::kFont) },
    { PreviewFamily::Pe,        formats::kPe,        std::size(formats::kPe) },
};

inline bool FormatHasExtension(const PreviewFormat& format, std::wstring_view extension) {
    for (size_t i = 0; i < format.count; ++i) {
        if (format.extensions[i] == extension) return true;
    }
    return false;
}

// Membership in one family. Families are independent lists, exactly like the
// separate functions they replaced.
inline bool IsFamilyExtension(PreviewFamily family, std::wstring_view extension) {
    for (const PreviewFormat& format : kPreviewFormats) {
        if (format.family == family && FormatHasExtension(format, extension)) return true;
    }
    return false;
}

inline bool IsTextExtension(std::wstring_view extension) {
    return IsFamilyExtension(PreviewFamily::Text, extension);
}

inline bool IsImageExtension(std::wstring_view extension) {
    return IsFamilyExtension(PreviewFamily::Image, extension);
}

inline bool IsVideoExtension(std::wstring_view extension) {
    for (const auto value : formats::kVideo) if (value == extension) return true;
    return false;
}
inline bool IsAudioExtension(std::wstring_view extension) {
    for (const auto value : formats::kAudio) if (value == extension) return true;
    return false;
}

inline bool IsVectorExtension(std::wstring_view extension) {
    return IsFamilyExtension(PreviewFamily::Vector, extension);
}

inline bool IsMetaFileExtension(std::wstring_view extension) {
    return IsFamilyExtension(PreviewFamily::MetaFile, extension);
}

inline bool IsPdfRasterExtension(std::wstring_view extension) {
    return IsFamilyExtension(PreviewFamily::PdfRaster, extension);
}

inline bool IsArchiveExtension(std::wstring_view extension) {
    return IsFamilyExtension(PreviewFamily::Archive, extension);
}

inline bool IsPsdExtension(std::wstring_view extension) {
    return IsFamilyExtension(PreviewFamily::Psd, extension);
}

inline bool IsFontExtension(std::wstring_view extension) {
    return IsFamilyExtension(PreviewFamily::Font, extension);
}

// Any row of kPreviewFormats: the host has its own decoder for the file.
inline bool IsNativeExtension(std::wstring_view extension) {
    for (const PreviewFormat& format : kPreviewFormats) {
        if (FormatHasExtension(format, extension)) return true;
    }
    return false;
}

} // namespace pulse::preview
