#pragma once

#include <string_view>

namespace pulse::dialog_diagnostics {

inline bool KnownDiagnosticValue(std::string_view key, std::string_view value) {
    const auto one_of = [value](std::string_view choices) {
        while (!choices.empty()) {
            const auto end = choices.find(' ');
            if (value == choices.substr(0, end)) return true;
            if (end == choices.npos) break;
            choices.remove_prefix(end + 1);
        }
        return false;
    };
    if (key == "entry_family") return one_of("COM OFN_W OFN_A SHBROWSE_W SHBROWSE_A");
    if (key == "launch_route") return one_of("ordinary managed");
    if (key == "dialog_kind") return one_of("open save unknown");
    if (key == "terminal_status") return one_of("pending native accepted cancelled failed closed");
    if (key == "fallback_reason") return value.empty() || one_of(
        "none unknown resolve-not-ready provider-unavailable initial-folder-unavailable contract "
        "provider-prepare-failed provider-failed-before-ui ENTRY_DISABLED_OR_UNAVAILABLE "
        "INITIAL_FOLDER_UNREPRESENTABLE PROVIDER_START_FAILED_BEFORE_UI PROVIDER_PREPARE_FAILED_BEFORE_UI "
        "NATIVE_CONTRACT_FALLBACK");
    if (key != "reason_name") return false;
    return one_of(
        "TOKEN_ACCEPTED PROCESS_TOKEN_OBSERVED THREAD_IMPERSONATION THREAD_TOKEN_QUERY_FAILED "
        "PROCESS_TOKEN_QUERY_FAILED ELEVATION_QUERY_FAILED ELEVATED_BUNDLE_UNTRUSTED RESTRICTED_TOKEN "
        "APPCONTAINER_QUERY_FAILED APPCONTAINER INTEGRITY_QUERY_FAILED INVALID_INTEGRITY_SID "
        "BELOW_MEDIUM_INTEGRITY ELIGIBLE AUTOMATIC_DISABLED REGISTRATION_QUERY_FAILED REGISTRATION_CONFLICT "
        "REGISTERED_MODULE_MISMATCH REGISTRATION_CHANGED_OR_NOT_CURRENT PROVIDER_UNAVAILABLE "
        "CONTRACT_NOT_CERTIFIED TRUSTED None SetterFailed UnrecognizedSuccess UnknownSetting UnsupportedOptions "
        "FilesystemNotForced RecentItemsPolicy MissingValidationOptions DefaultDirectoryPolicy NonFilesystemFolder "
        "UnsupportedFilename UnsupportedExtension UnsupportedFilter UnsupportedSettingOrder FolderFileSettings "
        "SaveContract Events Customization Labels Places Persistence SaveMetadata ShellFilter InvalidData Limit "
        "Unknown NONE SETTER_FAILED UNRECOGNIZED_SUCCESS UNKNOWN_SETTING UNSUPPORTED_OPTIONS "
        "FILESYSTEM_NOT_FORCED RECENT_ITEMS_POLICY MISSING_VALIDATION_OPTIONS DEFAULT_DIRECTORY_POLICY "
        "NON_FILESYSTEM_FOLDER UNSUPPORTED_FILENAME UNSUPPORTED_EXTENSION UNSUPPORTED_FILTER "
        "UNSUPPORTED_SETTING_ORDER FOLDER_FILE_SETTINGS SAVE_CONTRACT EVENTS CUSTOMIZATION LABELS PLACES "
        "PERSISTENCE SAVE_METADATA SHELL_FILTER INVALID_DATA LIMIT UNKNOWN");
}

} // namespace pulse::dialog_diagnostics
