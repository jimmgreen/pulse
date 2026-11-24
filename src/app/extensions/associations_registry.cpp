#include "associations_registry.h"

#include <windows.h>
#include <algorithm>
#include <set>

namespace pulse::extensions {
namespace {

std::wstring WideUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), out.data(), n);
    return out;
}

std::string Utf8Wide(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

bool SameInsensitive(std::wstring_view lhs, std::wstring_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        wchar_t a = lhs[i];
        wchar_t b = rhs[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) return false;
    }
    return true;
}

bool IsWindowsBuiltinViewer(std::wstring_view progid) noexcept {
    return SameInsensitive(progid, L"CompressedFolder") ||
           SameInsensitive(progid, L"CABFolder") ||
           SameInsensitive(progid, L"ArchiveFolder");
}

std::wstring ClassesKey(std::wstring_view suffix) {
    std::wstring out(L"Software\\Classes\\");
    out.append(suffix);
    return out;
}

std::wstring UserChoiceKey(std::wstring_view ext) {
    std::wstring out(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\");
    out.append(ext);
    out.append(L"\\UserChoice");
    return out;
}

std::wstring ResolveDefaultIcon(const std::wstring& pulse_exe,
                                const std::wstring& package_dir,
                                std::string_view icon_resource) {
    if (!icon_resource.empty() && !package_dir.empty()) {
        const size_t comma = icon_resource.find(',');
        const std::string_view file_part = comma == std::string_view::npos ? icon_resource : icon_resource.substr(0, comma);
        const std::wstring wide_file = WideUtf8(file_part);
        if (!wide_file.empty()) {
            std::wstring result = package_dir;
            if (result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
            result.append(wide_file);
            if (comma != std::string_view::npos) {
                result.push_back(L',');
                result.append(WideUtf8(icon_resource.substr(comma + 1)));
            }
            return result;
        }
    }
    return pulse_exe + L",0";
}

bool BackupAndClaimDefault(AssociationRegistryStore& store,
                           const std::wstring& record_key,
                           const std::wstring& ext_w,
                           const std::wstring& progid) {
    const std::wstring ext_classes = ClassesKey(ext_w);
    const auto current_hkcu = store.ReadString(ext_classes, nullptr);
    if (current_hkcu && SameInsensitive(*current_hkcu, progid)) return true;

    const std::wstring backup_key = record_key + L"\\Previous\\" + ext_w;
    const auto saved = store.ReadDword(backup_key, L"Saved");
    if (!saved.has_value()) {
        const auto existing = store.ReadRaw(ext_classes, nullptr);
        const uint32_t present = existing.has_value() ? 1u : 0u;
        const uint32_t type = existing ? existing->type : REG_NONE;
        const uint8_t* ptr = existing && !existing->data.empty() ? existing->data.data() : nullptr;
        const size_t bytes = existing ? existing->data.size() : 0u;
        if (!store.SetDword(backup_key, L"Present", present) ||
            !store.SetDword(backup_key, L"Type", type) ||
            !store.SetRaw(backup_key, L"Data", REG_BINARY, ptr, bytes) ||
            !store.SetDword(backup_key, L"Saved", 1u)) {
            return false;
        }
    } else if (*saved != 1u) {
        return false;
    }
    return store.SetString(ext_classes, nullptr, progid);
}

bool RestoreDefault(AssociationRegistryStore& store,
                    const std::wstring& record_key,
                    const std::wstring& ext_w) {
    const std::wstring backup_key = record_key + L"\\Previous\\" + ext_w;
    const std::wstring ext_classes = ClassesKey(ext_w);
    const auto saved = store.ReadDword(backup_key, L"Saved");
    if (!saved.has_value()) {
        return store.DeleteValue(ext_classes, nullptr);
    }
    if (*saved != 1u) return false;
    const auto present = store.ReadDword(backup_key, L"Present");
    if (!present.has_value()) return false;
    if (*present == 0u) return store.DeleteValue(ext_classes, nullptr);
    const auto type = store.ReadDword(backup_key, L"Type");
    const auto data = store.ReadRaw(backup_key, L"Data");
    if (!type.has_value() || !data.has_value()) return false;
    const uint8_t* ptr = data->data.empty() ? nullptr : data->data.data();
    return store.SetRaw(ext_classes, nullptr, *type, ptr, data->data.size());
}

bool RemoveSingleExtension(AssociationRegistryStore& store,
                           std::string_view package_id,
                           const std::wstring& record_key,
                           const std::wstring& ext_w) {
    const std::string ext_utf8 = Utf8Wide(ext_w);
    const std::wstring progid = ExtensionProgId(package_id, ext_utf8);
    const std::wstring ext_classes = ClassesKey(ext_w);
    const std::wstring open_with = ext_classes + L"\\OpenWithProgids";

    store.DeleteValue(open_with, progid.c_str());
    bool ok = true;
    const auto current_hkcu = store.ReadString(ext_classes, nullptr);
    if (current_hkcu && SameInsensitive(*current_hkcu, progid)) {
        ok = RestoreDefault(store, record_key, ext_w) && ok;
    }
    store.DeleteTree(record_key + L"\\Previous\\" + ext_w);
    store.DeleteTree(ClassesKey(progid));
    store.DeleteKeyIfEmpty(open_with);
    store.DeleteKeyIfEmpty(ext_classes);

    const std::wstring file_assocs = std::wstring(kExtAssocCapabilitiesKey) + L"\\FileAssociations";
    if (const auto cap = store.ReadString(file_assocs, ext_w.c_str()); cap && SameInsensitive(*cap, progid)) {
        store.DeleteValue(file_assocs, ext_w.c_str());
    }
    store.DeleteValue(record_key, ext_w.c_str());
    return ok;
}

void CleanupCapabilitiesIfEmpty(AssociationRegistryStore& store) {
    const std::wstring file_assocs = std::wstring(kExtAssocCapabilitiesKey) + L"\\FileAssociations";
    if (!store.EnumValueNames(file_assocs).empty()) return;
    if (const auto reg = store.ReadString(kExtAssocRegisteredAppsKey, kExtAssocAppName);
        reg && SameInsensitive(*reg, kExtAssocCapabilitiesKey)) {
        store.DeleteValue(kExtAssocRegisteredAppsKey, kExtAssocAppName);
    }
    store.DeleteTree(kExtAssocCapabilitiesKey);
}

} // namespace

std::wstring ExtensionProgId(std::string_view package_id, std::string_view extension) {
    uint32_t hash = 2166136261u;
    for (const unsigned char c : package_id) {
        hash ^= c;
        hash *= 16777619u;
    }
    std::wstring out = L"Pulse.Ext.";
    for (const char c : package_id) {
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        out.push_back(alnum ? static_cast<wchar_t>(c) : L'_');
    }
    out.push_back(L'_');
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back(hex[(hash >> shift) & 0xFu]);
    }
    out.push_back(L'.');
    const std::string_view ext_no_dot = (!extension.empty() && extension.front() == '.')
                                            ? extension.substr(1)
                                            : extension;
    for (const char c : ext_no_dot) {
        out.push_back(static_cast<wchar_t>(c));
    }
    return out;
}

