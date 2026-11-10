#include "extension_package_fixture.h"
#include "../app/extensions/store/package_download.h"

namespace {
using namespace package_fixture;
namespace app = pulse::app;
int passed = 0, failed = 0;
void Check(const std::string& label, bool ok, const std::string& error = {}) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", label.c_str(), ok || error.empty() ? "" : ": ", ok ? "" : error.c_str());
    ok ? ++passed : ++failed;
}
struct Data {
    Fixture fixture;
    std::string archive;
    Catalog catalog;
    Data(const Key& key, int count = 0, const std::string& id = "test.archive") : fixture(key, count, Pe(), id) {
        archive = MakeZip(fixture.files).bytes;
        fixture.Assets(archive);
        fixture.release.version = "1.2.3";
        fixture.release.package.url = "https://github.com/test/packages/releases/download/v1/test.pulse-ext";
        fixture.release.signature.url = "https://github.com/test/packages/releases/download/v1/test.sig.json";
        Sync();
    }
    void Sync() {
        fixture.extension.versions = {fixture.release};
        catalog.publishers = {fixture.publisher};
        catalog.extensions = {fixture.extension};
    }
};
enum class Mode { Good, Failure, HalfFailure, Oversize, NullChunk, CancelChunk, CancelAfter,
    CancelThenClear, CancelCode, ChangeSource };
struct Step { std::wstring url; std::string bytes; Mode mode = Mode::Good; };
struct Reader {
    std::vector<Step> steps;
    size_t calls = 0;
    bool expected = true;
    std::atomic<bool>& cancel;
    explicit Reader(std::atomic<bool>& flag) : cancel(flag) {}
    app::UpdateResponseReader Function() {
        return [this](std::wstring_view url, uint64_t maximum, const std::atomic<bool>&,
            const std::function<bool(const void*, DWORD)>& consume, app::UpdateError& category, DWORD& code) {
            if (calls >= steps.size()) { ++calls; expected = false; return false; }
            const auto step = steps[calls++];
            expected = expected && url == step.url && maximum != 0;
            category = app::UpdateError::None; code = ERROR_SUCCESS;
            if (step.mode == Mode::Failure) { category = app::UpdateError::Network; code = ERROR_TIMEOUT; return false; }
            if (step.mode == Mode::HalfFailure) {
                (void)consume(step.bytes.data(), static_cast<DWORD>(step.bytes.size() / 2));
                category = app::UpdateError::Network; code = ERROR_CONNECTION_ABORTED; return false;
            }
            if (step.mode == Mode::Oversize || step.mode == Mode::NullChunk) {
                const DWORD n = step.mode == Mode::Oversize ? static_cast<DWORD>(maximum + 1) : 1;
                expected = !consume(step.mode == Mode::Oversize ? step.bytes.data() : nullptr, n) && expected;
                // Deliberately ignore false, then send a valid full asset and claim success.
                expected = !consume(step.bytes.data(), static_cast<DWORD>(step.bytes.size())) && expected;
                return true;
            }
            if (step.mode == Mode::CancelCode) { code = ERROR_CANCELLED; return true; }
            const auto half = step.bytes.size() / 2;
            if (!consume(step.bytes.data(), static_cast<DWORD>(half))) return false;
            if (step.mode == Mode::ChangeSource) app::SetUpdateSource(app::UpdateSource::GitHub);
            if (step.mode == Mode::CancelChunk || step.mode == Mode::CancelThenClear) {
                cancel.store(true);
                expected = !consume(step.bytes.data() + half, static_cast<DWORD>(step.bytes.size() - half)) && expected;
                if (step.mode == Mode::CancelThenClear) cancel.store(false);
                return true; // Malicious reader also ignores cancellation callback rejection.
            }
            if (!consume(step.bytes.data() + half, static_cast<DWORD>(step.bytes.size() - half))) return false;
            if (step.mode == Mode::CancelAfter) cancel.store(true);
            return true;
        };
    }
};
std::vector<std::wstring> Sources(const Asset& asset) {
    return app::UpdateSourceUrls(std::wstring(asset.url.begin(), asset.url.end()), app::UpdateSource::Automatic);
}
struct SourceRestore {
    app::UpdateSource original = app::CurrentUpdateSource();
    ~SourceRestore() { app::SetUpdateSource(original); }
};
} // namespace

