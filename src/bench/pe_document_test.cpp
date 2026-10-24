#include "../preview_host/pe_document.h"
#include "../preview_host/pe_dependencies.h"
#include "../common/preview_extensions.h"
#include "preview_host_client.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
void Put16(Bytes& b, size_t p, uint16_t v) {
    b[p] = static_cast<uint8_t>(v); b[p + 1] = static_cast<uint8_t>(v >> 8);
}
void Put32(Bytes& b, size_t p, uint32_t v) {
    Put16(b, p, static_cast<uint16_t>(v)); Put16(b, p + 2, static_cast<uint16_t>(v >> 16));
}
void String(Bytes& b, size_t p, const char* s) { std::memcpy(b.data() + p, s, std::strlen(s) + 1); }
size_t Wide(Bytes& b, size_t p, const wchar_t* s) {
    do { Put16(b, p, static_cast<uint16_t>(*s)); p += 2; } while (*s++);
    return (p + 3) & ~size_t{3};
}
Bytes Fixture(bool wide) {
    Bytes b(0x2200);
    Put16(b, 0, 0x5a4d); Put32(b, 60, 0x80); Put32(b, 0x80, 0x4550);
    Put16(b, 0x84, wide ? 0x8664 : 0x14c); Put16(b, 0x86, 1);
    Put16(b, 0x94, wide ? 240 : 224); Put16(b, 0x96, 0x2002);
    const size_t o = 0x98, d = o + (wide ? 112 : 96), s = o + (wide ? 240 : 224);
    Put16(b, o, wide ? 0x20b : 0x10b); Put32(b, o + 16, 0x1000);
    Put32(b, o + (wide ? 24 : 28), 0x400000); Put32(b, o + 56, 0x4000);
    Put32(b, o + 60, 0x200); Put16(b, o + 68, 3); Put32(b, d - 4, 16);
    String(b, s, ".rdata"); Put32(b, s + 8, 0x2000); Put32(b, s + 12, 0x1000);
    Put32(b, s + 16, 0x2000); Put32(b, s + 20, 0x200);
    Put32(b, d + 8, 0x1100); Put32(b, d + 12, 40);
    Put32(b, 0x300, 0x1180); Put32(b, 0x30c, 0x1160);
    String(b, 0x360, "KERNEL32.dll"); Put32(b, 0x380, 0x11c0);
    Put32(b, 0x380 + (wide ? 12 : 4), 0x80000000u);
    Put16(b, 0x380 + (wide ? 8 : 4), 7); String(b, 0x3c2, "CreateFileW");
    Put32(b, d, 0x1300); Put32(b, d + 4, 0x100);
    Put32(b, 0x50c, 0x1380); Put32(b, 0x510, 10); Put32(b, 0x514, 2);
    Put32(b, 0x518, 1); Put32(b, 0x51c, 0x1340); Put32(b, 0x520, 0x1350); Put32(b, 0x524, 0x1360);
    Put32(b, 0x540, 0x1390); Put32(b, 0x544, 0x1000); Put32(b, 0x550, 0x1370);
    String(b, 0x570, "Forwarded"); String(b, 0x580, "fixture.dll"); String(b, 0x590, "NTDLL.RtlExample");
    Put32(b, d + 16, 0x1500); Put32(b, d + 20, 0x200);
    for (size_t p : {size_t{0x700}, size_t{0x718}, size_t{0x730}}) Put16(b, p + 14, 1);
    Put32(b, 0x710, 16); Put32(b, 0x714, 0x80000018u);
    Put32(b, 0x728, 1); Put32(b, 0x72c, 0x80000030u);
    Put32(b, 0x740, 1033); Put32(b, 0x744, 0x48);
    Put32(b, 0x748, 0x1600);
    size_t cursor = Wide(b, 0x806, L"VS_VERSION_INFO");
    Put16(b, 0x802, 52); Put32(b, cursor, 0xfeef04bd);
    Put32(b, cursor + 8, 0x00010002); Put32(b, cursor + 12, 0x00030004);
    Put32(b, cursor + 16, 0x00050006); Put32(b, cursor + 20, 0x00070008);
    cursor += 52;
    const size_t child = cursor;
    cursor = Wide(b, cursor + 6, L"CompanyName");
    Put16(b, child + 2, 8); Put16(b, child + 4, 1);
    cursor = Wide(b, cursor, L"Example"); Put16(b, child, static_cast<uint16_t>(cursor - child));
    Put16(b, 0x800, static_cast<uint16_t>(cursor - 0x800)); Put32(b, 0x74c, static_cast<uint32_t>(cursor - 0x800));
    Put32(b, d + 13 * 8, 0x1800); Put32(b, d + 13 * 8 + 4, 64);
    Put32(b, 0xa00, 1); Put32(b, 0xa04, 0x1860); Put32(b, 0xa10, 0x1880);
    String(b, 0xa60, "DELAY.dll"); Put32(b, 0xa80, 0x18c0); String(b, 0xac2, "DelayedFunction");
    return b;
}
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
std::wstring Dependencies(const std::wstring& text) {
    const auto start = text.find(L"\nDLL Dependencies\n");
    if (start == std::wstring::npos) return {};
    const auto end = text.find(L"\nVersion information", start);
    return text.substr(start, end == std::wstring::npos ? end : end - start);
}
Bytes ImportFixture(const std::vector<std::string>& names) {
    auto bytes = Fixture(true);
    const size_t directories = 0x98 + 112;
    std::fill_n(bytes.begin() + directories, 16 * 8, uint8_t{0});
    std::fill(bytes.begin() + 0x300, bytes.end(), uint8_t{0});
    Put32(bytes, directories + 8, 0x1100);
    Put32(bytes, directories + 12, static_cast<uint32_t>((names.size() + 1) * 20));
    for (size_t i = 0; i < names.size(); ++i) {
        Put32(bytes, 0x300 + i * 20 + 12, static_cast<uint32_t>(0x1700 + i * 128));
        String(bytes, 0x900 + i * 128, names[i].c_str());
    }
    return bytes;
}
struct FixtureDirectory {
    std::wstring path;
    std::vector<std::wstring> files;
    FixtureDirectory() {
        wchar_t temp[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temp)) return;
        path = std::wstring(temp) + L"pulse-pe-dependencies-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64());
        if (!CreateDirectoryW(path.c_str(), nullptr)) path.clear();
    }
    bool Write(const std::wstring& name, const Bytes& bytes) {
        if (path.empty()) return false;
        const auto file_path = path + L"\\" + name;
        HANDLE file = CreateFileW(file_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size();
        CloseHandle(file);
        if (std::find(files.begin(), files.end(), file_path) == files.end()) files.push_back(file_path);
        return ok;
    }
    ~FixtureDirectory() {
        for (const auto& file : files) DeleteFileW(file.c_str());
        if (!path.empty()) RemoveDirectoryW(path.c_str());
    }
};
void DependencyTests(pulse_test::Host* host = nullptr) {
    FixtureDirectory directory;
    const std::wstring parent = L"pulse_pe_parent_fixture.dll";
    const auto root = ImportFixture({"pulse_pe_parent_fixture.dll", "pulse_pe_direct_missing_fixture.dll",
                                     "api-ms-pulse-fixture-l1-1-0.dll"});
    if (!directory.Write(L"root.exe", root) ||
        !directory.Write(parent, ImportFixture({"pulse_pe_transitive_missing_fixture.dll"}))) {
        Check(false, "dependency fixture files created"); return;
    }
    const auto read = [&](const Bytes& bytes, std::wstring& text) {
        const auto path = directory.path + L"\\root.exe";
        if (host) {
            if (!directory.Write(L"root.exe", bytes)) return false;
            pulse_test::Result result;
            if (!host->Request(path, result) || result.response.kind != pulse::ipc::PreviewContentKind::Text) return false;
            text = result.text;
            return true;
        }
        bool truncated = false;
        return pulse::preview::MakePeDocument(path, bytes, text, truncated);
    };
    std::wstring text;
    Check(read(root, text), host ? "dependency fixture through host" : "path-aware dependency report");
    Check(text.find(L"[MISSING] pulse_pe_direct_missing_fixture.dll") != std::wstring::npos,
          "missing direct DLL has missing state");
    Check(text.find(L"[WARNING] pulse_pe_parent_fixture.dll") != std::wstring::npos &&
          text.find(directory.path + L"\\" + parent) != std::wstring::npos &&
          text.find(L"    [MISSING] pulse_pe_transitive_missing_fixture.dll") != std::wstring::npos,
          "resolved parent path warns about missing descendant");
    Check(text.find(L"[API SET] api-ms-pulse-fixture-l1-1-0.dll") != std::wstring::npos &&
          text.find(L"[MISSING] api-ms-") == std::wstring::npos, "API-set contract stays neutral");
    Check(text.size() <= 32768 && text.find(L"\nImports\n") != std::wstring::npos &&
          text.find(L"\nExports\n") != std::wstring::npos, "tree preserves bounded import and export sections");
    Check(directory.Write(parent, ImportFixture({"root.exe"})), "cyclic dependency fixture written");
    const auto cycle = ImportFixture({"pulse_pe_parent_fixture.dll"});
    Check(read(cycle, text) && text.find(L"[CYCLE] root.exe") != std::wstring::npos &&
          text.find(L"[UNRESOLVED] pulse_pe_parent_fixture.dll") != std::wstring::npos,
          "dependency cycle and incomplete parent stay neutral");
    auto incompatible = ImportFixture({});
    Put16(incompatible, 0x84, 0x14c);
    Check(directory.Write(parent, incompatible), "incompatible dependency fixture written");
    Check(read(cycle, text) && text.find(L"architecture mismatch") != std::wstring::npos &&
          text.find(L"[MISSING]") == std::wstring::npos, "wrong architecture is unresolved rather than missing");
    Check(directory.Write(parent, ImportFixture({})), "resolved leaf fixture written");
    Check(read(cycle, text) && text.find(L"[OK] pulse_pe_parent_fixture.dll") != std::wstring::npos,
          "resolved leaf shows full path and success state");
    bool written = true;
    for (int i = 0; i < 10; ++i) {
        const auto next = "pulse_pe_depth_" + std::to_string(i + 1) + ".dll";
        written = directory.Write(L"pulse_pe_depth_" + std::to_wstring(i) + L".dll",
                                  ImportFixture({next})) && written;
    }
    Check(written && read(ImportFixture({"pulse_pe_depth_0.dll"}), text) &&
          text.find(L"[LIMIT]") != std::wstring::npos && text.size() <= 32768 &&
          text.find(L"[MISSING]") == std::wstring::npos, "recursive depth budget stays bounded and neutral");
    auto oversized = ImportFixture({});
    oversized.resize(9u * 1024u * 1024u);
    Check(directory.Write(parent, oversized) && read(cycle, text) &&
          text.find(L"file/analysis byte or time limit") != std::wstring::npos &&
          text.find(L"[MISSING]") == std::wstring::npos, "oversized dependency is bounded and neutral");
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--host") == 0) {
        pulse_test::Host host;
        if (!host.Start()) { Check(false, "preview host starts"); return 1; }
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        pulse_test::Result result;
        Check(host.Request(path, result) && result.response.kind == pulse::ipc::PreviewContentKind::Text &&
              result.text.find(L"Portable Executable") != std::wstring::npos &&
              result.text.find(L"Architecture: x64") != std::wstring::npos &&
              result.text.find(L"Imports") != std::wstring::npos, "real EXE through preview host");
        Check(Dependencies(result.text).find(L" — ") != std::wstring::npos &&
              result.text.find(L"\nDLL Dependencies\n") < result.text.find(L"\nImports\n"),
              "real EXE dependency summary precedes import functions");
        GetSystemDirectoryW(path, MAX_PATH);
        const std::wstring dll = std::wstring(path) + L"\\version.dll";
        result = {};
        Check(host.Request(dll, result) && result.response.kind == pulse::ipc::PreviewContentKind::Text &&
              result.text.find(L"CompanyName: Microsoft") != std::wstring::npos &&
              result.text.find(L"Product version:") != std::wstring::npos &&
              result.text.find(L"GetFileVersionInfo") != std::wstring::npos,
              "real DLL version, company and exports through preview host");
        Check(Dependencies(result.text).find(L" — ") != std::wstring::npos &&
              result.text.find(L"\nDLL Dependencies\n") < result.text.find(L"\nImports\n"),
              "real DLL has separate dependency summary");
        DependencyTests(&host);
        return failures ? 1 : 0;
    }
    Check(pulse::preview::IsNativeExtension(L".exe") && pulse::preview::IsNativeExtension(L".dll") &&
          !pulse::preview::IsTextExtension(L".exe"), "PE formats route to native preview");
    for (const bool wide : {false, true}) {
        const auto b = Fixture(wide);
        std::wstring text; bool truncated = false;
        Check(pulse::preview::MakePeDocument(b, text, truncated) && !truncated,
              wide ? "PE32+ complete report" : "PE32 complete report");
        Check(text.find(wide ? L"x64" : L"x86") != std::wstring::npos && text.find(L".rdata") != std::wstring::npos,
              "architecture and sections");
        Check(text.find(L"CreateFileW") != std::wstring::npos && text.find(L"ordinal 7") != std::wstring::npos,
              "named and ordinal imports");
        Check(text.find(L"Forwarded -> NTDLL.RtlExample") != std::wstring::npos &&
              text.find(L"11  (ordinal only)") != std::wstring::npos, "forwarded and ordinal exports");
        Check(text.find(L"File version: 1.2.3.4") != std::wstring::npos &&
              text.find(L"Product version: 5.6.7.8") != std::wstring::npos &&
              text.find(L"CompanyName: Example") != std::wstring::npos, "version resource strings");
        Check(text.find(L"DELAY.dll") != std::wstring::npos && text.find(L"DelayedFunction") != std::wstring::npos,
              "delay imports");
        const auto dependencies = Dependencies(text);
        Check(dependencies.find(L"KERNEL32.dll (imported)") != std::wstring::npos &&
              dependencies.find(L"DELAY.dll (delay-loaded)") != std::wstring::npos &&
              dependencies.find(L"CreateFileW") == std::wstring::npos &&
              dependencies.find(L"DelayedFunction") == std::wstring::npos &&
              text.find(L"\nDLL Dependencies\n") < text.find(L"\nImports\n"),
              "direct and delay-loaded DLL summary is separate from functions");
        auto duplicate = b;
        Put32(duplicate, 0x98 + (wide ? 112 : 96) + 12, 60);
        std::copy_n(b.begin() + 0x300, 20, duplicate.begin() + 0x314);
        String(duplicate, 0xa60, "kernel32.DLL");
        pulse::preview::MakePeDocument(duplicate, text, truncated);
        const auto deduplicated = Dependencies(text);
        const auto module = deduplicated.find(L"KERNEL32.dll (imported, delay-loaded)");
        Check(!truncated && module != std::wstring::npos &&
              deduplicated.find(L"KERNEL32.dll", module + 1) == std::wstring::npos &&
              deduplicated.find(L"kernel32.DLL") == std::wstring::npos &&
              text.find(L"DelayedFunction") != std::wstring::npos,
              "DLL dependencies deduplicate repeated and differently cased module names");
        bool bounded = true;
        for (size_t size = 0; size < b.size(); ++size) {
            pulse::preview::MakePeDocument(std::span(b).first(size), text, truncated);
            bounded = bounded && text.size() <= 32768;
        }
        Check(bounded, "every truncated file prefix remains bounded");
        for (size_t p = 0; p + 4 <= b.size(); p += 4) {
            auto corrupt = b;
            Put32(corrupt, p, 0xffffffffu);
            pulse::preview::MakePeDocument(corrupt, text, truncated);
            bounded = bounded && text.size() <= 32768;
        }
        Check(bounded, "corrupt offsets and counts remain bounded");
    }
    std::wstring text; bool truncated = false;
    Check(!pulse::preview::MakePeDocument(Bytes(64), text, truncated), "non-PE falls back");
    auto b = Fixture(false);
    Put32(b, 0x300, 0xfffffff0u);
    Check(pulse::preview::MakePeDocument(b, text, truncated) && truncated &&
          text.find(L"metadata is invalid") != std::wstring::npos, "invalid RVA reports partial metadata");
    b = Fixture(false); Put32(b, 0x514, 0xffffffffu);
    Check(pulse::preview::MakePeDocument(b, text, truncated) && truncated && text.size() <= 32768,
          "huge export count is capped");
    b = Fixture(false); Put32(b, 0x72c, 0x80000018u);
    Check(pulse::preview::MakePeDocument(b, text, truncated) && truncated && text.size() <= 32768,
          "cyclic resource tree is capped");
    b = Fixture(false); Put32(b, 60, 0xfffffff0u);
    Check(!pulse::preview::MakePeDocument(b, text, truncated), "overflowing NT header offset falls back");
    b = Fixture(false); Put16(b, 0x94, 2);
    Check(!pulse::preview::MakePeDocument(b, text, truncated), "short optional header falls back");
    DependencyTests();
    return failures ? 1 : 0;
}
