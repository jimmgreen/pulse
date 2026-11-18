#include "setup_runtime_transaction.h"
#include "setup_common.h"
#include "setup_service.h"
#include <cstring>
#include <vector>
namespace pulse::setup {
namespace {
constexpr const wchar_t *keys[] = {L"Software\\Classes\\CLSID\\{B3C9ED45-7A51-48A2-9DF1-D46BA187A312}\\InprocServer32",
                                   L"Software\\Classes\\CLSID\\{B3C9ED45-7A51-48A2-9DF1-D46BA187A312}\\InprocServer32",
                                   L"Software\\Classes\\.pulse-start-pin",
                                   L"Software\\Classes\\Pulse.StartPin.Request\\shell\\PulseStartPin",
                                   L"Software\\Classes\\Pulse.StartPin.Request\\shell\\PulseStartPin\\command",
                                   L"Software\\Classes\\Pulse.StartPin.Request\\shell\\PulseStartPin\\command"};
constexpr const wchar_t *names[] = {L"", L"ThreadingModel", L"", L"", L"", L"DelegateExecute"};
struct ServiceHandle {
    SC_HANDLE h;
    ~ServiceHandle() {
        if (h)
            CloseServiceHandle(h);
    }
};
void Put(std::string &out, DWORD value) {
    out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}
bool Get(const std::string &in, size_t &at, DWORD &value) {
    if (at > in.size() || in.size() - at < 4)
        return false;
    std::memcpy(&value, in.data() + at, 4);
    at += 4;
    return true;
}
bool Fail(std::wstring &error, DWORD code) {
    error = L"Runtime registration transaction: " + Win32Error(code);
    return false;
}
} // namespace
bool RuntimeRegistrationTransaction::Prepare(std::wstring &error) {
    if (!real_)
        return true;
    if (api_.capture) {
        std::string captured;
        if (!api_.capture(captured, error))
            return false;
        HANDLE file = CreateFileW((app_ + L"\\.pulse-setup\\runtime-registration").c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return Fail(error, GetLastError());
        DWORD written = 0;
        const bool ok = WriteFile(file, captured.data(), static_cast<DWORD>(captured.size()), &written, nullptr) &&
                        written == captured.size() && FlushFileBuffers(file);
        const DWORD e = GetLastError();
        CloseHandle(file);
        return ok || Fail(error, e);
    }
    std::string data = "PRT2";
    ServiceHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.h)
        return Fail(error, GetLastError());
    ServiceHandle svc{OpenServiceW(scm.h, kServiceName, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS)};
    if (!svc.h && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        return Fail(error, GetLastError());
    Put(data, svc.h ? 1 : 0);
    if (svc.h) {
        DWORD needed = 0;
        QueryServiceConfigW(svc.h, nullptr, 0, &needed);
        if (!needed || needed > 65536)
            return Fail(error, ERROR_INVALID_DATA);
        std::vector<BYTE> buffer(needed);
        auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
        SERVICE_STATUS status{};
        if (!QueryServiceConfigW(svc.h, config, needed, &needed) || !QueryServiceStatus(svc.h, &status))
            return Fail(error, GetLastError());
        Put(data, config->dwStartType);
        Put(data, config->dwServiceType);
        Put(data, config->dwErrorControl);
        Put(data, status.dwCurrentState == SERVICE_RUNNING ? 1 : 0);
        const auto bytes = (wcslen(config->lpBinaryPathName) + 1) * sizeof(wchar_t);
        Put(data, static_cast<DWORD>(bytes));
        data.append(reinterpret_cast<const char *>(config->lpBinaryPathName), bytes);
        const auto display_bytes = (wcslen(config->lpDisplayName) + 1) * sizeof(wchar_t);
        Put(data, static_cast<DWORD>(display_bytes));
        data.append(reinterpret_cast<const char *>(config->lpDisplayName), display_bytes);
    }
    for (size_t i = 0; i < std::size(keys); ++i) {
        HKEY key = nullptr;
        auto r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
        DWORD type = 0, size = 0;
        std::vector<BYTE> bytes;
        if (r == ERROR_SUCCESS) {
            r = RegQueryValueExW(key, names[i], nullptr, &type, nullptr, &size);
            if (r == ERROR_SUCCESS && size <= 65536) {
                bytes.resize(size);
                r = RegQueryValueExW(key, names[i], nullptr, &type, bytes.data(), &size);
            } else if (size > 65536)
                r = ERROR_INVALID_DATA;
            RegCloseKey(key);
        }
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND && r != ERROR_PATH_NOT_FOUND)
            return Fail(error, r);
        Put(data, r == ERROR_SUCCESS ? 1 : 0);
        Put(data, type);
        Put(data, static_cast<DWORD>(bytes.size()));
        data.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    HANDLE file = CreateFileW((app_ + L"\\.pulse-setup\\runtime-registration").c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return Fail(error, GetLastError());
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size() && FlushFileBuffers(file);
    const DWORD e = GetLastError();
    CloseHandle(file);
    return ok || Fail(error, e);
}
bool RuntimeRegistrationTransaction::Recover(std::wstring &error) {
    if (!real_)
        return true;
    const auto path = app_ + L"\\.pulse-setup\\runtime-registration";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ||
               Fail(error, GetLastError());
    std::string data;
    if (!ReadWholeFile(path, data) || data.size() > 1024 * 1024 || !data.starts_with("PRT2"))
        return Fail(error, ERROR_INVALID_DATA);
    if (api_.restore)
        return api_.restore(data, error);
    size_t at = 4;
    DWORD existed = 0, start = 0, type = 0, control = 0, running = 0, size = 0;
    if (!Get(data, at, existed) || existed > 1)
        return Fail(error, ERROR_INVALID_DATA);
    std::wstring binary, display;
    if (existed) {
        if (!Get(data, at, start) || !Get(data, at, type) || !Get(data, at, control) || !Get(data, at, running) ||
            !Get(data, at, size) || size < 2 || size % 2 || size > 65536 || size > data.size() - at)
            return Fail(error, ERROR_INVALID_DATA);
        binary.resize(size / 2);
        std::memcpy(binary.data(), data.data() + at, size);
        at += size;
        if (binary.back() != 0)
            return Fail(error, ERROR_INVALID_DATA);
        binary.pop_back();
        if (!Get(data, at, size) || size < 2 || size % 2 || size > 65536 || size > data.size() - at)
            return Fail(error, ERROR_INVALID_DATA);
        display.resize(size / 2);
        std::memcpy(display.data(), data.data() + at, size);
        at += size;
        if (display.back() != 0)
            return Fail(error, ERROR_INVALID_DATA);
        display.pop_back();
    }
    struct Value {
        DWORD exists, type;
        std::vector<BYTE> bytes;
    };
    std::vector<Value> values;
    for (size_t i = 0; i < std::size(keys); ++i) {
        Value v{};
        if (!Get(data, at, v.exists) || v.exists > 1 || !Get(data, at, v.type) || !Get(data, at, size) ||
            size > 65536 || size > data.size() - at)
            return Fail(error, ERROR_INVALID_DATA);
        v.bytes.assign(data.begin() + at, data.begin() + at + size);
        at += size;
        values.push_back(std::move(v));
    }
    if (at != data.size())
        return Fail(error, ERROR_INVALID_DATA);
    ServiceHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.h)
        return Fail(error, GetLastError());
    ServiceHandle svc{OpenServiceW(
        scm.h, kServiceName, SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS)};
    if (existed) {
        if (!svc.h)
            return Fail(error, GetLastError());
        const DWORD stopped = StopService(kServiceName, 15000);
        if (stopped)
            return Fail(error, stopped);
        if (!ChangeServiceConfigW(svc.h, type, start, control, binary.c_str(), nullptr, nullptr, nullptr, nullptr,
                                  nullptr, display.c_str()))
            return Fail(error, GetLastError());
        if (running && !StartServiceW(svc.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
            return Fail(error, GetLastError());
    } else if (svc.h) {
        SERVICE_STATUS status{};
        ControlService(svc.h, SERVICE_CONTROL_STOP, &status);
        if (!DeleteService(svc.h) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
            return Fail(error, GetLastError());
    } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        return Fail(error, GetLastError());
    for (size_t i = 0; i < values.size(); ++i) {
        HKEY key = nullptr;
        const auto &v = values[i];
        auto r = v.exists ? RegCreateKeyExW(HKEY_LOCAL_MACHINE, keys[i], 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                                            nullptr, &key, nullptr)
                          : RegOpenKeyExW(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
        if (!v.exists && (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND))
            continue;
        if (r != ERROR_SUCCESS)
            return Fail(error, r);
        r = v.exists ? RegSetValueExW(key, names[i], 0, v.type, v.bytes.data(), static_cast<DWORD>(v.bytes.size()))
                     : RegDeleteValueW(key, names[i]);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND)
            return Fail(error, r);
    }
    return true;
}
} // namespace pulse::setup
