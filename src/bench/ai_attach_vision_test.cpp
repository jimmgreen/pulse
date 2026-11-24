// ai_attach_vision_test.cpp - Tests for Batches 3 & 4: local document reading
// (UTF-8/code/folder/binary/truncation) and WIC multimodal image encoding.
#include "../app/ai_file_reader.h"
#include "../app/ai_vision.h"
#include "../common/json_value.h"
#include "../common/utf8_file.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* name) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "[FAIL] %s\n", name);
    }
}

bool Write24BppBmp(const std::filesystem::path& path, int width, int height) {
    const int stride = ((width * 3 + 3) / 4) * 4;
    const uint32_t img_size = static_cast<uint32_t>(stride * height);
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + img_size;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = img_size;

    std::vector<uint8_t> pixels(img_size, 0);
    for (int y = 0; y < height; ++y) {
        uint8_t* row = pixels.data() + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            row[x * 3 + 0] = static_cast<uint8_t>((x * 255) / (width > 1 ? width - 1 : 1));
            row[x * 3 + 1] = static_cast<uint8_t>((y * 255) / (height > 1 ? height - 1 : 1));
            row[x * 3 + 2] = static_cast<uint8_t>(((x + y) / 4) & 0xFF);
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    out.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return out.good();
}

} // namespace

