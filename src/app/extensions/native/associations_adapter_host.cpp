#include "associations_adapter_host.h"
#include "../../../common/app_runtime_paths.h"
#include "../../../common/localization.h"
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace pulse::extensions::native {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring WideUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), out.data(), n);
    return out;
}

std::wstring ResolvePulseExePath() {
    std::wstring stable = app_runtime_paths::StableEntryPath();
    if (!stable.empty()) return stable;
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n && n < buf.size() ? n : 0);
    return buf;
}

struct ScopedMtaCom {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    ~ScopedMtaCom() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

class Win32AssociationRegistryStore final : public AssociationRegistryStore {
public:
    std::optional<std::wstring> ReadString(const std::wstring& subkey, const wchar_t* value) override {
        DWORD type = 0;
        DWORD bytes = 0;
        const LONG probe = RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), value, RRF_RT_REG_SZ, &type, nullptr, &bytes);
        if (probe != ERROR_SUCCESS) return std::nullopt;
        if (bytes < sizeof(wchar_t)) return std::wstring{};
        std::wstring out(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), value, RRF_RT_REG_SZ, nullptr, out.data(), &bytes) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        while (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    std::optional<AssocRegBinary> ReadRaw(const std::wstring& subkey, const wchar_t* value) override {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return std::nullopt;
        DWORD type = 0;
        DWORD bytes = 0;
        const LONG probe = RegQueryValueExW(key, value, nullptr, &type, nullptr, &bytes);
        if (probe != ERROR_SUCCESS) {
            RegCloseKey(key);
            return std::nullopt;
        }
        AssocRegBinary out;
        out.type = type;
        out.data.resize(bytes);
        if (bytes > 0) {
            DWORD actual = bytes;
            if (RegQueryValueExW(key, value, nullptr, &type, out.data.data(), &actual) != ERROR_SUCCESS) {
                RegCloseKey(key);
                return std::nullopt;
            }
            out.data.resize(actual);
        }
        RegCloseKey(key);
        return out;
    }

    std::optional<uint32_t> ReadDword(const std::wstring& subkey, const wchar_t* value) override {
        DWORD out = 0;
        DWORD bytes = sizeof(out);
        if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), value, RRF_RT_REG_DWORD, nullptr, &out, &bytes) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(out);
    }

    bool SetString(const std::wstring& subkey, const wchar_t* value, const std::wstring& data) override {
        return RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), value, REG_SZ, data.c_str(),
                               static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }

    bool SetRaw(const std::wstring& subkey, const wchar_t* value, uint32_t type,
                const uint8_t* data, size_t size) override {
        return RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), value, type, data,
                               static_cast<DWORD>(size)) == ERROR_SUCCESS;
    }

    bool SetDword(const std::wstring& subkey, const wchar_t* value, uint32_t data) override {
        const DWORD v = data;
        return RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), value, REG_DWORD, &v, sizeof(v)) == ERROR_SUCCESS;
    }

    bool SetNone(const std::wstring& subkey, const wchar_t* value) override {
        return RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), value, REG_NONE, nullptr, 0) == ERROR_SUCCESS;
    }

    bool DeleteValue(const std::wstring& subkey, const wchar_t* value) override {
        const LONG res = RegDeleteKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), value);
        return res == ERROR_SUCCESS || res == ERROR_FILE_NOT_FOUND || res == ERROR_PATH_NOT_FOUND;
    }

    bool DeleteTree(const std::wstring& subkey) override {
        const LONG res = RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
        return res == ERROR_SUCCESS || res == ERROR_FILE_NOT_FOUND || res == ERROR_PATH_NOT_FOUND;
    }

    void DeleteKeyIfEmpty(const std::wstring& subkey) override {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0,
                          KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS) {
            return;
        }
        DWORD subkeys = 0;
        DWORD values = 0;
        const LONG info = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                                           &values, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(key);
        if (info == ERROR_SUCCESS && subkeys == 0 && values == 0) {
            RegDeleteKeyW(HKEY_CURRENT_USER, subkey.c_str());
        }
    }

    std::vector<std::wstring> EnumValueNames(const std::wstring& subkey) override {
        std::vector<std::wstring> names;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return names;
        }
        DWORD values = 0;
        DWORD max_name_len = 0;
        if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                             &values, &max_name_len, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
            values > 0) {
            std::wstring buf(static_cast<size_t>(max_name_len) + 2, L'\0');
            for (DWORD i = 0; i < values; ++i) {
                DWORD len = static_cast<DWORD>(buf.size());
                if (RegEnumValueW(key, i, buf.data(), &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
                    len > 0) {
                    names.emplace_back(buf.data(), len);
                }
            }
        }
        RegCloseKey(key);
        return names;
    }

    AssocDefaultResult QueryEffectiveDefault(const std::wstring& extension, std::wstring& out_progid) override {
        out_progid.clear();
        ComPtr<IApplicationAssociationRegistration> registration;
        HRESULT hr = CoCreateInstance(__uuidof(ApplicationAssociationRegistration), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(registration.GetAddressOf()));
        if (FAILED(hr)) return AssocDefaultResult::Error;
        PWSTR value = nullptr;
        hr = registration->QueryCurrentDefault(extension.c_str(), AT_FILEEXTENSION, AL_EFFECTIVE, &value);
        if (SUCCEEDED(hr) && value) {
            out_progid = value;
            CoTaskMemFree(value);
            return AssocDefaultResult::Ok;
        }
        CoTaskMemFree(value);
        if (hr == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) ||
            hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            return AssocDefaultResult::NoAssociation;
        }
        return AssocDefaultResult::Error;
    }

    std::wstring QueryFriendlyApp(const std::wstring& extension, const std::wstring& progid) override {
        wchar_t name[260]{};
        DWORD length = ARRAYSIZE(name);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME, extension.c_str(),
                                      nullptr, name, &length)) && name[0]) {
            return name;
        }
        return progid;
    }

    AssocRegHandlerState ProbeHandlerState(const std::wstring& progid) override {
        if (progid.empty()) return AssocRegHandlerState::Missing;
        HKEY key = nullptr;
        const LONG opened = RegOpenKeyExW(HKEY_CLASSES_ROOT, progid.c_str(), 0, KEY_READ, &key);
        if (opened == ERROR_FILE_NOT_FOUND) return AssocRegHandlerState::Missing;
        if (opened != ERROR_SUCCESS) return AssocRegHandlerState::Unknown;
        RegCloseKey(key);

        wchar_t executable[32768]{};
        DWORD length = ARRAYSIZE(executable);
        const HRESULT hr = AssocQueryStringW(ASSOCF_INIT_FIXED_PROGID, ASSOCSTR_EXECUTABLE,
                                             progid.c_str(), L"open", executable, &length);
        if (FAILED(hr) || !executable[0]) return AssocRegHandlerState::Unknown;
        if (_wcsicmp(PathFindFileNameW(executable), L"OpenWith.exe") == 0) return AssocRegHandlerState::Missing;
        const DWORD attrs = GetFileAttributesW(executable);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? AssocRegHandlerState::Unknown : AssocRegHandlerState::Available;
        }
        const DWORD err = GetLastError();
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                   ? AssocRegHandlerState::Missing
                   : AssocRegHandlerState::Unknown;
    }

    void NotifyAssocChanged() override {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    }
};

