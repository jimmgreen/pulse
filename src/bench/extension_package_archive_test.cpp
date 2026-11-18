#include "extension_package_fixture.h"

namespace {
using namespace package_fixture;
int passed = 0, failed = 0;
void Check(const char* label, bool ok, const std::string& error = {}) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", label, ok || error.empty() ? "" : ": ", ok ? "" : error.c_str());
    ok ? ++passed : ++failed;
}
std::string Read(const std::string& path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
} // namespace
int main(int argc, char** argv) {
    try {
        Key key, other; std::atomic<bool> cancel = false; std::string error; VerifiedPackage out;
        auto verify = [&](Fixture& f, std::string_view bytes) { return VerifyCatalogPackage(bytes, f.signature, f.extension, f.release, f.publisher, cancel, out, error); };
        for (int count : {0, 1, 3, 64}) {
            Fixture f(key, count); auto z = MakeZip(f.files); f.Assets(z.bytes);
            const bool ok = verify(f, z.bytes);
            Check(("valid native/v1 file count " + std::to_string(count)).c_str(), ok && out && out.Files().size() == static_cast<size_t>(count) &&
                out.ManifestBytes() == f.manifest && out.SignatureBytes() == f.signature && out.Revision() == f.release.manifest_sha256 && out.PublisherId() == key.publisher.id && out.Declaration().name == "安全包 測試", error);
        }
        {
            Fixture f(key, 3, Pe() + std::string(1024 * 1024, '\0'));
            for (auto& file : f.files) file.method = 8;
            auto z = MakeZip(f.files); f.Assets(z.bytes);
            Check("DEFLATE multi-stream-loop payload", verify(f, z.bytes) && out.Files()[0].bytes.size() > 1024 * 1024, error);
        }
        const auto sentinel = out.Revision();
        auto reject = [&](const char* label, Fixture f, Zip z, const std::function<void(Fixture&, Zip&)>& mutate, bool assets = true) {
            mutate(f, z); if (assets) f.Assets(z.bytes);
            Check(label, !verify(f, z.bytes) && !error.empty() && out.Revision() == sentinel, error);
        };
        Fixture base(key, 3); auto zip = MakeZip(base.files); base.Assets(zip.bytes);
        reject("wrong asset hash", base, zip, [](Fixture& f, Zip&) { f.release.package.sha256[0] ^= 1; }, false);
        reject("wrong asset size", base, zip, [](Fixture& f, Zip&) { ++f.release.package.size; }, false);
        reject("wrong signature asset hash", base, zip, [](Fixture& f, Zip&) { f.release.signature.sha256[0] ^= 1; }, false);
        reject("tampered signature", base, zip, [](Fixture& f, Zip&) { f.signature[f.signature.size() - 5] ^= 1; });
        reject("wrong signer", base, zip, [&](Fixture& f, Zip&) { f.signature = other.Sign(f.manifest); });
        reject("publisher association", base, zip, [](Fixture& f, Zip&) { f.extension.publisher = "wrong"; });
        reject("publisher fingerprint", base, zip, [](Fixture& f, Zip&) { f.publisher.id[0] ^= 1; f.extension.publisher = f.publisher.id; });
        reject("publisher key", base, zip, [&](Fixture& f, Zip&) { f.publisher.public_key = other.publisher.public_key; });
        reject("manifest hash", base, zip, [](Fixture& f, Zip&) { f.release.manifest_sha256[0] ^= 1; });
        reject("manifest id", base, zip, [](Fixture& f, Zip&) { f.extension.id = "wrong.id"; });
        reject("resources", base, zip, [](Fixture& f, Zip&) { f.release.resources.clear(); });
        auto files_case = [&](const char* name, const std::function<void(std::vector<ZipFile>&)>& mutate) {
            auto files = base.files; mutate(files); reject(name, base, MakeZip(files), [](Fixture&, Zip&) {});
        };
        auto required_metadata_case = [&](const char* label, const std::function<void(std::vector<ZipFile>&)>& mutate) {
            auto fixture = base;
            mutate(fixture.files);
            const auto bytes = MakeZip(fixture.files).bytes;
            fixture.Assets(bytes); // Ensure the member-name validation, not asset hashes, rejects it.
            bool rejected_without_exception = false;
            try { rejected_without_exception = !verify(fixture, bytes); }
            catch (const std::exception& ex) { error = std::string("exception escaped bool API: ") + ex.what(); }
            Check(label, rejected_without_exception && !error.empty() && out.Revision() == sentinel, error);
        };
        required_metadata_case("Manifest.json rejects without exception and preserves out", [](auto& f) { f[0].name = "Manifest.json"; });
        required_metadata_case("Package.json rejects without exception and preserves out", [](auto& f) { f[1].name = "Package.json"; });
        required_metadata_case("both case-variant metadata names reject without exception", [](auto& f) { f[0].name = "Manifest.json"; f[1].name = "Package.json"; });
        required_metadata_case("missing manifest with exact package.json rejects", [](auto& f) { f.erase(f.begin()); });
        required_metadata_case("missing package with exact manifest.json rejects", [](auto& f) { f.erase(f.begin() + 1); });
        required_metadata_case("both required metadata members missing reject", [](auto& f) { f.erase(f.begin(), f.begin() + 2); });
        files_case("missing member", [](auto& f) { f.pop_back(); });
        files_case("extra member", [](auto& f) { f.push_back({"extra.txt", ""}); });
        files_case("duplicate", [](auto& f) { f.push_back(f.back()); });
        files_case("case collision", [](auto& f) { f.push_back({"TEST.dll", ""}); });
        for (const auto* name : {"../evil", "foo/bar", "foo\\bar", "C:evil", "file:ads", "CON.txt", "LPT1.dll", "bad.", ".", "..", "a "}) {
            files_case(("unsafe member " + std::string(name)).c_str(), [&](auto& f) { f.back().name = name; });
        }
        files_case("ZIP symlink (no filesystem privilege needed)", [](auto& f) { f.back().attr = 0120777u << 16; });
        files_case("ZIP Unix hardlink extra", [](auto& f) { f.back().extra = std::string(16, '\0') + "test.dll"; W16(f.back().extra, 0, 0x000d); W16(f.back().extra, 2, 20); });
        files_case("ZIP ASi Unix link extra", [](auto& f) { f.back().extra = std::string(18, '\0'); W16(f.back().extra, 0, 0x756e); W16(f.back().extra, 2, 14); });
        files_case("ZIP64 extra", [](auto& f) { f.back().extra = std::string(12, '\0'); W16(f.back().extra, 0, 1); W16(f.back().extra, 2, 8); });
        files_case("NUL in member name", [](auto& f) { f.back().name = std::string("a\0b", 3); });
        files_case("ZIP directory", [](auto& f) { f.back().attr = 0040755u << 16; });
        files_case("DOS directory attribute", [](auto& f) { f.back().attr |= 0x10; });
        files_case("encrypted", [](auto& f) { f.back().flags = 1; });
        files_case("unknown compression", [](auto& f) { f.back().method = 99; });
        files_case("corrupt metadata", [](auto& f) { f[1].bytes += "garbage"; });
        files_case("unknown metadata field", [](auto& f) { f[1].bytes.insert(1, "\"extra\":1,"); });
        files_case("duplicate metadata field", [](auto& f) { f[1].bytes.insert(1, "\"apiVersion\":1,"); });
        files_case("noninteger metadata", [](auto& f) { const auto p = f[1].bytes.find("\"apiVersion\":1"); f[1].bytes.insert(p + 14, ".0"); });
        files_case("native content hash", [](auto& f) { f.back().bytes += "bad"; });
        reject("invalid PE with correct signed hash", Fixture(key, 1, "not a DLL"), MakeZip(Fixture(key, 1, "not a DLL").files), [](Fixture&, Zip&) {});
        for (const auto offset : {size_t{68}, size_t{87}, size_t{88}}) {
            auto pe = Pe(); pe[offset] = 0; Fixture f(key, 1, pe);
            reject("PE x64/magic/DLL characteristic", f, MakeZip(f.files), [](Fixture&, Zip&) {});
        }
        reject("trailing data", base, zip, [](Fixture&, Zip& z) { z.bytes += "x"; });
        reject("multi disk", base, zip, [](Fixture&, Zip& z) { W16(z.bytes, z.end + 4, 1); });
        reject("ZIP64 directory sentinel", base, zip, [](Fixture&, Zip& z) { W32(z.bytes, z.end + 16, UINT32_MAX); });
        reject("central directory limit", base, zip, [](Fixture&, Zip& z) { W32(z.bytes, z.end + 12, 16385); });
        reject("member count limit", base, zip, [](Fixture&, Zip& z) { W16(z.bytes, z.end + 10, 67); });
        reject("manifest declared size limit", base, zip, [](Fixture&, Zip& z) { W32(z.bytes, z.central[0] + 24, 65537); });
        reject("metadata declared size limit", base, zip, [](Fixture&, Zip& z) { W32(z.bytes, z.central[1] + 24, 16385); });
        reject("file declared size limit / zipbomb", base, zip, [](Fixture&, Zip& z) { W32(z.bytes, z.central[2] + 24, 64u * 1024 * 1024 + 1); });
        Fixture total(key, 5); auto total_zip = MakeZip(total.files);
        reject("aggregate declared size limit", total, total_zip, [](Fixture&, Zip& z) { for (size_t i = 2; i < z.central.size(); ++i) W32(z.bytes, z.central[i] + 24, 64u * 1024 * 1024); });
        reject("local central mismatch", base, zip, [](Fixture&, Zip& z) { W16(z.bytes, z.local[0] + 8, 8); });
        reject("CRC corruption", base, zip, [](Fixture&, Zip& z) { z.bytes[z.local[2] + 30 + 8] ^= 1; });
        {
            Fixture f(key, 1, Pe() + std::string(1024 * 1024, '\0'));
            for (auto& file : f.files) file.method = 8;
            auto z = MakeZip(f.files);
            reject("DEFLATE expansion exceeds declared size", f, z, [](Fixture&, Zip& bomb) {
                W32(bomb.bytes, bomb.central[2] + 24, 16); W32(bomb.bytes, bomb.local[2] + 22, 16);
            });
            reject("damaged DEFLATE data", f, z, [](Fixture&, Zip& bad) { bad.bytes[bad.local[2] + 30 + 8] = 7; });
        }
        reject("truncated", base, zip, [](Fixture&, Zip& z) { z.bytes.resize(z.bytes.size() - 10); });
        cancel = true; Check("cancel preserves result", !verify(base, zip.bytes) && error == "cancelled" && out.Revision() == sentinel); cancel = false;
        if (argc == 5) {
            const auto bytes = Read(argv[1]), sig = Read(argv[2]); Value ext, pub;
            Require(pulse::json::Parse(Read(argv[3]), ext) && pulse::json::Parse(Read(argv[4]), pub));
            const auto& r = ext.find("versions")->at(0); Extension e; Release rel; Publisher p;
            e.id = ext.get_string("id"); e.publisher = ext.get_string("publisher"); p.id = pub.get_string("id"); p.public_key = pub.get_string("publicKey");
            rel.manifest_sha256 = r.get_string("manifestSha256");
            for (const auto& v : r.find("resources")->items()) rel.resources.push_back(v.as_string());
            for (auto pair : {std::pair{"package", &rel.package}, std::pair{"signature", &rel.signature}}) {
                const auto* a = r.find(pair.first); int64_t n = 0; Require(a && a->find("size")->get_int(n)); pair.second->size = static_cast<uint64_t>(n); pair.second->sha256 = a->get_string("sha256");
            }
            Check("read-only real 7zip deflated package/catalog metadata compatibility", VerifyCatalogPackage(bytes, sig, e, rel, p, cancel, out, error) && out.Files().size() == 4 && out.Declaration().id == "pulse.7zip", error);
        }
        std::printf("%d passed, %d failed\n", passed, failed); return failed ? 1 : 0;
    } catch (const std::exception& ex) { std::printf("[FAIL] %s\n", ex.what()); return 1; }
}
