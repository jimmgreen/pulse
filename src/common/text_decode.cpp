#include "text_decode.h"

#include <algorithm>
#include <cwctype>

namespace pulse::text {
namespace {

constexpr DWORD kRecallOnOpen = 0x00040000;
constexpr DWORD kRecallOnData = 0x00400000;
constexpr DWORD kPinned = 0x00080000;

bool IsOneOf(std::wstring_view extension,
             std::initializer_list<std::wstring_view> values) noexcept {
    return std::find(values.begin(), values.end(), extension) != values.end();
}

} // namespace

bool IsOfflinePlaceholder(DWORD attributes) noexcept {
    return (attributes & (kRecallOnOpen | kRecallOnData)) && !(attributes & kPinned);
}

bool IsKnownTextExtension(std::wstring_view extension) noexcept {
    return IsOneOf(extension, {
        L".txt", L".md", L".log", L".json", L".xml", L".yaml", L".yml",
        L".ini", L".cfg", L".conf", L".csv", L".tsv", L".cpp", L".c",
        L".h", L".hpp", L".cc", L".cxx", L".cs", L".java", L".js",
        L".jsx", L".ts", L".tsx", L".py", L".rs", L".go", L".php",
        L".html", L".htm", L".css", L".scss", L".sql", L".ps1", L".bat",
        L".cmd", L".sh", L".qml", L".cmake", L".toml", L".properties"
    });
}

bool LooksBinary(const std::vector<uint8_t>& bytes) noexcept {
    if (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
                             (bytes[0] == 0xFE && bytes[1] == 0xFF))) return false;
    size_t controls = 0;
    const size_t sample = (std::min)(bytes.size(), static_cast<size_t>(64 * 1024));
    for (size_t i = 0; i < sample; ++i) {
        const uint8_t c = bytes[i];
        if (c == 0) return true;
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ++controls;
    }
    return sample > 0 && controls * 20 > sample;
}

bool Decode(const std::vector<uint8_t>& bytes, std::wstring& output) {
    output.clear();
    if (bytes.empty()) return true;
    size_t offset = 0;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        offset = 2;
        output.reserve((bytes.size() - offset) / 2);
        for (size_t i = offset; i + 1 < bytes.size(); i += 2)
            output.push_back(static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8)));
        return true;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        offset = 2;
        output.reserve((bytes.size() - offset) / 2);
        for (size_t i = offset; i + 1 < bytes.size(); i += 2)
            output.push_back(static_cast<wchar_t>((bytes[i] << 8) | bytes[i + 1]));
        return true;
    }
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        offset = 3;
    const char* raw = reinterpret_cast<const char*>(bytes.data() + offset);
    const int raw_size = static_cast<int>(bytes.size() - offset);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int chars = MultiByteToWideChar(code_page, flags, raw, raw_size, nullptr, 0);
    if (chars <= 0) {
        code_page = CP_ACP;
        flags = 0;
        chars = MultiByteToWideChar(code_page, flags, raw, raw_size, nullptr, 0);
    }
    if (chars <= 0) return false;
    output.resize(chars);
    return MultiByteToWideChar(code_page, flags, raw, raw_size, output.data(), chars) == chars;
}

bool ReadFile(const std::wstring& path, uint64_t maximum_bytes, std::wstring& output,
              uint64_t& bytes_read, DWORD* error) {
    output.clear();
    bytes_read = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > maximum_bytes) {
        if (error) *error = size.QuadPart > 0 ? ERROR_FILE_TOO_LARGE : GetLastError();
        CloseHandle(file);
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - done,
                                                          static_cast<size_t>(UINT32_MAX)));
        if (!::ReadFile(file, bytes.data() + done, chunk, &read, nullptr) || read == 0) {
            if (error) *error = GetLastError();
            CloseHandle(file);
            return false;
        }
        done += read;
    }
    CloseHandle(file);
    bytes_read = done;
    if (LooksBinary(bytes)) {
        if (error) *error = ERROR_BAD_FORMAT;
        return false;
    }
    return Decode(bytes, output);
}

} // namespace pulse::text
