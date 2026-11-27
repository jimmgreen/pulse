#include "setup_dialog_bridge.h"

#include "setup_common.h"
#include "setup_service.h"
#include "../common/dialog_bridge_paths.h"
#include "../common/dialog_com_hklm.h"

#include <bcrypt.h>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace pulse::setup {
namespace {

namespace fs = std::filesystem;
namespace paths = pulse::dialog_bridge_paths;

struct ScHandle {
    SC_HANDLE h = nullptr;
    ~ScHandle() { if (h) CloseServiceHandle(h); }
};

bool ExistsFile(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Sha256FileHex(const std::wstring& path) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(file);
        return {};
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        DWORD obj_len = 0, res_len = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len),
                              sizeof(obj_len), &res_len, 0) == 0 && obj_len > 0) {
            std::vector<UCHAR> obj(obj_len);
            if (BCryptCreateHash(alg, &hash, obj.data(), obj_len, nullptr, 0, 0) == 0) {
                std::vector<UCHAR> buf(64 * 1024);
                DWORD read = 0;
                bool ok = true;
                for (;;) {
                    if (!ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) { ok = false; break; }
                    if (read == 0) break;
                    if (BCryptHashData(hash, buf.data(), read, 0) != 0) { ok = false; break; }
                }
                UCHAR digest[32]{};
                if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                    hex.reserve(64);
                    static constexpr char kHex[] = "0123456789abcdef";
                    for (UCHAR b : digest) {
                        hex.push_back(kHex[(b >> 4) & 0xFu]);
                        hex.push_back(kHex[b & 0xFu]);
                    }
                }
                BCryptDestroyHash(hash);
            }
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(file);
    return hex;
}

std::string PairDigest(const std::string& core, const std::string& helper) {
    const std::string input = "PulseExplorerBridgeV1\n" + core + "\n" + helper;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32]{};
    std::string output;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                              static_cast<ULONG>(input.size()), 0) == 0 &&
                BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                for (const auto byte : digest) {
                    output += "0123456789abcdef"[byte >> 4];
                    output += "0123456789abcdef"[byte & 15];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return output;
}

DWORD StageImmutableVerified(const std::wstring& source, const std::wstring& target,
                             const std::string& digest) {
    if (digest.size() != 64) return ERROR_INVALID_DATA;
    if (!CopyFileW(source.c_str(), target.c_str(), TRUE)) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return error;
    }
    return Sha256FileHex(target) == digest ? ERROR_SUCCESS : ERROR_CRC;
}

std::wstring AsciiToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

bool SameDigestFiles(const std::wstring& a, const std::wstring& b) noexcept {
    if (!ExistsFile(a) || !ExistsFile(b)) return false;
    const auto ha = Sha256FileHex(a);
    return !ha.empty() && ha == Sha256FileHex(b);
}

