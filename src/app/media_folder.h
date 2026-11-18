#pragma once
// 媒体文件夹专用列: which folders get them and how their values read.
// Pure (names and numbers in, text out) so the worker, the UI and the tests
// share one definition.
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace pulse::app::media {

enum class Kind : uint8_t { None = 0, Photo = 1, Music = 2, Video = 3 };

namespace detail {
inline bool ExtIs(std::wstring_view ext, const wchar_t* const* list) noexcept {
    for (; *list; ++list) {
        const std::wstring_view candidate(*list);
        if (candidate.size() != ext.size()) continue;
        bool same = true;
        for (size_t i = 0; i < ext.size() && same; ++i) {
            wchar_t c = ext[i];
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            same = c == candidate[i];
        }
        if (same) return true;
    }
    return false;
}
}  // namespace detail

// By extension only: classification must never open files.
inline Kind KindOfName(std::wstring_view name) noexcept {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || dot + 1 >= name.size()) return Kind::None;
    const std::wstring_view ext = name.substr(dot + 1);
    static const wchar_t* const kPhoto[] = {L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"heic", L"heif", L"hif",
        L"webp", L"avif", L"tif", L"tiff", L"bmp", L"gif", L"jxl", L"dng", L"cr2", L"cr3", L"nef", L"arw",
        L"orf", L"rw2", L"raf", L"srw", L"pef", nullptr};
    static const wchar_t* const kMusic[] = {L"mp3", L"flac", L"m4a", L"aac", L"wav", L"ogg", L"oga", L"opus",
        L"wma", L"ape", L"aif", L"aiff", L"alac", L"wv", L"dsf", L"dff", nullptr};
    static const wchar_t* const kVideo[] = {L"mp4", L"m4v", L"mkv", L"mov", L"avi", L"wmv", L"webm", L"ts",
        L"m2ts", L"mts", L"flv", L"mpg", L"mpeg", L"3gp", L"vob", L"ogv", L"rmvb", L"rm", L"asf", nullptr};
    if (detail::ExtIs(ext, kPhoto)) return Kind::Photo;
    if (detail::ExtIs(ext, kMusic)) return Kind::Music;
    if (detail::ExtIs(ext, kVideo)) return Kind::Video;
    return Kind::None;
}

struct Tally {
    uint32_t files = 0, photo = 0, music = 0, video = 0;
    void Add(std::wstring_view name, bool is_dir) noexcept {
        if (is_dir) return;
        ++files;
        switch (KindOfName(name)) {
        case Kind::Photo: ++photo; break;
        case Kind::Music: ++music; break;
        case Kind::Video: ++video; break;
        default: break;
        }
    }
};

// At least kMinFiles files, and one kind makes up at least 60% of them
// (folders do not count either way).
constexpr uint32_t kMinFiles = 5;
inline Kind Classify(const Tally& t) noexcept {
    if (t.files < kMinFiles) return Kind::None;
    const uint32_t top = t.photo >= t.music && t.photo >= t.video ? t.photo
                       : t.music >= t.video ? t.music : t.video;
    if (top == 0 || uint64_t{top} * 5u < uint64_t{t.files} * 3u) return Kind::None;
    if (top == t.photo) return Kind::Photo;
    if (top == t.music) return Kind::Music;
    return Kind::Video;
}

// 3:07, 1:02:03; empty when unknown.
inline std::wstring FormatDuration(uint64_t duration_100ns) {
    if (duration_100ns == 0) return {};
    const uint64_t total = (duration_100ns + 5'000'000u) / 10'000'000u;   // nearest second
    const uint64_t h = total / 3600u, m = (total / 60u) % 60u, s = total % 60u;
    wchar_t text[32];
    if (h) std::swprintf(text, 32, L"%llu:%02llu:%02llu", static_cast<unsigned long long>(h),
                         static_cast<unsigned long long>(m), static_cast<unsigned long long>(s));
    else std::swprintf(text, 32, L"%llu:%02llu", static_cast<unsigned long long>(m),
                       static_cast<unsigned long long>(s));
    return text;
}

// 4000 × 3000; empty when unknown.
inline std::wstring FormatDimensions(uint32_t w, uint32_t h) {
    if (!w || !h) return {};
    return std::to_wstring(w) + L" \x00D7 " + std::to_wstring(h);
}

// Frames per 1000 s (PKEY_Video_FrameRate) → 30, 29.97, 23.98; empty when unknown.
inline std::wstring FormatFrameRate(uint32_t fps_milli) {
    if (!fps_milli) return {};
    const uint32_t hundredths = (fps_milli + 5u) / 10u;
    wchar_t text[24];
    if (hundredths % 100u == 0) std::swprintf(text, 24, L"%u", hundredths / 100u);
    else if (hundredths % 10u == 0) std::swprintf(text, 24, L"%u.%u", hundredths / 100u, (hundredths / 10u) % 10u);
    else std::swprintf(text, 24, L"%u.%02u", hundredths / 100u, hundredths % 100u);
    return text;
}

// Video grouping: by the short edge, or the long one for very wide frames
// (3840 × 1600 is still 4K). Unknown sorts last.
enum class Tier : uint8_t { Uhd = 0, FullHd = 1, Hd = 2, Sd = 3, Unknown = 4 };
inline Tier TierOf(uint32_t w, uint32_t h) noexcept {
    if (!w || !h) return Tier::Unknown;
    const uint32_t s = w < h ? w : h, l = w < h ? h : w;
    if (s >= 2000 || l >= 3800) return Tier::Uhd;
    if (s >= 1000 || l >= 1900) return Tier::FullHd;
    if (s >= 700 || l >= 1260) return Tier::Hd;
    return Tier::Sd;
}

// Orders two optional values with unknown ones last whatever the direction:
// returns <0 / 0 / >0 for a before / same / after b in an ascending list,
// and `unknown` reports that at least one side is missing (the caller must
// not reverse such a result for a descending sort).
template <class T>
int CompareKnownFirst(bool a_known, const T& a, bool b_known, const T& b, bool& unknown) noexcept {
    unknown = !a_known || !b_known;
    if (!a_known && !b_known) return 0;
    if (!a_known) return 1;
    if (!b_known) return -1;
    return a < b ? -1 : b < a ? 1 : 0;
}

}  // namespace pulse::app::media