int main() {
    SourceRestore restore;
    try {
        Key key, other;
        std::atomic<bool> cancel = false;
        std::string error;
        PackageDownload out;
        app::SetUpdateSource(app::UpdateSource::Automatic);
        for (int count : {0, 3, 1}) {
            Data data(key, count);
            const auto p = Sources(data.fixture.release.package), s = Sources(data.fixture.release.signature);
            Reader reader(cancel); reader.steps = {{p[0], data.archive}, {s[0], data.fixture.signature}};
            const bool ok = FetchCatalogPackage(data.catalog, "test.archive", "1.2.3", cancel, out, error, reader.Function());
            Check("valid v1/native count " + std::to_string(count), ok && out.package && out.package.Files().size() == static_cast<size_t>(count) &&
                out.package.ManifestBytes() == data.fixture.manifest && out.package.PublisherId() == key.publisher.id &&
                out.package_url == p[0] && out.signature_url == s[0] && out.requested_source == app::UpdateSource::Automatic &&
                reader.expected && reader.calls == 2 && error.empty(), error);
        }
        const auto sentinel = out;
        const auto unchanged = [&] {
            return out.package && out.package.Revision() == sentinel.package.Revision() && out.package.ManifestBytes() == sentinel.package.ManifestBytes() &&
                out.package.PublisherId() == sentinel.package.PublisherId() && out.package_url == sentinel.package_url &&
                out.signature_url == sentinel.signature_url && out.requested_source == sentinel.requested_source;
        };
        Data base(key, 3);
        const auto p = Sources(base.fixture.release.package), s = Sources(base.fixture.release.signature);
        const auto reject_without_io = [&](const std::string& label, Catalog catalog, std::string id = "test.archive", std::string version = "1.2.3") {
            Reader reader(cancel);
            const bool ok = FetchCatalogPackage(catalog, id, version, cancel, out, error, reader.Function());
            Check(label, !ok && reader.calls == 0 && !error.empty() && unchanged(), error);
        };
        reject_without_io("missing extension before network", base.catalog, "absent");
        reject_without_io("missing version before network", base.catalog, "test.archive", "9.9.9");
        reject_without_io("version match is exact", base.catalog, "test.archive", "1.2.03");
        reject_without_io("empty catalog before network", {});
        auto bad = base.catalog; bad.publishers.clear(); reject_without_io("missing associated publisher before network", bad);
        bad = base.catalog; bad.extensions.push_back(bad.extensions[0]); reject_without_io("duplicate extension rejects ambiguity", bad);
        bad = base.catalog; bad.extensions[0].versions.push_back(bad.extensions[0].versions[0]); reject_without_io("duplicate release rejects ambiguity", bad);
        bad = base.catalog; bad.publishers.push_back(bad.publishers[0]); reject_without_io("duplicate publisher rejects ambiguity", bad);
        for (bool signature : {false, true}) {
            for (uint64_t size : {uint64_t{0}, (signature ? kMaxPackageDownloadSignatureBytes : kMaxPackageDownloadBytes) + 1, UINT64_MAX}) {
                bad = base.catalog;
                auto& asset = signature ? bad.extensions[0].versions[0].signature : bad.extensions[0].versions[0].package;
                asset.size = size;
                reject_without_io(std::string(signature ? "signature" : "package") + " invalid size " + std::to_string(size), bad);
            }
            bad = base.catalog;
            auto& asset = signature ? bad.extensions[0].versions[0].signature : bad.extensions[0].versions[0].package;
            asset.sha256 = "bad";
            reject_without_io(signature ? "signature invalid hash before network" : "package invalid hash before network", bad);
        }
        bad = base.catalog; bad.extensions[0].versions[0].manifest_sha256 = "bad";
        reject_without_io("manifest hash syntax before network", bad);
        for (const auto* url : {"http://github.com/a/b/releases/download/v/a.pulse-ext", "https://evil.example/a.pulse-ext",
            "https://github.com@evil.example/a.pulse-ext", "https://github.com/a/b/releases/download/../a.pulse-ext",
            "https://github.com/a/b/releases/download/v/a.pulse-ext?token=secret", "https://github.com/a/b/releases/download/v/%2e.pulse-ext",
            "https://github.com/a/b/releases/download/v/a\\b.pulse-ext", "https://github.com/a/b/releases/download/v/a.sig.json"}) {
            bad = base.catalog; bad.extensions[0].versions[0].package.url = url;
            reject_without_io(std::string("unsafe/unauthorized URL ") + url, bad);
        }
        bad = base.catalog; bad.extensions[0].versions[0].signature.url = "https://github.com/a/b/releases/download/v/a.pulse-ext";
        reject_without_io("wrong signature URL suffix before network", bad);
        cancel.store(true); reject_without_io("pre-cancel no network and out unchanged", base.catalog); cancel.store(false);
        {
            Data selected(other, 1, "test.selected");
            auto catalog = base.catalog;
            catalog.publishers.push_back(selected.fixture.publisher);
            auto extension = selected.fixture.extension;
            auto unrelated = base.fixture.release; unrelated.version = "9.0.0";
            extension.versions.insert(extension.versions.begin(), unrelated);
            catalog.extensions.push_back(extension);
            Reader reader(cancel); reader.steps = {{p[0], selected.archive}, {s[0], selected.fixture.signature}};
            PackageDownload result;
            const bool ok = FetchCatalogPackage(catalog, "test.selected", "1.2.3", cancel, result, error, reader.Function());
            Check("select id/version/publisher in same catalog not first objects", ok && reader.expected && reader.calls == 2 &&
                result.package.Declaration().id == "test.selected" && result.package.PublisherId() == other.publisher.id, error);
            reject_without_io("never select a version from another extension", catalog, "test.archive", "9.0.0");
        }
        const auto success_steps = [&](const std::string& label, std::vector<Step> steps, size_t pi, size_t si) {
            Reader reader(cancel); reader.steps = std::move(steps); PackageDownload result;
            const bool ok = FetchCatalogPackage(base.catalog, "test.archive", "1.2.3", cancel, result, error, reader.Function());
            Check(label, ok && reader.expected && reader.calls == reader.steps.size() && result.package_url == p[pi] && result.signature_url == s[si] &&
                result.package.Revision() == base.fixture.release.manifest_sha256, error);
        };
        success_steps("ordered mirror fallback and independent asset sources", {{p[0], {}, Mode::Failure}, {p[1], {}, Mode::Failure}, {p[2], base.archive},
            {s[0], {}, Mode::Failure}, {s[1], base.fixture.signature}}, 2, 1);
        success_steps("partial package stream reset before next mirror", {{p[0], base.archive, Mode::HalfFailure}, {p[1], base.archive}, {s[0], base.fixture.signature}}, 1, 0);
        success_steps("partial signature stream reset before next mirror", {{p[0], base.archive}, {s[0], base.fixture.signature, Mode::HalfFailure}, {s[1], base.fixture.signature}}, 0, 1);
        auto wrong_archive = base.archive; wrong_archive[0] ^= 1;
        auto wrong_signature = base.fixture.signature; wrong_signature[0] ^= 1;
        success_steps("same-size wrong package hash fallback", {{p[0], wrong_archive}, {p[1], base.archive}, {s[0], base.fixture.signature}}, 1, 0);
        success_steps("same-size wrong signature hash fallback", {{p[0], base.archive}, {s[0], wrong_signature}, {s[1], base.fixture.signature}}, 0, 1);
        success_steps("short completed package fallback", {{p[0], base.archive.substr(1)}, {p[1], base.archive}, {s[0], base.fixture.signature}}, 1, 0);
        success_steps("short completed signature fallback", {{p[0], base.archive}, {s[0], base.fixture.signature.substr(1)}, {s[1], base.fixture.signature}}, 0, 1);
        success_steps("empty completed package fallback", {{p[0], {}}, {p[1], base.archive}, {s[0], base.fixture.signature}}, 1, 0);
        success_steps("empty completed signature fallback", {{p[0], base.archive}, {s[0], {}}, {s[1], base.fixture.signature}}, 0, 1);
        success_steps("malicious oversize callback false ignored cannot accept package", {{p[0], base.archive, Mode::Oversize}, {p[1], base.archive}, {s[0], base.fixture.signature}}, 1, 0);
        success_steps("malicious oversize callback false ignored cannot accept signature", {{p[0], base.archive}, {s[0], base.fixture.signature, Mode::Oversize}, {s[1], base.fixture.signature}}, 0, 1);
        success_steps("malicious null chunk callback false ignored", {{p[0], base.archive, Mode::NullChunk}, {p[1], base.archive}, {s[0], base.fixture.signature}}, 1, 0);
        success_steps("source captured once despite preference change mid-package", {{p[0], base.archive, Mode::ChangeSource}, {s[0], {}, Mode::Failure}, {s[1], base.fixture.signature}}, 0, 1);
        app::SetUpdateSource(app::UpdateSource::Automatic);
        const auto failed_steps = [&](const std::string& label, const Catalog& catalog, std::vector<Step> steps, bool cancellation) {
            Reader reader(cancel); reader.steps = std::move(steps);
            const bool ok = FetchCatalogPackage(catalog, "test.archive", "1.2.3", cancel, out, error, reader.Function());
            Check(label, !ok && unchanged() && reader.expected && reader.calls == reader.steps.size() &&
                !error.empty() && (!cancellation || error == "cancelled"), error);
            cancel.store(false);
        };
        failed_steps("all sources fail leaves out unchanged", base.catalog, {{p[0], {}, Mode::Failure}, {p[1], {}, Mode::Failure}, {p[2], {}, Mode::Failure}}, false);
        failed_steps("all sources bad hash leaves out unchanged", base.catalog, {{p[0], wrong_archive}, {p[1], wrong_archive}, {p[2], wrong_archive}}, false);
        for (auto mode : {Mode::CancelChunk, Mode::CancelAfter, Mode::CancelThenClear, Mode::CancelCode}) {
            failed_steps("package cancel stops fallback mode " + std::to_string(static_cast<int>(mode)), base.catalog, {{p[0], base.archive, mode}}, true);
            failed_steps("signature cancel stops fallback mode " + std::to_string(static_cast<int>(mode)), base.catalog,
                {{p[0], base.archive}, {s[0], base.fixture.signature, mode}}, true);
        }
        for (const auto source : {app::UpdateSource::GitHub, app::UpdateSource::Mirror1, app::UpdateSource::Mirror2}) {
            app::SetUpdateSource(source);
            Reader reader(cancel);
            reader.steps = {{app::UpdateSourceUrl(p[2], source), base.archive}, {app::UpdateSourceUrl(s[2], source), base.fixture.signature}};
            PackageDownload result;
            const bool ok = FetchCatalogPackage(base.catalog, "test.archive", "1.2.3", cancel, result, error, reader.Function());
            Check("explicit source captured " + std::to_string(static_cast<int>(source)), ok && reader.expected && reader.calls == 2 && result.requested_source == source, error);
        }
        app::SetUpdateSource(app::UpdateSource::Automatic);
        auto mismatch = base;
        mismatch.fixture.signature = other.Sign(mismatch.fixture.manifest); mismatch.fixture.Assets(mismatch.archive); mismatch.Sync();
        failed_steps("downloaded exact assets but wrong package signer rejected", mismatch.catalog, {{p[0], mismatch.archive}, {s[0], mismatch.fixture.signature}}, false);
        mismatch = base; mismatch.fixture.signature = key.Sign(base.fixture.manifest + " "); mismatch.fixture.Assets(mismatch.archive); mismatch.Sync();
        failed_steps("downloaded exact assets but signature for different manifest rejected", mismatch.catalog, {{p[0], mismatch.archive}, {s[0], mismatch.fixture.signature}}, false);
        bad = base.catalog; bad.extensions[0].versions[0].manifest_sha256 = std::string(64, '0');
        failed_steps("downloaded exact assets but manifest hash mismatch rejected", bad, {{p[0], base.archive}, {s[0], base.fixture.signature}}, false);
        Data wrong_id(key, 1, "test.other"); wrong_id.fixture.extension.id = "test.archive"; wrong_id.Sync();
        failed_steps("downloaded exact assets but manifest id mismatch rejected", wrong_id.catalog, {{p[0], wrong_id.archive}, {s[0], wrong_id.fixture.signature}}, false);
        bad = base.catalog; bad.extensions[0].versions[0].resources.clear();
        failed_steps("downloaded exact assets but resources mismatch rejected", bad, {{p[0], base.archive}, {s[0], base.fixture.signature}}, false);
        bad = base.catalog; bad.extensions[0].publisher = other.publisher.id; bad.publishers = {other.publisher};
        failed_steps("catalog publisher cannot be substituted by ZIP signer", bad, {{p[0], base.archive}, {s[0], base.fixture.signature}}, false);
        Check("empty reader safely rejects with unchanged out", !FetchCatalogPackage(base.catalog, "test.archive", "1.2.3", cancel, out, error, {}) && unchanged() && !error.empty(), error);
    } catch (const std::exception& ex) { Check("unexpected exception", false, ex.what()); }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
