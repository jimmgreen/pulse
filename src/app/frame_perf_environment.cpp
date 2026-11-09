#include "frame_perf_environment.h"
#include <windows.h>
#include <bcrypt.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>
#include <mutex>

namespace pulse {
namespace {
struct FontSnapshot {
    bool available = false;
    std::wstring family, locale;
    float size = 0;
    int weight = 0, style = 0, stretch = 0, text_mode = 0, font_percent = 0, scale_milli = 0;
};
FontSnapshot shared_font_snapshot;
std::mutex font_snapshot_mutex;

std::string Quote(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    std::string result = "\"";
    for (const unsigned char c : utf8) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    return result + '"';
}

std::string Hash(const wchar_t* path, uint64_t& size, DWORD& error) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return {}; }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length)) { error = GetLastError(); CloseHandle(file); return {}; }
    size = static_cast<uint64_t>(length.QuadPart);
    if (size > 512ull * 1024 * 1024) { error = ERROR_FILE_TOO_LARGE; CloseHandle(file); return {}; }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<UCHAR, 64 * 1024> buffer{};
    uint64_t remaining = size;
    while (ok && remaining) {
        const DWORD wanted = static_cast<DWORD>((std::min)(remaining, static_cast<uint64_t>(buffer.size())));
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), wanted, &read, nullptr)) { error = GetLastError(); ok = false; }
        else if (!read) { error = ERROR_HANDLE_EOF; ok = false; }
        else { remaining -= read; ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0; }
    }
    std::array<UCHAR, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok) { if (!error) error = ERROR_GEN_FAILURE; return {}; }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto c : digest) { result += hex[c >> 4]; result += hex[c & 15]; }
    return result;
}

std::string Version(const wchar_t* path) {
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &unused);
    if (!size || size > 1024 * 1024) return {};
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* version = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path, 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &length) || length < sizeof(*version)) return {};
    return std::to_string(HIWORD(version->dwFileVersionMS)) + '.' + std::to_string(LOWORD(version->dwFileVersionMS)) + '.' +
        std::to_string(HIWORD(version->dwFileVersionLS)) + '.' + std::to_string(LOWORD(version->dwFileVersionLS));
}
}

void CaptureFramePerfFont(IDWriteTextFormat* format, int text_mode, int font_percent, int scale_milli) noexcept {
    try {
        FontSnapshot snapshot;
        if (format) {
            const UINT32 family_length = format->GetFontFamilyNameLength();
            const UINT32 locale_length = format->GetLocaleNameLength();
            if (family_length <= 1024 && locale_length <= 1024) {
                snapshot.family.resize(static_cast<size_t>(family_length) + 1);
                snapshot.locale.resize(static_cast<size_t>(locale_length) + 1);
                snapshot.available = SUCCEEDED(format->GetFontFamilyName(snapshot.family.data(), family_length + 1)) &&
                    SUCCEEDED(format->GetLocaleName(snapshot.locale.data(), locale_length + 1));
                snapshot.family.resize(family_length);
                snapshot.locale.resize(locale_length);
                snapshot.size = format->GetFontSize();
                snapshot.weight = static_cast<int>(format->GetFontWeight());
                snapshot.style = static_cast<int>(format->GetFontStyle());
                snapshot.stretch = static_cast<int>(format->GetFontStretch());
            }
        }
        snapshot.text_mode = text_mode;
        snapshot.font_percent = font_percent;
        snapshot.scale_milli = scale_milli;
        std::lock_guard lock(font_snapshot_mutex);
        shared_font_snapshot = std::move(snapshot);
    } catch (...) {}
}

