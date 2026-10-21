#include "known_folder_links.h"
#include "places.h"
#include "../fs/fs_enum.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>

namespace pulse::app {

namespace {

static const KnownFolderDescriptor kDescriptors[] = {
    { BuiltinQuickAccess::Recent, nullptr, L"\xE823", L"Recent", 0x60A5FA, l10n::StringId::Recent },
    { BuiltinQuickAccess::Desktop, &FOLDERID_Desktop, L"\xE7F4", L"Desktop", 0x38BDF8, l10n::StringId::Desktop },
    { BuiltinQuickAccess::Downloads, &FOLDERID_Downloads, L"\xE896", L"Downloads", 0xC084FC, l10n::StringId::Downloads },
    { BuiltinQuickAccess::RecycleBin, nullptr, L"\xE75C", L"Bin", 0x94A3B8, l10n::StringId::RecycleBin },
    { BuiltinQuickAccess::Documents, &FOLDERID_Documents, L"\xE8A5", L"Documents", 0x3B82F6, l10n::StringId::Documents },
    { BuiltinQuickAccess::Music, &FOLDERID_Music, L"\xE8D6", L"Music", 0xF97316, l10n::StringId::Music },
    { BuiltinQuickAccess::Pictures, &FOLDERID_Pictures, L"\xEB9F", L"Pictures", 0xEC4899, l10n::StringId::Pictures },
    { BuiltinQuickAccess::Videos, &FOLDERID_Videos, L"\xE714", L"Videos", 0x10B981, l10n::StringId::Videos },
};

std::wstring DefaultShellResolve(REFKNOWNFOLDERID fid) {
    PWSTR path = nullptr;
    // KF_FLAG_DONT_VERIFY: fast return without blocking on network shares or physical spins.
    if (SUCCEEDED(SHGetKnownFolderPath(fid, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path) {
        std::wstring result = fs::NormalizePath(path);
        CoTaskMemFree(path);
        return result;
    }
    return {};
}

} // namespace

const KnownFolderDescriptor* GetKnownFolderDescriptor(BuiltinQuickAccess id) noexcept {
    for (const auto& desc : kDescriptors) {
        if (desc.id == id) return &desc;
    }
    return nullptr;
}

std::span<const KnownFolderDescriptor> GetAllKnownFolderDescriptors() noexcept {
    return kDescriptors;
}

const KnownFolderDescriptor* KnownFolderSnapshot::FindByPath(const std::wstring& path) const noexcept {
    if (path.empty()) return nullptr;
    const std::wstring norm = fs::NormalizePath(path);
    for (BuiltinQuickAccess id : kQuickAccessDisplayOrder) {
        const auto& target = GetPath(id);
        if (target.empty()) continue;
        if (_wcsicmp(norm.c_str(), target.c_str()) == 0) {
            return GetKnownFolderDescriptor(id);
        }
    }
    return nullptr;
}

KnownFolderSnapshot ResolveKnownFolders(const KnownFolderSnapshot* previous,
                                        KnownFolderResolverFn resolver) {
    KnownFolderSnapshot snap;
    if (previous) snap = *previous;

    const auto resolve_fn = resolver ? resolver : DefaultShellResolve;

    // Virtual paths
    snap.SetPath(BuiltinQuickAccess::Recent, MakeRecentPath());
    snap.SetPath(BuiltinQuickAccess::RecycleBin, MakeRecyclePath());

    for (const auto& desc : kDescriptors) {
        if (!desc.fid) continue;
        std::wstring path = resolve_fn(*desc.fid);
        if (!path.empty()) {
            snap.SetPath(desc.id, std::move(path));
        } else if (!previous || previous->GetPath(desc.id).empty()) {
            // Unset/empty if resolving failed and there was no previous valid path
            snap.SetPath(desc.id, {});
        }
    }
    return snap;
}

} // namespace pulse::app
