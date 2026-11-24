#include "dialog_auto_repair.h"
#include "dialog_auto_ownership.h"
#include <cwctype>

namespace pulse::dialog_auto {
namespace {
Status InspectWith(const RegistrationApi& api, Architecture architecture) {
    return api.inspect ? api.inspect(architecture) : Inspect(architecture);
}
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring Root(std::wstring root) {
    while (root.size() > 3 && root.back() == L'\\') root.pop_back();
    return root;
}
}

RebindResult RebindStaleRegistration(const std::wstring& install_root,
    const std::vector<InstalledDialogDll>& current, const RegistrationApi& api) noexcept {
    RebindResult result;
    try {
        const auto root = Root(install_root);
        std::vector<RegistrationTarget> stale, targets;
        for (const auto& installed : current) {
            if (installed.dll_path.empty()) { ++result.skipped; continue; }
            const auto status = InspectWith(api, installed.architecture);
            if (status.error || status.conflict || !status.enabled || status.dll_path.empty() ||
                SamePath(status.dll_path, installed.dll_path)) { ++result.skipped; continue; }
            // Owned: this root's DialogBridge\<digest>\ (or legacy root/bridge) DLL. Orphaned:
            // an active Pulse registration whose directory is genuinely gone.
            if (!IsOwnedDialogAutoDll(root, true, installed.architecture, status.dll_path) &&
                !IsOrphanedMissingPulseDialogDll(installed.architecture, status)) { ++result.skipped; continue; }
            stale.push_back({installed.architecture, status.dll_path});
            targets.push_back({installed.architecture, installed.dll_path, status.dll_path});
        }
        if (targets.empty()) return result;
        // Same sequence as the settings page: disable the old path, then enable
        // the new one only if the journal still names that old path.
        const auto disabled = ExecuteRegistrationChange(stale, false, api);
        if (disabled.error) {
            result.error = disabled.error;
            result.rollback_error = disabled.rollback_error;
            return result;
        }
        const auto enabled = ExecuteRegistrationChange(targets, true, api);
        result.error = enabled.error;
        result.rollback_error = enabled.rollback_error;
        if (!enabled.error) result.rebound = static_cast<unsigned>(targets.size());
    } catch (...) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
    }
    return result;
}

std::vector<std::wstring> RegisteredBundleDigests(const std::wstring& install_root, const RegistrationApi& api) noexcept {
    std::vector<std::wstring> digests;
    try {
        const auto bridge = Root(install_root) + L"\\DialogBridge\\";
        for (const auto architecture : {Architecture::X64, Architecture::X86}) {
            const auto status = InspectWith(api, architecture);
            const wchar_t* name = DllNameFor(architecture);
            const auto& path = status.dll_path;
            if (!status.enabled || !name || path.size() != bridge.size() + 65 + wcslen(name) ||
                _wcsnicmp(path.c_str(), bridge.c_str(), bridge.size()) != 0 ||
                path[bridge.size() + 64] != L'\\' || _wcsicmp(path.c_str() + bridge.size() + 65, name) != 0) continue;
            std::wstring digest = path.substr(bridge.size(), 64);
            bool hex = true;
            for (auto& c : digest) {
                c = static_cast<wchar_t>(std::towlower(c));
                hex = hex && ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'));
            }
            if (hex) digests.push_back(std::move(digest));
        }
    } catch (...) {
    }
    return digests;
}
}
