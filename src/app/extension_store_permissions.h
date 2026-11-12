#pragma once

#include "../common/localization.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pulse::app {

inline std::optional<l10n::StringId> ResourcePermissionStringId(std::string_view resource) noexcept {
    using Id = l10n::StringId;
    static constexpr std::pair<std::string_view, Id> kResourceMap[] = {
        {"context.read", Id::StorePermContextRead},
        {"context.events", Id::StorePermContextEvents},
        {"files.readCurrentDirectory", Id::StorePermFilesReadCurrentDir},
        {"files.renameCurrentDirectory", Id::StorePermFilesRenameCurrentDir},
        {"files.readDirectory", Id::StorePermFilesReadDir},
        {"files.watchDirectory", Id::StorePermFilesWatchDir},
        {"files.open", Id::StorePermFilesOpen},
        {"files.transferCurrentSelection", Id::StorePermFilesTransferSelection},
        {"files.deleteCurrentSelection", Id::StorePermFilesDeleteSelection},
        {"files.transferToDirectory", Id::StorePermFilesTransferToDir},
        {"files.transferFromDirectory", Id::StorePermFilesTransferFromDir},
        {"files.transferDirectoryTree", Id::StorePermFilesTransferDirTree},
        {"operations.own", Id::StorePermOperationsOwn},
        {"settings.own", Id::StorePermSettingsOwn},
        {"audit.own", Id::StorePermAuditOwn},
    };
    for (const auto& [name, id] : kResourceMap) {
        if (name == resource) return id;
    }
    return std::nullopt;
}

inline std::wstring AsciiIdentifierWide(std::string_view value) {
    std::wstring wide;
    wide.reserve(value.size());
    for (unsigned char ch : value) {
        wide.push_back(ch >= 0x20 && ch <= 0x7e ? static_cast<wchar_t>(ch) : L'?');
    }
    return wide;
}

inline std::wstring LocalizedResourcePermission(std::string_view resource) {
    if (const auto id = ResourcePermissionStringId(resource)) {
        const auto& text = l10n::Get(*id);
        if (!text.empty()) return text;
    }
    return AsciiIdentifierWide(resource);
}

inline std::wstring AbbreviateHexDigest(std::string_view digest, size_t edge = 8) {
    if (digest.size() <= edge * 2 + 2) return AsciiIdentifierWide(digest);
    return AsciiIdentifierWide(digest.substr(0, edge)) + L"\u2026" +
           AsciiIdentifierWide(digest.substr(digest.size() - edge));
}

inline std::wstring ReplaceStoreToken(std::wstring text, std::wstring_view token, std::wstring_view value) {
    if (const size_t pos = text.find(token); pos != std::wstring::npos) {
        text.replace(pos, token.size(), value);
    }
    return text;
}

inline std::wstring LocalizedStoreInstallError(std::string_view error) {
    using Id = l10n::StringId;
    if (error == "package_signature_invalid" ||
        (error.find("signature") != std::string_view::npos && error != "signature_required")) {
        return l10n::Get(Id::StoreErrorSignatureInvalid) + L" (" + AsciiIdentifierWide(error) + L")";
    }
    if (error == "signature_required" || error.find("unsigned package rejected") != std::string_view::npos) {
        return l10n::Get(Id::StoreErrorSignatureRequired);
    }
    if (error == "installed_publisher_mismatch") {
        return l10n::Get(Id::StoreErrorPublisherMismatch);
    }
    return AsciiIdentifierWide(error);
}

}  // namespace pulse::app
