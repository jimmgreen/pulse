// Standalone admin fixture. Includes implementation to exercise the exact private detach path.
#include "../app/drive_verbs.cpp"
#include <cstdio>
#include <shellapi.h>
#include <shlobj.h>
#include <fstream>
#include <filesystem>

int TestIso(const wchar_t* root, const wchar_t* nonce, bool probe_only = false) {
    using namespace pulse::app;
    if (GetDriveTypeW(root) != DRIVE_CDROM) return 30;
    const auto file = std::wstring(root) + nonce + L".txt";
    HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (held == INVALID_HANDLE_VALUE) return 31;
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root, volume, MAX_PATH)) { CloseHandle(held); return 32; }
    pulse::storage::VirtualDiskIdentity identity;
    const DWORD identity_error = pulse::storage::CaptureVirtualDiskIdentity(volume, identity);
    if (identity_error) { CloseHandle(held); std::printf("ISO identity error=%lu\n", identity_error); return 35; }
    if (probe_only) {
        for (int mode = 0; mode < 6; ++mode) {
            VIRTUAL_STORAGE_TYPE type{}; type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
            type.DeviceId = mode >= 3 ? VIRTUAL_STORAGE_TYPE_DEVICE_ISO : VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
            OPEN_VIRTUAL_DISK_PARAMETERS open{};
            const int version = mode % 3;
            open.Version = version == 0 ? OPEN_VIRTUAL_DISK_VERSION_1 : OPEN_VIRTUAL_DISK_VERSION_2;
            if (version != 0) { open.Version2.GetInfoOnly = version == 1; open.Version2.ReadOnly = TRUE; }
            HANDLE disk = INVALID_HANDLE_VALUE;
            const auto access = version == 0 ? static_cast<VIRTUAL_DISK_ACCESS_MASK>(VIRTUAL_DISK_ACCESS_GET_INFO | VIRTUAL_DISK_ACCESS_DETACH) : VIRTUAL_DISK_ACCESS_NONE;
            const DWORD opened = OpenVirtualDisk(&type, identity.image_path.c_str(), access, OPEN_VIRTUAL_DISK_FLAG_NONE, &open, &disk);
            std::printf("ISO API mode=%d open=%lu\n", mode, opened);
            if (!opened) {
                wchar_t physical[MAX_PATH]{}; ULONG bytes = sizeof(physical);
                std::printf("physical=%lu\n", GetVirtualDiskPhysicalPath(disk, &bytes, physical));
                GET_VIRTUAL_DISK_INFO info{}; info.Version = GET_VIRTUAL_DISK_INFO_IS_LOADED; ULONG size = sizeof(info);
                const DWORD loaded = GetVirtualDiskInformation(disk, &size, &info, nullptr);
                std::printf("loaded_query=%lu loaded=%d\n", loaded, info.IsLoaded);
                CloseHandle(disk);
            }
        }
        CloseHandle(held);
        return 0;
    }
    const DWORD occupied = EjectVirtualDiskWithAuthorization(identity, nullptr);
    CloseHandle(held);
    std::printf("ISO occupied eject result=%lu\n", occupied);
    if (occupied != ERROR_DEVICE_IN_USE) { pulse::ops::ShutdownElevatedTransferHelper(); return 33; }
    DWORD released = ERROR_DEVICE_IN_USE;
    for (int attempt = 0; attempt < 20 && released == ERROR_DEVICE_IN_USE; ++attempt) {
        released = EjectVirtualDiskWithAuthorization(identity, nullptr);
        if (released == ERROR_DEVICE_IN_USE) Sleep(100);
    }
    pulse::ops::ShutdownElevatedTransferHelper();
    std::printf("ISO released eject result=%lu\n", released);
    return released == ERROR_SUCCESS ? 0 : 34;
}

