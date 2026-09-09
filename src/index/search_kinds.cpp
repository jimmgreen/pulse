#include "search_kinds.h"
#include "index_query.h"

namespace pulse::index {
namespace {

void SplitExts(std::wstring_view raw, std::vector<std::wstring>& out) {
    std::wstring s = Fold(raw);
    size_t i = 0;
    while (i < s.size()) {
        size_t sep = s.find_first_of(L";,", i);
        if (sep == std::wstring::npos) sep = s.size();
        std::wstring one = s.substr(i, sep - i);
        if (!one.empty() && one[0] == L'.') one.erase(0, 1);
        if (!one.empty()) out.push_back(std::move(one));
        i = sep + 1;
    }
}

} // namespace

const wchar_t* KindId(SearchKind kind) {
    switch (kind) {
    case SearchKind::Folder: return L"folder";
    case SearchKind::Document: return L"document";
    case SearchKind::Image: return L"image";
    case SearchKind::Video: return L"video";
    case SearchKind::Audio: return L"audio";
    case SearchKind::Archive: return L"archive";
    case SearchKind::Code: return L"code";
    case SearchKind::Custom: return L"custom";
    default: return L"";
    }
}

SearchKind ParseKindId(std::wstring_view raw) {
    const std::wstring s = Fold(raw);
    if (s.empty() || s == L"any" || s == L"all" || s == L"任意") return SearchKind::Any;
    if (s == L"folder" || s == L"folders" || s == L"dir" || s == L"文件夹") return SearchKind::Folder;
    if (s == L"document" || s == L"documents" || s == L"doc" || s == L"文档") return SearchKind::Document;
    if (s == L"image" || s == L"images" || s == L"picture" || s == L"pic" || s == L"图片")
        return SearchKind::Image;
    if (s == L"video" || s == L"videos" || s == L"movie" || s == L"视频") return SearchKind::Video;
    if (s == L"audio" || s == L"music" || s == L"音频") return SearchKind::Audio;
    if (s == L"archive" || s == L"archives" || s == L"zip" || s == L"压缩" || s == L"压缩包")
        return SearchKind::Archive;
    if (s == L"code" || s == L"source" || s == L"代码") return SearchKind::Code;
    if (s == L"custom" || s == L"自定义") return SearchKind::Custom;
    return SearchKind::Custom;
}

std::wstring KindExtensions(SearchKind kind) {
    switch (kind) {
    case SearchKind::Document:
        return L"pdf;doc;docx;xls;xlsx;ppt;pptx;txt;md;rtf;odt;ods;odp;csv";
    case SearchKind::Image:
        return L"jpg;jpeg;png;gif;webp;bmp;svg;heic;tif;tiff;ico;raw";
    case SearchKind::Video:
        return L"mp4;mkv;avi;mov;wmv;webm;flv;m4v;mpeg;mpg";
    case SearchKind::Audio:
        return L"mp3;wav;flac;aac;ogg;m4a;wma;aiff;opus";
    case SearchKind::Archive:
        return L"zip;7z;rar;tar;gz;tgz;iso;cab;xz;bz2";
    case SearchKind::Code:
        return L"cpp;c;h;hpp;cc;cxx;cs;java;js;jsx;ts;tsx;py;rs;go;php;html;htm;css;scss;"
               L"sql;ps1;json;xml;yml;yaml;cmake;toml;qml";
    default:
        return {};
    }
}

bool ExpandKindToken(std::wstring_view raw, std::vector<std::wstring>& exts, bool& folders_only) {
    folders_only = false;
    const SearchKind kind = ParseKindId(raw);
    if (kind == SearchKind::Any) return true;
    if (kind == SearchKind::Folder) {
        folders_only = true;
        return true;
    }
    if (kind == SearchKind::Custom) {
        SplitExts(raw, exts);
        return !exts.empty();
    }
    SplitExts(KindExtensions(kind), exts);
    return !exts.empty();
}

std::wstring TextExtensionQueryToken() {
    return L"ext:txt;md;log;json;xml;yaml;yml;ini;cfg;conf;csv;tsv;cpp;c;h;hpp;cc;cxx;cs;"
           L"java;js;jsx;ts;tsx;py;rs;go;php;html;htm;css;scss;sql;ps1;bat;cmd;sh;qml;"
           L"cmake;toml;properties";
}

} // namespace pulse::index