int main() {
    using namespace pulse::app::ai;
    namespace fs = std::filesystem;

    // 1. Extension helpers
    Check(LowerExtension(L"C:\\docs\\Photo.PNG") == L"png", "LowerExtension upper");
    Check(LowerExtension(L"C:\\docs.v1\\README") == L"", "LowerExtension no ext with dot in dir");
    Check(IsSupportedImageExtension(L"png") && IsSupportedImageExtension(L"jpeg") && IsSupportedImageExtension(L"webp"),
          "IsSupportedImageExtension valid");
    Check(!IsSupportedImageExtension(L"pdf") && !IsSupportedImageExtension(L"cpp"), "IsSupportedImageExtension reject");
    Check(IsReadableDocumentExtension(L"md") && IsReadableDocumentExtension(L"cpp") &&
              IsReadableDocumentExtension(L"pdf") && IsReadableDocumentExtension(L"docx"),
          "IsReadableDocumentExtension valid");

    // 2. Base64 RFC 4648 test vectors
    Check(Base64Encode(reinterpret_cast<const uint8_t*>(""), 0) == "", "Base64 empty");
    Check(Base64Encode(reinterpret_cast<const uint8_t*>("f"), 1) == "Zg==", "Base64 f");
    Check(Base64Encode(reinterpret_cast<const uint8_t*>("fo"), 2) == "Zm8=", "Base64 fo");
    Check(Base64Encode(reinterpret_cast<const uint8_t*>("foo"), 3) == "Zm9v", "Base64 foo");
    Check(Base64Encode(reinterpret_cast<const uint8_t*>("foobar"), 6) == "Zm9vYmFy", "Base64 foobar");

    // Create isolated temp directory
    wchar_t tmp_buf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp_buf);
    const fs::path dir = fs::path(tmp_buf) / (L"pulse_attach_vision_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    std::atomic<bool> cancelled{false};

    // 3. UTF-8 Chinese Markdown file reading & truncation
    const fs::path md_path = dir / L"report.md";
    std::wstring md_text = L"# 2026 Q3 财务报价单\n\n项目总预算：¥128,500。\n";
    for (int i = 0; i < 60; ++i) md_text += L"第 " + std::to_wstring(i) + L" 行详细说明内容测试。\n";
    Check(pulse::WriteUtf8FileAtomic(md_path.wstring(), md_text), "Write UTF-8 markdown fixture");

    {
        const LocalFileExcerpt ex = ReadLocalFileExcerpt(md_path.wstring(), 10000, cancelled);
        Check(ex.ok && !ex.is_image && !ex.is_dir && !ex.truncated, "ReadLocalFileExcerpt full md");
        Check(ex.format == "md", "ReadLocalFileExcerpt format md");
        Check(ex.text.find("2026 Q3") != std::string::npos && ex.text.find("\xE6\x8A\xA5\xE4\xBB\xB7\xE5\x8D\x95") != std::string::npos,
              "ReadLocalFileExcerpt preserves UTF-8 Chinese");
    }
    {
        const LocalFileExcerpt ex = ReadLocalFileExcerpt(md_path.wstring(), 220, cancelled);
        Check(ex.ok && ex.truncated, "ReadLocalFileExcerpt truncated md");
        Check(!ex.text.empty() && ex.text.size() < 700, "ReadLocalFileExcerpt respects char cap");
    }

    // 4. Binary file detection
    const fs::path bin_path = dir / L"blob.dat";
    {
        std::ofstream bf(bin_path, std::ios::binary);
        const uint8_t bytes[] = {0x7F, 'E', 'L', 'F', 0x00, 0x01, 0x02, 0x00, 0xFF, 0x00};
        bf.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    {
        const LocalFileExcerpt ex = ReadLocalFileExcerpt(bin_path.wstring(), 4000, cancelled);
        Check(!ex.ok && ex.error_code == "binary_file", "ReadLocalFileExcerpt rejects binary");
    }

    // 5. Folder listing
    {
        const LocalFileExcerpt ex = ReadLocalFileExcerpt(dir.wstring(), 4000, cancelled);
        Check(ex.ok && ex.is_dir && ex.format == "folder", "ReadLocalFileExcerpt folder");
        Check(ex.text.find("report.md") != std::string::npos && ex.text.find("blob.dat") != std::string::npos,
              "ReadLocalFileExcerpt folder lists entries");
    }

    // 6. Missing & cancelled handling
    {
        const LocalFileExcerpt ex = ReadLocalFileExcerpt((dir / L"missing.txt").wstring(), 4000, cancelled);
        Check(!ex.ok && ex.error_code == "not_found", "ReadLocalFileExcerpt missing file");
    }
    {
        std::atomic<bool> stop{true};
        const LocalFileExcerpt ex = ReadLocalFileExcerpt(md_path.wstring(), 4000, stop);
        Check(!ex.ok && ex.error_code == "cancelled", "ReadLocalFileExcerpt cancelled");
    }

    // 7. Real WIC image downscaling + JPEG encoding + Base64 data URL
    const fs::path bmp_path = dir / L"wide_chart.bmp";
    Check(Write24BppBmp(bmp_path, 2200, 1100), "Write 2200x1100 BMP fixture");
    {
        const LocalFileExcerpt ex = ReadLocalFileExcerpt(bmp_path.wstring(), 4000, cancelled);
        Check(ex.ok && ex.is_image && ex.text.empty(), "ReadLocalFileExcerpt detects image without text");
    }
    {
        const EncodedImage enc = EncodeImageFileForVision(bmp_path.wstring(), 1568, 900 * 1024);
        Check(enc.ok, "EncodeImageFileForVision succeeds");
        Check(enc.mime == "image/jpeg", "EncodeImageFileForVision jpeg mime");
        Check(enc.width == 1568 && enc.height == 784, "EncodeImageFileForVision preserves 2:1 aspect downscale");
        Check(enc.encoded_bytes > 1000 && enc.encoded_bytes <= 900 * 1024, "EncodeImageFileForVision size within cap");
        Check(enc.data_url.rfind("data:image/jpeg;base64,", 0) == 0 && enc.data_url.size() > 100,
              "EncodeImageFileForVision valid data URL");
    }
    {
        const EncodedImage bad = EncodeImageFileForVision(bin_path.wstring());
        Check(!bad.ok && !bad.error_message.empty(), "EncodeImageFileForVision rejects corrupt image");
    }

    fs::remove_all(dir, ec);
    std::printf("[SUMMARY] ai_attach_vision_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