std::wstring PackageAssocRecordKey(std::string_view package_id) {
    std::wstring key(kExtAssocRecordRoot);
    key.push_back(L'\\');
    key.append(WideUtf8(package_id));
    return key;
}

std::vector<AssociationQueryItem> QueryAssociations(
    AssociationRegistryStore& store,
    std::string_view package_id,
    const std::vector<std::pair<std::string, std::string>>& handler_extensions) {
    std::vector<AssociationQueryItem> results;
    results.reserve(handler_extensions.size());
    for (const auto& [handler_id, ext] : handler_extensions) {
        AssociationQueryItem item;
        item.handler_id = handler_id;
        item.extension = ext;
        const std::wstring ext_w = WideUtf8(ext);
        const std::wstring our_progid = ExtensionProgId(package_id, ext);
        std::wstring current_progid;
        const auto def_res = store.QueryEffectiveDefault(ext_w, current_progid);
        const auto handler_state = (def_res == AssocDefaultResult::Ok)
                                       ? store.ProbeHandlerState(current_progid)
                                       : AssocRegHandlerState::Unknown;

        if (def_res == AssocDefaultResult::Error) {
            item.owner = PULSE_ASSOC_OWNER_UNKNOWN;
        } else if (def_res == AssocDefaultResult::NoAssociation || handler_state == AssocRegHandlerState::Missing) {
            item.owner = PULSE_ASSOC_OWNER_FREE;
        } else if (SameInsensitive(current_progid, our_progid) && handler_state == AssocRegHandlerState::Available) {
            item.owner = PULSE_ASSOC_OWNER_PULSE;
        } else if (handler_state == AssocRegHandlerState::Unknown && !IsWindowsBuiltinViewer(current_progid)) {
            item.owner = PULSE_ASSOC_OWNER_UNKNOWN;
        } else {
            item.owner = PULSE_ASSOC_OWNER_OTHER;
            item.current_app = Utf8Wide(store.QueryFriendlyApp(ext_w, current_progid));
            if (IsWindowsBuiltinViewer(current_progid)) {
                item.flags |= PULSE_ASSOC_STATUS_FLAG_WINDOWS_DEFAULT;
            }
        }
        results.push_back(std::move(item));
    }
    return results;
}

