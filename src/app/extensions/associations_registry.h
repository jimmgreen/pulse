#pragma once

#include "../../../sdk/native/include/pulse/associations.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions {

enum class AssocRegHandlerState { Available, Missing, Unknown };
enum class AssocDefaultResult { Ok, NoAssociation, Error };

struct AssocRegBinary {
    uint32_t type = 1; // REG_SZ
    std::vector<uint8_t> data;
    bool operator==(const AssocRegBinary&) const = default;
};

struct AssociationRegistryStore {
    virtual ~AssociationRegistryStore() = default;
    virtual std::optional<std::wstring> ReadString(const std::wstring& subkey, const wchar_t* value) = 0;
    virtual std::optional<AssocRegBinary> ReadRaw(const std::wstring& subkey, const wchar_t* value) = 0;
    virtual std::optional<uint32_t> ReadDword(const std::wstring& subkey, const wchar_t* value) = 0;
    virtual bool SetString(const std::wstring& subkey, const wchar_t* value, const std::wstring& data) = 0;
    virtual bool SetRaw(const std::wstring& subkey, const wchar_t* value, uint32_t type,
                        const uint8_t* data, size_t size) = 0;
    virtual bool SetDword(const std::wstring& subkey, const wchar_t* value, uint32_t data) = 0;
    virtual bool SetNone(const std::wstring& subkey, const wchar_t* value) = 0;
    virtual bool DeleteValue(const std::wstring& subkey, const wchar_t* value) = 0;
    virtual bool DeleteTree(const std::wstring& subkey) = 0;
    virtual void DeleteKeyIfEmpty(const std::wstring& subkey) = 0;
    virtual std::vector<std::wstring> EnumValueNames(const std::wstring& subkey) = 0;
    virtual AssocDefaultResult QueryEffectiveDefault(const std::wstring& extension, std::wstring& out_progid) = 0;
    virtual std::wstring QueryFriendlyApp(const std::wstring& extension, const std::wstring& progid) = 0;
    virtual AssocRegHandlerState ProbeHandlerState(const std::wstring& progid) = 0;
    virtual void NotifyAssocChanged() = 0;
};

struct AssociationEntryRequest {
    std::string handler_id;
    std::string extension;
    std::string display_name;
    std::string verb_label;
    std::string icon_resource;
    uint32_t action = PULSE_ASSOC_ACTION_COMMAND;
};

struct AssociationQueryItem {
    std::string handler_id;
    std::string extension;
    uint32_t owner = PULSE_ASSOC_OWNER_UNREGISTERED;
    uint32_t flags = 0;
    std::string current_app;
    bool operator==(const AssociationQueryItem&) const = default;
};

inline constexpr wchar_t kExtAssocRecordRoot[] = L"Software\\Pulse\\ExtAssociations";
inline constexpr wchar_t kExtAssocCapabilitiesKey[] = L"Software\\Pulse\\Capabilities";
inline constexpr wchar_t kExtAssocRegisteredAppsKey[] = L"Software\\RegisteredApplications";
inline constexpr wchar_t kExtAssocAppName[] = L"Pulse";

std::wstring ExtensionProgId(std::string_view package_id, std::string_view extension);
std::wstring PackageAssocRecordKey(std::string_view package_id);

std::vector<AssociationQueryItem> QueryAssociations(AssociationRegistryStore& store,
                                                    std::string_view package_id,
                                                    const std::vector<std::pair<std::string, std::string>>& handler_extensions);

bool ApplyAssociations(AssociationRegistryStore& store,
                       std::string_view package_id,
                       const std::wstring& pulse_exe,
                       const std::wstring& package_dir,
                       const std::wstring& app_description,
                       const std::vector<AssociationEntryRequest>& entries);

bool RemoveExtensions(AssociationRegistryStore& store,
                      std::string_view package_id,
                      const std::vector<std::string>& extensions);

bool RemoveAllPackageAssociations(AssociationRegistryStore& store, std::string_view package_id);

} // namespace pulse::extensions