DWORD TestNativeShellEject(const std::wstring& root, const std::wstring& expected_volume) {
    const DWORD identity_error = pulse::app::CheckDriveEjectIdentity(root, expected_volume);
    if (identity_error) return identity_error;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return HRESULT_CODE(initialized);
    PIDLIST_ABSOLUTE pidl = nullptr;
    IShellFolder* folder = nullptr;
    IContextMenu* context = nullptr;
    PCUITEMID_CHILD child = nullptr;
    HMENU menu = nullptr;
    HRESULT hr = SHParseDisplayName(root.c_str(), nullptr, &pidl, 0, nullptr);
    if (SUCCEEDED(hr)) hr = SHBindToParent(pidl, IID_PPV_ARGS(&folder), &child);
    if (SUCCEEDED(hr)) hr = folder->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&context));
    if (SUCCEEDED(hr)) {
        menu = CreatePopupMenu();
        hr = menu ? context->QueryContextMenu(menu, 0, 1, 0x7fff, CMF_NORMAL) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        CMINVOKECOMMANDINFOEX invoke{sizeof(invoke)};
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI | CMIC_MASK_NOASYNC;
        invoke.lpVerb = "eject"; invoke.lpVerbW = L"eject"; invoke.nShow = SW_HIDE;
        hr = context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&invoke));
    }
    if (menu) DestroyMenu(menu);
    if (context) context->Release();
    if (folder) folder->Release();
    CoTaskMemFree(pidl); CoUninitialize();
    std::printf("Shell eject HRESULT=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
    for (int attempt = 0; attempt < 50; ++attempt) {
        wchar_t current[MAX_PATH]{};
        if (!GetVolumeNameForVolumeMountPointW(root.c_str(), current, MAX_PATH) || _wcsicmp(current, expected_volume.c_str()) != 0)
            return ERROR_SUCCESS;
        Sleep(100);
    }
    return ERROR_DEVICE_IN_USE;
}

