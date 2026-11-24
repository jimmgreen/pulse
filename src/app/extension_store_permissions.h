#pragma once

#include "../common/localization.h"
#include "../common/json_value.h"
#include <charconv>

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
    constexpr std::string_view rename_prefix = "install_atomic_rename:";
    if (error.starts_with(rename_prefix)) {
        const auto detail = error.substr(rename_prefix.size());
        const auto end = detail.find(':');
        const auto code_text = detail.substr(0, end);
        unsigned code = 0;
        const auto parsed = std::from_chars(code_text.data(), code_text.data() + code_text.size(), code);
        if (parsed.ec == std::errc{} && parsed.ptr == code_text.data() + code_text.size() &&
            (code == 5 || code == 32 || code == 33)) {
            std::wstring message = l10n::Get(code == 5 ? Id::StoreErrorInstallDenied : Id::StoreErrorInstallBusy);
            message += L" (" + AsciiIdentifierWide(code_text) + L")";
            if (end != std::string_view::npos && detail.substr(end).starts_with(":target=")) {
                message += L"\n" + json::Wide(detail.substr(end + std::string_view(":target=").size()));
            }
            return message;
        }
        return AsciiIdentifierWide(error);
    }
    if (error == "package_signature_invalid" ||
        (error.find("signature") != std::string_view::npos && error != "signature_required")) {
        return l10n::Get(Id::StoreErrorSignatureInvalid) + L" (" + AsciiIdentifierWide(error) + L")";
    }
    if (error == "signature_required" || error.find("unsigned package rejected") != std::string_view::npos) {
        return l10n::Get(Id::StoreErrorSignatureRequired) + L"\n" + l10n::Get(Id::StoreUnsignedDevHint);
    }
    if (error == "installed_publisher_mismatch") {
        return l10n::Get(Id::StoreErrorPublisherMismatch);
    }
    return AsciiIdentifierWide(error);
}

}  // namespace pulse::app