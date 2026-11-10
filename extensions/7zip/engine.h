// engine.h - 7-Zip engine for the Pulse 7-Zip extension. Every function here
// blocks: call it on a worker. No function shows UI; passwords arrive through
// the AskPassword callback, which the caller may route to the UI thread.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine {

enum class Status { Ok, Cancelled, NeedPassword, WrongPassword, Unsupported, InvalidArchive, IoError, LimitReached, InvalidInput };

struct Entry {
    std::wstring path;      // archive-relative, '\' separators
    uint64_t size = 0, packed = 0;
    uint64_t modified = 0;  // FILETIME ticks, 0 when absent
    uint32_t attributes = 0;
    bool dir = false, encrypted = false, link = false;
};

struct Listing {
    Status status = Status::InvalidArchive;
    std::string error;
    std::wstring format;    // 7-Zip handler name: 7z, zip, Rar5, tar ...
    std::vector<Entry> entries;
    bool truncated = false; // stopped at max_entries
    bool nested = false;    // opened through an outer format (tar.gz, split volumes)
};

struct Progress {
    std::wstring current;
    uint64_t completed_bytes = 0, total_bytes = 0, completed_items = 0, total_items = 0;
};

using Cancelled = std::function<bool()>;
using Report = std::function<void(const Progress&)>;
// Returns false to cancel. `retry` is true after a wrong password.
using AskPassword = std::function<bool(bool retry, std::wstring& password)>;

// Formats 7z.dll can open, as "name: ext ext ..." lines (diagnostics, settings page).
std::vector<std::wstring> SupportedFormats();
// Lowercase dotted extensions the context-menu commands register (at most 32).
std::vector<std::string> ArchiveExtensions();

Listing List(const std::wstring& path, const Cancelled& cancelled, const AskPassword& password,
             size_t max_entries = SIZE_MAX);

struct ExtractResult {
    Status status = Status::IoError;
    std::string error;
    uint64_t files = 0, bytes = 0, skipped_links = 0;
};
// Into an existing empty directory. Never overwrites, never follows or creates
// links. test_only: decode and check every item, write nothing.
ExtractResult Extract(const std::wstring& path, const std::wstring& destination, bool test_only,
                      const Cancelled& cancelled, const Report& report, const AskPassword& password);

enum class Format { SevenZip, Zip, Tar, TarGz, TarXz, TarBz2 };
struct CreateOptions {
    Format format = Format::SevenZip;
    unsigned level = 5;             // 0 store, 1 fastest, 3 fast, 5 normal, 7 maximum, 9 ultra
    bool solid = true;              // 7z
    std::wstring password;          // empty: no encryption (7z, zip)
    bool encrypt_names = false;     // 7z: header encryption
    bool zip_aes = true;            // zip: AES-256 (false: ZipCrypto)
    uint64_t volume_size = 0;       // bytes; 0 = one file (.001 .002 ... otherwise)
};
struct CreateResult {
    Status status = Status::IoError;
    std::string error;
    std::vector<std::wstring> outputs;  // files written into the destination
    uint64_t files = 0, bytes = 0, skipped = 0;
};
// `name` is the archive file name with its extension; written into the existing
// empty directory `destination`.
CreateResult Create(const std::vector<std::wstring>& sources, const std::wstring& destination,
                    const std::wstring& name, const CreateOptions& options,
                    const Cancelled& cancelled, const Report& report);
std::wstring FormatExtension(Format format);  // ".7z", ".tar.gz" ...

struct HashResult {
    std::wstring path;
    uint64_t size = 0;
    std::string crc32, sha1, sha256;  // lowercase hex
    std::string error;
};
std::vector<HashResult> Hash(const std::vector<std::wstring>& files, const Cancelled& cancelled, const Report& report);

// Archive-relative name check shared by extraction and tests.
bool SafeRelativePath(const std::wstring& path);
} // namespace engine
