#include "developer_cli.h"
#include "developer_contract.h"
#include "manifest.h"
#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <string>

namespace pulse::extensions {
namespace {
DeveloperCliResult Validation(bool valid, std::string code, std::string error, std::wstring_view path) {
    DeveloperCliResult result;
    result.exit_code = valid ? 0 : 2;
    result.output = json::Value::Object();
    result.output.set("valid", valid);
    result.output.set("error", std::move(error));
    result.output.set("code", std::move(code));
    result.output.set("path", json::Utf8(path));
    return result;
}
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}

std::optional<DeveloperCliResult> EvaluateDeveloperCli(int argc, wchar_t** argv) {
    int api_index = -1;
    int validate_index = -1;
    int flags = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--extension-api") == 0) { api_index = i; ++flags; }
        if (std::wcscmp(argv[i], L"--extension-validate-json") == 0) { validate_index = i; ++flags; }
    }
    if (!flags) return std::nullopt;
    // A final pathless check makes older hosts fail closed instead of opening UI.
    const bool compatibility_guard = argc >= 3 && std::wcscmp(argv[argc - 1], L"--extension-check") == 0;
    const int effective_argc = argc - (compatibility_guard ? 1 : 0);
    const std::wstring_view path = validate_index >= 0 && validate_index + 1 < argc
        ? std::wstring_view(argv[validate_index + 1]) : std::wstring_view();
    if (flags != 1 || (api_index >= 0 && (effective_argc != 2 || api_index != 1)) ||
        (validate_index >= 0 && (effective_argc != 3 || validate_index != 1)))
        return Validation(false, "invalid_args", "Use --extension-api or --extension-validate-json with one absolute path; only a final pathless --extension-check compatibility guard may be appended.", path);
    if (api_index >= 0) return DeveloperCliResult{DescribeExtensionApi(), 0};
    try {
        const std::filesystem::path file_path(path);
        if (path.empty() || !file_path.is_absolute())
            return Validation(false, "invalid_path", "The manifest path must be absolute.", path);
        FileHandle file{CreateFileW(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE || GetFileType(file.value) != FILE_TYPE_DISK)
            return Validation(false, "read_failed", "Cannot open the manifest file for reading.", path);
        // Read at most one byte beyond the limit, including if the file grows.
        std::string bytes(kMaxManifestBytes + 1, '\0');
        size_t used = 0;
        while (used < bytes.size()) {
            DWORD received = 0;
            if (!ReadFile(file.value, bytes.data() + used, static_cast<DWORD>(bytes.size() - used), &received, nullptr))
                return Validation(false, "read_failed", "Cannot read the manifest file.", path);
            if (!received) break;
            used += received;
        }
        if (used > kMaxManifestBytes)
            return Validation(false, "too_large", "The manifest exceeds the 65536-byte limit.", path);
        bytes.resize(used);
        const auto parsed = ParseManifest(bytes);
        if (!parsed) return Validation(false, "invalid_manifest", parsed.error, path);
        return Validation(true, "", "", path);
    } catch (const std::filesystem::filesystem_error&) {
        return Validation(false, "invalid_path", "The manifest path is invalid.", path);
    }
}

std::optional<int> HandleDeveloperCli(int argc, wchar_t** argv) {
    auto result = EvaluateDeveloperCli(argc, argv);
    if (!result) return std::nullopt;
    const std::string bytes = json::Serialize(result->output) + "\n";
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) return 3;
    size_t written = 0;
    while (written < bytes.size()) {
        const DWORD count = static_cast<DWORD>((std::min)(bytes.size() - written, size_t{32768}));
        DWORD sent = 0;
        if (!WriteFile(output, bytes.data() + written, count, &sent, nullptr) || !sent) return 3;
        written += sent;
    }
    return result->exit_code;
}
}
