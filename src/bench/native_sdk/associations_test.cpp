#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../app/extensions/associations_cleanup.h"
#include "../../app/extensions/associations_registry.h"
#include "../../app/extensions/ext_open_intent.h"
#include "../../app/extensions/manifest.h"
#include "../../app/extensions/native/associations_adapter.h"
#include "../../app/extensions/native/diagnostics_adapter.h"

#include <pulse/client.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace pulse::extensions;
using namespace pulse::extensions::native;
using pulse::sdk::AssociationEntrySpec;
using pulse::sdk::AssociationStatusData;
using pulse::sdk::AssociationsClient;
using pulse::sdk::Diagnostic;
using pulse::sdk::DiagnosticsClient;
using pulse::sdk::View;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) ++g_passed;
    else ++g_failed;
}

std::wstring FoldAscii(std::wstring_view in) {
    std::wstring out(in);
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

class MockRegistryStore final : public AssociationRegistryStore {
public:
    struct Entry {
        std::wstring original_name;
        uint32_t type = REG_NONE;
        std::vector<uint8_t> data;
    };

    std::map<std::wstring, std::map<std::wstring, Entry>> keys; // folded_subkey -> folded_value -> Entry
    std::map<std::wstring, std::pair<AssocDefaultResult, std::wstring>> effective_defaults; // folded_ext -> (result, progid)
    std::map<std::wstring, AssocRegHandlerState> handler_states; // folded_progid -> state
    std::map<std::wstring, std::wstring> friendly_apps; // folded_ext|folded_progid -> app
    int notify_count = 0;

    static std::wstring ValueKey(const wchar_t* name) {
        return name ? FoldAscii(name) : std::wstring{};
    }

    static std::wstring OriginalValueName(const wchar_t* name) {
        return name ? std::wstring(name) : std::wstring{};
    }

    std::optional<std::wstring> ReadString(const std::wstring& subkey, const wchar_t* value) override {
        const auto raw = ReadRaw(subkey, value);
        if (!raw || raw->type != REG_SZ) return std::nullopt;
        if (raw->data.size() < sizeof(wchar_t)) return std::wstring{};
        std::wstring out(raw->data.size() / sizeof(wchar_t), L'\0');
        std::memcpy(out.data(), raw->data.data(), out.size() * sizeof(wchar_t));
        while (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    std::optional<AssocRegBinary> ReadRaw(const std::wstring& subkey, const wchar_t* value) override {
        const auto kit = keys.find(FoldAscii(subkey));
        if (kit == keys.end()) return std::nullopt;
        const auto vit = kit->second.find(ValueKey(value));
        if (vit == kit->second.end()) return std::nullopt;
        return AssocRegBinary{vit->second.type, vit->second.data};
    }

    std::optional<uint32_t> ReadDword(const std::wstring& subkey, const wchar_t* value) override {
        const auto raw = ReadRaw(subkey, value);
        if (!raw || raw->type != REG_DWORD || raw->data.size() != sizeof(uint32_t)) return std::nullopt;
        uint32_t out = 0;
        std::memcpy(&out, raw->data.data(), sizeof(out));
        return out;
    }

    bool SetString(const std::wstring& subkey, const wchar_t* value, const std::wstring& data) override {
        const auto* bytes = reinterpret_cast<const uint8_t*>(data.c_str());
        return SetRaw(subkey, value, REG_SZ, bytes, (data.size() + 1) * sizeof(wchar_t));
    }

    bool SetRaw(const std::wstring& subkey, const wchar_t* value, uint32_t type,
                const uint8_t* data, size_t size) override {
        Entry e;
        e.original_name = OriginalValueName(value);
        e.type = type;
        if (data && size > 0) e.data.assign(data, data + size);
        keys[FoldAscii(subkey)][ValueKey(value)] = std::move(e);
        return true;
    }

    bool SetDword(const std::wstring& subkey, const wchar_t* value, uint32_t data) override {
        return SetRaw(subkey, value, REG_DWORD, reinterpret_cast<const uint8_t*>(&data), sizeof(data));
    }

    bool SetNone(const std::wstring& subkey, const wchar_t* value) override {
        return SetRaw(subkey, value, REG_NONE, nullptr, 0);
    }

    bool DeleteValue(const std::wstring& subkey, const wchar_t* value) override {
        const auto kit = keys.find(FoldAscii(subkey));
        if (kit == keys.end()) return true;
        kit->second.erase(ValueKey(value));
        return true;
    }

    bool DeleteTree(const std::wstring& subkey) override {
        const std::wstring folded = FoldAscii(subkey);
        const std::wstring prefix = folded + L"\\";
        for (auto it = keys.begin(); it != keys.end();) {
            if (it->first == folded || it->first.starts_with(prefix)) {
                it = keys.erase(it);
            } else {
                ++it;
            }
        }
        return true;
    }

    void DeleteKeyIfEmpty(const std::wstring& subkey) override {
        const std::wstring folded = FoldAscii(subkey);
        const std::wstring prefix = folded + L"\\";
        for (const auto& [k, _] : keys) {
            if (k.starts_with(prefix)) return;
        }
        const auto kit = keys.find(folded);
        if (kit != keys.end() && kit->second.empty()) keys.erase(kit);
    }

    std::vector<std::wstring> EnumValueNames(const std::wstring& subkey) override {
        std::vector<std::wstring> names;
        const auto kit = keys.find(FoldAscii(subkey));
        if (kit == keys.end()) return names;
        for (const auto& [_, entry] : kit->second) {
            if (!entry.original_name.empty()) names.push_back(entry.original_name);
        }
        return names;
    }

    AssocDefaultResult QueryEffectiveDefault(const std::wstring& extension, std::wstring& out_progid) override {
        out_progid.clear();
        const std::wstring ext_f = FoldAscii(extension);
        if (const auto it = effective_defaults.find(ext_f); it != effective_defaults.end()) {
            out_progid = it->second.second;
            return it->second.first;
        }
        const auto hkcu = ReadString(L"Software\\Classes\\" + extension, nullptr);
        if (hkcu && !hkcu->empty()) {
            out_progid = *hkcu;
            return AssocDefaultResult::Ok;
        }
        return AssocDefaultResult::NoAssociation;
    }

    std::wstring QueryFriendlyApp(const std::wstring& extension, const std::wstring& progid) override {
        const auto it = friendly_apps.find(FoldAscii(extension) + L"|" + FoldAscii(progid));
        if (it != friendly_apps.end()) return it->second;
        return progid;
    }

    AssocRegHandlerState ProbeHandlerState(const std::wstring& progid) override {
        if (progid.empty()) return AssocRegHandlerState::Missing;
        const std::wstring pf = FoldAscii(progid);
        if (const auto it = handler_states.find(pf); it != handler_states.end()) {
            return it->second;
        }
        const auto cmd = ReadString(L"Software\\Classes\\" + progid + L"\\shell\\open\\command", nullptr);
        if (cmd && !cmd->empty()) return AssocRegHandlerState::Available;
        return keys.contains(FoldAscii(L"Software\\Classes\\" + progid))
                   ? AssocRegHandlerState::Unknown
                   : AssocRegHandlerState::Missing;
    }

    void NotifyAssocChanged() override { ++notify_count; }
};

void TestExtOpenIntent() {
    const auto formatted = FormatExtOpenIntent(L"acme.archiver", L"archive-open", L"\"D:\\My Files\\a.7z\"", false);
    Check(formatted == L"pulse:ext-open:acme.archiver/archive-open/D:\\My Files\\a.7z",
          "FormatExtOpenIntent formats command open intent and strips quotes");

    const auto parsed = ParseExtOpenIntent(L"PULSE:EXT-OPEN:acme.archiver/archive-open/\"C:\\测试\\pack.cbz\"");
    Check(parsed && !parsed->preview && parsed->package_id == "acme.archiver" &&
              parsed->handler_id == "archive-open" && parsed->path == L"C:\\测试\\pack.cbz",
          "ParseExtOpenIntent parses case-insensitive prefix and Unicode path");

    const auto formatted_preview = FormatExtOpenIntent(L"acme.comic", L"comic-view", L"C:\\Comics\\v1.cbz", true);
    Check(formatted_preview == L"pulse:ext-preview:acme.comic/comic-view/C:\\Comics\\v1.cbz" &&
              IsExtOpenIntent(formatted_preview),
          "FormatExtOpenIntent(preview=true) formats preview intent");

    const auto parsed_preview = ParseExtOpenIntent(formatted_preview);
    Check(parsed_preview && parsed_preview->preview && parsed_preview->package_id == "acme.comic" &&
              parsed_preview->handler_id == "comic-view" && parsed_preview->path == L"C:\\Comics\\v1.cbz",
          "ParseExtOpenIntent preserves preview=true flag");

    Check(!ParseExtOpenIntent(L"pulse:ext-open:bad pkg/h/C:\\a.7z").has_value(),
          "ParseExtOpenIntent rejects spaces in package token");
    Check(!ParseExtOpenIntent(L"pulse:ext-open:acme.archiver/missing-path/").has_value(),
          "ParseExtOpenIntent rejects empty path");
    Check(!ParseExtOpenIntent(L"pulse:ext-open:acme.archiver/h/C:\\bad\npath").has_value(),
          "ParseExtOpenIntent rejects control characters in path");
    Check(FormatExtOpenIntent(L"acme/bad", L"h", L"C:\\a.7z").empty(),
          "FormatExtOpenIntent rejects slashes in package token");
}

void TestRegistryLifecycle() {
    MockRegistryStore store;
    const std::wstring progid_a_7z = ExtensionProgId("pkg.alpha", ".7z");
    const std::wstring progid_b_7z = ExtensionProgId("pkg.beta", ".7z");
    Check(progid_a_7z.starts_with(L"Pulse.Ext.") && !progid_a_7z.starts_with(L"Pulse.Archive.") &&
              progid_a_7z != progid_b_7z,
          "ExtensionProgId isolates per-package ProgIDs without Pulse.Archive collision");

    // Seed mixed OS states before apply:
    // .7z: free (unassociated)
    // .zip: Windows CompressedFolder default
    // .rar: UserChoice WinRAR.RAR (protected!)
    // .cbz: Prior HKCU REG_EXPAND_SZ ProgId that resolves to dead/missing handler (claimable with exact backup)
    store.effective_defaults[L".zip"] = {AssocDefaultResult::Ok, L"CompressedFolder"};
    store.handler_states[L"compressedfolder"] = AssocRegHandlerState::Available;
    store.friendly_apps[L".zip|compressedfolder"] = L"Windows Explorer";

    store.SetString(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.rar\\UserChoice",
                    L"ProgId", L"WinRAR.RAR");
    store.effective_defaults[L".rar"] = {AssocDefaultResult::Ok, L"WinRAR.RAR"};
    store.handler_states[L"winrar.rar"] = AssocRegHandlerState::Available;
    store.friendly_apps[L".rar|winrar.rar"] = L"WinRAR";

    const std::wstring stale_cbz = L"%MY_STALE_APP%.Cbz";
    store.SetRaw(L"Software\\Classes\\.cbz", nullptr, REG_EXPAND_SZ,
                 reinterpret_cast<const uint8_t*>(stale_cbz.c_str()),
                 (stale_cbz.size() + 1) * sizeof(wchar_t));
    store.effective_defaults[L".cbz"] = {AssocDefaultResult::Ok, stale_cbz};
    store.handler_states[FoldAscii(stale_cbz)] = AssocRegHandlerState::Missing;

    // Initial query before Apply
    const auto pre_query = QueryAssociations(
        store, "pkg.alpha",
        {{"arch", ".7z"}, {"arch", ".zip"}, {"arch", ".rar"}, {"comic", ".cbz"}});
    Check(pre_query.size() == 4 &&
              pre_query[0].owner == PULSE_ASSOC_OWNER_FREE &&
              pre_query[1].owner == PULSE_ASSOC_OWNER_OTHER &&
              (pre_query[1].flags & PULSE_ASSOC_STATUS_FLAG_WINDOWS_DEFAULT) != 0 &&
              pre_query[1].current_app == "Windows Explorer" &&
              pre_query[2].owner == PULSE_ASSOC_OWNER_OTHER &&
              (pre_query[2].flags & PULSE_ASSOC_STATUS_FLAG_WINDOWS_DEFAULT) == 0 &&
              pre_query[2].current_app == "WinRAR" &&
              pre_query[3].owner == PULSE_ASSOC_OWNER_FREE,
          "QueryAssociations accurately classifies FREE, OTHER+WINDOWS_DEFAULT, OTHER, and stale Missing");

    // Apply entries for pkg.alpha
    const std::vector<AssociationEntryRequest> alpha_entries = {
        {"arch", ".7z", "7Z Archive", "Open with Alpha", "icons.dll,-101", PULSE_ASSOC_ACTION_COMMAND},
        {"arch", ".zip", "ZIP Archive", "Open with Alpha", "icons.dll,-102", PULSE_ASSOC_ACTION_COMMAND},
        {"arch", ".rar", "RAR Archive", "", "", PULSE_ASSOC_ACTION_COMMAND},
        {"comic", ".cbz", "Comic Archive", "Preview Comic", "", PULSE_ASSOC_ACTION_PREVIEW},
    };
    Check(ApplyAssociations(store, "pkg.alpha", L"C:\\Pulse\\Pulse.exe", L"C:\\Pkg\\Alpha", L"Pulse", alpha_entries),
          "ApplyAssociations succeeds across mixed claim/non-claim extensions");

    const auto cmd_7z = store.ReadString(L"Software\\Classes\\" + progid_a_7z + L"\\shell\\open\\command", nullptr);
    const auto cmd_cbz = store.ReadString(
        L"Software\\Classes\\" + ExtensionProgId("pkg.alpha", ".cbz") + L"\\shell\\open\\command", nullptr);
    const auto icon_7z = store.ReadString(L"Software\\Classes\\" + progid_a_7z + L"\\DefaultIcon", nullptr);
    const auto icon_rar = store.ReadString(
        L"Software\\Classes\\" + ExtensionProgId("pkg.alpha", ".rar") + L"\\DefaultIcon", nullptr);
    Check(cmd_7z && *cmd_7z == L"\"C:\\Pulse\\Pulse.exe\" --ext-open pkg.alpha arch \"%1\"",
          "ApplyAssociations writes --ext-open command for ACTION_COMMAND");
    Check(cmd_cbz && *cmd_cbz == L"\"C:\\Pulse\\Pulse.exe\" --ext-preview pkg.alpha comic \"%1\"",
          "ApplyAssociations writes --ext-preview command for ACTION_PREVIEW");
    Check(icon_7z && *icon_7z == L"C:\\Pkg\\Alpha\\icons.dll,-101" &&
              icon_rar && *icon_rar == L"C:\\Pulse\\Pulse.exe,0",
          "ApplyAssociations resolves package flat icon resource or falls back to Pulse.exe,0");

    // Verify claiming & UserChoice protection
    Check(store.ReadString(L"Software\\Classes\\.7z", nullptr) == progid_a_7z &&
              store.ReadString(L"Software\\Classes\\.zip", nullptr) == ExtensionProgId("pkg.alpha", ".zip") &&
              store.ReadString(L"Software\\Classes\\.cbz", nullptr) == ExtensionProgId("pkg.alpha", ".cbz"),
          "ApplyAssociations claims FREE, CompressedFolder, and missing-handler HKCU defaults");
    Check(!store.ReadString(L"Software\\Classes\\.rar", nullptr).has_value() &&
              store.ReadString(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.rar\\UserChoice", L"ProgId") == L"WinRAR.RAR" &&
              store.ReadRaw(L"Software\\Classes\\.rar\\OpenWithProgids", ExtensionProgId("pkg.alpha", ".rar").c_str()).has_value() &&
              store.ReadString(std::wstring(kExtAssocCapabilitiesKey) + L"\\FileAssociations", L".rar") == ExtensionProgId("pkg.alpha", ".rar"),
          "ApplyAssociations never overwrites UserChoice yet registers OpenWithProgids and Default Apps Capabilities");

    // Update effective default simulation after HKCU claim for .7z and .cbz -> Query shows PULSE
    store.effective_defaults.erase(L".7z");
    store.effective_defaults.erase(L".cbz");
    const auto mid_query = QueryAssociations(store, "pkg.alpha", {{"arch", ".7z"}, {"arch", ".rar"}, {"comic", ".cbz"}});
    Check(mid_query[0].owner == PULSE_ASSOC_OWNER_PULSE &&
              mid_query[1].owner == PULSE_ASSOC_OWNER_OTHER &&
              mid_query[2].owner == PULSE_ASSOC_OWNER_PULSE,
          "QueryAssociations reports PULSE after HKCU ProgID claim");

    // Reconcile case-insensitively and check omitted extension removal:
    // Also register a second package pkg.beta on .tar so Capabilities remains shared when alpha is removed.
    Check(ApplyAssociations(store, "pkg.beta", L"C:\\Pulse\\Pulse.exe", L"C:\\Pkg\\Beta", L"Pulse",
                            {{"beta-h", ".tar", "TAR", "Open", "", PULSE_ASSOC_ACTION_COMMAND}}),
          "ApplyAssociations registers second package in shared Capabilities");

    // Now alpha drops .zip via apply replace-set (keeping .7Z uppercase test in record value)
    store.SetDword(PackageAssocRecordKey("pkg.alpha"), L".7Z", 1u);
    const std::vector<AssociationEntryRequest> alpha_reduced = {
        {"arch", ".7z", "7Z Archive", "Open with Alpha", "", PULSE_ASSOC_ACTION_COMMAND},
        {"comic", ".cbz", "Comic Archive", "Preview Comic", "", PULSE_ASSOC_ACTION_PREVIEW},
    };
    Check(ApplyAssociations(store, "pkg.alpha", L"C:\\Pulse\\Pulse.exe", L"C:\\Pkg\\Alpha", L"Pulse", alpha_reduced),
          "ApplyAssociations replace-set succeeds");
    Check(store.ReadString(L"Software\\Classes\\.7z", nullptr) == progid_a_7z &&
              !store.ReadString(L"Software\\Classes\\.zip", nullptr).has_value() &&
              !store.keys.contains(FoldAscii(L"Software\\Classes\\" + ExtensionProgId("pkg.alpha", ".zip"))),
          "ApplyAssociations keeps uppercase-recorded .7z while cleanly removing omitted .zip");

    // RemoveAll for pkg.alpha restores absent .7z and restores exact REG_EXPAND_SZ .cbz backup,
    // while keeping shared Capabilities intact because pkg.beta still uses it.
    Check(RemoveAllPackageAssociations(store, "pkg.alpha"), "RemoveAllPackageAssociations for alpha succeeds");
    const auto restored_cbz = store.ReadRaw(L"Software\\Classes\\.cbz", nullptr);
    Check(!store.ReadRaw(L"Software\\Classes\\.7z", nullptr).has_value() &&
              restored_cbz && restored_cbz->type == REG_EXPAND_SZ &&
              std::memcmp(restored_cbz->data.data(), stale_cbz.c_str(), (stale_cbz.size() + 1) * sizeof(wchar_t)) == 0 &&
              store.ReadString(kExtAssocRegisteredAppsKey, kExtAssocAppName) == std::wstring(kExtAssocCapabilitiesKey),
          "RemoveAllPackageAssociations restores exact HKCU defaults and keeps shared Capabilities for pkg.beta");

    // Removing last package cleans Capabilities and RegisteredApplications completely.
    Check(RemoveAllPackageAssociations(store, "pkg.beta") &&
              !store.ReadString(kExtAssocRegisteredAppsKey, kExtAssocAppName).has_value() &&
              !store.keys.contains(FoldAscii(kExtAssocCapabilitiesKey)),
          "RemoveAllPackageAssociations removes shared Capabilities when last package is removed");
}

void TestAdapterAndClient() {
    auto diagnostics_sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diagnostics_adapter(diagnostics_sink, [] { return true; });
    PulseDiagnosticsV1 diag_table{};
    Check(diagnostics_adapter.Query(PULSE_DIAGNOSTICS_VERSION_1, &diag_table, sizeof(diag_table)) == PULSE_OK,
          "DiagnosticsAdapter provides V1 table");

    MockRegistryStore store;
    int wake_count = 0;
    uint64_t cancelled_req_id = 0;
    std::string chooser_ext;
    int default_apps_calls = 0;

    ManifestFileHandlerContribution h1;
    h1.id = "archive";
    h1.extensions = {".7z", ".zip"};
    h1.command = "extract";
    ManifestFileHandlerContribution h2;
    h2.id = "comic";
    h2.extensions = {".cbz"};
    h2.command = "preview";
    h2.action = FileHandlerAction::Preview;

    AssociationsBackend backend;
    backend.valid_owner = [] { return true; };
    backend.query = [&store](uint64_t,
                             std::vector<std::pair<std::string, std::string>> pairs,
                             std::function<void(AssociationsQueryReply)> complete) -> PulseStatus {
        AssociationsQueryReply reply;
        reply.status = PULSE_OK;
        reply.items = QueryAssociations(store, "sample.pkg", pairs);
        if (complete) complete(std::move(reply));
        return PULSE_OK;
    };
    backend.apply = [&store](uint64_t,
                             std::vector<AssociationEntryRequest> entries,
                             std::function<void(PulseStatus)> complete) -> PulseStatus {
        const bool ok = ApplyAssociations(store, "sample.pkg", L"C:\\Pulse\\Pulse.exe", L"C:\\Sample", L"Pulse", entries);
        if (complete) complete(ok ? PULSE_OK : PULSE_ERROR_UNAVAILABLE);
        return PULSE_OK;
    };
    backend.remove = [&store](uint64_t,
                              std::vector<std::string> exts,
                              std::function<void(PulseStatus)> complete) -> PulseStatus {
        const bool ok = exts.empty()
                            ? RemoveAllPackageAssociations(store, "sample.pkg")
                            : RemoveExtensions(store, "sample.pkg", exts);
        if (complete) complete(ok ? PULSE_OK : PULSE_ERROR_UNAVAILABLE);
        return PULSE_OK;
    };
    backend.cancel = [&cancelled_req_id](uint64_t id) { cancelled_req_id = id; };
    backend.open_chooser = [&chooser_ext](std::string ext) {
        chooser_ext = std::move(ext);
        return PULSE_OK;
    };
    backend.open_default_apps = [&default_apps_calls]() {
        ++default_apps_calls;
        return PULSE_OK;
    };

    AssociationsAdapter adapter("sample.pkg", {h1, h2}, backend, [] { return true; },
                                diagnostics_sink, [&wake_count] { ++wake_count; });

    PulseAssociationsV1 assoc_table{};
    Check(adapter.Query(PULSE_ASSOCIATIONS_VERSION_1, &assoc_table, sizeof(assoc_table)) == PULSE_OK,
          "AssociationsAdapter queries V1 table");
    PulseAssociationsV1 wrong_size{};
    Check(adapter.Query(PULSE_ASSOCIATIONS_VERSION_1, &wrong_size, sizeof(wrong_size) - 1) == PULSE_ERROR_ARGUMENT &&
              adapter.Query(99u, &wrong_size, sizeof(wrong_size)) == PULSE_ERROR_ABI,
          "AssociationsAdapter validates ABI version and struct size");

    AssociationsClient client(assoc_table);
    DiagnosticsClient diag_client(diag_table);
    Check(client.Available(), "AssociationsClient reports available");

    // 1. Permission and argument validations + DiagnosticsSink
    AssociationsClient::Request bad_req;
    Check(client.Query({".rar"}, bad_req, [](PulseStatus, const std::vector<AssociationStatusData>&) {}) == PULSE_ERROR_PERMISSION &&
              client.Query({".exe"}, bad_req, [](PulseStatus, const std::vector<AssociationStatusData>&) {}) == PULSE_ERROR_ARGUMENT &&
              client.Query({".7z", ".7z"}, bad_req, [](PulseStatus, const std::vector<AssociationStatusData>&) {}) == PULSE_ERROR_ARGUMENT,
          "Query rejects undeclared, forbidden, and duplicate extensions");

    Diagnostic last_diag;
    Check(diag_client.LastError(last_diag) == PULSE_OK &&
              last_diag.interface_name == PULSE_ASSOCIATIONS_INTERFACE_NAME &&
              last_diag.operation == "query" &&
              last_diag.status == PULSE_ERROR_ARGUMENT,
          "DiagnosticsSink records structured validation error for pulse.associations");

    Check(client.Apply({{PULSE_ASSOC_ACTION_COMMAND, "unknown", ".7z", "7Z", "", ""}},
                       bad_req, [](PulseStatus) {}) == PULSE_ERROR_PERMISSION &&
              client.Apply({{PULSE_ASSOC_ACTION_COMMAND, "comic", ".7z", "7Z", "", ""}},
                           bad_req, [](PulseStatus) {}) == PULSE_ERROR_PERMISSION &&
              client.Apply({{99u, "archive", ".7z", "7Z", "", ""}},
                           bad_req, [](PulseStatus) {}) == PULSE_ERROR_ARGUMENT &&
              client.Apply({{PULSE_ASSOC_ACTION_COMMAND, "archive", ".7z", "7Z", "", "..\\x.dll"}},
                           bad_req, [](PulseStatus) {}) == PULSE_ERROR_ARGUMENT,
          "Apply enforces handler/extension ownership, valid action, and flat icon_resource");

    // 2. Async Apply -> QueryAll -> Remove -> RemoveAll through Poll()
    PulseStatus apply_status = PULSE_ERROR_UNAVAILABLE;
    AssociationsClient::Request apply_req;
    const PulseStatus start_apply = client.Apply(
        {
            {PULSE_ASSOC_ACTION_COMMAND, "archive", ".7z", "7Z Archive", "Open in Pulse", "icons.dll,0"},
            {PULSE_ASSOC_ACTION_PREVIEW, "comic", ".cbz", "Comic Archive", "Preview", ""},
        },
        apply_req,
        [&](PulseStatus st) { apply_status = st; });
    Check(start_apply == PULSE_OK && apply_req.Pending() && adapter.HasPendingWork() && wake_count == 1,
          "Apply schedules request and triggers wake");
    adapter.Poll();
    Check(apply_status == PULSE_OK && !apply_req.Pending() && !adapter.HasPendingWork(),
          "Poll delivers Apply completion and clears Request::Pending()");

    std::vector<AssociationStatusData> queried;
    PulseStatus query_status = PULSE_ERROR_UNAVAILABLE;
    const PulseStatus q_start = client.QueryAll(
        apply_req, [&](PulseStatus st, const std::vector<AssociationStatusData>& items) {
            query_status = st;
            queried = items;
        });
    Check(q_start == PULSE_OK && apply_req.Pending(), "QueryAll started for all declared extensions");
    adapter.Poll();
    Check(query_status == PULSE_OK && queried.size() == 3 &&
              queried[0].extension == ".7z" && queried[0].owner == PULSE_ASSOC_OWNER_PULSE &&
              queried[1].extension == ".zip" && queried[1].owner == PULSE_ASSOC_OWNER_FREE &&
              queried[2].extension == ".cbz" && queried[2].owner == PULSE_ASSOC_OWNER_PULSE,
          "QueryAll returns declared extension states");

    // 3. Cancellation suppresses callback before Poll
    bool cancel_fired = false;
    {
        AssociationsClient::Request cancel_req;
        Check(client.Remove({".7z"}, cancel_req, [&](PulseStatus) { cancel_fired = true; }) == PULSE_OK,
              "Remove schedules request");
        const PulseAssociationRequestId rid = cancel_req.Id();
        cancel_req.Cancel();
        Check(cancelled_req_id == rid && !cancel_req.Pending() && !adapter.HasPendingWork(),
              "Request::Cancel invokes backend cancel and drops pending entry");
    }
    adapter.Poll();
    Check(!cancel_fired, "Cancelled association callback is never invoked on Poll()");

    // 4. OpenChooser & OpenDefaultApps validation
    Check(client.OpenChooser(".rar") == PULSE_ERROR_PERMISSION &&
              client.OpenChooser(".exe") == PULSE_ERROR_ARGUMENT &&
              client.OpenChooser(".7z") == PULSE_OK && chooser_ext == ".7z" &&
              client.OpenDefaultApps() == PULSE_OK && default_apps_calls == 1,
          "OpenChooser enforces declared extensions and OpenDefaultApps delegates cleanly");

    // 5. Wrong-thread guard
    PulseStatus worker_status = PULSE_OK;
    std::thread worker([&]() { worker_status = client.OpenDefaultApps(); });
    worker.join();
    Check(worker_status == PULSE_ERROR_STATE, "Cross-thread access is rejected with PULSE_ERROR_STATE");

    // 6. Close revokes adapter safely
    adapter.Close();
    Check(client.OpenDefaultApps() == PULSE_ERROR_STATE, "Closed AssociationsAdapter rejects calls");
}

void TestCleanupHook() {
    std::vector<std::string> calls;
    SetPackageAssociationsCleanupHook([&](std::string_view pkg) { calls.emplace_back(pkg); });
    NotifyPackageAssociationsCleanup("");
    NotifyPackageAssociationsCleanup("acme.pkg");
    Check(calls == std::vector<std::string>{"acme.pkg"},
          "SetPackageAssociationsCleanupHook notifies non-empty package ids");

    SetPackageAssociationsCleanupHook([](std::string_view) { throw std::runtime_error("boom"); });
    bool no_throw = true;
    try {
        NotifyPackageAssociationsCleanup("acme.pkg");
    } catch (...) {
        no_throw = false;
    }
    Check(no_throw, "NotifyPackageAssociationsCleanup isolates hook exceptions");

    calls.clear();
    SetPackageAssociationsCleanupHook(nullptr);
    NotifyPackageAssociationsCleanup("acme.pkg");
    Check(calls.empty(), "Clearing cleanup hook stops subsequent notifications");
}

} // namespace

int main() {
    TestExtOpenIntent();
    TestRegistryLifecycle();
    TestAdapterAndClient();
    TestCleanupHook();
    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
