#include "../setup/setup_dialog_bridge.h"
#include "../common/dialog_bridge_paths.h"
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_bundle.h"
#include <bcrypt.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <vector>
#include <cstring>

namespace {
namespace fs = std::filesystem;
namespace paths = pulse::dialog_bridge_paths;
namespace admin = pulse::dialog_admin;
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
DWORD ReadHeld(const std::wstring& path, File& file, std::vector<unsigned char>& bytes, DWORD limit) {
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(file.value, &info) || !GetFileSizeEx(file.value, &size)) return GetLastError();
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        size.QuadPart <= 0 || size.QuadPart > limit) return ERROR_INVALID_DATA;
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return GetLastError();
    return read == bytes.size() ? ERROR_SUCCESS : ERROR_READ_FAULT;
}
std::string Hash(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32]{};
    std::string result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) == 0 &&
                BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                for (auto byte : digest) {
                    result += "0123456789abcdef"[byte >> 4];
                    result += "0123456789abcdef"[byte & 15];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return result;
}
bool Image(const std::vector<unsigned char>& bytes, bool dll, WORD machine = IMAGE_FILE_MACHINE_AMD64) {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<size_t>(dos.e_lfanew) > bytes.size() - sizeof(IMAGE_DOS_HEADER)) return false;
    const auto offset = static_cast<size_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    std::memcpy(&signature, bytes.data() + offset, sizeof(signature));
    std::memcpy(&header, bytes.data() + offset + sizeof(signature), sizeof(header));
    const size_t optional = offset + sizeof(signature) + sizeof(header);
    if (header.SizeOfOptionalHeader < sizeof(WORD) || header.SizeOfOptionalHeader > bytes.size() - optional) return false;
    WORD magic = 0;
    std::memcpy(&magic, bytes.data() + optional, sizeof(magic));
    const WORD expected_magic = machine == IMAGE_FILE_MACHINE_I386 ? IMAGE_NT_OPTIONAL_HDR32_MAGIC : IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    return signature == IMAGE_NT_SIGNATURE && header.Machine == machine &&
        magic == expected_magic &&
        (header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) &&
        !!(header.Characteristics & IMAGE_FILE_DLL) == dll;
}
int Fail(const char* stage, DWORD error) {
    std::cout << "FAIL stage=" << stage << " error=" << error << '\n';
    return 1;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        bool apply = false;
        bool check_requested = false;
        std::map<std::wstring, std::wstring> options;
        for (int i = 1; i < argc; ++i) {
            const std::wstring name = argv[i];
            if (name == L"--apply") {
                if (check_requested) return Fail("conflicting-mode", ERROR_INVALID_PARAMETER);
                apply = true; continue;
            }
            if (name == L"--check") {
                if (apply) return Fail("conflicting-mode", ERROR_INVALID_PARAMETER);
                check_requested = true; continue;
            }
            if (i + 1 == argc || (name != L"--install-root" && name != L"--common-root" &&
                name != L"--core64" && name != L"--helper64" && name != L"--core-sha256" &&
                name != L"--helper-sha256" && name != L"--backup-pointer" &&
                name != L"--core32" && name != L"--helper32" &&
                name != L"--core32-sha256" && name != L"--helper32-sha256") || options.contains(name))
                return Fail("arguments", ERROR_INVALID_PARAMETER);
            options[name] = argv[++i];
        }
        for (const auto name : {L"--install-root", L"--common-root", L"--core64", L"--helper64", L"--core-sha256", L"--helper-sha256"})
            if (options[name].empty()) return Fail("required-argument", ERROR_INVALID_PARAMETER);
        const bool update_x86 = options.contains(L"--core32") || options.contains(L"--helper32") ||
            options.contains(L"--core32-sha256") || options.contains(L"--helper32-sha256");
        if (update_x86) {
            for (const auto name : {L"--core32", L"--helper32", L"--core32-sha256", L"--helper32-sha256"})
                if (options[name].empty()) return Fail("x86-pair-required", ERROR_INVALID_PARAMETER);
        }
        const auto absolute = [&](const wchar_t* name) {
            const fs::path value(options[name]);
            return value.is_absolute() ? value.lexically_normal().wstring() : std::wstring{};
        };
        const auto install = absolute(L"--install-root"), common = absolute(L"--common-root");
        const auto core = absolute(L"--core64"), helper = absolute(L"--helper64");
        const auto core32 = update_x86 ? absolute(L"--core32") : std::wstring{};
        const auto helper32 = update_x86 ? absolute(L"--helper32") : std::wstring{};
        if (install.empty() || core.empty() || helper.empty() || !Same(common, paths::CommonDialogBridgeRoot()))
            return Fail("absolute-production-paths", ERROR_INVALID_NAME);
        if (update_x86 && (core32.empty() || helper32.empty())) return Fail("absolute-x86-sources", ERROR_INVALID_NAME);
        DWORD error = admin::VerifyProtectedPath(install, true);
        if (!error) error = admin::VerifyProtectedPath(common, true);
        if (!error) error = admin::VerifyProtectedPath(common + L"\\active-bridge.json", false);
        if (!error) error = admin::VerifyProtectedPath(install + L"\\app-current.json", false);
        if (error) return Fail("protected-paths", error);
        File lock;
        if (apply) {
            if (!admin::IsElevated()) return Fail("elevation", ERROR_ELEVATION_REQUIRED);
            lock.value = CreateFileW((install + L"\\.pulse-versions.lock").c_str(), GENERIC_WRITE, 0,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (lock.value == INVALID_HANDLE_VALUE) return Fail("version-lock", GetLastError());
            if (fs::exists(fs::path(install) / L".pulse-setup")) return Fail("pending-setup", ERROR_BUSY);
        }
        pulse::dialog_bundle::BundleLease installed;
        error = pulse::dialog_bundle::ResolveInstalledAppRuntime(install, installed);
        if (!error) error = admin::VerifyProtectedTree(installed.Directory());
        if (error) return Fail("installed-runtime", error);
        paths::ActiveBridgePointer previous;
        error = paths::ReadActiveBridgePointer(common, previous);
        if (error) return Fail("active-pointer", error);
        if (!Same(previous.install_root, install) || !Same(previous.host_exe, installed.Host()))
            return Fail("installed-host-binding", ERROR_INVALID_DATA);
        File core_file, helper_file;
        std::vector<unsigned char> core_bytes, helper_bytes;
        error = ReadHeld(core, core_file, core_bytes, 256 * 1024 * 1024);
        if (!error) error = ReadHeld(helper, helper_file, helper_bytes, 32 * 1024 * 1024);
        if (error) return Fail("source-open", error);
        const auto core_hash = Hash(core_bytes), helper_hash = Hash(helper_bytes);
        if (core_hash.size() != 64 || helper_hash.size() != 64 ||
            std::wstring(core_hash.begin(), core_hash.end()) != options[L"--core-sha256"] ||
            std::wstring(helper_hash.begin(), helper_hash.end()) != options[L"--helper-sha256"])
            return Fail("source-digest", ERROR_CRC);
        if (!Image(core_bytes, true) || !Image(helper_bytes, false)) return Fail("source-machine", ERROR_BAD_EXE_FORMAT);
        File core32_file, helper32_file;
        std::vector<unsigned char> core32_bytes, helper32_bytes;
        std::string core32_hash, helper32_hash;
        if (update_x86) {
            error = ReadHeld(core32, core32_file, core32_bytes, 256 * 1024 * 1024);
            if (!error) error = ReadHeld(helper32, helper32_file, helper32_bytes, 32 * 1024 * 1024);
            if (error) return Fail("x86-source-open", error);
            core32_hash = Hash(core32_bytes);
            helper32_hash = Hash(helper32_bytes);
            if (core32_hash.size() != 64 || helper32_hash.size() != 64 ||
                std::wstring(core32_hash.begin(), core32_hash.end()) != options[L"--core32-sha256"] ||
                std::wstring(helper32_hash.begin(), helper32_hash.end()) != options[L"--helper32-sha256"])
                return Fail("x86-source-digest", ERROR_CRC);
            if (!Image(core32_bytes, true, IMAGE_FILE_MACHINE_I386) || !Image(helper32_bytes, false, IMAGE_FILE_MACHINE_I386))
                return Fail("x86-source-machine", ERROR_BAD_EXE_FORMAT);
        }
        std::cout << "CHECK generation=" << previous.generation << " core_sha256=" << core_hash
                  << " helper_sha256=" << helper_hash << " app_runtime=" << installed.ManifestDigest() << '\n';
        if (update_x86) std::cout << "CHECK x86_core_sha256=" << core32_hash << " x86_helper_sha256=" << helper32_hash << '\n';
        if (!apply) return 0;
        const auto backup = absolute(L"--backup-pointer");
        if (backup.empty()) return Fail("backup-path", ERROR_INVALID_NAME);
        if (!CopyFileW((common + L"\\active-bridge.json").c_str(), backup.c_str(), TRUE))
            return Fail("backup-pointer", GetLastError());
        pulse::setup::BridgeStageRequest request;
        request.app_dir = previous.install_root;
        request.host_exe = previous.host_exe;
        request.bridge_root_override = common;
        request.core_source_x64 = core;
        request.explorer_bridge_source_x64 = helper;
        request.core_source_x86 = core32;
        request.explorer_bridge_source_x86 = helper32;
        request.core_only = true;
        request.configure_service = request.start_service = request.register_hklm_com = false;
        const auto result = pulse::setup::ProvisionDialogBridge(request);
        if (result.error) return Fail("provision", result.error);
        paths::ActiveBridgePointer current;
        error = paths::ReadActiveBridgePointer(common, current);
        const bool x86_valid = update_x86 ?
            (current.core_digest_x86 == core32_hash && current.explorer_bridge_sha256_x86 == helper32_hash &&
             !current.core_dir_x86.empty() && current.core_dir_x86 == result.core_dir_x86 &&
             result.core32_staged && result.explorer_bridge32_staged) :
            (current.core_dir_x86 == previous.core_dir_x86 && current.core_digest_x86 == previous.core_digest_x86 &&
             current.explorer_bridge_sha256_x86 == previous.explorer_bridge_sha256_x86);
        if (error || current.core_digest_x64 != core_hash || current.explorer_bridge_sha256_x64 != helper_hash ||
            current.host_exe != previous.host_exe || current.install_root != previous.install_root ||
            !x86_valid || current.disabled != previous.disabled ||
            current.generation != previous.generation + 1)
            return Fail("published-pointer", error ? error : ERROR_INVALID_DATA);
        pulse::dialog_bundle::BundleLease after;
        error = pulse::dialog_bundle::ResolveInstalledAppRuntime(install, after);
        if (error || after.ManifestDigest() != installed.ManifestDigest() || after.Host() != installed.Host())
            return Fail("app-runtime-unchanged", error ? error : ERROR_INVALID_DATA);
        std::wcout << L"APPLIED generation=" << current.generation << L" core_dir=" << current.core_dir_x64
                   << L" host_unchanged=" << current.host_exe << L" backup=" << backup << L'\n';
        if (update_x86) std::wcout << L"APPLIED x86_core_dir=" << current.core_dir_x86 << L'\n';
        return 0;
    } catch (...) { return Fail("exception", ERROR_GEN_FAILURE); }
}
