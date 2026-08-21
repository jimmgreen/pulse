#pragma once
// UTF-8 JSON/config files. MSVC's default std::wofstream uses the C locale and
// sets failbit on any wchar_t outside 0x00-0x7F, so Chinese tag names, UNC
// folders, and starred paths never reached disk.
#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>

namespace pulse {

inline bool DecodeUtf8Bytes(const std::vector<uint8_t>& bytes, std::wstring& text) {
    size_t offset = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        offset = 3;
    if (offset == bytes.size()) {
        text.clear();
        return true;
    }
    const int byte_count = static_cast<int>(bytes.size() - offset);
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           reinterpret_cast<const char*>(bytes.data() + offset),
                                           byte_count, nullptr, 0);
    if (chars <= 0) return false;
    text.resize(static_cast<size_t>(chars));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               reinterpret_cast<const char*>(bytes.data() + offset), byte_count,
                               text.data(), chars) == chars;
}

inline bool EncodeUtf8Bytes(const std::wstring& text, std::vector<uint8_t>& bytes) {
    if (text.empty()) {
        bytes.clear();
        return true;
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    bytes.resize(static_cast<size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()),
                               reinterpret_cast<char*>(bytes.data()), size,
                               nullptr, nullptr) == size;
}

inline bool ReadUtf8File(const std::wstring& path, std::wstring& text) {
    text.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    constexpr uint64_t kMaxBytes = 16ull * 1024 * 1024;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxBytes) {
        CloseHandle(file);
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD read = 0;
        const DWORD remaining = static_cast<DWORD>((std::min)(
            bytes.size() - done, static_cast<size_t>(UINT32_MAX)));
        if (!ReadFile(file, bytes.data() + done, remaining, &read, nullptr) || read == 0) {
            CloseHandle(file);
            return false;
        }
        done += read;
    }
    CloseHandle(file);
    return DecodeUtf8Bytes(bytes, text);
}

inline bool WriteUtf8FileAtomic(const std::wstring& path, const std::wstring& text) {
    std::vector<uint8_t> bytes;
    if (!EncodeUtf8Bytes(text, bytes)) return false;
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>((std::min)(
            bytes.size() - done, static_cast<size_t>(UINT32_MAX)));
        if (!WriteFile(file, bytes.data() + done, remaining, &written, nullptr) || written == 0) {
            CloseHandle(file);
            DeleteFileW(temp.c_str());
            return false;
        }
        done += written;
    }
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!flushed) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

} // namespace pulse