// Replaces `dst` safely even if a running process currently holds `dst` mapped as a DLL/EXE:
// renames the locked image to `dst.old.<tick>` and schedules delete-on-reboot before copying.
DWORD StageFileSafe(const std::wstring& src, const std::wstring& dst) noexcept {
    if (!ExistsFile(src)) return ERROR_FILE_NOT_FOUND;
    if (SameDigestFiles(src, dst)) return ERROR_SUCCESS;

    const std::wstring tmp = dst + L".new." + std::to_wstring(GetCurrentProcessId()) +
                             L"." + std::to_wstring(GetTickCount64());
    if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE)) {
        return GetLastError();
    }

    if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return ERROR_SUCCESS;
    }

    const DWORD move_err = GetLastError();
    if (move_err == ERROR_ACCESS_DENIED || move_err == ERROR_SHARING_VIOLATION) {
        const std::wstring retired = dst + L".old." + std::to_wstring(GetTickCount64());
        if (MoveFileExW(dst.c_str(), retired.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            MoveFileExW(retired.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return ERROR_SUCCESS;
            }
            // Roll back rename if second move failed.
            MoveFileExW(retired.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    const DWORD final_err = GetLastError();
    DeleteFileW(tmp.c_str());
    return final_err ? final_err : move_err;
}

std::wstring FindSourceFile(const std::wstring& stage_dir, const std::wstring& app_dir,
                            const wchar_t* filename) {
    if (!stage_dir.empty()) {
        const std::wstring p1 = stage_dir + L"\\" + filename;
        if (ExistsFile(p1)) return p1;
    }
    if (!app_dir.empty()) {
        const std::wstring p2 = app_dir + L"\\DialogBridgeStage\\" + filename;
        if (ExistsFile(p2)) return p2;
        const std::wstring p3 = app_dir + L"\\" + filename;
        if (ExistsFile(p3)) return p3;
    }
    return {};
}

void PruneObsoleteCoreVersions(const std::wstring& versions_root,
                               const std::wstring& keep_x64,
                               const std::wstring& keep_x86,
                               const std::wstring& prev_x64,
                               const std::wstring& prev_x86) noexcept {
    try {
        std::error_code ec;
        if (!fs::exists(versions_root, ec)) return;
        auto should_keep = [&](const std::wstring& path) {
            auto eq = [&](const std::wstring& target) {
                return !target.empty() &&
                       CompareStringOrdinal(path.c_str(), -1, target.c_str(), -1, TRUE) == CSTR_EQUAL;
            };
            return eq(keep_x64) || eq(keep_x86) || eq(prev_x64) || eq(prev_x86);
        };
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(versions_root, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type_ec;
            if (it->is_directory(type_ec) && !type_ec) dirs.push_back(it->path());
        }
        for (const auto& dir_path : dirs) {
            if (!should_keep(dir_path.wstring())) {
                std::error_code remove_ec;
                fs::remove_all(dir_path, remove_ec); // Silently skips locked directories without aborting remaining ones.
            }
        }
    } catch (...) {}
}

void PruneStaleBridgeArtifacts(const std::wstring& bridge_root) noexcept {
    try {
        std::error_code ec;
        if (!fs::exists(bridge_root, ec)) return;
        std::vector<fs::path> stale;
        for (fs::directory_iterator it(bridge_root, ec), end; !ec && it != end; it.increment(ec)) {
            const DWORD attrs = GetFileAttributesW(it->path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
            const std::wstring name = it->path().filename().wstring();
            if (name.find(L".old.") != std::wstring::npos || name.find(L".new.") != std::wstring::npos) {
                stale.push_back(it->path());
            }
        }
        for (const auto& file : stale) {
            DeleteFileW(file.c_str()); // Best-effort; locked files remain scheduled for reboot removal.
        }
    } catch (...) {}
}

} // namespace

DWORD InstallOrUpdateDialogBrokerService(const std::wstring& broker_exe, bool start_now,
                                         bool& running_out) noexcept {
    running_out = false;
    if (broker_exe.empty() || !ExistsFile(broker_exe)) return ERROR_FILE_NOT_FOUND;

    ScHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT)};
    if (!scm.h) return GetLastError();

    const std::wstring bin_cmd = L"\"" + broker_exe + L"\"";
    ScHandle svc{OpenServiceW(scm.h, kDialogBrokerServiceName,
                              SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS)};
    if (!svc.h) {
        const DWORD open_err = GetLastError();
        if (open_err != ERROR_SERVICE_DOES_NOT_EXIST) return open_err;
        svc.h = CreateServiceW(
            scm.h,
            kDialogBrokerServiceName,
            L"Pulse Dialog Broker",
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            bin_cmd.c_str(),
            nullptr, nullptr, nullptr,
            nullptr, // LocalSystem
            nullptr);
        if (!svc.h) return GetLastError();
    } else {
        StopService(kDialogBrokerServiceName, 10000);
        if (!ChangeServiceConfigW(
                svc.h, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                bin_cmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, L"Pulse Dialog Broker")) {
            return GetLastError();
        }
    }

    SERVICE_DESCRIPTIONW desc{};
    desc.lpDescription = const_cast<LPWSTR>(
        L"Coordinates Pulse native file dialog integration across desktop and elevated applications.");
    ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Configure automatic crash recovery: restart after 5s/15s, reset counter after 1 day.
    SC_ACTION actions[3]{
        {SC_ACTION_RESTART, 5000},
        {SC_ACTION_RESTART, 15000},
        {SC_ACTION_NONE, 0},
    };
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 86400;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    if (start_now) {
        if (!StartServiceW(svc.h, 0, nullptr)) {
            const DWORD err = GetLastError();
            if (err != ERROR_SERVICE_ALREADY_RUNNING) return err;
        }
        SERVICE_STATUS_PROCESS status{};
        for (int i = 0; i < 40; ++i) {
            DWORD needed = 0;
            if (QueryServiceStatusEx(svc.h, SC_STATUS_PROCESS_INFO,
                                     reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
                if (status.dwCurrentState == SERVICE_RUNNING) {
                    running_out = true;
                    break;
                }
                if (status.dwCurrentState == SERVICE_STOPPED) break;
            }
            Sleep(100);
        }
    }
    return ERROR_SUCCESS;
}

BridgeStageResult ProvisionDialogBridge(const BridgeStageRequest& request) noexcept {
    BridgeStageResult out{};
    try {
        out.bridge_root = paths::CommonDialogBridgeRoot(request.bridge_root_override);
        if (out.bridge_root.empty()) {
            out.error = ERROR_PATH_NOT_FOUND;
            return out;
        }

        // 1. Create (and optionally harden DACL with ALL APPLICATION PACKAGES read+execute).
        if (request.harden_dacl) {
            const DWORD dir_err = paths::EnsureProtectedBridgeDirectory(out.bridge_root, true);
            if (dir_err != ERROR_SUCCESS) {
                out.error = dir_err;
                return out;
            }
        } else {
            std::error_code ec;
            fs::create_directories(out.bridge_root, ec);
            if (ec) {
                out.error = ERROR_PATH_NOT_FOUND;
                return out;
            }
        }

        const std::wstring versions_root = out.bridge_root + L"\\v";
        {
            std::error_code ec;
            fs::create_directories(versions_root, ec);
        }

        // Preserve previous active pointer to advance generation monotonically and retain previous Core dir.
        paths::ActiveBridgePointer prev{};
        const DWORD previous_error = paths::ReadActiveBridgePointer(out.bridge_root, prev);
        if (request.core_only && (previous_error || prev.install_root != request.app_dir ||
            prev.host_exe != request.host_exe || prev.generation == UINT64_MAX)) {
            out.error = previous_error ? previous_error : ERROR_INVALID_DATA;
            return out;
        }

        // 2. Stage fixed-path Stub DLLs, Broker, Inject32, Overlay (stopping Broker first if updating it).
        const auto find = [&](const wchar_t* name) {
            return request.core_only ? std::wstring{} : FindSourceFile(request.stage_source_dir, request.app_dir, name);
        };
        const auto src_stub64 = find(paths::kStubDllName64);
        const auto src_stub32 = find(paths::kStubDllName32);
        const auto src_core64 = request.core_source_x64.empty() ? find(paths::kCoreDllName64) : request.core_source_x64;
        const auto src_core32 = request.core_source_x86.empty() ? find(paths::kCoreDllName32) : request.core_source_x86;
        const auto src_broker = find(paths::kBrokerExeName);
        const auto src_inj32 = find(paths::kInject32ExeName);
        const auto src_overlay = find(paths::kOverlayExeName);

        if (request.configure_service && !src_broker.empty()) {
            StopService(kDialogBrokerServiceName, 10000);
        }

        auto stage_fixed = [&](const std::wstring& src, const wchar_t* name, bool& flag) -> DWORD {
            if (src.empty()) return ERROR_SUCCESS;
            const DWORD err = StageFileSafe(src, out.bridge_root + L"\\" + name);
            if (err == ERROR_SUCCESS) flag = true;
            return err;
        };

        if (DWORD err = stage_fixed(src_stub64, paths::kStubDllName64, out.stub64_staged)) { out.error = err; return out; }
        if (DWORD err = stage_fixed(src_stub32, paths::kStubDllName32, out.stub32_staged)) { out.error = err; return out; }
        if (DWORD err = stage_fixed(src_broker, paths::kBrokerExeName, out.broker_staged)) { out.error = err; return out; }
        if (DWORD err = stage_fixed(src_inj32,  paths::kInject32ExeName, out.inject32_staged)) { out.error = err; return out; }
        if (DWORD err = stage_fixed(src_overlay, paths::kOverlayExeName, out.overlay_staged)) { out.error = err; return out; }

        // 3. Stage hash-addressed versioned Core DLLs under v\<sha256>\ for zero-restart hot switching.
        paths::ActiveBridgePointer next{};
        next.schema = 1;
        next.generation = prev.generation + 1;
        next.disabled = prev.disabled;
        next.install_root = request.app_dir;
        next.host_exe = !request.host_exe.empty() ? request.host_exe
                        : (!request.app_dir.empty() ? request.app_dir + L"\\pulse.exe" : L"");

        auto stage_core = [&](const std::wstring& source, const wchar_t* name,
                              const std::wstring& helper, const std::wstring& previous_dir,
                              const std::string& previous_digest, std::wstring& out_dir,
                              std::string& out_digest, std::string& helper_digest, bool& flag, bool& helper_flag) -> DWORD {
            if (source.empty() && helper.empty()) return ERROR_SUCCESS;
            const auto src = source.empty() ? previous_dir + L"\\" + name : source;
            if (source.empty() && (previous_dir.empty() || previous_digest.size() != 64)) return ERROR_FILE_NOT_FOUND;
            const auto sha = Sha256FileHex(src);
            if (sha.size() != 64) return ERROR_READ_FAULT;
            if (source.empty() && sha != previous_digest) return ERROR_CRC;
            const auto helper_sha = helper.empty() ? std::string{} : Sha256FileHex(helper);
            if (!helper.empty() && helper_sha.size() != 64) return ERROR_READ_FAULT;
            const auto identity = helper.empty() ? sha.substr(0, 16) : PairDigest(sha, helper_sha);
            if (identity.empty()) return ERROR_READ_FAULT;
            const std::wstring ver_dir = versions_root + L"\\" + AsciiToWide(identity);
            std::error_code ec;
            fs::create_directories(ver_dir, ec);
            if (ec) return ERROR_CANNOT_MAKE;
            if (request.harden_dacl) {
                const DWORD error = paths::EnsureProtectedBridgeDirectory(ver_dir, true);
                if (error) return error;
            }
            const DWORD err = StageImmutableVerified(src, ver_dir + L"\\" + name, sha);
            if (err != ERROR_SUCCESS) return err;
            if (!helper.empty()) {
                const DWORD error = StageImmutableVerified(helper, ver_dir + L"\\" + paths::kExplorerBridgeExeName, helper_sha);
                if (error) return error;
                helper_flag = true;
            }
            out_dir = ver_dir;
            out_digest = sha;
            helper_digest = helper_sha;
            flag = true;
            return ERROR_SUCCESS;
        };

        if (DWORD err = stage_core(src_core64, paths::kCoreDllName64, request.explorer_bridge_source_x64,
                prev.core_dir_x64, prev.core_digest_x64, next.core_dir_x64, next.core_digest_x64,
                next.explorer_bridge_sha256_x64, out.core64_staged, out.explorer_bridge64_staged)) {
            out.error = err;
            return out;
        }
        if (DWORD err = stage_core(src_core32, paths::kCoreDllName32, request.explorer_bridge_source_x86,
                prev.core_dir_x86, prev.core_digest_x86, next.core_dir_x86, next.core_digest_x86,
                next.explorer_bridge_sha256_x86, out.core32_staged, out.explorer_bridge32_staged)) {
            out.error = err;
            return out;
        }

        // Keep previous Core pointers if one arch wasn't supplied in a partial update.
        if (next.core_dir_x64.empty()) {
            next.core_dir_x64 = prev.core_dir_x64;
            next.core_digest_x64 = prev.core_digest_x64;
            next.explorer_bridge_sha256_x64 = prev.explorer_bridge_sha256_x64;
        }
        if (next.core_dir_x86.empty()) {
            next.core_dir_x86 = prev.core_dir_x86;
            next.core_digest_x86 = prev.core_digest_x86;
            next.explorer_bridge_sha256_x86 = prev.explorer_bridge_sha256_x86;
        }

        out.core_dir_x64 = next.core_dir_x64;
        out.core_dir_x86 = next.core_dir_x86;
        out.generation = next.generation;

        // 4. Atomically publish active-bridge.json (triggers live Stub hot-switch & Broker state refresh).
        if (out.core64_staged || out.core32_staged || !next.core_dir_x64.empty() || !next.core_dir_x86.empty()) {
            const DWORD ptr_err = paths::WriteActiveBridgePointer(out.bridge_root, next);
            if (ptr_err != ERROR_SUCCESS) {
                out.error = ptr_err;
                return out;
            }
        }

        // Clean up retired Core versions older than current + previous and unlocked .old./.new. files.
        if (!request.core_only) {
            PruneObsoleteCoreVersions(versions_root, next.core_dir_x64, next.core_dir_x86,
                                      prev.core_dir_x64, prev.core_dir_x86);
            PruneStaleBridgeArtifacts(out.bridge_root);
        }

        // 5. Optional HKLM COM Stub registration (explicit opt-in with full backup).
        if (request.register_hklm_com && !request.core_only) {
            if (out.stub64_staged) {
                dialog_com_hklm::RegisterStub(out.bridge_root + L"\\" + paths::kStubDllName64, false);
            }
            if (out.stub32_staged) {
                dialog_com_hklm::RegisterStub(out.bridge_root + L"\\" + paths::kStubDllName32, true);
            }
        }

        // 6. Install / update / start PulseDialogBroker SYSTEM service when staged.
        if (request.configure_service && out.broker_staged) {
            const DWORD svc_err = InstallOrUpdateDialogBrokerService(
                out.bridge_root + L"\\" + paths::kBrokerExeName, request.start_service, out.service_running);
            if (svc_err != ERROR_SUCCESS) {
                out.error = svc_err;
                return out;
            }
            out.service_configured = true;
        }

        return out;
    } catch (...) {
        out.error = ERROR_GEN_FAILURE;
        return out;
    }
}

} // namespace pulse::setup
