#include "pdfium_text.h"
#include "pdf_unicode.h"
#include "document_protocol.h"
#include "document_literal_match.h"
#include "../../third_party/pdfium/include/fpdfview.h"
#include "../../third_party/pdfium/include/fpdf_text.h"
#include <array>
#include <vector>
#if __has_include("pdfium_pin.h")
#include "pdfium_pin.h"
#include <bcrypt.h>
#include <cwchar>
#include <cwctype>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace pulse::index {
namespace {
#ifdef PULSE_PDFIUM_SHA256
bool FileSha256Is(HANDLE file, const wchar_t* expected) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return false;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<unsigned char> buffer(1 << 20);
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) ok = false;
        else if (!read) break;
        else ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0));
    }
    unsigned char digest[32]{};
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok || wcslen(expected) != 64) return false;
    static const wchar_t kHex[] = L"0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        if (towlower(expected[2 * i]) != kHex[digest[i] >> 4] || towlower(expected[2 * i + 1]) != kHex[digest[i] & 15])
            return false;
    }
    return true;
}

// From the content enhancement pack (%LOCALAPPDATA%) Pulse.Document.exe uses
// the pdfium.dll of the install directory pulse.exe names in PULSE_PDFIUM_DIR,
// and only the exact build this version ships: the variable is inherited, so
// it is never trusted for anything else. The file stays open without write
// sharing from hashing until it is mapped.
HMODULE LoadInstallPdfium() {
    wchar_t dir[32768]{}; const DWORD n = GetEnvironmentVariableW(L"PULSE_PDFIUM_DIR", dir, ARRAYSIZE(dir));
    if (!n || n >= ARRAYSIZE(dir)) return nullptr;
    std::wstring dll(dir, n);
    if (dll.back() != L'\\' && dll.back() != L'/') dll += L'\\';
    dll += L"pdfium.dll";
    const HANDLE file = CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    HMODULE module = nullptr;
    if (FileSha256Is(file, PULSE_PDFIUM_SHA256))
        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CloseHandle(file);
    return module;
}
#endif
struct Api {
    HMODULE module = nullptr;
#define PDF_FUNCTION(name) decltype(&name) name##_fn = nullptr
    PDF_FUNCTION(FPDF_InitLibraryWithConfig); PDF_FUNCTION(FPDF_DestroyLibrary);
    PDF_FUNCTION(FPDF_LoadCustomDocument); PDF_FUNCTION(FPDF_CloseDocument);
    PDF_FUNCTION(FPDF_GetLastError); PDF_FUNCTION(FPDF_GetPageCount);
    PDF_FUNCTION(FPDF_LoadPage); PDF_FUNCTION(FPDF_ClosePage);
    PDF_FUNCTION(FPDFText_LoadPage); PDF_FUNCTION(FPDFText_ClosePage);
    PDF_FUNCTION(FPDFText_CountChars); PDF_FUNCTION(FPDFText_GetUnicode);
#undef PDF_FUNCTION
    bool ready = false;
    Api() {
        wchar_t path[32768]{}; const auto n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        if (!n || n >= ARRAYSIZE(path)) return;
        std::wstring dll(path, n); const auto slash = dll.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        dll.resize(slash + 1); dll += L"pdfium.dll";
        if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES)
            module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#ifdef PULSE_PDFIUM_SHA256
        else
            module = LoadInstallPdfium();
#endif
        if (!module) return;
#define LOAD_PDF(name) name##_fn = reinterpret_cast<decltype(name##_fn)>(GetProcAddress(module, #name)); if (!name##_fn) return
        LOAD_PDF(FPDF_InitLibraryWithConfig); LOAD_PDF(FPDF_DestroyLibrary);
        LOAD_PDF(FPDF_LoadCustomDocument); LOAD_PDF(FPDF_CloseDocument);
        LOAD_PDF(FPDF_GetLastError); LOAD_PDF(FPDF_GetPageCount);
        LOAD_PDF(FPDF_LoadPage); LOAD_PDF(FPDF_ClosePage);
        LOAD_PDF(FPDFText_LoadPage); LOAD_PDF(FPDFText_ClosePage);
        LOAD_PDF(FPDFText_CountChars); LOAD_PDF(FPDFText_GetUnicode);
#undef LOAD_PDF
        FPDF_LIBRARY_CONFIG config{}; config.version = 2;
        FPDF_InitLibraryWithConfig_fn(&config); ready = true;
    }
    ~Api() { if (ready) FPDF_DestroyLibrary_fn(); if (module) FreeLibrary(module); }
};
struct Reader {
    HANDLE file;
    uint64_t size;
    DocumentReadMetrics& metrics;
    std::array<unsigned char, 64 * 1024> cache{};
    uint64_t offset = 0;
    DWORD available = 0;
    static int Read(void* context, unsigned long position, unsigned char* data, unsigned long count) {
        auto& self = *static_cast<Reader*>(context);
        DocumentMetricScope callback_time(self.metrics.pdf_reader_us);
        ++self.metrics.pdf_read_calls;
        self.metrics.pdf_requested_bytes += count;
        if (position > self.size || count > self.size - position) return 0;
        while (count) {
            if (position < self.offset || position >= self.offset + self.available) {
                self.offset = position;
                LARGE_INTEGER offset{}; offset.QuadPart = position;
                DocumentMetricScope io_time(self.metrics.pdf_file_io_us);
                if (!SetFilePointerEx(self.file, offset, nullptr, FILE_BEGIN)) return 0;
                ++self.metrics.pdf_file_reads;
                if (!ReadFile(self.file, self.cache.data(), static_cast<DWORD>((std::min)(uint64_t{self.cache.size()}, self.size - position)), &self.available, nullptr) || !self.available) return 0;
                self.metrics.pdf_file_bytes += self.available;
            } else ++self.metrics.pdf_cache_hits;
            const auto from = static_cast<size_t>(position - self.offset);
            const auto n = (std::min)(static_cast<size_t>(count), static_cast<size_t>(self.available) - from);
            memcpy(data, self.cache.data() + from, n);
            count -= static_cast<unsigned long>(n); position += static_cast<unsigned long>(n); data += n;
        }
        return 1;
    }
};
HRESULT Error(Api& api) {
    switch (api.FPDF_GetLastError_fn()) {
    case FPDF_ERR_PASSWORD: return E_ACCESSDENIED;
    case FPDF_ERR_SECURITY: return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    case FPDF_ERR_FILE: return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    default: return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }
}
bool Cjk(uint32_t value) {
    return (value >= 0x3400 && value <= 0x9fff) || (value >= 0xf900 && value <= 0xfaff) ||
        (value >= 0x20000 && value <= 0x323af);
}
uint32_t Codepoint(std::wstring_view value, size_t at) {
    const uint32_t first = value[at];
    if (first >= 0xd800 && first <= 0xdbff && at + 1 < value.size() && value[at + 1] >= 0xdc00 && value[at + 1] <= 0xdfff)
        return 0x10000 + ((first - 0xd800) << 10) + value[at + 1] - 0xdc00;
    return first;
}
void AppendPage(std::wstring& output, std::wstring_view page) {
    uint32_t previous = 0;
    for (size_t i = 0; i < page.size();) {
        if (page[i] == L' ' || page[i] == L'\t' || page[i] == L'\r' || page[i] == L'\n') {
            size_t end = i + 1;
            while (end < page.size() && (page[end] == L' ' || page[end] == L'\t' || page[end] == L'\r' || page[end] == L'\n')) ++end;
            // CAD exports often turn every Chinese glyph into a separate text line.
            // Chinese has no inter-word spaces; join only within a page, preserving Latin boundaries.
            if (!(Cjk(previous) && end < page.size() && Cjk(Codepoint(page, end)))) output.append(page.substr(i, end - i));
            i = end;
        } else {
            previous = Codepoint(page, i);
            const size_t count = previous > 0xffff ? 2 : 1;
            output.append(page.substr(i, count)); i += count;
        }
    }
}
}
HRESULT ExtractPdfiumText(HANDLE file, uint64_t bytes, std::wstring& output,
    std::wstring_view needle, bool case_sensitive, DocumentReadMetrics& metrics) {
    output.clear();
    DocumentMetricScope loading(metrics.pdf_load_us);
    static Api api;
    if (!api.ready) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if (bytes > document::kMaximumFileBytes) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    Reader reader{file, bytes, metrics};
    FPDF_FILEACCESS access{static_cast<unsigned long>(bytes), Reader::Read, &reader};
    const auto doc = api.FPDF_LoadCustomDocument_fn(&access, nullptr);
    loading.Finish();
    if (!doc) return Error(api);
    struct Document { Api& api; FPDF_DOCUMENT value; ~Document() { api.FPDF_CloseDocument_fn(value); } } owned{api, doc};
    const int pages = api.FPDF_GetPageCount_fn(doc);
    if (pages < 0 || pages > 1000000) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    DocumentLiteralMatch match(needle, case_sensitive);
    for (int page_index = 0; page_index < pages; ++page_index) {
        DocumentMetricScope page_timing(metrics.pdf_page_us);
        const auto page = api.FPDF_LoadPage_fn(doc, page_index);
        if (!page) return Error(api);
        struct Page { Api& api; FPDF_PAGE value; ~Page() { api.FPDF_ClosePage_fn(value); } } page_owner{api, page};
        const auto text = api.FPDFText_LoadPage_fn(page);
        page_timing.Finish();
        if (!text) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        ++metrics.pdf_pages;
        DocumentMetricScope text_timing(metrics.pdf_text_us);
        struct Text { Api& api; FPDF_TEXTPAGE value; ~Text() { api.FPDFText_ClosePage_fn(value); } } text_owner{api, text};
        const int chars = api.FPDFText_CountChars_fn(text);
        if (chars < 0) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        const size_t separator = !output.empty() && output.back() != L'\n' ? 1 : 0;
        if (separator > document::kMaximumTextChars - output.size())
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        const size_t capacity = document::kMaximumTextChars - output.size() - separator;
        std::wstring page_text;
        page_text.reserve((std::min)(static_cast<size_t>(chars), capacity));
        for (int i = 0; i < chars; ++i) {
            uint32_t value = api.FPDFText_GetUnicode_fn(text, i);
            // The bundled PDFium exposes ToUnicode surrogate pairs as two
            // character indices; other mappings can return a full scalar.
            if (value >= 0xd800 && value <= 0xdbff) {
                if (i + 1 >= chars) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
                const uint32_t low = api.FPDFText_GetUnicode_fn(text, ++i);
                if (low < 0xdc00 || low > 0xdfff) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
                value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
            }
            const HRESULT result = AppendPdfUnicode(page_text, value, capacity);
            if (FAILED(result)) return result;
        }
        if (separator) output += L'\n';
        AppendPage(output, page_text);
        if (match.Found(output)) return S_OK;
    }
    return output.find_first_not_of(L" \t\r\n\0", 0, 5) == std::wstring::npos ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : S_OK;
}
}
