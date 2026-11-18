#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions {

inline constexpr size_t kMaxManifestBytes = 64 * 1024;
inline constexpr size_t kMaxIdBytes = 64;
inline constexpr size_t kMaxLabelBytes = 128;
inline constexpr size_t kMaxResources = 15;

struct NativeFile {
    std::string path;
    std::string sha256;
};

struct NativeRuntime {
    int version = 1;
    std::string architecture;
    std::string library;
    std::string sha256;
    std::vector<NativeFile> files;
};

struct Manifest {
    int api_version = 1;
    std::optional<NativeRuntime> native;
    std::string id;
    std::string name;
    std::vector<std::string> resources;
};

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string error;
    explicit operator bool() const noexcept { return manifest.has_value(); }
};

// Labels are UTF-8; limits apply to bytes. IDs use ASCII letters, digits, ._-.
ManifestResult ParseManifest(std::string_view utf8);

} // namespace pulse::extensions