bool ApplyAssociations(AssociationRegistryStore& store,
                       std::string_view package_id,
                       const std::wstring& pulse_exe,
                       const std::wstring& package_dir,
                       const std::wstring& app_description,
                       const std::vector<AssociationEntryRequest>& entries) {
    if (package_id.empty() || pulse_exe.empty()) return false;
    if (entries.empty()) return RemoveAllPackageAssociations(store, package_id);

    const std::wstring record_key = PackageAssocRecordKey(package_id);
    const std::wstring cap_key(kExtAssocCapabilitiesKey);
    const std::wstring file_assocs = cap_key + L"\\FileAssociations";
    const std::wstring pkg_w = WideUtf8(package_id);

    bool ok = store.SetString(cap_key, L"ApplicationName", kExtAssocAppName) &&
              store.SetString(cap_key, L"ApplicationDescription",
                              app_description.empty() ? std::wstring(L"Pulse") : app_description) &&
              store.SetString(cap_key, L"ApplicationIcon", pulse_exe + L",0") &&
              store.SetString(kExtAssocRegisteredAppsKey, kExtAssocAppName, cap_key);

    std::set<std::wstring> active_extensions;
    for (const auto& entry : entries) {
        const std::wstring ext_w = WideUtf8(entry.extension);
        if (ext_w.empty()) continue;
        active_extensions.insert(ext_w);

        const std::wstring progid = ExtensionProgId(package_id, entry.extension);
        const std::wstring progid_key = ClassesKey(progid);
        const std::wstring ext_classes = ClassesKey(ext_w);
        const std::wstring display_w = WideUtf8(entry.display_name);
        const std::wstring verb_w = entry.verb_label.empty() ? std::wstring(L"Open in Pulse") : WideUtf8(entry.verb_label);
        const std::wstring icon_w = ResolveDefaultIcon(pulse_exe, package_dir, entry.icon_resource);
        const std::wstring verb_flag = entry.action == PULSE_ASSOC_ACTION_PREVIEW ? L"\" --ext-preview " : L"\" --ext-open ";
        const std::wstring cmd_w = L"\"" + pulse_exe + verb_flag + pkg_w + L" " +
                                   WideUtf8(entry.handler_id) + L" \"%1\"";

        ok = store.SetString(progid_key, nullptr, display_w) &&
             store.SetString(progid_key, L"FriendlyTypeName", display_w) &&
             store.SetString(progid_key + L"\\DefaultIcon", nullptr, icon_w) &&
             store.SetString(progid_key + L"\\shell", nullptr, L"open") &&
             store.SetString(progid_key + L"\\shell\\open", nullptr, verb_w) &&
             store.SetString(progid_key + L"\\shell\\open\\command", nullptr, cmd_w) &&
             store.SetNone(ext_classes + L"\\OpenWithProgids", progid.c_str()) &&
             store.SetString(file_assocs, ext_w.c_str(), progid) &&
             ok;

        const auto user_default = store.ReadString(ext_classes, nullptr);
        std::wstring effective;
        const auto effective_res = store.QueryEffectiveDefault(ext_w, effective);
        const auto user_choice = store.ReadString(UserChoiceKey(ext_w), L"ProgId");

        const bool protected_choice = user_choice && !user_choice->empty() &&
                                      store.ProbeHandlerState(*user_choice) != AssocRegHandlerState::Missing;
        const bool protected_default = user_default && !user_default->empty() &&
                                       !IsWindowsBuiltinViewer(*user_default) &&
                                       !SameInsensitive(*user_default, progid) &&
                                       store.ProbeHandlerState(*user_default) != AssocRegHandlerState::Missing;
        const bool unclaimed = !protected_choice && !protected_default &&
                               (effective_res == AssocDefaultResult::NoAssociation ||
                                (effective_res == AssocDefaultResult::Ok &&
                                 (IsWindowsBuiltinViewer(effective) ||
                                  store.ProbeHandlerState(effective) == AssocRegHandlerState::Missing)));

        uint32_t claimed = 0;
        if (user_default && SameInsensitive(*user_default, progid)) {
            claimed = 1;
        } else if (unclaimed) {
            if (BackupAndClaimDefault(store, record_key, ext_w, progid)) claimed = 1;
            else ok = false;
        }
        ok = store.SetDword(record_key, ext_w.c_str(), claimed) && ok;
    }

    for (const auto& existing_ext : store.EnumValueNames(record_key)) {
        if (existing_ext.empty() || existing_ext.front() != L'.') continue;
        std::wstring lower_ext = existing_ext;
        for (wchar_t& c : lower_ext) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (!active_extensions.contains(lower_ext)) {
            ok = RemoveSingleExtension(store, package_id, record_key, lower_ext) && ok;
        }
    }

    store.NotifyAssocChanged();
    return ok;
}

