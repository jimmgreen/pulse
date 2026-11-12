#include "../app/extensions/icon_registry.h"
#include "../app/extensions/manifest.h"
#include "../app/extensions/native/ui_model.h"
#include "../app/extensions/native_image_verify.h"
#include "../app/extensions/package_store.h"
#include "../app/extensions/ui_tree.h"
#include "../common/json_value.h"
#include <pulse/client.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
using namespace pulse::extensions;
using pulse::json::Value;
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
Value Native(const std::string& hash) {
    auto value = Value::Object();
    value.set("version", 1); value.set("architecture", "windows-x64");
    value.set("library", "plugin.dll"); value.set("sha256", hash);
    return value;
}
Value Manifest(const char* id, const Value& native) {
    auto value = Value::Object();
    value.set("apiVersion", 1); value.set("id", id); value.set("name", id);
    value.set("native", native);
    return value;
}
void Reject(const Value& native, const char* name) {
    const auto result = ParseManifest(pulse::json::Serialize(Manifest("native", native)));
    Check(!result && !result.error.empty(), name);
}
template<class T>
void Put(std::string& bytes, size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
std::string Image(bool dll = true) {
    std::string bytes(1024, '\0');
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 128;
    Put(bytes, 0, dos);
    const DWORD signature = IMAGE_NT_SIGNATURE; Put(bytes, 128, signature);
    IMAGE_FILE_HEADER file{};
    file.Machine = IMAGE_FILE_MACHINE_AMD64; file.NumberOfSections = 1;
    file.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    file.Characteristics = (dll ? IMAGE_FILE_DLL : 0) | IMAGE_FILE_EXECUTABLE_IMAGE;
    Put(bytes, 132, file);
    IMAGE_OPTIONAL_HEADER64 optional{};
    optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC; optional.SizeOfImage = 8192;
    optional.SizeOfHeaders = 512; optional.SectionAlignment = 4096; optional.FileAlignment = 512;
    Put(bytes, 152, optional);
    IMAGE_SECTION_HEADER section{};
    std::memcpy(section.Name, ".text", 5);
    section.VirtualAddress = 4096; section.Misc.VirtualSize = 512;
    section.PointerToRawData = 512; section.SizeOfRawData = 512;
    Put(bytes, 152 + sizeof(optional), section);
    return bytes;
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        root = std::filesystem::absolute(std::filesystem::path("bench_data") /
            ("native-contract-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
        std::filesystem::create_directories(root / "versions");
        std::filesystem::create_directory(root / "binaries");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Write(const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("Cannot write native contract fixture");
    }
    Value Entry(const char* id, const Value& manifest, bool enabled = true) {
        const auto text = pulse::json::Serialize(manifest);
        const auto hash = PackageRevision(text);
        Write(root / "versions" / (hash + ".json"), text);
        auto entry = Value::Object(); entry.set("id", id); entry.set("name", id);
        entry.set("enabled", enabled); entry.set("activeRevision", hash);
        return entry;
    }
    void Registry(std::initializer_list<Value> entries) {
        auto root_value = Value::Object(); root_value.set("formatVersion", 1);
        auto packages = Value::Array(); for (const auto& entry : entries) packages.push(entry);
        root_value.set("packages", packages);
        Write(root / "registry.json", pulse::json::Serialize(root_value));
    }
    void RejectStore(const char* name) {
        const auto result = LoadPackageStore(root.wstring());
        Check(!result && !result.error.empty() && result.packages.empty() && result.native_paths.empty() &&
            result.revisions.empty() && result.manifest_bytes.empty() && result.signatures.empty(), name);
    }
};
void ManifestCases() {
    const auto native = Native(std::string(64, 'a'));
    const auto parsed = ParseManifest(pulse::json::Serialize(Manifest("native", native)));
    Check(parsed && parsed.manifest->native && parsed.manifest->native->version == 1 &&
        parsed.manifest->native->architecture == "windows-x64" && parsed.manifest->native->library == "plugin.dll" &&
        parsed.manifest->native->sha256 == std::string(64, 'a') && parsed.manifest->id == "native" && parsed.manifest->resources.empty(),
        "native declaration accepts no commands and no declarative panel");
    auto multifile = native;
    auto files = Value::Array();
    auto resource = Value::Object(); resource.set("path", "License.txt"); resource.set("sha256", std::string(64, 'b'));
    files.push(resource); multifile.set("files", files);
    const auto accepted = ParseManifest(pulse::json::Serialize(Manifest("native", multifile)));
    Check(accepted && accepted.manifest->native->files.size() == 1, "native accepts declared resource without changing entry ABI");
    for (const auto* name : {"PLUGIN.DLL", "../a", "a/b", "CON.txt", "a.", "package.json", "MANIFEST.JSON"}) {
        auto bad = multifile; auto entries = Value::Array(); auto item = resource; item.set("path", name); entries.push(item); bad.set("files", entries);
        Reject(bad, "native rejects invalid or duplicate sidecar filename");
    }
    auto duplicate = multifile; files.push(resource); duplicate.set("files", files);
    Reject(duplicate, "native rejects duplicate resources");
    auto empty = multifile; empty.set("files", Value::Array()); Reject(empty, "native rejects empty explicit file list");
    for (const auto& version : {Value(0), Value(2), Value(true), Value("1"), Value(1.5), Value()}) {
        auto bad = native; bad.set("version", version); Reject(bad, "native rejects invalid version type or value");
    }
    for (const auto* architecture : {"Windows-x64", "windows-X64", "windows-x86", "windows-arm64", ""}) {
        auto bad = native; bad.set("architecture", architecture); Reject(bad, "native architecture is exact windows-x64");
    }
    for (const auto* library : {"../plugin.dll", "a/plugin.dll", "a\\plugin.dll", "C:plugin.dll", "plugin.dll:stream", "plugin.DLL", "plugin.dll.", "plugin.dll ", ".dll", "CON.dll", "con.any.dll", "PRN.dll", "AUX.dll", "nul.dll", "COM1.dll", "lpt9.dll", "COM\xc2\xb9.dll", "CLOCK$.dll", "CONIN$.dll", "CONOUT$.dll", "con .dll", "bad?.dll", "bad|.dll", "bad*.dll", "bad<.dll", "bad>.dll", "bad\".dll"}) {
        auto bad = native; bad.set("library", library); Reject(bad, "native rejects paths streams reserved names or invalid DLL spelling");
    }
    for (const auto& hash : {std::string(64, 'A'), std::string(64, 'g'), std::string(63, 'a'), std::string(65, '0'), std::string()}) {
        auto bad = native; bad.set("sha256", hash); Reject(bad, "native hash is exactly 64 lowercase hex characters");
    }
    for (const auto* field : {"version", "architecture", "library", "sha256"}) {
        auto missing = Value::Object();
        for (const auto& key : native.keys()) if (key != field) missing.set(key, *native.find(key));
        Reject(missing, "native rejects each missing required field");
        auto bad = native; bad.set(field, Value()); Reject(bad, "native rejects null required fields");
    }
    auto bad = native; bad.set("extra", true); Reject(bad, "native rejects unknown fields");
    Reject(Value(), "native rejects null declaration");
    Reject(Value::Array(), "native rejects array declaration");
    auto mixed = Manifest("native", native); mixed.set("runtime", Value::Object());
    Check(!ParseManifest(pulse::json::Serialize(mixed)), "native packages reject the removed runtime field");
    auto changed = native; changed.set("sha256", std::string(64, 'b'));
    Check(PackageRevision(pulse::json::Serialize(Manifest("native", native))) !=
        PackageRevision(pulse::json::Serialize(Manifest("native", changed))), "native binary hash change creates a new manifest revision");
    {
        std::ifstream sz_in("extensions/7zip/manifest.json", std::ios::binary);
        std::string sz_bytes((std::istreambuf_iterator<char>(sz_in)), {});
        const auto sz = ParseManifest(sz_bytes);
        Check(sz && sz.manifest->id == "pulse.7zip" && sz.manifest->command_contributions_declared &&
            sz.manifest->command_contributions.size() == 6 &&
            sz.manifest->command_contributions[0].id == "extract" &&
            sz.manifest->command_contributions[0].extensions.size() == 32,
            "official 7-Zip manifest static command contributions parse cleanly");
        auto empty_contributes = Manifest("native", native);
        auto contributes = Value::Object();
        contributes.set("commands", Value::Array());
        empty_contributes.set("contributes", contributes);
        const auto empty_parsed = ParseManifest(pulse::json::Serialize(empty_contributes));
        Check(empty_parsed && empty_parsed.manifest->command_contributions_declared &&
            empty_parsed.manifest->command_contributions.empty(),
            "explicit empty contributes.commands marks static command discovery");
        auto bad_ext = empty_contributes;
        auto cmds = Value::Array();
        auto cmd = Value::Object();
        cmd.set("id", "run"); cmd.set("title", "Run");
        auto exts = Value::Array(); exts.push(".ZIP"); cmd.set("extensions", exts);
        cmds.push(cmd);
        contributes.set("commands", cmds);
        bad_ext.set("contributes", contributes);
        Check(!ParseManifest(pulse::json::Serialize(bad_ext)), "contributes.commands rejects uppercase file extensions");
    }
}
void ImageAndStoreCases() {
    Fixture fixture;
    auto image = Image();
    const auto hash = PackageRevision(image);
    const auto path = fixture.root / "binaries" / (hash + ".dll");
    fixture.Write(path, image);
    HANDLE locked = INVALID_HANDLE_VALUE;
    std::string error;
    Check(OpenVerifiedNativeImage(path.wstring(), hash, locked, error) && locked != INVALID_HANDLE_VALUE && error.empty(),
        "matching AMD64 PE32+ DLL is verified without executing it");
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(writer == INVALID_HANDLE_VALUE && !DeleteFileW(path.c_str()), "verified image handle denies writes and deletion");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    locked = INVALID_HANDLE_VALUE;
    writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "closing the verified image handle restores ordinary file access");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    {
        std::vector<HANDLE> directories;
        Check(OpenVerifiedNativeImage(path.wstring(), hash, locked, error, &directories) && !directories.empty(),
            "module verification retains ancestor locks when requested");
        const auto renamed = fixture.root / "moved-binaries";
        Check(!MoveFileW((fixture.root / "binaries").c_str(), renamed.c_str()),
            "retained ancestor locks prevent directory rename before module loading");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        locked = INVALID_HANDLE_VALUE;
        for (const auto directory : directories) CloseHandle(directory);
    }
    Check(!OpenVerifiedNativeImage(path.wstring(), std::string(64, '0'), locked, error) && locked == INVALID_HANDLE_VALUE,
        "image verification fails for a mismatched manifest hash");
    Check(!OpenVerifiedNativeImage(L"relative.dll", hash, locked, error) &&
        !OpenVerifiedNativeImage(L"\\\\server\\share\\image.dll", hash, locked, error), "image verifier rejects relative and network paths");
    const auto native = fixture.Entry("native", Manifest("native", Native(hash)));
    Value declaration;
    pulse::json::Parse(R"({"apiVersion":1,"id":"plain","name":"plain"})", declaration);
    const auto plain = fixture.Entry("plain", declaration);
    fixture.Registry({plain, native});
    auto loaded = LoadPackageStore(fixture.root.wstring());
    Check(loaded && loaded.packages.size() == 2 && loaded.native_paths.size() == 2 && loaded.native_paths[0].empty() &&
        loaded.native_paths[1] == path.wstring(), "store paths align with mixed declarative and native packages");
    image.back() = 1; fixture.Write(path, image);
    fixture.RejectStore("changed native binary fails transactionally after an earlier valid package");
    std::filesystem::remove(path);
    fixture.RejectStore("missing native binary fails transactionally");
    const auto malformed = [&](std::string bytes, const char* name) {
        const auto digest = PackageRevision(bytes);
        const auto dll = fixture.root / "binaries" / (digest + ".dll");
        fixture.Write(dll, bytes);
        Check(!OpenVerifiedNativeImage(dll.wstring(), digest, locked, error) && locked == INVALID_HANDLE_VALUE, name);
        const auto entry = fixture.Entry("native", Manifest("native", Native(digest)));
        fixture.Registry({plain, entry}); fixture.RejectStore("store rejects hash-matching malformed image transactionally");
    };
    malformed("not a PE DLL", "image verifier rejects hash-matching non-PE data");
    auto bad_image = Image(); const WORD x86 = IMAGE_FILE_MACHINE_I386; Put(bad_image, 132, x86);
    malformed(bad_image, "image verifier rejects non-AMD64 machine");
    bad_image = Image(); const WORD pe32 = IMAGE_NT_OPTIONAL_HDR32_MAGIC; Put(bad_image, 152, pe32);
    malformed(bad_image, "image verifier rejects non-PE32+ optional header");
    bad_image = Image(); const WORD executable = IMAGE_FILE_EXECUTABLE_IMAGE; Put(bad_image, 150, executable);
    malformed(bad_image, "image verifier rejects non-DLL executable");
    malformed(Image().substr(0, 700), "image verifier rejects truncated section data");
    bad_image = Image(); const LONG invalid_offset = MAXLONG; Put(bad_image, offsetof(IMAGE_DOS_HEADER, e_lfanew), invalid_offset);
    malformed(bad_image, "image verifier rejects overflowing PE header offset");
    fixture.Write(path, Image());
    { std::fstream oversized(path, std::ios::binary | std::ios::in | std::ios::out); oversized.seekp(static_cast<std::streamoff>(kMaxNativeImageBytes)); oversized.put('\0'); }
    Check(!OpenVerifiedNativeImage(path.wstring(), hash, locked, error), "native image limit rejects files larger than 64 MiB");
    fixture.Write(path, Image());
    const auto link = fixture.root / "linked.dll";
    if (CreateSymbolicLinkW(link.c_str(), path.c_str(), 0x2)) {
        Check(!OpenVerifiedNativeImage(link.wstring(), hash, locked, error), "native image rejects a reparse file");
        std::filesystem::remove(link);
    } else std::printf("[SKIP] native reparse fixture requires symlink creation support\n");
    std::filesystem::remove_all(fixture.root / "binaries");
    auto disabled = native; disabled.set("enabled", false); fixture.Registry({plain, disabled});
    loaded = LoadPackageStore(fixture.root.wstring());
    Check(loaded && loaded.packages.size() == 1 && loaded.native_paths.size() == 1 && loaded.native_paths[0].empty(),
        "disabled native package never opens its missing binaries directory or DLL");
    fixture.Registry({native}); fixture.RejectStore("enabled native package rejects missing binaries directory");
    {
        const auto dll_bytes = Image(true);
        const auto exe_bytes = Image(false);
        Check(ValidateNativeImageBytes(dll_bytes) && !ValidateNativeImageBytes(exe_bytes),
            "DLL validator accepts AMD64 PE32+ DLL and rejects executable");
        Check(ValidateNativeExecutableBytes(exe_bytes) && !ValidateNativeExecutableBytes(dll_bytes),
            "executable sidecar validator accepts AMD64 PE32+ executable and rejects DLL");
        const auto dll_sha = PackageRevision(dll_bytes);
        const auto exe_sha = PackageRevision(exe_bytes);
        auto pkg_native = Native(dll_sha);
        auto sidecars = Value::Array();
        auto worker_file = Value::Object();
        worker_file.set("path", "pulse_7zip_worker.exe");
        worker_file.set("sha256", exe_sha);
        sidecars.push(worker_file);
        pkg_native.set("files", sidecars);
        const auto pkg_manifest = Manifest("worker.pkg", pkg_native);
        const auto pkg_entry = fixture.Entry("worker.pkg", pkg_manifest);
        const auto rev = pkg_entry.get_string("activeRevision");
        const auto pkg_dir = fixture.root / "packages" / rev;
        std::filesystem::create_directories(pkg_dir);
        fixture.Write(pkg_dir / "plugin.dll", dll_bytes);
        fixture.Write(pkg_dir / "pulse_7zip_worker.exe", exe_bytes);
        fixture.Registry({pkg_entry});
        const auto ok_store = LoadPackageStore(fixture.root.wstring());
        Check(ok_store && ok_store.packages.size() == 1, "multi-file native package accepts valid AMD64 PE32+ .exe worker sidecar");

        // DLL masquerading as .exe sidecar must fail closed even when SHA-256 matches
        auto bad_native = Native(dll_sha);
        auto bad_sidecars = Value::Array();
        auto bad_worker = Value::Object();
        bad_worker.set("path", "pulse_7zip_worker.exe");
        bad_worker.set("sha256", dll_sha);
        bad_sidecars.push(bad_worker);
        bad_native.set("files", bad_sidecars);
        const auto bad_entry = fixture.Entry("worker.pkg", Manifest("worker.pkg", bad_native));
        const auto bad_rev = bad_entry.get_string("activeRevision");
        const auto bad_dir = fixture.root / "packages" / bad_rev;
        std::filesystem::create_directories(bad_dir);
        fixture.Write(bad_dir / "plugin.dll", dll_bytes);
        fixture.Write(bad_dir / "pulse_7zip_worker.exe", dll_bytes);
        fixture.Registry({bad_entry});
        fixture.RejectStore("multi-file native package rejects DLL masquerading as .exe sidecar");
    }
}
void IconAndCanvasContractCases() {
    bool all_builtins = icons::kBuiltinIconNames.size() == (PULSE_ICON_BUILTIN_MAX + 1u);
    for (uint32_t i = 0; i <= PULSE_ICON_BUILTIN_MAX; ++i) {
        const auto resolved = icons::ResolveBuiltinIconName(icons::kBuiltinIconNames[i]);
        all_builtins = all_builtins && resolved.has_value() && *resolved == i;
    }
    Check(all_builtins &&
        icons::ResolveBuiltinIconName("compress") == static_cast<uint32_t>(PULSE_ICON_ARCHIVE) &&
        icons::ResolveBuiltinIconName("settings") == static_cast<uint32_t>(PULSE_ICON_GEAR) &&
        icons::ResolveBuiltinIconName("delete") == static_cast<uint32_t>(PULSE_ICON_TRASH) &&
        icons::ResolveBuiltinIconName("extension") == static_cast<uint32_t>(PULSE_ICON_PUZZLE),
        "all 68 built-in StoreIcon names and aliases resolve bijectively");

    Check(icons::ValidateSvgPathSyntax("M 2 12 L 12 2 H 20 V 22 C 20 22 14 18 12 18 S 4 22 2 12 A 4 4 0 0 1 6 8 Z") &&
        !icons::ValidateSvgPathSyntax("") &&
        !icons::ValidateSvgPathSyntax("12 14") &&
        !icons::ValidateSvgPathSyntax("L 4 4") &&
        !icons::ValidateSvgPathSyntax("M 2 2 Q 4 4 8 8"),
        "SVG path validator enforces bounded M/L/H/V/C/S/A/Z command grammar");

    auto& reg = icons::GlobalRegistry();
    auto icon_builder = pulse::sdk::IconBuilder("dyn-check")
        .ViewBox(24.0)
        .StrokePath("M 3 12 L 9 18 L 21 6", 1.8, false, PULSE_ICON_COLOR_ACCENT)
        .FillPath("M 18 4 L 22 4 L 22 8 Z", PULSE_ICON_COLOR_SUCCESS, 0u, 0.9);
    const auto& desc = icon_builder.Build();
    PulseIconId dyn_id = 0;
    Check(reg.RegisterDynamic("demo.pkg", &desc, &dyn_id) == PULSE_OK && icons::IsCustomIconId(dyn_id),
        "dynamic multi-layer vector icon registers with custom icon ID");
    const auto snap1 = reg.Snapshot(dyn_id);
    Check(snap1 && snap1->layers.size() == 2 && snap1->view_box_milli == 24000u,
        "registered custom icon snapshot preserves layers and viewBox");
    auto updated_builder = pulse::sdk::IconBuilder("dyn-check").StrokePath("M 4 4 L 20 20");
    const auto& updated_desc = updated_builder.Build();
    Check(reg.UpdateDynamic("demo.pkg", dyn_id, &updated_desc) == PULSE_OK &&
        reg.UpdateDynamic("other.pkg", dyn_id, &updated_desc) == PULSE_ERROR_STATE,
        "dynamic icon update enforces package ownership and bumps revision");
    const auto snap2 = reg.Snapshot(dyn_id);
    Check(snap2 && snap2->revision > snap1->revision && snap2->layers.size() == 1,
        "updated custom icon increments geometry cache revision");
    Check(icons::ResolveWideIconGlyph(L"icon:sparkle") == static_cast<uint32_t>(PULSE_ICON_SPARKLE) &&
        icons::ResolveWideIconGlyph(L"icon:#" + std::to_wstring(dyn_id)) == dyn_id &&
        icons::ResolveWideIconGlyph(L"icon:demo.pkg/dyn-check") == dyn_id,
        "icon glyph strings resolve built-in names, package/name, and numeric icon IDs");
    Check(reg.UnregisterDynamic("demo.pkg", dyn_id) == PULSE_OK && !reg.Snapshot(dyn_id).has_value(),
        "unregistering dynamic icon removes snapshot");

    const auto native = Native(std::string(64, 'c'));
    auto rich = Manifest("demo.icons", native);
    Value contributes;
    Check(pulse::json::Parse(R"({
        "icons": [
            {
                "id": "brand",
                "viewBox": 24,
                "layers": [
                    {"path": "M 4 12 L 12 4 L 20 12 L 12 20 Z", "stroke": true, "strokeWidth": 1.8, "color": "accent"},
                    {"path": "M 10 10 L 14 10 L 14 14 L 10 14 Z", "stroke": false, "fill": true, "closed": true, "color": "#22AA88", "opacity": 0.85}
                ]
            }
        ],
        "commands": [
            {
                "id": "scan-here",
                "title": {"zh": "扫描此目录", "en": "Scan Folder"},
                "detail": "{base}",
                "group": "Demo",
                "groupIcon": "sparkle",
                "submenu": true,
                "primary": true,
                "separatorBefore": true,
                "separatorAfter": true,
                "checked": true,
                "icon": "brand",
                "kinds": ["background"],
                "minSelection": 0,
                "maxSelection": 0
            }
        ]
    })", contributes), "parse rich contributes fixture JSON");
    rich.set("contributes", contributes);
    const auto parsed_rich = ParseManifest(pulse::json::Serialize(rich));
    Check(parsed_rich &&
        parsed_rich.manifest->icon_contributions.size() == 1 &&
        parsed_rich.manifest->command_contributions.size() == 1 &&
        parsed_rich.manifest->command_contributions[0].kinds == PULSE_COMMAND_BACKGROUND &&
        parsed_rich.manifest->command_contributions[0].icon_name == "brand" &&
        parsed_rich.manifest->command_contributions[0].group_icon == PULSE_ICON_SPARKLE &&
        (parsed_rich.manifest->command_contributions[0].flags &
            (PULSE_COMMAND_GROUP_SUBMENU | PULSE_COMMAND_PRIMARY | PULSE_COMMAND_SEPARATOR_BEFORE |
             PULSE_COMMAND_SEPARATOR_AFTER | PULSE_COMMAND_CHECKED)) ==
            (PULSE_COMMAND_GROUP_SUBMENU | PULSE_COMMAND_PRIMARY | PULSE_COMMAND_SEPARATOR_BEFORE |
             PULSE_COMMAND_SEPARATOR_AFTER | PULSE_COMMAND_CHECKED),
        "manifest contributes.icons and extended context-menu command flags parse cleanly");
    SyncManifestIcons(*parsed_rich.manifest);
    const auto brand_id = icons::GlobalRegistry().ResolveName("demo.icons", "brand");
    Check(brand_id.has_value() && icons::IsCustomIconId(*brand_id),
        "SyncManifestIcons registers manifest vector icons under package namespace");
    icons::GlobalRegistry().ClearManifestIconsForMissingPackages({});

    auto canvas_node = pulse::sdk::CanvasBuilder("plot")
        .ViewBox(200, 80)
        .Clear("surface")
        .Rect(4, 4, 192, 72, 8, "accent", "none", 1.5)
        .Path("none", "accent", 2.0, "M 12 60 L 60 28 L 120 44 L 188 16")
        .Icon("sparkle", 164, 10, 20, "warning")
        .Text(14, 12, 80, 20, "text", 12, "left", "98%")
        .MinHeight(80)
        .State("series")
        .BuildNode();
    auto btn_node = pulse::sdk::ModelNode(PULSE_UI_MODEL_BUTTON, "act")
        .String(&PulseUiModelNode::text, "Sparkle")
        .String(&PulseUiModelNode::command, "run")
        .String(&PulseUiModelNode::glyph, pulse::sdk::IconGlyph("sparkle"));
    auto tree = pulse::sdk::ModelTree(
        pulse::sdk::ModelNode(PULSE_UI_MODEL_STACK, "root").Children({std::move(canvas_node), std::move(btn_node)}))
        .State("series", "circle 60 28 4 accent accent 1")
        .Command("run");
    UiTree converted;
    std::set<std::string> declared_cmds;
    std::string ui_error;
    Check(native::ConvertUiModel(&tree.Build(), converted, declared_cmds, ui_error) == PULSE_OK &&
        converted.root.children.size() == 2 &&
        converted.root.children[0].type == UiNodeType::Canvas &&
        converted.root.children[0].min_height == 80 &&
        converted.root.children[0].options.size() == 6 &&
        converted.root.children[1].glyph == "icon:sparkle",
        "PULSE_UI_MODEL_CANVAS converts CanvasBuilder draw commands and icon glyph cleanly");
}
}
int main() {
    try { ManifestCases(); ImageAndStoreCases(); IconAndCanvasContractCases(); }
    catch (const std::exception& exception) { std::printf("[FAIL] native fixture: %s\n", exception.what()); return 1; }
    std::printf("native_contract_test: %d failed\n", failures);
    return failures ? 1 : 0;
}