struct WorkerJob {
    uint64_t request_id = 0; // 0 = internal cleanup job
    std::function<bool()> cancelled;
    std::function<void()> run;
};

class AssociationsWorker {
public:
    ~AssociationsWorker() { Shutdown(); }

    void Enqueue(WorkerJob job) {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        if (!thread_.joinable()) {
            thread_ = std::thread([this] { Loop(); });
        }
        queue_.push_back(std::move(job));
        cv_.notify_one();
    }

    void Shutdown() {
        std::thread to_join;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            queue_.clear();
            to_join = std::move(thread_);
        }
        cv_.notify_all();
        if (to_join.joinable()) to_join.join();
    }

    void ResetForFutureUse() {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable()) stopping_ = false;
    }

private:
    void Loop() {
        ScopedMtaCom com;
        for (;;) {
            WorkerJob job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            if (job.cancelled && job.cancelled()) continue;
            if (job.run) {
                try {
                    job.run();
                } catch (...) {}
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<WorkerJob> queue_;
    std::thread thread_;
    bool stopping_ = false;
};

AssociationsWorker& SharedWorker() {
    static auto& worker = *new AssociationsWorker;
    return worker;
}

} // namespace

AssociationsBackend MakeAssociationsBackend(HWND owner, std::string package_id, std::wstring package_dir) {
    SharedWorker().ResetForFutureUse();
    auto cancelled_set = std::make_shared<std::set<uint64_t>>();
    auto cancel_mutex = std::make_shared<std::mutex>();
    const std::wstring app_desc = l10n::Get(l10n::StringId::AppDescription);

    AssociationsBackend backend;
    backend.valid_owner = [owner] { return IsWindow(owner) != FALSE; };
    backend.cancel = [cancelled_set, cancel_mutex](uint64_t id) {
        std::lock_guard lock(*cancel_mutex);
        cancelled_set->insert(id);
    };
    backend.query = [package_id, cancelled_set, cancel_mutex](
                        uint64_t id,
                        std::vector<std::pair<std::string, std::string>> handler_extensions,
                        std::function<void(AssociationsQueryReply)> complete) -> PulseStatus {
        WorkerJob job;
        job.request_id = id;
        job.cancelled = [cancelled_set, cancel_mutex, id] {
            std::lock_guard lock(*cancel_mutex);
            return cancelled_set->contains(id);
        };
        job.run = [package_id, handler_extensions = std::move(handler_extensions),
                   complete = std::move(complete), cancelled = job.cancelled,
                   cancelled_set, cancel_mutex, id]() {
            if (cancelled && cancelled()) return;
            Win32AssociationRegistryStore store;
            AssociationsQueryReply reply;
            reply.status = PULSE_OK;
            reply.items = QueryAssociations(store, package_id, handler_extensions);
            {
                std::lock_guard lock(*cancel_mutex);
                cancelled_set->erase(id);
            }
            if (complete) complete(std::move(reply));
        };
        SharedWorker().Enqueue(std::move(job));
        return PULSE_OK;
    };
    backend.apply = [package_id, package_dir, app_desc, cancelled_set, cancel_mutex](
                        uint64_t id,
                        std::vector<AssociationEntryRequest> entries,
                        std::function<void(PulseStatus)> complete) -> PulseStatus {
        WorkerJob job;
        job.request_id = id;
        job.cancelled = [cancelled_set, cancel_mutex, id] {
            std::lock_guard lock(*cancel_mutex);
            return cancelled_set->contains(id);
        };
        job.run = [package_id, package_dir, app_desc, entries = std::move(entries),
                   complete = std::move(complete), cancelled = job.cancelled,
                   cancelled_set, cancel_mutex, id]() {
            if (cancelled && cancelled()) return;
            Win32AssociationRegistryStore store;
            const std::wstring exe = ResolvePulseExePath();
            const bool ok = ApplyAssociations(store, package_id, exe, package_dir, app_desc, entries);
            {
                std::lock_guard lock(*cancel_mutex);
                cancelled_set->erase(id);
            }
            if (complete) complete(ok ? PULSE_OK : PULSE_ERROR_UNAVAILABLE);
        };
        SharedWorker().Enqueue(std::move(job));
        return PULSE_OK;
    };
    backend.remove = [package_id, cancelled_set, cancel_mutex](
                         uint64_t id,
                         std::vector<std::string> extensions,
                         std::function<void(PulseStatus)> complete) -> PulseStatus {
        WorkerJob job;
        job.request_id = id;
        job.cancelled = [cancelled_set, cancel_mutex, id] {
            std::lock_guard lock(*cancel_mutex);
            return cancelled_set->contains(id);
        };
        job.run = [package_id, extensions = std::move(extensions),
                   complete = std::move(complete), cancelled = job.cancelled,
                   cancelled_set, cancel_mutex, id]() {
            if (cancelled && cancelled()) return;
            Win32AssociationRegistryStore store;
            const bool ok = extensions.empty()
                                ? RemoveAllPackageAssociations(store, package_id)
                                : RemoveExtensions(store, package_id, extensions);
            {
                std::lock_guard lock(*cancel_mutex);
                cancelled_set->erase(id);
            }
            if (complete) complete(ok ? PULSE_OK : PULSE_ERROR_UNAVAILABLE);
        };
        SharedWorker().Enqueue(std::move(job));
        return PULSE_OK;
    };
    backend.open_chooser = [owner](std::string extension) -> PulseStatus {
        if (!IsWindow(owner)) return PULSE_ERROR_STATE;
        const std::wstring ext_w = WideUtf8(extension);
        if (ext_w.empty()) return PULSE_ERROR_ARGUMENT;
        const std::wstring sample = L"pulse_file" + ext_w;
        OPENASINFO info{sample.c_str(), nullptr, OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT};
        const HRESULT hr = SHOpenWithDialog(owner, &info);
        if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return PULSE_OK;
        return PULSE_ERROR_UNAVAILABLE;
    };
    backend.open_default_apps = [owner]() -> PulseStatus {
        if (!IsWindow(owner)) return PULSE_ERROR_STATE;
        const HINSTANCE res = ShellExecuteW(
            owner, L"open", L"ms-settings:defaultapps?registeredAppUser=Pulse", nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<intptr_t>(res) > 32 ? PULSE_OK : PULSE_ERROR_UNAVAILABLE;
    };
    return backend;
}

bool RemovePackageAssociationsOnHost(std::string_view package_id) {
    if (package_id.empty()) return false;
    ScopedMtaCom com;
    Win32AssociationRegistryStore store;
    return RemoveAllPackageAssociations(store, package_id);
}

void EnqueuePackageAssociationsCleanupOnHost(std::string_view package_id) {
    if (package_id.empty()) return;
    SharedWorker().ResetForFutureUse();
    WorkerJob job;
    std::string pkg(package_id);
    job.run = [pkg = std::move(pkg)]() {
        Win32AssociationRegistryStore store;
        RemoveAllPackageAssociations(store, pkg);
    };
    SharedWorker().Enqueue(std::move(job));
}

void ShutdownAssociationsHost() {
    SharedWorker().Shutdown();
}

} // namespace pulse::extensions::native
