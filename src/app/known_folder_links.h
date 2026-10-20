#pragma once

#include "../common/localization.h"
#include <string>
#include <string_view>
#include <array>
#include <span>
#include <cstdint>
#include <initguid.h>
#include <knownfolders.h>

namespace pulse::app {

// Stable built-in quick access IDs.
// Existing IDs 0..3 are fixed for backward compatibility with session mask:
//   Recent=0, Desktop=1, Downloads=2, RecycleBin=3.
// New known folders 4..7 are appended:
//   Documents=4, Music=5, Pictures=6, Videos=7.
enum class BuiltinQuickAccess : int {
    Recent = 0,
    Desktop = 1,
    Downloads = 2,
    RecycleBin = 3,
    Documents = 4,
    Music = 5,
    Pictures = 6,
    Videos = 7,
    Count = 8,
};

inline constexpr int kBuiltinQuickAccessCount = static_cast<int>(BuiltinQuickAccess::Count);

// Separate display order:
// Recent, Desktop, Downloads, Documents, Pictures, Music, Videos, RecycleBin.
inline constexpr BuiltinQuickAccess kQuickAccessDisplayOrder[] = {
    BuiltinQuickAccess::Recent,
    BuiltinQuickAccess::Desktop,
    BuiltinQuickAccess::Downloads,
    BuiltinQuickAccess::Documents,
    BuiltinQuickAccess::Pictures,
    BuiltinQuickAccess::Music,
    BuiltinQuickAccess::Videos,
    BuiltinQuickAccess::RecycleBin,
};

struct KnownFolderDescriptor {
    BuiltinQuickAccess id;
    const KNOWNFOLDERID* fid; // nullptr for virtual entries (Recent, RecycleBin)
    const wchar_t* glyph;
    const wchar_t* fallback;
    uint32_t color_rgb;
    l10n::StringId l10n_id;
};

// Returns descriptor by BuiltinQuickAccess id, or nullptr if invalid.
const KnownFolderDescriptor* GetKnownFolderDescriptor(BuiltinQuickAccess id) noexcept;

// Returns all descriptors in canonical registration order.
std::span<const KnownFolderDescriptor> GetAllKnownFolderDescriptors() noexcept;

// Snapshot of resolved known folder paths (normalized root paths).
struct KnownFolderSnapshot {
    std::array<std::wstring, static_cast<size_t>(BuiltinQuickAccess::Count)> paths{};

    const std::wstring& GetPath(BuiltinQuickAccess id) const noexcept {
        const auto idx = static_cast<size_t>(id);
        if (idx < paths.size()) return paths[idx];
        static const std::wstring kEmpty;
        return kEmpty;
    }

    void SetPath(BuiltinQuickAccess id, std::wstring path) {
        const auto idx = static_cast<size_t>(id);
        if (idx < paths.size()) paths[idx] = std::move(path);
    }

    // Exact root path matching (case-insensitive, normalized).
    // Subdirectories or regular folders with the same basename do NOT match.
    // When multiple known folders map to the same root path, returns the first
    // matching descriptor in kQuickAccessDisplayOrder.
    const KnownFolderDescriptor* FindByPath(const std::wstring& path) const noexcept;
};

// Resolver callback type for dependency injection in tests.
using KnownFolderResolverFn = std::wstring(*)(REFKNOWNFOLDERID fid);

// Resolves all physical known folders using SHGetKnownFolderPath (with KF_FLAG_DONT_VERIFY).
// If resolution fails, retains the previous snapshot path if non-empty, or leaves empty.
// Does NOT fabricate fake %USERPROFILE% paths or create directories on disk.
KnownFolderSnapshot ResolveKnownFolders(const KnownFolderSnapshot* previous = nullptr,
                                        KnownFolderResolverFn resolver = nullptr);

} // namespace pulse::app