int TestMedium(const wchar_t* manifest, bool native_shell = false) {
    using namespace pulse::app;
    std::wifstream input{std::filesystem::path(manifest)};
    std::wstring root, sibling, image;
    std::getline(input, root); std::getline(input, sibling); std::getline(input, image);
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    if (!image.starts_with(std::wstring(temp) + L"pulse-eject-isolated-") || !image.ends_with(L".vhdx")) return 40;
    std::wifstream marker{std::filesystem::path(root) / L"pulse-eject-fixture.txt"};
    std::wstring ownership; std::getline(marker, ownership);
    if (ownership != image) return 41;
    marker.close(); input.close();
    HANDLE token = nullptr; TOKEN_ELEVATION elevation{}; DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 42;
    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size); CloseHandle(token);
    if (elevation.TokenIsElevated) return 43;
    DriveEjectPlan plan;
    DWORD error = PrepareDriveEject(root, plan);
    if (error) { std::printf("[FAIL] medium prepare: %lu\n", error); return 44; }
    pulse::storage::VirtualDiskIdentity identity;
    error = pulse::storage::CaptureVirtualDiskIdentity(plan.volume_name, identity);
    if (error) { std::printf("[FAIL] medium capture: %lu\n", error); return 45; }
    BY_HANDLE_FILE_INFORMATION expected{}, actual{};
    auto file_id = [](const std::wstring& path, BY_HANDLE_FILE_INFORMATION& info) {
        HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const bool ok = GetFileInformationByHandle(file, &info) != FALSE; CloseHandle(file); return ok;
    };
    if (!file_id(image, expected) || !file_id(identity.image_path, actual) || expected.dwVolumeSerialNumber != actual.dwVolumeSerialNumber ||
        expected.nFileIndexHigh != actual.nFileIndexHigh || expected.nFileIndexLow != actual.nFileIndexLow) return 46;
    HANDLE held = CreateFileW((sibling + L"held.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (held == INVALID_HANDLE_VALUE) return 48;
    error = native_shell ? TestNativeShellEject(root, plan.volume_name) : pulse::storage::DetachVirtualDiskSafely(identity);
    if (error == ERROR_SUCCESS || GetDriveTypeW(root.c_str()) != DRIVE_FIXED || GetDriveTypeW(sibling.c_str()) != DRIVE_FIXED) {
        CloseHandle(held); std::printf("[FAIL] medium occupied eject: %lu\n", error); return 47;
    }
    std::puts("[PASS] medium token refuses occupied fixture through safe removal negotiation");
    if (!native_shell) {
    auto forged = identity; ++forged.index_low;
    std::atomic<bool> cancel{false};
    const auto rejected = pulse::ops::EjectWithElevatedHelper(forged, nullptr, cancel);
    if (rejected.hr != HRESULT_FROM_WIN32(ERROR_MEDIA_CHANGED) || rejected.mutated) {
        CloseHandle(held); pulse::ops::ShutdownElevatedTransferHelper(); return 51;
    }
    std::puts("[PASS] authorized helper rejects mismatched image file identity without mutation");
    }
    error = native_shell ? TestNativeShellEject(root, plan.volume_name) : EjectVirtualDiskWithAuthorization(identity, nullptr);
    CloseHandle(held);
    if (error == ERROR_SUCCESS || GetDriveTypeW(root.c_str()) != DRIVE_FIXED || GetDriveTypeW(sibling.c_str()) != DRIVE_FIXED) {
        std::printf("[FAIL] occupied sibling removal: %lu\n", error); pulse::ops::ShutdownElevatedTransferHelper(); return 49;
    }
    std::puts("[PASS] real elevated helper refuses open sibling partition");
    for (int attempt = 0; attempt < 20; ++attempt) {
        error = native_shell ? TestNativeShellEject(root, plan.volume_name) : EjectVirtualDiskWithAuthorization(identity, nullptr);
        if (error != ERROR_DEVICE_IN_USE) break;
        Sleep(100);
    }
    pulse::ops::ShutdownElevatedTransferHelper();
    if (error) {
        pulse::storage::EjectDiagnostics diagnostic;
        pulse::storage::DetachImage(plan.volume_name, identity.image_path, nullptr, &diagnostic);
        std::wprintf(L"[FAIL] released=%lu CR=%lu veto=%lu caps=%lu name=%ls\n", error, diagnostic.config_result, diagnostic.veto_type, diagnostic.capabilities, diagnostic.veto_name.c_str());
        return 50;
    }
    VIRTUAL_STORAGE_TYPE verify_type{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
    OPEN_VIRTUAL_DISK_PARAMETERS verify_open{}; verify_open.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    HANDLE verify_disk = INVALID_HANDLE_VALUE;
    error = OpenVirtualDisk(&verify_type, image.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO, OPEN_VIRTUAL_DISK_FLAG_NONE, &verify_open, &verify_disk);
    if (error) return 52;
    wchar_t physical[MAX_PATH]{}; ULONG bytes = sizeof(physical);
    const DWORD physical_result = GetVirtualDiskPhysicalPath(verify_disk, &bytes, physical);
    CloseHandle(verify_disk);
    if (physical_result == ERROR_SUCCESS) return 53;
    std::puts("[PASS] ordinary process completes safe VHDX eject and physical path is absent");
    return 0;
}

int TestExistingClassification(const wchar_t* manifest) {
    using namespace pulse::app;
    std::wifstream input{std::filesystem::path(manifest)};
    std::wstring root; std::getline(input, root);
    if (root.size() != 3 || root[1] != L':' || GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return 60;
    const auto extended = std::wstring(L"\\\\?\\") + root;
    const bool direct_query = QueryDriveEjectable(root, DRIVE_FIXED);
    const bool extended_query = QueryDriveEjectable(extended, DRIVE_FIXED);
    InvalidateDriveEjectableCache();
    RefreshDriveEjectable(extended, DRIVE_FIXED); // This PC/sidebar enumerate normalized paths.
    const bool menu = DriveEjectable(root, DRIVE_FIXED); // Selection uses DriveRootOf.
    const bool normalized = DriveEjectable(extended, DRIVE_FIXED);
    auto lower = root; if (lower[0] >= L'A' && lower[0] <= L'Z') lower[0] += L'a' - L'A';
    const bool lowercase = DriveEjectable(lower, DRIVE_FIXED);
    std::printf("direct=%d normalized=%d menu=%d normalized_cache=%d lowercase=%d\n",
                direct_query, extended_query, menu, normalized, lowercase);
    InvalidateDriveEjectableCache();
    const bool cleared = !DriveEjectable(root, DRIVE_FIXED) && !DriveEjectable(extended, DRIVE_FIXED);
    std::printf("invalidation=%d\n", cleared);
    return direct_query && extended_query && menu && normalized && lowercase && cleared ? 0 : 61;
}

int ProbeIso(const wchar_t* manifest) {
    using namespace pulse::app;
    std::ifstream input{std::filesystem::path(manifest)};
    const std::string json{std::istreambuf_iterator<char>(input), {}};
    auto text = [&](const char* name) {
        std::wstring value;
        const auto key = json.find(std::string("\"") + name + "\"");
        if (key == std::string::npos) return value;
        const auto colon = json.find(':', key);
        const auto quote = json.find('"', colon);
        if (quote == std::string::npos) return value;
        for (size_t i = quote + 1; i < json.size() && json[i] != '"'; ++i) {
            char c = json[i];
            if (c == '\\') { if (++i == json.size() || json[i] != '\\') return std::wstring{}; c = '\\'; }
            value += static_cast<unsigned char>(c);
        }
        return value;
    };
    const auto root = text("root"), marker = text("marker");
    if (root.size() != 3 || root[1] != L':' || !marker.starts_with(L"pulseiso") ||
        marker.find_first_not_of(L"pulseiso0123456789abcdef") != std::wstring::npos || GetDriveTypeW(root.c_str()) != DRIVE_CDROM) return 70;
    std::ifstream proof{std::filesystem::path(root) / (marker + L".txt")};
    std::string contents; std::getline(proof, contents); proof.close();
    if (!contents.empty() && contents.back() == '\r') contents.pop_back();
    if (std::wstring(contents.begin(), contents.end()) != marker) return 71;
    HANDLE token = nullptr; TOKEN_ELEVATION elevation{}; DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 72;
    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size); CloseHandle(token);
    std::printf("token_elevated=%lu\n", elevation.TokenIsElevated);
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume, MAX_PATH)) return 73;
    for (const auto& path : {root, std::wstring(volume)}) {
        const auto device = VolumeDevice(path);
        HANDLE h = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        const DWORD create_error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        std::wprintf(L"device=%ls create_error=%lu\n", device.c_str(), create_error);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD bytes = 0;
        const BOOL locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr);
        const DWORD lock_error = locked ? ERROR_SUCCESS : GetLastError();
        std::printf("lock_error=%lu\n", lock_error);
        if (locked) {
            const BOOL unlocked = DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr);
            std::printf("unlock_error=%lu\n", unlocked ? ERROR_SUCCESS : GetLastError());
        }
        CloseHandle(h);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if ((argc == 3 || argc == 4) && wcscmp(argv[1], L"--probe-iso") == 0) {
        if (argc == 4) { FILE* log = nullptr; _wfreopen_s(&log, argv[3], L"w", stdout); }
        return ProbeIso(argv[2]);
    }
    if (argc == 3 && wcscmp(argv[1], L"--classify-existing") == 0) return TestExistingClassification(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"--shell-medium") == 0) return TestMedium(argv[2], true);
    if (argc == 3 && wcscmp(argv[1], L"--medium") == 0) return TestMedium(argv[2]);
    if (argc == 4 && wcscmp(argv[1], L"--iso-api") == 0) return TestIso(argv[2], argv[3], true);
    if (argc == 4 && wcscmp(argv[1], L"--iso") == 0) return TestIso(argv[2], argv[3]);
    if (argc == 4 && wcscmp(argv[1], L"--serve") == 0) { FILE* log = nullptr; _wfreopen_s(&log, argv[3], L"w", stdout); }
    if (argc > 2 && !((argc == 3 || argc == 4) && wcscmp(argv[1], L"--serve") == 0)) return 64;
    if (argc == 2 && std::wstring_view(argv[1]).starts_with(L"--")) return 64;
    if (argc == 2) { FILE* log = nullptr; _wfreopen_s(&log, argv[1], L"w", stdout); }
    using namespace pulse::app;
    if (VolumeDevice(L"\\\\?\\Volume{test}\\") != L"\\\\?\\Volume{test}") return 20;
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const auto system_root = std::wstring(windows, 3);
    if (CheckDriveEjectIdentity(system_root, L"\\\\?\\Volume{unrelated}\\") != ERROR_MEDIA_CHANGED) return 22;
    std::puts("[PASS] mismatched volume identity rejects drive-letter reuse");
    std::vector<std::wstring> invalid_roots;
    if (GetDriveEjectRoots(L"not-a-volume", invalid_roots) == ERROR_SUCCESS) return 23;
    InvalidateDriveEjectableCache();
    if (DriveEjectable(L"Z:\\", DRIVE_FIXED) || !DriveEjectable(L"Z:\\", DRIVE_CDROM)) return 1;
    std::puts("[PASS] empty fixed-disk cache and optical classification");
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto base = std::wstring(temp) + L"pulse-eject-isolated-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto image = base + L".vhdx";
    const auto script = base + L".txt";
    struct Fixture {
        std::wstring image, script;
        HANDLE disk = INVALID_HANDLE_VALUE;
        bool created = false;
        ~Fixture() {
            if (!created) return;
            if (disk == INVALID_HANDLE_VALUE) {
                VIRTUAL_STORAGE_TYPE type{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
                OPEN_VIRTUAL_DISK_PARAMETERS open{}; open.Version = OPEN_VIRTUAL_DISK_VERSION_1;
                OpenVirtualDisk(&type, image.c_str(), VIRTUAL_DISK_ACCESS_DETACH, OPEN_VIRTUAL_DISK_FLAG_NONE, &open, &disk);
            }
            if (disk != INVALID_HANDLE_VALUE) {
                DetachVirtualDisk(disk, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
                CloseHandle(disk);
            }
            DeleteFileW(script.c_str());
            if (!DeleteFileW(image.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                std::printf("[FAIL] cleanup image: %lu\n", GetLastError());
        }
    } fixture{image, script};
    VIRTUAL_STORAGE_TYPE storage{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
    CREATE_VIRTUAL_DISK_PARAMETERS params{};
    params.Version = CREATE_VIRTUAL_DISK_VERSION_1;
    params.Version1.MaximumSize = 256ull * 1024 * 1024;
    params.Version1.SectorSizeInBytes = 512;
    DWORD error = CreateVirtualDisk(&storage, image.c_str(), VIRTUAL_DISK_ACCESS_ALL, nullptr,
        CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &params, nullptr, &fixture.disk);
    if (error) { std::printf("[BLOCKED] create: %lu\n", error); return 2; }
    fixture.created = true;
    ATTACH_VIRTUAL_DISK_PARAMETERS attach{};
    attach.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    error = AttachVirtualDisk(fixture.disk, nullptr, ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME, 0, &attach, nullptr);
    if (error) { std::printf("[BLOCKED] attach: %lu\n", error); return 2; }
    wchar_t physical[MAX_PATH]{};
    ULONG bytes = sizeof(physical);
    error = GetVirtualDiskPhysicalPath(fixture.disk, &bytes, physical);
    unsigned disk_number = 0;
    if (error || swscanf_s(physical, L"\\\\.\\PhysicalDrive%u", &disk_number) != 1) return 3;
    {
        std::ofstream out{std::filesystem::path(script)};
        out << "select disk " << disk_number << "\nconvert mbr\ncreate partition primary size=64\nformat fs=ntfs quick label=PulseEjectA\nassign\ncreate partition primary size=64\nformat fs=ntfs quick label=PulseEjectB\nassign\nexit\n";
    }
    wchar_t rechecked[MAX_PATH]{};
    bytes = sizeof(rechecked);
    if (GetVirtualDiskPhysicalPath(fixture.disk, &bytes, rechecked) || wcscmp(physical, rechecked)) return 21;
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const auto executable = std::wstring(system) + L"\\diskpart.exe";
    auto command = L"\"" + executable + L"\" /s \"" + script + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 4;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 0;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if (status) { std::printf("[FAIL] diskpart: %lu\n", status); return 5; }
    std::vector<std::wstring> roots;
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        std::wstring root{letter}; root += L":\\";
        std::wstring candidate;
        if (VirtualDiskImage(root, candidate) != ERROR_SUCCESS) continue;
        auto file_id = [](const std::wstring& path, BY_HANDLE_FILE_INFORMATION& info) {
            HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            const bool ok = GetFileInformationByHandle(file, &info) != FALSE;
            CloseHandle(file); return ok;
        };
        BY_HANDLE_FILE_INFORMATION expected{}, actual{};
        if (!file_id(image, expected) || !file_id(candidate, actual) ||
            expected.dwVolumeSerialNumber != actual.dwVolumeSerialNumber ||
            expected.nFileIndexHigh != actual.nFileIndexHigh || expected.nFileIndexLow != actual.nFileIndexLow) continue;
        HANDLE h = CreateFileW(VolumeDevice(root).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER number{};
        if (DeviceNumber(h, number) && number.DeviceNumber == disk_number) roots.push_back(root);
        CloseHandle(h);
    }
    if (roots.size() != 2) { std::printf("[FAIL] fixture root count: %zu\n", roots.size()); return 6; }
    RefreshDriveEjectable(roots[0], DRIVE_FIXED);
    if (!DriveEjectable(roots[0], DRIVE_FIXED)) return 7;
    InvalidateDriveEjectableCache();
    if (DriveEjectable(roots[0], DRIVE_FIXED)) return 24;
    RefreshDriveEjectable(roots[0], DRIVE_FIXED);
    std::vector<std::wstring> related;
    error = GetDriveEjectRoots(roots[0], related);
    if (error || related.size() != 2) { std::printf("[FAIL] roots: %lu / %zu\n", error, related.size()); return 8; }
    std::puts("[PASS] fixed VHDX recognition and both partition roots");
    CloseHandle(fixture.disk); fixture.disk = INVALID_HANDLE_VALUE;
    HANDLE held = CreateFileW((roots[1] + L"held.txt").c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, 0, nullptr);
    if (held == INVALID_HANDLE_VALUE) return 9;
    error = DetachImage(roots[0], image);
    CloseHandle(held);
    if (error != ERROR_DEVICE_IN_USE || GetDriveTypeW(roots[0].c_str()) != DRIVE_FIXED ||
        GetDriveTypeW(roots[1].c_str()) != DRIVE_FIXED) { std::printf("[FAIL] occupied sibling: %lu\n", error); return 10; }
    std::puts("[PASS] open sibling partition file prevents detach; both volumes remain");
#ifdef PULSE_VIRTUAL_DISK_FAULT_TEST
    wchar_t recovery_volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(roots[0].c_str(), recovery_volume, MAX_PATH)) return 82;
    pulse::storage::VirtualDiskIdentity recovery_identity;
    if (pulse::storage::CaptureVirtualDiskIdentity(recovery_volume, recovery_identity)) return 83;
    error = pulse::storage::TestDetachFailureRecovery(recovery_identity, ERROR_GEN_FAILURE);
    if (error != ERROR_GEN_FAILURE) { std::printf("[FAIL] injected detach failure: %lu\n", error); return 84; }
    bool restored = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        wchar_t restored_volume[MAX_PATH]{};
        restored = GetVolumeNameForVolumeMountPointW(roots[0].c_str(), restored_volume, MAX_PATH) &&
            _wcsicmp(restored_volume, recovery_volume) == 0 && GetDriveTypeW(roots[1].c_str()) == DRIVE_FIXED &&
            GetFileAttributesW((roots[1] + L"held.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (restored) {
            HANDLE first = CreateFileW(roots[0].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            HANDLE second = CreateFileW((roots[1] + L"held.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
            restored = first != INVALID_HANDLE_VALUE && second != INVALID_HANDLE_VALUE;
            if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
            if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
        }
        if (restored) break;
        Sleep(100);
    }
    if (!restored) { std::puts("[FAIL] failed detach left volume removed"); return 85; }
    std::puts("[PASS] injected post-query detach failure restores both original volumes");
#endif
    if ((argc == 3 || argc == 4) && wcscmp(argv[1], L"--serve") == 0) {
        { std::wofstream marker{std::filesystem::path(roots[0]) / L"pulse-eject-fixture.txt"}; marker << image << L'\n'; }
        { std::wofstream ready{std::filesystem::path(argv[2])}; ready << roots[0] << L'\n' << roots[1] << L'\n' << image << L'\n'; }
        const auto started = GetTickCount64();
        const auto done = std::wstring(argv[2]) + L".done";
        while (GetTickCount64() - started < 3600000 && GetFileAttributesW(done.c_str()) == INVALID_FILE_ATTRIBUTES) Sleep(100);
        DeleteFileW(argv[2]); DeleteFileW(done.c_str());
        return 0;
    }
    error = DetachImage(roots[0], image);
    if (error) { std::printf("[FAIL] unoccupied detach: %lu\n", error); return 12; }
    OPEN_VIRTUAL_DISK_PARAMETERS verify_open{}; verify_open.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    HANDLE verify_disk = INVALID_HANDLE_VALUE;
    error = OpenVirtualDisk(&storage, image.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO, OPEN_VIRTUAL_DISK_FLAG_NONE, &verify_open, &verify_disk);
    if (error) return 80;
    bytes = sizeof(physical);
    const DWORD physical_result = GetVirtualDiskPhysicalPath(verify_disk, &bytes, physical);
    CloseHandle(verify_disk);
    if (physical_result == ERROR_SUCCESS) { std::puts("[FAIL] successful eject left image attached"); return 81; }
    std::puts("[PASS] unoccupied multi-partition VHDX detaches and has no physical path");
    return 0;
}
