#pragma once
#include <windows.h>
#include "../common/rename_filename.h"
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <type_traits>

namespace pulse::elevated {
inline constexpr uint32_t kMagic = 0x45504c53;
inline constexpr uint16_t kVersion = 7;
inline constexpr uint32_t kMaxPayload = 1024 * 1024;
inline constexpr uint32_t kMaxPaths = 4096;
inline constexpr uint32_t kMaxString = 32760;
// v3 Transfer appends an explicit single-root Move leaf name (empty = basename).
// Old peers fail the version handshake; never reinterpret a different payload.
// v7 appends an explicit enumeration-complete flag to status payloads.
// v6 adds Transfer mode 2: no-overwrite rename with source and parent file identities.
// v5 adds identity-bound virtual-disk ejection (private elevated protocol only).
// v4 adds a single absolute-path CreateFolder request, without Shell dialogs.
inline bool SafeTransferName(const std::wstring& name, size_t count, bool move) {
    return name.empty() || (move && count == 1 && name.size() <= kMaxString && pulse::IsRenameFilename(name));
}
using Nonce = std::array<unsigned char, 16>;
enum class Kind : uint16_t { Hello = 1, Transfer, Cancel, Pause, Resume, ConflictReply, Progress, Conflict, CompletedItem, Result, Shutdown, RecycleDelete, PermanentDelete, DeletedItem, CreateFolder, EjectVirtualDisk };
#pragma pack(push, 1)
struct Header {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    Kind kind = Kind::Hello;
    uint32_t bytes = 0;
    uint64_t request = 0;
    Nonce nonce{};
};
#pragma pack(pop)
static_assert(sizeof(Header) == 36);
inline bool ValidHeader(const Header& header, const Nonce& nonce) {
    return header.magic == kMagic && header.version == kVersion && header.bytes <= kMaxPayload &&
        header.nonce == nonce && header.kind >= Kind::Hello && header.kind <= Kind::EjectVirtualDisk;
}
struct Writer {
    std::vector<unsigned char> bytes;
    bool good = true;
    template<class T> void Number(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (bytes.size() + sizeof(T) > kMaxPayload) { good = false; return; }
        const auto* raw = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), raw, raw + sizeof(T));
    }
    void Text(const std::wstring& value) {
        if (value.size() > kMaxString || bytes.size() + 4 + value.size() * sizeof(wchar_t) > kMaxPayload) { good = false; return; }
        Number(static_cast<uint32_t>(value.size()));
        const auto* raw = reinterpret_cast<const unsigned char*>(value.data());
        bytes.insert(bytes.end(), raw, raw + value.size() * sizeof(wchar_t));
    }
};
struct Reader {
    const std::vector<unsigned char>& bytes;
    size_t offset = 0;
    bool good = true;
    template<class T> T Number() {
        static_assert(std::is_trivially_copyable_v<T>);
        T result{};
        if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) { good = false; return result; }
        std::memcpy(&result, bytes.data() + offset, sizeof(T)); offset += sizeof(T); return result;
    }
    std::wstring Text() {
        const auto count = Number<uint32_t>();
        if (!good || count > kMaxString || offset > bytes.size() || bytes.size() - offset < count * sizeof(wchar_t)) { good = false; return {}; }
        std::wstring result(count, L'\0');
        std::memcpy(result.data(), bytes.data() + offset, count * sizeof(wchar_t));
        offset += count * sizeof(wchar_t);
        if (result.find(L'\0') != std::wstring::npos) good = false;
        return result;
    }
    bool Done() const { return good && offset == bytes.size(); }
};
inline bool SafePath(std::wstring value) {
    if (value.empty() || value.size() > kMaxString) return false;
    if (value.starts_with(L"\\\\?\\UNC\\")) value = L"\\\\" + value.substr(8);
    else if (value.starts_with(L"\\\\?\\")) value = value.substr(4);
    const bool drive = value.size() >= 3 && ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) && value[1] == L':' && value[2] == L'\\';
    const bool unc = value.starts_with(L"\\\\") && value.size() > 4 && value[2] != L'.' && value[2] != L'?';
    if (!drive && !unc) return false;
    for (size_t i = 0; i < value.size(); ++i)
        if (value[i] < 32 || value[i] == L'"' || value[i] == L'*' || value[i] == L'?' || value[i] == L'/' || (value[i] == L':' && !(drive && i == 1))) return false;
    size_t start = drive ? 3 : 2;
    unsigned components = 0;
    while (start < value.size()) {
        const size_t end = value.find(L'\\', start);
        auto part = value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        ++components;
        const auto dot = part.find(L'.'); if (dot != std::wstring::npos) part.resize(dot);
        std::transform(part.begin(), part.end(), part.begin(), [](wchar_t c) { return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c; });
        if (part == L"CON" || part == L"PRN" || part == L"AUX" || part == L"NUL" || (unc && components == 2 && part == L"PIPE") ||
            (part.size() == 4 && (part.starts_with(L"COM") || part.starts_with(L"LPT")) && part[3] >= L'1' && part[3] <= L'9')) return false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return !unc || components >= 2;
}
inline std::wstring NonceText(const Nonce& nonce) {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring text; text.reserve(32);
    for (auto value : nonce) { text += hex[value >> 4]; text += hex[value & 15]; }
    return text;
}
inline bool ParseNonce(const std::wstring& text, Nonce& nonce) {
    if (text.size() != 32) return false;
    for (size_t i = 0; i < nonce.size(); ++i) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; ++j) {
            const wchar_t c = text[i * 2 + j];
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
            value = value * 16 + static_cast<unsigned>(c <= L'9' ? c - L'0' : c - L'a' + 10);
        }
        nonce[i] = static_cast<unsigned char>(value);
    }
    return true;
}
}