bool RemoveExtensions(AssociationRegistryStore& store,
                      std::string_view package_id,
                      const std::vector<std::string>& extensions) {
    if (package_id.empty()) return false;
    const std::wstring record_key = PackageAssocRecordKey(package_id);
    bool ok = true;
    for (const auto& ext : extensions) {
        ok = RemoveSingleExtension(store, package_id, record_key, WideUtf8(ext)) && ok;
    }
    if (store.EnumValueNames(record_key).empty()) {
        store.DeleteTree(record_key);
    }
    CleanupCapabilitiesIfEmpty(store);
    store.NotifyAssocChanged();
    return ok;
}

bool RemoveAllPackageAssociations(AssociationRegistryStore& store, std::string_view package_id) {
    if (package_id.empty()) return false;
    const std::wstring record_key = PackageAssocRecordKey(package_id);
    const auto recorded = store.EnumValueNames(record_key);
    if (recorded.empty()) {
        store.DeleteTree(record_key);
        return true;
    }
    bool ok = true;
    for (const auto& ext_w : recorded) {
        if (!ext_w.empty() && ext_w.front() == L'.') {
            ok = RemoveSingleExtension(store, package_id, record_key, ext_w) && ok;
        }
    }
    if (ok) store.DeleteTree(record_key);
    CleanupCapabilitiesIfEmpty(store);
    store.NotifyAssocChanged();
    return ok;
}

} // namespace pulse::extensions
