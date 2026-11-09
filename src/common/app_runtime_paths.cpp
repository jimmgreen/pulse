#include "app_runtime_paths.h"
#include "dialog_bundle.h"
#include <filesystem>
#include <algorithm>
#include <windows.h>
namespace pulse::app_runtime_paths {
namespace {
struct Paths {
    std::wstring runtime, entry, root;
    dialog_bundle::BundleLease lease;
    Paths() {
        std::wstring executable(32768, L'\0');
        const DWORD count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!count || count >= executable.size()) return;
        executable.resize(count);
        const std::filesystem::path image(executable), directory = image.parent_path();
        runtime = root = directory.wstring(); entry = executable;
        if (_wcsicmp(image.filename().c_str(), L"pulse.exe") != 0) return;
        const auto hash = directory.filename().wstring();
        if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        }) || directory.parent_path().filename() != L"DialogBridge") return;
        const auto installed = directory.parent_path().parent_path();
        const auto stable = installed / L"pulse.exe";
        // Identity belongs to this verified version, independently of an
        // upgrade replacing (or temporarily removing) the current pointer.
        const DWORD attributes = GetFileAttributesW(stable.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
        std::string digest;
        digest.reserve(hash.size());
        for (wchar_t c : hash) digest.push_back(static_cast<char>(c));
        if (dialog_bundle::VerifyBundle(directory.wstring(), digest, lease) != ERROR_SUCCESS) return;
        root = installed.wstring(); entry = stable.wstring();
    }
};
const Paths& Get() { static const Paths paths; return paths; }
}
std::wstring RuntimeDirectory() { return Get().runtime; }
std::wstring StableEntryPath() { return Get().entry; }
std::wstring InstallRoot() { return Get().root; }
}
