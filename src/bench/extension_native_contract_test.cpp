#include "../app/extensions/manifest.h"
#include "../app/extensions/native_image_verify.h"
#include "../app/extensions/package_store.h"
#include "../common/json_value.h"
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
std::string Image() {
    std::string bytes(1024, '\0');
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 128;
    Put(bytes, 0, dos);
    const DWORD signature = IMAGE_NT_SIGNATURE; Put(bytes, 128, signature);
    IMAGE_FILE_HEADER file{};
    file.Machine = IMAGE_FILE_MACHINE_AMD64; file.NumberOfSections = 1;
    file.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    file.Characteristics = IMAGE_FILE_DLL | IMAGE_FILE_EXECUTABLE_IMAGE;
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
}
}
int main() {
    try { ManifestCases(); ImageAndStoreCases(); }
    catch (const std::exception& exception) { std::printf("[FAIL] native fixture: %s\n", exception.what()); return 1; }
    std::printf("native_contract_test: %d failed\n", failures);
    return failures ? 1 : 0;
}
