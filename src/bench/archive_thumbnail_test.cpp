// archive_thumbnail_test.cpp - Quick Look's archive thumbnail view: pictures
// decoded out of an archive (preview host) and the ArchivePreview grid model.
#include "../preview_host/archive_member.h"
#include "../ui/archive_preview.h"
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Every name prefixed with "./": the root "./", "./img/", "./img/Red.png"
// (4x2, pure red) and "./notes.txt".
const char kZip[] =
    "\x50\x4b\x03\x04\x14\x00\x00\x00\x00\x00\x00\x00\x74\x5d\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x02\x00\x00\x00\x2e\x2f\x50\x4b\x03\x04\x14\x00\x00\x00\x00\x00\x00\x00\x74\x5d\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x2e\x2f\x69\x6d\x67\x2f\x50\x4b\x03\x04"
    "\x14\x00\x00\x00\x00\x00\x00\x00\x74\x5d\x9e\xf4\x16\xe6\x49\x00\x00\x00\x49\x00\x00\x00\x0d\x00"
    "\x00\x00\x2e\x2f\x69\x6d\x67\x2f\x52\x65\x64\x2e\x70\x6e\x67\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00"
    "\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x04\x00\x00\x00\x02\x08\x02\x00\x00\x00\xf0\xca\xea\x34"
    "\x00\x00\x00\x10\x49\x44\x41\x54\x78\x9c\x63\xf8\xcf\xc0\x00\x47\x0c\xc8\x1c\x00\x6f\xaa\x07\xf9"
    "\x80\xdc\x00\x28\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82\x50\x4b\x03\x04\x14\x00\x00\x00"
    "\x00\x00\x00\x00\x74\x5d\x86\xa6\x10\x36\x05\x00\x00\x00\x05\x00\x00\x00\x0b\x00\x00\x00\x2e\x2f"
    "\x6e\x6f\x74\x65\x73\x2e\x74\x78\x74\x68\x65\x6c\x6c\x6f\x50\x4b\x01\x02\x14\x03\x14\x00\x00\x00"
    "\x00\x00\x00\x00\x74\x5d\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x80\x01\x00\x00\x00\x00\x2e\x2f\x50\x4b\x01\x02\x14\x03\x14\x00\x00\x00"
    "\x00\x00\x00\x00\x74\x5d\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x80\x01\x20\x00\x00\x00\x2e\x2f\x69\x6d\x67\x2f\x50\x4b\x01\x02\x14\x03"
    "\x14\x00\x00\x00\x00\x00\x00\x00\x74\x5d\x9e\xf4\x16\xe6\x49\x00\x00\x00\x49\x00\x00\x00\x0d\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x01\x44\x00\x00\x00\x2e\x2f\x69\x6d\x67\x2f\x52\x65"
    "\x64\x2e\x70\x6e\x67\x50\x4b\x01\x02\x14\x03\x14\x00\x00\x00\x00\x00\x00\x00\x74\x5d\x86\xa6\x10"
    "\x36\x05\x00\x00\x00\x05\x00\x00\x00\x0b\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x01\xb8"
    "\x00\x00\x00\x2e\x2f\x6e\x6f\x74\x65\x73\x2e\x74\x78\x74\x50\x4b\x05\x06\x00\x00\x00\x00\x04\x00"
    "\x04\x00\xd8\x00\x00\x00\xe6\x00\x00\x00\x00\x00";

} // namespace

int wmain() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / L"pulse_archive_thumbnail_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path zip = dir / L"pictures.zip";
    {
        std::ofstream out(zip, std::ios::binary);
        out.write(kZip, sizeof(kZip) - 1);
    }
    using pulse::preview::DecodeArchiveMemberThumbnail;
    std::vector<uint8_t> px;
    UINT w = 0, h = 0, stride = 0, sw = 0, sh = 0;
    std::wstring error;
    const std::wstring archive = zip.wstring();
    const bool decoded = DecodeArchiveMemberThumbnail(archive + L"|img\\red.png", 256, px, w, h, stride, sw, sh, &error);
    std::printf("  decode: ok=%d %ux%u source %ux%u error=%ls\n", decoded, w, h, sw, sh, error.c_str());
    Check(decoded && w == 4 && h == 2 && sw == 4 && sh == 2 && stride == 16 && px.size() == 32,
          "a picture inside the archive decodes (\"./\" names, '\\\\' separators, any case)");
    Check(decoded && px.size() >= 4 && px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255,
          "decoded pixels are premultiplied BGRA red");
    error.clear();
    Check(!DecodeArchiveMemberThumbnail(archive + L"|notes.txt", 256, px, w, h, stride, sw, sh, &error) &&
          error == L"archive-member-not-picture", "a text member is not a picture");
    error.clear();
    Check(!DecodeArchiveMemberThumbnail(archive + L"|img/missing.png", 256, px, w, h, stride, sw, sh, &error) &&
          error == L"entry-not-found", "a missing member is reported");
    error.clear();
    Check(!DecodeArchiveMemberThumbnail(archive, 256, px, w, h, stride, sw, sh, &error) &&
          error == L"archive-member-path", "a path without a member is refused");

    // Grid model: a lone top folder opens at once; Enter goes in, Backspace out.
    using pulse::ui::ArchivePreview;
    ArchivePreview preview;
    const std::wstring payload =
        L"PULSEARC\t1\tzip\t-\t0\n"
        L"0\td\t30\t-\t-\troot\n"
        L"1\td\t20\t-\t-\tphotos\n"
        L"2\t-\t10\t-\t-\ta.png\n"
        L"2\t-\t10\t-\t-\tb.jpg\n"
        L"1\t-\t10\t-\t-\treadme.txt\n";
    Check(preview.SetPayload(payload, L"t.zip", 100), "payload parses");
    Check(preview.SetGridMode(true) && preview.GridMode(), "thumbnail view turns on");
    Check(preview.GridFolderPath() == L"root", "a lone top folder opens at once");
    Check(preview.Key(VK_HOME) && preview.SelectedPath() == L"root\\photos" && preview.SelectedIsDirectory(),
          "Home selects the first tile (folders first)");
    Check(preview.Key(VK_RETURN) && preview.GridFolderPath() == L"root\\photos", "Enter opens the folder tile");
    Check(preview.Key(VK_END) && preview.SelectedPath() == L"root\\photos\\b.jpg", "End selects the last tile");
    Check(preview.Key(VK_BACK) && preview.GridFolderPath() == L"root" && preview.SelectedPath() == L"root\\photos",
          "Backspace goes up and keeps the folder selected");
    Check(!preview.Key(VK_BACK) && preview.GridFolderPath() == L"root", "nothing above the lone top folder");
    Check(preview.SetFilter(L".png") && preview.Key(VK_HOME) && preview.SelectedPath() == L"root\\photos\\a.png",
          "a search lists matching tiles from every folder");
    preview.SetFilter(L"");
    ArchivePreview next;
    Check(next.SetPayload(payload, L"u.zip", 100) && next.GridMode(), "the next archive opens in the last view");
    Check(preview.SetGridMode(false) && !preview.GridMode(), "back to the list");
    ArchivePreview after;
    Check(after.SetPayload(payload, L"v.zip", 100) && !after.GridMode(), "and the list is remembered too");
    fs::remove_all(dir, ec);
    CoUninitialize();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
