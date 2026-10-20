#pragma once
// Preview pack releases this build of Pulse installs. Every file is pinned by
// size and SHA-256 here, so a download needs no signature or manifest: if the
// bytes do not match, they are thrown away. scripts/package_preview_packs.py
// generates the catalog and matching assets for a shared installer release.
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace pulse::app {

struct PackFile {
    const wchar_t* name;           // file name inside the pack directory
    uint64_t size;                 // after decompression
    const wchar_t* sha256;         // of the decompressed file
    uint64_t packed_size;          // compressed release asset size
    const wchar_t* packed_sha256;  // compressed release asset hash
    const wchar_t* download_name = nullptr; // release asset name; null means <name>.lzms
};

struct PackRelease {
    const wchar_t* key;            // packs::PackKey, the directory under packs\ .
    const wchar_t* version;        // also the version directory name
    const wchar_t* base_url;       // release download directory, ending in '/'
    const PackFile* files;
    size_t file_count;             // 0 while the pack is not published
};

#if defined(_M_ARM64EC)
#error Pulse preview packs require a native x64 or ARM64 process target
#elif defined(_M_ARM64) || defined(__aarch64__)
#include "pack_catalog_arm64_generated.h"
inline constexpr const PackRelease& kMediaPackRelease = kMediaPackArm64Release;
inline constexpr const PackRelease& kImagePackRelease = kImagePackArm64Release;
inline constexpr const PackRelease& kRawPackRelease = kRawPackArm64Release;
inline constexpr const PackRelease& kArchivePackRelease = kArchivePackArm64Release;
#elif defined(_M_X64) || defined(__x86_64__)
#include "pack_catalog_generated.h"
#else
#error Pulse preview packs require a native x64 or ARM64 process target
#endif

} // namespace pulse::app
