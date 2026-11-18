#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <string_view>
#include <vector>
#pragma comment(lib, "bcrypt.lib")
namespace pulse::extensions {
namespace revision_detail {
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};
}
inline std::string PackageRevision(std::string_view bytes) {
    revision_detail::Hash state;
    if (BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD object_size = 0;
    DWORD count = 0;
    if (BCryptGetProperty(state.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size), &count, 0) < 0) return {};
    state.object.resize(object_size);
    if (BCryptCreateHash(state.algorithm, &state.hash, state.object.data(), object_size, nullptr, 0, 0) < 0 ||
        BCryptHashData(state.hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0) return {};
    UCHAR digest[32]{};
    if (BCryptFinishHash(state.hash, digest, sizeof(digest), 0) < 0) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string revision;
    revision.reserve(64);
    for (const auto byte : digest) { revision.push_back(hex[byte >> 4]); revision.push_back(hex[byte & 15]); }
    return revision;
}

}
