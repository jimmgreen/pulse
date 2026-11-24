#pragma once
// Reads the solid payload appended to the setup bootstrapper
// (format: tools/pack_installer/pack_installer.py).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulse::setup {

struct PayloadFile {
    std::wstring path;      // relative, forward slashes converted to backslashes
    uint64_t size = 0;
    std::string sha256;     // lowercase hex
    std::optional<size_t> link_to;  // earlier canonical member in compact payloads
};

struct PayloadInfo {
    std::string version;
    std::string channel;              // "windows" | "win81" (empty in old payloads)
    uint64_t min_windows_build = 0;   // 0 = no check
    std::vector<PayloadFile> files;
    uint64_t payload_offset = 0;
    uint64_t payload_size = 0;
    uint64_t total_bytes = 0;
    uint64_t stored_bytes = 0;  // decompressed and written; hard-link aliases excluded
};

struct ExtractProgress {
    uint64_t done_bytes = 0;
    uint64_t total_bytes = 0;
    const PayloadFile* current = nullptr;
};

// Return false from the callback to cancel.
using ProgressCallback = std::function<bool(const ExtractProgress&)>;

class Payload {
public:
    // Opens `image` (normally the running exe), validates the trailer and the
    // SHA-256 of the compressed stream. Returns false and fills `error` on failure.
    bool Open(const std::filesystem::path& image, std::wstring& error);
    const PayloadInfo& Info() const { return info_; }

    // Streams the payload into `target`, verifying each file's SHA-256 before
    // it is renamed into place. Existing files are replaced.
    bool ExtractTo(const std::filesystem::path& target, const ProgressCallback& progress,
                   std::wstring& error);
    // Only count physically shared bytes, including when a version is reused.
    uint64_t SharedBytesAt(const std::filesystem::path& target) const;

private:
    bool ReadManifest(std::wstring& error);
    std::filesystem::path image_;
    PayloadInfo info_;
};

}  // namespace pulse::setup
