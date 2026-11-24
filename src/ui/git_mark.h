#pragma once
// Git working-tree state of one list row (in-app Git status). The app fills
// PaneViewModel::git_marks; the list tints the name and, in Details, shows a
// pill in the badge slot. Draw and hit-test share GitMarkText / GitDirBadgeText
// for width.
#include "../common/localization.h"
#include <d2d1.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::ui {

// Locked: SVN only (a lock this working copy holds).
enum class GitMark : uint8_t { None = 0, Added, Renamed, Modified, Deleted, Conflict, Locked };

inline const std::wstring& GitMarkText(GitMark mark) {
    using l10n::StringId;
    switch (mark) {
    case GitMark::Added: return l10n::Get(StringId::GitMarkAdded);
    case GitMark::Renamed: return l10n::Get(StringId::GitMarkRenamed);
    case GitMark::Modified: return l10n::Get(StringId::GitMarkModified);
    case GitMark::Deleted: return l10n::Get(StringId::GitMarkDeleted);
    case GitMark::Conflict: return l10n::Get(StringId::GitMarkConflict);
    case GitMark::Locked: return l10n::Get(StringId::GitMarkLocked);
    default: break;
    }
    static const std::wstring kNone;
    return kNone;
}

inline const std::wstring& GitMarkLabel(GitMark mark, bool plain_terms) {
    using l10n::StringId;
    if (plain_terms) {
        switch (mark) {
        case GitMark::Added: return l10n::Get(StringId::GitMarkAddedPlain);
        case GitMark::Renamed: return l10n::Get(StringId::GitMarkRenamedPlain);
        case GitMark::Modified: return l10n::Get(StringId::GitMarkModifiedPlain);
        case GitMark::Deleted: return l10n::Get(StringId::GitMarkDeletedPlain);
        case GitMark::Conflict: return l10n::Get(StringId::GitMarkConflictPlain);
        case GitMark::Locked: return l10n::Get(StringId::GitMarkLocked);
        default: break;
        }
    } else {
        switch (mark) {
        case GitMark::Added: return l10n::Get(StringId::GitMarkAddedCode);
        case GitMark::Renamed: return l10n::Get(StringId::GitMarkRenamedCode);
        case GitMark::Modified: return l10n::Get(StringId::GitMarkModifiedCode);
        case GitMark::Deleted: return l10n::Get(StringId::GitMarkDeletedCode);
        case GitMark::Conflict: return l10n::Get(StringId::GitMarkConflictCode);
        case GitMark::Locked: return l10n::Get(StringId::GitMarkLocked);
        default: break;
        }
    }
    static const std::wstring kNone;
    return kNone;
}

inline std::wstring GitDirBadgeText(GitMark mark, int count, bool plain_terms) {
    if (mark == GitMark::None || count <= 0) return {};
    if (plain_terms) {
        if (mark == GitMark::Conflict) return l10n::Get(l10n::StringId::GitMarkConflictPlain);
        std::wstring text = l10n::Get(l10n::StringId::GitDirSummaryPlain);
        if (const size_t at = text.find(L"{n}"); at != std::wstring::npos)
            text.replace(at, 3, std::to_wstring(count));
        return text;
    }
    std::wstring text = l10n::Get(l10n::StringId::GitDirSummaryGit);
    if (const size_t at = text.find(L"{n}"); at != std::wstring::npos)
        text.replace(at, 3, std::to_wstring(count));
    if (const size_t at = text.find(L"{code}"); at != std::wstring::npos)
        text.replace(at, 6, GitMarkLabel(mark, false));
    return text;
}

// GitHub's light palette / VS Code's dark one: readable on either surface.
inline D2D1_COLOR_F GitMarkColor(GitMark mark, bool dark) {
    uint32_t rgb = 0;
    switch (mark) {
    case GitMark::Added: rgb = dark ? 0x73C991u : 0x1A7F37u; break;
    case GitMark::Renamed: rgb = dark ? 0x5EC4D8u : 0x0E7490u; break;
    case GitMark::Modified: rgb = dark ? 0xE2C08Du : 0x9A6700u; break;
    case GitMark::Deleted: rgb = dark ? 0xF0827Au : 0xB42318u; break;
    case GitMark::Conflict: rgb = dark ? 0xFF7B72u : 0xCF222Eu; break;
    case GitMark::Locked: rgb = dark ? 0xB5B9C4u : 0x5F6470u; break;
    default: return D2D1::ColorF(0, 0.0f);
    }
    return D2D1::ColorF(rgb, 1.0f);
}

}  // namespace pulse::ui
