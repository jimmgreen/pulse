#include "native_image_verify.h"
#include "package_revision.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace pulse::extensions {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool Fail(std::string& error, const char* message) {
    error = message;
    return false;
}
template<class T>
bool ReadStructure(std::string_view bytes, size_t offset, T& value) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}
bool Amd64Dll(std::string_view bytes) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadStructure(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos))) return false;
    const auto offset = static_cast<size_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER file{};
    IMAGE_OPTIONAL_HEADER64 optional{};
    if (!ReadStructure(bytes, offset, signature) || signature != IMAGE_NT_SIGNATURE ||
        !ReadStructure(bytes, offset + sizeof(signature), file) || file.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(file.Characteristics & IMAGE_FILE_DLL) || !(file.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        !file.NumberOfSections || file.NumberOfSections > 96 || file.SizeOfOptionalHeader < sizeof(optional) ||
        !ReadStructure(bytes, offset + sizeof(signature) + sizeof(file), optional) ||
        optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !optional.SizeOfImage ||
        !optional.SizeOfHeaders || optional.SizeOfHeaders > bytes.size()) return false;
    const size_t sections = offset + sizeof(signature) + sizeof(file) + file.SizeOfOptionalHeader;
    if (sections > bytes.size() || static_cast<size_t>(file.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > bytes.size() - sections ||
        sections + static_cast<size_t>(file.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > optional.SizeOfHeaders) return false;
    for (size_t i = 0; i < file.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!ReadStructure(bytes, sections + i * sizeof(section), section)) return false;
        if (section.SizeOfRawData && (section.PointerToRawData < optional.SizeOfHeaders ||
            section.PointerToRawData > bytes.size() || section.SizeOfRawData > bytes.size() - section.PointerToRawData)) return false;
        if (section.VirtualAddress >= optional.SizeOfImage || section.Misc.VirtualSize > optional.SizeOfImage - section.VirtualAddress) return false;
    }
    return true;
}
}

static bool OpenVerifiedFile(const std::wstring& path, std::string_view expected_sha256,
    HANDLE& verified_file, std::string& error, std::vector<HANDLE>* ancestor_locks, bool dll) {
    verified_file = INVALID_HANDLE_VALUE;
    error.clear();
    try {
        if (expected_sha256.size() != 64 || !std::all_of(expected_sha256.begin(), expected_sha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) return Fail(error, "Native image requires a lowercase SHA-256 digest.");
        const auto image_path = std::filesystem::path(path).lexically_normal();
        const auto drive = image_path.root_name().wstring();
        if (!image_path.is_absolute() || drive.size() != 2 || drive[1] != L':' ||
            image_path.relative_path().wstring().find(L':') != std::wstring::npos)
            return Fail(error, "Native image must use an absolute local path without alternate streams.");
        const UINT drive_type = GetDriveTypeW(image_path.root_path().c_str());
        if (drive_type != DRIVE_FIXED && drive_type != DRIVE_REMOVABLE && drive_type != DRIVE_RAMDISK)
            return Fail(error, "Native image requires a local drive.");
        // Keep ancestors stable until the final file is open and verified.
        std::vector<std::unique_ptr<Handle>> ancestors;
        for (auto parent = image_path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            auto directory = std::make_unique<Handle>();
            directory->value = CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            BY_HANDLE_FILE_INFORMATION information{};
            if (directory->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory->value, &information) ||
                !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return Fail(error, "Native image ancestors must be directories without reparse points.");
            ancestors.push_back(std::move(directory));
            if (parent == parent.root_path()) break;
        }
        Handle image;
        image.value = CreateFileW(image_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        BY_HANDLE_FILE_INFORMATION information{};
        LARGE_INTEGER size{};
        if (image.value == INVALID_HANDLE_VALUE || GetFileType(image.value) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(image.value, &information) ||
            (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(image.value, &size)) return Fail(error, "Cannot lock a regular, non-reparse native image.");
        if (size.QuadPart < 0 || (dll && size.QuadPart == 0) || static_cast<unsigned long long>(size.QuadPart) > kMaxNativeImageBytes)
            return Fail(error, "Native image must contain 1-67108864 bytes.");
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        size_t used = 0;
        while (used < bytes.size()) {
            DWORD count = 0;
            if (!ReadFile(image.value, bytes.data() + used, static_cast<DWORD>(bytes.size() - used), &count, nullptr) || !count)
                return Fail(error, "Cannot read the complete native image.");
            used += count;
        }
        if (PackageRevision(bytes) != expected_sha256) return Fail(error, "Native image SHA-256 does not match the manifest.");
        if (dll && !Amd64Dll(bytes)) return Fail(error, "Native image is not a complete AMD64 PE32+ DLL.");
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(image.value, start, nullptr, FILE_BEGIN)) return Fail(error, "Cannot rewind the verified native image.");
        if (ancestor_locks) {
            if (!ancestor_locks->empty()) return Fail(error, "Ancestor lock destination must be empty.");
            ancestor_locks->reserve(ancestors.size());
            for (auto& directory : ancestors) {
                ancestor_locks->push_back(directory->value);
                directory->value = INVALID_HANDLE_VALUE;
            }
        }
        verified_file = image.value;
        image.value = INVALID_HANDLE_VALUE;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
bool OpenVerifiedNativeImage(const std::wstring& path, std::string_view expected_sha256,
    HANDLE& verified_file, std::string& error, std::vector<HANDLE>* ancestor_locks) {
    return OpenVerifiedFile(path, expected_sha256, verified_file, error, ancestor_locks, true);
}
bool OpenVerifiedNativePackage(const std::wstring& entry_path, const NativeRuntime& native,
    std::vector<HANDLE>& locks, std::string& error) {
    if (!locks.empty()) return Fail(error, "Package lock destination must be empty.");
    std::vector<HANDLE> acquired;
    const auto release = [&] { for (HANDLE handle : acquired) CloseHandle(handle); acquired.clear(); };
    try {
        const auto parent = std::filesystem::path(entry_path).parent_path();
        std::vector<NativeFile> files{{native.library, native.sha256}};
        files.insert(files.end(), native.files.begin(), native.files.end());
        acquired.reserve(files.size() * 2 + 16);
        uint64_t total = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            const auto& file = files[i];
            const auto path = i == 0 ? std::filesystem::path(entry_path) : parent / file.path;
            auto extension = path.extension().wstring();
            for (auto& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            HANDLE handle = INVALID_HANDLE_VALUE;
            std::vector<HANDLE> ancestors;
            if (!OpenVerifiedFile(path.wstring(), file.sha256, handle, error, &ancestors, i == 0 || extension == L".dll")) {
                release(); return false;
            }
            // Reserve before transfer so allocation failure cannot leak acquired locks.
            try { acquired.reserve(acquired.size() + ancestors.size() + 1); }
            catch (...) { CloseHandle(handle); for (HANDLE item : ancestors) CloseHandle(item); throw; }
            acquired.push_back(handle);
            acquired.insert(acquired.end(), ancestors.begin(), ancestors.end());
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(handle, &size) || (total += static_cast<uint64_t>(size.QuadPart)) > 256ull * 1024 * 1024) {
                release(); return Fail(error, "Native package exceeds 256 MiB.");
            }
        }
        locks = std::move(acquired);
        return true;
    } catch (const std::exception& exception) {
        release(); error = exception.what(); return false;
    }
}

}
