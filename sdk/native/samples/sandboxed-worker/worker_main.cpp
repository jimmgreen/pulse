#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t kMaxFrameBytes = 4u * 1024u * 1024u;

bool ReadExact(HANDLE pipe, void* out, uint32_t total) noexcept {
    auto* cursor = static_cast<uint8_t*>(out);
    uint32_t remaining = total;
    while (remaining > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, cursor, remaining, &got, nullptr) || got == 0) {
            return false;
        }
        cursor += got;
        remaining -= got;
    }
    return true;
}

bool WriteExact(HANDLE pipe, const void* data, uint32_t total) noexcept {
    const auto* cursor = static_cast<const uint8_t*>(data);
    uint32_t remaining = total;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool WriteFrame(HANDLE pipe, std::string_view payload) noexcept {
    if (payload.size() > kMaxFrameBytes) {
        return false;
    }
    const uint32_t len = static_cast<uint32_t>(payload.size());
    if (!WriteExact(pipe, &len, sizeof(len))) {
        return false;
    }
    return len == 0 || WriteExact(pipe, payload.data(), len);
}

std::wstring WideFromUtf8(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        wide.data(),
        needed);
    return wide;
}

std::string HexDigest(const std::array<uint8_t, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out.push_back(kHex[(b >> 4) & 0x0Fu]);
        out.push_back(kHex[b & 0x0Fu]);
    }
    return out;
}

bool HashMemory(std::string_view bytes, std::string& hex_out) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::array<uint8_t, 32> digest{};
        if (BCryptHashData(
                hash,
                reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                static_cast<ULONG>(bytes.size()),
                0) == 0 &&
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
            hex_out = HexDigest(digest);
            ok = true;
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool HashFile(const std::wstring& path, uint64_t& size_out, std::string& hex_out) {
    size_out = 0;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        const std::wstring tag = L"directory:" + path;
        return HashMemory(
            std::string_view(reinterpret_cast<const char*>(tag.data()), tag.size() * sizeof(wchar_t)),
            hex_out);
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::array<uint8_t, 65536> chunk{};
            DWORD got = 0;
            bool read_ok = true;
            for (;;) {
                if (!ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) {
                    read_ok = false;
                    break;
                }
                if (got == 0) {
                    break;
                }
                size_out += got;
                if (BCryptHashData(hash, chunk.data(), got, 0) != 0) {
                    read_ok = false;
                    break;
                }
            }
            std::array<uint8_t, 32> digest{};
            if (read_ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
                hex_out = HexDigest(digest);
                ok = true;
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }

    CloseHandle(file);
    return ok;
}

} // namespace

int RunWorkerMain() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    HANDLE in_pipe = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out_pipe = GetStdHandle(STD_OUTPUT_HANDLE);
    if (in_pipe == INVALID_HANDLE_VALUE || in_pipe == nullptr ||
        out_pipe == INVALID_HANDLE_VALUE || out_pipe == nullptr) {
        return 2;
    }

    uint32_t req_len = 0;
    if (!ReadExact(in_pipe, &req_len, sizeof(req_len)) || req_len > kMaxFrameBytes) {
        return 2;
    }
    std::string payload(req_len, '\0');
    if (req_len > 0 && !ReadExact(in_pipe, payload.data(), req_len)) {
        return 2;
    }

    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < payload.size()) {
        const size_t nl = payload.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? payload.size() : nl;
        if (end > pos) {
            std::string line = payload.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }

    if (lines.empty()) {
        static constexpr std::string_view kSelfTest = "pulse-sandboxed-worker-selftest";
        std::string digest;
        if (!HashMemory(kSelfTest, digest)) {
            WriteFrame(out_pipe, "ERR\tbcrypt_failed");
            return 3;
        }
        const std::string reply = "OK\t0\t" + std::to_string(kSelfTest.size()) + "\t" + digest;
        return WriteFrame(out_pipe, reply) ? 0 : 4;
    }

    uint64_t total_bytes = 0;
    std::string first_digest;
    for (const auto& utf8_path : lines) {
        const std::wstring wide = WideFromUtf8(utf8_path);
        uint64_t item_bytes = 0;
        std::string digest;
        if (wide.empty() || !HashFile(wide, item_bytes, digest)) {
            WriteFrame(out_pipe, "ERR\tfile_read_failed");
            return 3;
        }
        total_bytes += item_bytes;
        if (first_digest.empty()) {
            first_digest = std::move(digest);
        }
    }

    const std::string reply =
        "OK\t" + std::to_string(lines.size()) + "\t" + std::to_string(total_bytes) + "\t" + first_digest;
    return WriteFrame(out_pipe, reply) ? 0 : 4;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return RunWorkerMain();
}
