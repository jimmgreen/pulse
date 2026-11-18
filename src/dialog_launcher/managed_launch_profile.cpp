#include "managed_launch_profile.h"
#include "../common/dialog_bundle.h"
#include <algorithm>
#include <filesystem>
#include <limits>

namespace pulse::dialog_launcher {
namespace {
namespace fs = std::filesystem;
constexpr uint32_t magic = 0x504c4d50;
#ifdef _WIN64
constexpr uint32_t machine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr uint32_t machine = IMAGE_FILE_MACHINE_I386;
#endif
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
DWORD Error() { const DWORD e = GetLastError(); return e ? e : ERROR_FUNCTION_FAILED; }
uint64_t Wide(DWORD low, DWORD high) { return (uint64_t(high) << 32) | low; }
bool Text(const std::wstring& text, bool empty = false) {
    if ((!empty && text.empty()) || text.size() >= 32768) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const uint16_t c = static_cast<uint16_t>(text[i]);
        if (!c) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
bool Absolute(const std::wstring& path) {
    if (!Text(path) || path.size() < 3 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))) return false;
    if (path.find(L'/', 0) != std::wstring::npos || path.find(L':', 2) != std::wstring::npos) return false;
    for (const auto& part : fs::path(path).relative_path()) if (part == L".." || part == L".") return false;
    return true;
}
bool Same(const std::wstring& a, const std::wstring& b) { return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0; }
DWORD NoReparse(const fs::path& path) {
    auto current = path.root_path();
    for (const auto& part : path.relative_path()) {
        current /= part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return Error();
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_NOT_SUPPORTED;
    }
    return ERROR_SUCCESS;
}
DWORD Resolve(const std::wstring& input, bool directory, std::wstring& output, ComponentIdentity* identity = nullptr,
              uint32_t* image_machine = nullptr, bool dll = false) {
    if (!Absolute(input)) return ERROR_BAD_PATHNAME;
    if (const DWORD e = NoReparse(input)) return e;
    Handle file{CreateFileW(input.c_str(), FILE_READ_ATTRIBUTES | (image_machine ? GENERIC_READ : 0),
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return Error();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) return Error();
    if (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) return ERROR_DIRECTORY;
    std::wstring final(32768, 0);
    const DWORD count = GetFinalPathNameByHandleW(file.value, final.data(), static_cast<DWORD>(final.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= final.size()) return ERROR_BAD_PATHNAME;
    final.resize(count);
    if (final.starts_with(L"\\\\?\\")) final.erase(0, 4);
    if (!Absolute(final)) return ERROR_BAD_PATHNAME;
    if (identity) *identity = {info.dwVolumeSerialNumber, Wide(info.nFileIndexLow, info.nFileIndexHigh),
        Wide(info.nFileSizeLow, info.nFileSizeHigh), Wide(info.ftLastWriteTime.dwLowDateTime, info.ftLastWriteTime.dwHighDateTime)};
    if (image_machine) {
        IMAGE_DOS_HEADER dos{}; DWORD read = 0;
        if (!ReadFile(file.value, &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos))) return ERROR_BAD_EXE_FORMAT;
        LARGE_INTEGER at{}; at.QuadPart = dos.e_lfanew;
        if (!SetFilePointerEx(file.value, at, nullptr, FILE_BEGIN)) return Error();
        DWORD signature = 0; IMAGE_FILE_HEADER header{}; WORD optional = 0;
        if (!ReadFile(file.value, &signature, sizeof(signature), &read, nullptr) || read != sizeof(signature) || signature != IMAGE_NT_SIGNATURE ||
            !ReadFile(file.value, &header, sizeof(header), &read, nullptr) || read != sizeof(header) ||
            !ReadFile(file.value, &optional, sizeof(optional), &read, nullptr) || read != sizeof(optional) ||
            header.SizeOfOptionalHeader < sizeof(WORD) ||
            !(header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
            ((header.Characteristics & IMAGE_FILE_DLL) != 0) != dll ||
            (header.Machine == IMAGE_FILE_MACHINE_AMD64 ? optional != IMAGE_NT_OPTIONAL_HDR64_MAGIC :
             header.Machine == IMAGE_FILE_MACHINE_I386 ? optional != IMAGE_NT_OPTIONAL_HDR32_MAGIC : true)) return ERROR_BAD_EXE_FORMAT;
        *image_machine = header.Machine;
    }
    output = std::move(final); return ERROR_SUCCESS;
}
bool Shape(const ManagedLaunchProfile& p) {
    if (p.format_version != 1 && p.format_version != 2) return false;
    if (p.format_version == 1) { if (!p.bundle_root.empty() || !p.manifest_digest.empty()) return false; }
    else if (!Absolute(p.bundle_root) || p.manifest_digest.size() != 64 ||
        !std::all_of(p.manifest_digest.begin(), p.manifest_digest.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return false;
    if (p.machine != IMAGE_FILE_MACHINE_I386 && p.machine != IMAGE_FILE_MACHINE_AMD64) return false;
    for (const auto* path : {&p.component_root, &p.host, &p.runtime, &p.launcher, &p.target, &p.cwd, &p.control_root})
        if (!Absolute(*path)) return false;
    if (p.arguments.size() > 64) return false;
    size_t units = 0;
    for (const auto& arg : p.arguments) { if (!Text(arg, true)) return false; units += arg.size(); }
    return units < 32768;
}
void Put(std::vector<unsigned char>& out, uint64_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<unsigned char>(n >> (i * 8)));
}
struct Reader {
    std::span<const unsigned char> bytes; size_t at = 0;
    bool Read(uint64_t& value, unsigned count) {
        if (bytes.size() - at < count) return false;
        value = 0; for (unsigned i = 0; i < count; ++i) value |= uint64_t(bytes[at++]) << (i * 8); return true;
    }
    bool String(std::wstring& value) {
        uint64_t count = 0, c = 0;
        if (!Read(count, 4) || count >= 32768 || count > (bytes.size() - at) / 2) return false;
        value.clear(); value.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) { if (!Read(c, 2)) return false; value.push_back(static_cast<wchar_t>(c)); }
        return Text(value, true);
    }
};
}
DWORD EncodeProfile(const ManagedLaunchProfile& p, std::vector<unsigned char>& output) noexcept {
    output.clear();
    try {
        if (!Shape(p)) return ERROR_INVALID_DATA;
        std::vector<unsigned char> bytes; Put(bytes, magic, 4); Put(bytes, p.format_version, 4); Put(bytes, p.machine, 4); Put(bytes, 0, 4);
        const auto string = [&](const std::wstring& s) { Put(bytes, s.size(), 4); for (wchar_t c : s) Put(bytes, static_cast<uint16_t>(c), 2); };
        for (const auto* path : {&p.component_root, &p.host, &p.runtime, &p.launcher, &p.target, &p.cwd, &p.control_root}) string(*path);
        for (const auto& id : p.components) { Put(bytes, id.volume, 4); Put(bytes, id.file, 8); Put(bytes, id.size, 8); Put(bytes, id.modified, 8); }
        Put(bytes, p.arguments.size(), 4); for (const auto& arg : p.arguments) string(arg);
        if (p.format_version == 2) { string(p.bundle_root); string(std::wstring(p.manifest_digest.begin(), p.manifest_digest.end())); }
        if (bytes.size() > kProfileMaximumBytes) return ERROR_INVALID_DATA;
        output = std::move(bytes); return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD DecodeProfile(std::span<const unsigned char> bytes, ManagedLaunchProfile& output) noexcept {
    output = {};
    try {
        if (bytes.size() > kProfileMaximumBytes) return ERROR_INVALID_DATA;
        Reader r{bytes}; uint64_t n = 0; ManagedLaunchProfile p;
        if (!r.Read(n, 4) || n != magic || !r.Read(n, 4) || (n != 1 && n != 2)) return ERROR_INVALID_DATA;
        p.format_version = static_cast<uint32_t>(n);
        if (!r.Read(n, 4)) return ERROR_INVALID_DATA;
        p.machine = static_cast<uint32_t>(n);
        if (!r.Read(n, 4) || n) return ERROR_INVALID_DATA;
        for (auto* path : {&p.component_root, &p.host, &p.runtime, &p.launcher, &p.target, &p.cwd, &p.control_root})
            if (!r.String(*path)) return ERROR_INVALID_DATA;
        for (auto& id : p.components) {
            if (!r.Read(n, 4)) return ERROR_INVALID_DATA; id.volume = static_cast<uint32_t>(n);
            if (!r.Read(id.file, 8) || !r.Read(id.size, 8) || !r.Read(id.modified, 8)) return ERROR_INVALID_DATA;
        }
        if (!r.Read(n, 4) || n > 64) return ERROR_INVALID_DATA;
        p.arguments.resize(static_cast<size_t>(n)); for (auto& arg : p.arguments) if (!r.String(arg)) return ERROR_INVALID_DATA;
        if (p.format_version == 2) {
            std::wstring digest;
            if (!r.String(p.bundle_root) || !r.String(digest) || digest.size() != 64) return ERROR_INVALID_DATA;
            for (auto c : digest) { if (c > 127) return ERROR_INVALID_DATA; p.manifest_digest.push_back(static_cast<char>(c)); }
        }
        if (r.at != bytes.size() || !Shape(p)) return ERROR_INVALID_DATA;
        output = std::move(p); return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD CaptureProfile(ManagedLaunchProfile& input, const std::wstring& actual_launcher) noexcept {
    try {
        auto p = input; uint32_t target_machine = 0, runtime_machine = 0, launcher_machine = 0, host_machine = 0;
        if (const DWORD e = Resolve(actual_launcher, false, p.launcher, &p.components[2], &launcher_machine)) return e;
        if (const DWORD e = Resolve(p.host, false, p.host, &p.components[0], &host_machine)) return e;
        if (const DWORD e = Resolve(p.runtime, false, p.runtime, &p.components[1], &runtime_machine, true)) return e;
        if (const DWORD e = Resolve(p.target, false, p.target, nullptr, &target_machine)) return e;
        if (const DWORD e = Resolve(p.cwd, true, p.cwd)) return e;
        if (const DWORD e = Resolve(p.control_root, true, p.control_root)) return e;
        p.component_root = fs::path(p.launcher).parent_path().wstring(); p.machine = machine;
        if (target_machine != machine || runtime_machine != machine || launcher_machine != machine) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
        if (!Same(fs::path(p.host).parent_path().wstring(), p.component_root) ||
            !Same(fs::path(p.runtime).parent_path().wstring(), p.component_root) ||
            _wcsicmp(fs::path(p.host).filename().c_str(), L"pulse.exe") != 0 || !Shape(p)) return ERROR_INVALID_DATA;
        if (p.format_version == 2) {
            dialog_bundle::BundleLease bundle;
            if (const DWORD e = dialog_bundle::VerifyBundle(p.bundle_root, p.manifest_digest, bundle)) return e;
            if (bundle.Machine() != p.machine || !Same(bundle.Launcher(), p.launcher) ||
                !Same(bundle.Host(), p.host) || !Same(bundle.Runtime(), p.runtime)) return ERROR_REVISION_MISMATCH;
        }
        input = std::move(p); return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD ValidateProfile(const ManagedLaunchProfile& p, const std::wstring& actual_launcher) noexcept {
    try {
        if (!Shape(p)) return ERROR_INVALID_DATA;
        auto current = p;
        if (const DWORD e = CaptureProfile(current, actual_launcher)) return e;
        if (current.machine != p.machine || !Same(current.component_root, p.component_root) ||
            !Same(current.launcher, p.launcher) || !Same(current.host, p.host) || !Same(current.runtime, p.runtime) ||
            !Same(current.target, p.target) || !Same(current.cwd, p.cwd) || !Same(current.control_root, p.control_root) ||
            current.components != p.components) return ERROR_REVISION_MISMATCH;
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD SaveProfile(const std::wstring& file, const ManagedLaunchProfile& p) noexcept {
    try {
        if (!Absolute(file)) return ERROR_BAD_PATHNAME;
        if (const DWORD e = NoReparse(fs::path(file).parent_path())) return e;
        std::vector<unsigned char> bytes; if (const DWORD e = EncodeProfile(p, bytes)) return e;
        Handle output{CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE) return Error();
        DWORD written = 0;
        if (!WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() ||
            !FlushFileBuffers(output.value)) return Error();
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD ReadDisableMarker(const std::wstring& control_root, bool& disabled) noexcept {
    disabled = false;
    try {
        if (!Absolute(control_root)) return ERROR_BAD_PATHNAME;
        const DWORD directory = GetFileAttributesW(control_root.c_str());
        if (directory == INVALID_FILE_ATTRIBUTES) return Error();
        if (!(directory & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
        const DWORD marker = GetFileAttributesW((fs::path(control_root) / L"disable").c_str());
        if (marker != INVALID_FILE_ATTRIBUTES) { disabled = true; return ERROR_SUCCESS; }
        const DWORD error = Error();
        return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD LoadProfile(const std::wstring& file, ManagedLaunchProfile& output) noexcept {
    output = {};
    try {
        if (!Absolute(file)) return ERROR_BAD_PATHNAME;
        if (const DWORD e = NoReparse(file)) return e;
        Handle input{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (input.value == INVALID_HANDLE_VALUE) return Error();
        LARGE_INTEGER size{}; if (!GetFileSizeEx(input.value, &size)) return Error();
        if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kProfileMaximumBytes)) return ERROR_INVALID_DATA;
        std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
        if (!ReadFile(input.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) return Error();
        return DecodeProfile(bytes, output);
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
}