bool WriteFramePerfEnvironment(const std::wstring& path) noexcept {
    try {
        FontSnapshot font_snapshot;
        {
            std::lock_guard lock(font_snapshot_mutex);
            font_snapshot = shared_font_snapshot;
        }
        std::string report = "{\"schema\":1,\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"tick_ms\":" + std::to_string(GetTickCount64()) + ",\"module_scope\":\"loaded_allowlist\",\"modules\":[";
        bool first = true;
        for (const auto* name : {L"", L"lumatext.dll", L"dwrite.dll", L"d2d1.dll", L"d3d11.dll", L"dxgi.dll"}) {
            HMODULE module = nullptr;
            const bool loaded = GetModuleHandleExW(0, *name ? name : nullptr, &module) != FALSE;
            const DWORD load_error = loaded ? 0 : GetLastError();
            std::array<wchar_t, 32768> filename{};
            const DWORD count = loaded ? GetModuleFileNameW(module, filename.data(), static_cast<DWORD>(filename.size())) : 0;
            const DWORD path_error = loaded && (!count || count >= filename.size()) ? GetLastError() : load_error;
            if (module) FreeLibrary(module);
            uint64_t size = 0; DWORD read_error = path_error;
            const auto hash = !path_error && count ? Hash(filename.data(), size, read_error) : std::string{};
            if (!first) report += ',';
            first = false;
            report += "{\"name\":" + Quote(*name ? name : L"process_executable") + ",\"loaded\":" + (loaded ? "true" : "false") +
                ",\"path\":" + Quote(!path_error && count ? filename.data() : L"") + ",\"read_error\":" + std::to_string(read_error) +
                ",\"size\":" + std::to_string(size) + ",\"sha256\":\"" + hash + "\",\"file_version\":\"" +
                (!path_error && count ? Version(filename.data()) : std::string{}) + "\"}";
        }
        report += "],\"font_snapshot_available\":" + std::string(font_snapshot.available ? "true" : "false");
        if (font_snapshot.available) {
            report += ",\"font_snapshot\":{\"family\":" + Quote(font_snapshot.family) + ",\"locale\":" + Quote(font_snapshot.locale) +
                ",\"size\":" + std::to_string(font_snapshot.size) + ",\"weight\":" + std::to_string(font_snapshot.weight) +
                ",\"style\":" + std::to_string(font_snapshot.style) + ",\"stretch\":" + std::to_string(font_snapshot.stretch) +
                ",\"text_mode\":" + std::to_string(font_snapshot.text_mode) + ",\"font_percent\":" + std::to_string(font_snapshot.font_percent) +
                ",\"scale_milli\":" + std::to_string(font_snapshot.scale_milli) + "}";
        }
        report += ",\"adapter_scope\":\"enumerated_not_active_device\",\"adapters\":[";
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        const HRESULT factory_result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        first = true;
        if (SUCCEEDED(factory_result)) for (UINT index = 0; index < 32; ++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory->EnumAdapters1(index, &adapter))) break;
            DXGI_ADAPTER_DESC1 desc{};
            const HRESULT desc_result = adapter->GetDesc1(&desc);
            LARGE_INTEGER driver{};
            const HRESULT driver_result = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
            if (!first) report += ',';
            first = false;
            report += "{\"description\":" + Quote(desc.Description) + ",\"description_hr\":" + std::to_string(desc_result) +
                ",\"vendor_id\":" + std::to_string(desc.VendorId) + ",\"device_id\":" + std::to_string(desc.DeviceId) +
                ",\"luid_low\":" + std::to_string(desc.AdapterLuid.LowPart) + ",\"luid_high\":" + std::to_string(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) +
                ",\"flags\":" + std::to_string(desc.Flags) + ",\"dedicated_video_memory\":" + std::to_string(desc.DedicatedVideoMemory) +
                ",\"driver_version\":" + std::to_string(driver.QuadPart) + ",\"driver_hr\":" + std::to_string(driver_result) + "}";
        }
        HIGHCONTRASTW contrast{sizeof(contrast)};
        const BOOL contrast_ok = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        const DWORD contrast_error = contrast_ok ? 0 : GetLastError();
        report += "],\"adapter_enumeration_hr\":" + std::to_string(factory_result) + ",\"high_contrast_flags\":" +
            std::to_string(contrast.dwFlags) + ",\"high_contrast_error\":" + std::to_string(contrast_error) + "}\n";
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        output.write(report.data(), static_cast<std::streamsize>(report.size()));
        output.close();
        return output.good();
    } catch (...) { return false; }
}
}
