#include <thread>
#include <stdexcept>
#include "preview_handler_host.h"
#include "preview_resource_budget.h"
#include "preview_handler_pan.h"
#include "../common/preview_extensions.h"
#include "../common/file_placeholder.h"
#include "../common/path_utils.h"
#include "../common/runtime_log.h"
#include <dwmapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propsys.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pulse::ui {
#ifdef PULSE_PREVIEW_HANDLER_TESTING
IUnknown* (*g_preview_handler_factory_for_test)() = nullptr;
std::atomic<uint32_t> g_preview_handler_provider_for_test{0};
HRESULT g_preview_handler_activation_hr_for_test = E_FAIL;
#endif
namespace {

constexpr wchar_t kClassName[] = L"PulsePreviewHandlerHost";
constexpr UINT kOpenDelayMs = 100;
constexpr wchar_t kPreviewHandlerIid[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
// The built-in "Windows TXT Previewer". Text-perceived associations without a
// handler of their own - txtfile, PerceivedType text - resolve to it, and it
// draws the same plain text Pulse's own text view shows without starting a
// surrogate process first.
constexpr CLSID kWindowsTxtPreviewer = {
    0x1531d583, 0x8375, 0x4d3f, {0xb5, 0xfb, 0xd2, 0x3b, 0xbd, 0x16, 0x9f, 0x22}
};
constexpr CLSID kQueryAssociations = {
    0xa07034fd, 0x6caa, 0x4954, {0xac, 0x3f, 0x97, 0xa2, 0x72, 0x16, 0xf9, 0x8a}
};
constexpr HRESULT kServerExecFailure = static_cast<HRESULT>(0x80080005);

// One apartment hosts the system preview handler and owns the overlay window, so
// a provider that stays inside its open stalls the pane and every later preview.
// An apartment that has not started or finished what it was asked for inside
// this budget is retired: the selection falls back and the next one starts a
// fresh apartment, which keeps a cold provider from taking the pane down with it.
constexpr ULONGLONG kCommandBudgetMs = 3000;

// Slow is not the same as stuck. Opening a preview can mean starting a whole
// application - Office starts Excel the first time an .xlsx is previewed - which
// takes seconds, so the file the pane is still showing keeps its preview for
// that long instead of being retired into the "load failed" placeholder. Only a
// request the pane has moved on from, or a command the apartment never picked
// up, is cut short on the budget above.
constexpr ULONGLONG kSlowOpenBudgetMs = 20000;

// A provider that stalled is not asked again for a while; the thumbnail path
// answers instead, so the pane keeps showing something while the provider warms
// up. The cooldown grows while the same extension keeps stalling.
constexpr ULONGLONG kSlowProviderCooldownMs = 2 * 60 * 1000;
struct SlowProvider {
    ULONGLONG until = 0;
    uint32_t strikes = 0;
};
std::mutex g_slow_provider_mutex;
std::unordered_map<std::wstring, SlowProvider> g_slow_providers;
std::unordered_set<std::wstring> g_unavailable_providers;
std::mutex g_association_mutex;
std::unordered_map<std::wstring, CLSID> g_clsid_cache;
std::unordered_map<std::wstring, ULONGLONG> g_clsid_negative;

void ResetAssociationCache() {
    std::lock_guard<std::mutex> lock(g_association_mutex);
    g_clsid_cache.clear();
    g_clsid_negative.clear();
}

void EvictAssociationCache(const std::wstring& extension) {
    if (extension.empty()) return;
    std::lock_guard<std::mutex> lock(g_association_mutex);
    g_clsid_cache.erase(extension);
}

ULONGLONG CommandBudgetMs() {
#ifdef PULSE_PREVIEW_HANDLER_TESTING
    // Tests observe a retire without waiting for the production budget.
    wchar_t text[32]{};
    if (GetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", text,
                                ARRAYSIZE(text)) > 0) {
        const unsigned long long value = std::wcstoull(text, nullptr, 10);
        if (value > 0) return static_cast<ULONGLONG>(value);
    }
#endif
    return kCommandBudgetMs;
}

ULONGLONG SlowOpenBudgetMs() {
#ifdef PULSE_PREVIEW_HANDLER_TESTING
    // Tests observe the slow-open give-up without waiting for the real budget.
    wchar_t text[32]{};
    if (GetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", text,
                                ARRAYSIZE(text)) > 0) {
        const unsigned long long value = std::wcstoull(text, nullptr, 10);
        if (value > 0) return static_cast<ULONGLONG>(value);
    }
#endif
    return kSlowOpenBudgetMs;
}

void NoteStalledProvider(const std::wstring& extension) {
    if (extension.empty()) return;
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
    SlowProvider& provider = g_slow_providers[extension];
    provider.strikes = provider.until > now ? provider.strikes + 1 : 1;
    provider.until = now + kSlowProviderCooldownMs * provider.strikes;
}

bool ProviderCoolingDown(const std::wstring& extension) {
    if (extension.empty()) return false;
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
    const auto it = g_slow_providers.find(extension);
    if (it == g_slow_providers.end()) return false;
    if (it->second.until <= now) {
        g_slow_providers.erase(it);
        return false;
    }
    return true;
}

void ResetSlowProvidersForTest() {
    {
        std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
        g_slow_providers.clear();
        g_unavailable_providers.clear();
    }
    ResetAssociationCache();
}

void NoteUnavailableProvider(const std::wstring& extension) {
    if (extension.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
        g_unavailable_providers.insert(extension);
    }
    EvictAssociationCache(extension);
}

bool ProviderUnavailable(const std::wstring& extension) {
    if (extension.empty()) return false;
    std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
    return g_unavailable_providers.count(extension) != 0;
}

// A provider that answered - however long it took, and whether or not the pane
// still wanted the preview - is working, so it is not kept away from the pane:
// the open it was retired for can still come back, and the next selection should
// get the handler rather than the placeholder.
void ClearSlowProvider(const std::wstring& extension) {
    if (extension.empty()) return;
    std::lock_guard<std::mutex> lock(g_slow_provider_mutex);
    g_slow_providers.erase(extension);
    g_unavailable_providers.erase(extension);
}

// Provider HWNDs have no cross-thread Win32 owner. Their logical owner is
// tracked by STA window events; retirement only queues an asynchronous hide,
// so a provider blocked inside COM cannot hold up the UI or owner destruction.
void DetachOverlay(HWND overlay) {
    if (!overlay) return;
    SetWindowPos(overlay, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW |
                 SWP_ASYNCWINDOWPOS);

}

struct ClsidHash {
    size_t operator()(const CLSID& c) const noexcept {
        const auto* p = reinterpret_cast<const uint64_t*>(&c);
        return static_cast<size_t>(p[0] ^ p[1]);
    }
};
struct ClsidEq {
    bool operator()(const CLSID& a, const CLSID& b) const noexcept {
        return IsEqualCLSID(a, b);
    }
};

thread_local std::unordered_map<CLSID, ComPtr<IClassFactory>, ClsidHash, ClsidEq> g_factories_64;
thread_local std::unordered_map<CLSID, ComPtr<IClassFactory>, ClsidHash, ClsidEq> g_factories_32;
std::once_flag g_register_class_once;
#ifdef PULSE_PREVIEW_HANDLER_TESTING
std::atomic<uint32_t> g_test_open_attempts{0};
// PlaceOverlay entries and returns. They differ only while the window manager
// is inside the provider's window, which is what makes a preview trail its
// owner during a drag.
std::atomic<uint32_t> g_test_place_calls{0};
std::atomic<uint32_t> g_test_place_done{0};
#endif

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

std::wstring ExtensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return {};
    std::wstring extension = path.substr(dot);
    for (wchar_t& c : extension) c = static_cast<wchar_t>(std::towlower(c));
    return extension;
}

bool IsOneOf(std::wstring_view extension, std::initializer_list<std::wstring_view> values) {
    return std::find(values.begin(), values.end(), extension) != values.end();
}

bool IsNativePreviewExtension(std::wstring_view extension) {
    return pulse::preview::IsNativeExtension(extension);
}

// The shell's content type for an extension, when an installer recorded one -
// Git for Windows, for instance, marks .gitignore / .gitattributes as
// text/plain. Text content means the native text view already draws the file,
// so no COM handler is started for it.
bool ExtensionHasTextContentType(const std::wstring& extension) {
    static std::unordered_map<std::wstring, bool> cache;
    {
        std::lock_guard<std::mutex> lock(g_association_mutex);
        if (auto it = cache.find(extension); it != cache.end()) return it->second;
    }
    wchar_t content[64]{};
    DWORD chars = ARRAYSIZE(content);
    const bool text =
        SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_CONTENTTYPE,
                                    extension.c_str(), nullptr, content, &chars)) &&
        _wcsnicmp(content, L"text/", 5) == 0;
    {
        std::lock_guard<std::mutex> lock(g_association_mutex);
        cache[extension] = text;
    }
    return text;
}

std::wstring TrimWhitespace(std::wstring_view text) {
    size_t start = 0;
    while (start < text.size() && std::iswspace(text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::iswspace(text[end - 1])) --end;
    return std::wstring(text.substr(start, end - start));
}

std::wstring ExpandEnvString(std::wstring_view raw) {
    std::wstring value(raw);
    if (value.find(L'%') == std::wstring::npos) return value;
    const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed <= 1 || needed >= 32768) return value;
    std::wstring buf(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), buf.data(), needed);
    if (written == 0 || written > needed) return value;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

bool ReadRegStringValue(HKEY key, const wchar_t* value_name, std::wstring& out, bool* exists = nullptr) {
    out.clear();
    if (exists) *exists = false;
    if (!key) return false;
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return false;
    if (exists) *exists = true;
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD read_bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    status = RegQueryValueExW(key, value_name, nullptr, &type,
        reinterpret_cast<BYTE*>(buf.data()), &read_bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return false;
    size_t chars = read_bytes / sizeof(wchar_t);
    if (chars > buf.size()) chars = buf.size();
    buf.resize(chars);
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    out = TrimWhitespace(buf);
    return !out.empty();
}

bool ReadSubkeyStringValue(HKEY parent, REGSAM view, const wchar_t* subkey,
                           const wchar_t* value_name, std::wstring& out) {
    out.clear();
    HKEY child = nullptr;
    if (RegOpenKeyExW(parent, subkey, 0, view, &child) != ERROR_SUCCESS)
        return false;
    const bool ok = ReadRegStringValue(child, value_name, out);
    RegCloseKey(child);
    return ok;
}

bool FileExistsNoDir(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ServerBinaryExists(const wchar_t* raw, bool is_exe) {
    if (!raw) return false;
    const std::wstring trimmed = TrimWhitespace(raw);
    if (trimmed.empty()) return false;
    const std::wstring expanded = ExpandEnvString(trimmed);
    if (expanded.empty()) return false;

    auto exists_or_search = [&](const std::wstring& candidate) -> bool {
        if (candidate.empty()) return false;
        if (FileExistsNoDir(candidate)) return true;
        const bool bare_name = candidate.find(L'\\') == std::wstring::npos &&
                               candidate.find(L'/') == std::wstring::npos;
        if (!bare_name) return false;
        return SearchPathW(nullptr, candidate.c_str(), is_exe ? L".exe" : L".dll",
                           0, nullptr, nullptr) > 0;
    };

    if (FileExistsNoDir(expanded)) return true;
    if (expanded.front() == L'"') {
        const size_t close = expanded.find(L'"', 1);
        if (close == std::wstring::npos || close <= 1) return false;
        return exists_or_search(expanded.substr(1, close - 1));
    }
    for (size_t pos = expanded.find(L' '); pos != std::wstring::npos; pos = expanded.find(L' ', pos + 1)) {
        if (exists_or_search(expanded.substr(0, pos))) return true;
    }
    return expanded.find(L' ') == std::wstring::npos && exists_or_search(expanded);
}

bool IsAppIdSurrogateActivatable(const wchar_t* appid_raw, REGSAM clsid_view) {
    if (!appid_raw) return false;
    const std::wstring trimmed = TrimWhitespace(appid_raw);
    if (trimmed.empty()) return false;
    CLSID appid{};
    if (FAILED(CLSIDFromString(trimmed.c_str(), &appid))) return false;
    wchar_t guid[64]{};
    if (StringFromGUID2(appid, guid, static_cast<int>(ARRAYSIZE(guid))) <= 0) return false;
    const std::wstring appid_path = std::wstring(L"AppID\\") + guid;
    const REGSAM views[] = {clsid_view, KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY};
    for (size_t idx = 0; idx < ARRAYSIZE(views); ++idx) {
        if (idx > 0 && views[idx] == clsid_view) continue;
        HKEY appid_key = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, appid_path.c_str(), 0, views[idx], &appid_key) != ERROR_SUCCESS)
            continue;
        bool activatable = false;
        std::wstring local_service;
        if (ReadRegStringValue(appid_key, L"LocalService", local_service)) {
            activatable = true;
        } else {
            bool surrogate_exists = false;
            std::wstring surrogate;
            (void)ReadRegStringValue(appid_key, L"DllSurrogate", surrogate, &surrogate_exists);
            if (surrogate_exists) {
                if (surrogate.empty()) {
                    activatable = true;
                } else if (ServerBinaryExists(surrogate.c_str(), true)) {
                    activatable = true;
#ifdef _WIN64
                    if ((clsid_view & KEY_WOW64_32KEY) != 0) {
                        const std::wstring expanded_sur = ExpandEnvString(surrogate);
                        std::wstring lower_sur;
                        lower_sur.reserve(expanded_sur.size());
                        for (wchar_t ch : expanded_sur) {
                            lower_sur.push_back(ch == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(ch)));
                        }
                        constexpr std::wstring_view kSys32Prevhost = L"\\system32\\prevhost.exe";
                        if (lower_sur.size() >= kSys32Prevhost.size() &&
                            lower_sur.compare(lower_sur.size() - kSys32Prevhost.size(),
                                              kSys32Prevhost.size(), kSys32Prevhost) == 0) {
                            activatable = false;
                        }
                    }
#endif
                }
            }
        }
        RegCloseKey(appid_key);
        if (activatable) return true;
    }
    return false;
}

bool IsPreviewHandlerClsidActivatable(const CLSID& clsid, int depth = 0) {
    wchar_t guid[64]{};
    if (StringFromGUID2(clsid, guid, static_cast<int>(ARRAYSIZE(guid))) <= 0)
        return false;
    const std::wstring packaged_key = std::wstring(L"PackagedCom\\ClassIndex\\") + guid;
    HKEY packaged = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, packaged_key.c_str(), 0, KEY_READ, &packaged) == ERROR_SUCCESS) {
        RegCloseKey(packaged);
        return true;
    }
    const std::wstring clsid_path = std::wstring(L"CLSID\\") + guid;
    const REGSAM views[] = {KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY};
    for (const REGSAM view : views) {
        HKEY clsid_key = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, clsid_path.c_str(), 0, view, &clsid_key) != ERROR_SUCCESS)
            continue;
        bool activatable = false;
        std::wstring server_exec;
        std::wstring local_server;
        if ((ReadSubkeyStringValue(clsid_key, view, L"LocalServer32", L"ServerExecutable", server_exec) &&
             ServerBinaryExists(server_exec.c_str(), true)) ||
            (ReadSubkeyStringValue(clsid_key, view, L"LocalServer32", nullptr, local_server) &&
             ServerBinaryExists(local_server.c_str(), true))) {
            activatable = true;
        } else {
            std::wstring appid;
            std::wstring inproc;
            if (ReadRegStringValue(clsid_key, L"AppID", appid) &&
                ReadSubkeyStringValue(clsid_key, view, L"InprocServer32", nullptr, inproc) &&
                ServerBinaryExists(inproc.c_str(), false) &&
                IsAppIdSurrogateActivatable(appid.c_str(), view)) {
                activatable = true;
            } else if (depth == 0) {
                std::wstring treat_as;
                CLSID treated{};
                if (ReadSubkeyStringValue(clsid_key, view, L"TreatAs", nullptr, treat_as) &&
                    SUCCEEDED(CLSIDFromString(treat_as.c_str(), &treated)) &&
                    !IsEqualCLSID(treated, clsid) &&
                    IsPreviewHandlerClsidActivatable(treated, 1)) {
                    activatable = true;
                }
            }
        }
        RegCloseKey(clsid_key);
        if (activatable) return true;
    }
    return false;
}

DWORD ActivationContextForBit(bool activate_32) noexcept {
#ifdef _WIN64
    return activate_32 ? (CLSCTX_LOCAL_SERVER | CLSCTX_ACTIVATE_32_BIT_SERVER) : CLSCTX_LOCAL_SERVER;
#else
    (void)activate_32;
    return CLSCTX_LOCAL_SERVER;
#endif
}

HRESULT QueryIsolatedClassObject(const CLSID& clsid, bool activate_32, ComPtr<IClassFactory>& factory) {
    factory.Reset();
    return CoGetClassObject(clsid, ActivationContextForBit(activate_32), nullptr, IID_PPV_ARGS(&factory));
}

bool FindPreviewHandlerClsid(const std::wstring& extension, CLSID& clsid) {
#ifdef PULSE_PREVIEW_HANDLER_TESTING
    if (g_preview_handler_factory_for_test) { clsid = GUID_NULL; clsid.Data1 = g_preview_handler_provider_for_test.load(); return true; }
#endif
    // A lookup that found nothing is remembered, but only for a while: the shell
    // can fail this query while it is busy with something else, and carrying
    // that answer for the whole session leaves the type unpreviewable until the
    // application restarts.
    constexpr ULONGLONG kNegativeTtlMs = 30 * 1000;
    {
        std::lock_guard<std::mutex> lock(g_association_mutex);
        if (auto it = g_clsid_cache.find(extension); it != g_clsid_cache.end()) {
            clsid = it->second;
            return true;
        }
        if (auto it = g_clsid_negative.find(extension); it != g_clsid_negative.end()) {
            if (GetTickCount64() - it->second < kNegativeTtlMs) return false;
            g_clsid_negative.erase(it);
        }
    }

    // The shell lookup runs outside the lock. This is reached from the paint
    // path too, and a lookup queued behind another thread's slow call used to
    // freeze the window along with it. Resolving the same extension twice is
    // harmless.
    std::vector<CLSID> candidates;
    auto push_candidate = [&](const CLSID& c) {
        if (IsEqualCLSID(c, GUID_NULL)) return;
        for (const auto& existing : candidates) {
            if (IsEqualCLSID(existing, c)) return;
        }
        candidates.push_back(c);
    };
    auto try_parse_and_push = [&](const std::wstring& text) {
        if (text.empty()) return;
        CLSID parsed{};
        if (SUCCEEDED(CLSIDFromString(text.c_str(), &parsed))) push_candidate(parsed);
    };
    auto query_shellex_from_key = [&](HKEY root, REGSAM view, const std::wstring& base_subkey) {
        if (base_subkey.empty()) return;
        const std::wstring subkey = base_subkey + L"\\ShellEx\\" + kPreviewHandlerIid;
        std::wstring guid_text;
        if (ReadSubkeyStringValue(root, view, subkey.c_str(), nullptr, guid_text)) {
            try_parse_and_push(guid_text);
        }
    };
    auto query_progid_handlers = [&](const std::wstring& progid) {
        if (progid.empty()) return;
        for (const REGSAM view : {KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY}) {
            query_shellex_from_key(HKEY_CLASSES_ROOT, view, progid);
            std::wstring curver;
            if (ReadSubkeyStringValue(HKEY_CLASSES_ROOT, view, (progid + L"\\CurVer").c_str(), nullptr, curver) &&
                curver != progid) {
                query_shellex_from_key(HKEY_CLASSES_ROOT, view, curver);
            }
        }
    };

    {
        wchar_t guid[64]{};
        DWORD chars = ARRAYSIZE(guid);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_SHELLEXTENSION,
                                      extension.c_str(), kPreviewHandlerIid, guid, &chars))) {
            try_parse_and_push(TrimWhitespace(guid));
        }
    }
    {
        std::wstring user_progid;
        const std::wstring user_choice_key =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + extension + L"\\UserChoice";
        if (ReadSubkeyStringValue(HKEY_CURRENT_USER, KEY_READ, user_choice_key.c_str(), L"ProgId", user_progid)) {
            query_progid_handlers(user_progid);
        }
    }
    {
        ComPtr<IQueryAssociations> assoc;
        if (SUCCEEDED(AssocCreate(kQueryAssociations, IID_PPV_ARGS(&assoc))) &&
            SUCCEEDED(assoc->Init(ASSOCF_INIT_DEFAULTTOSTAR, extension.c_str(), nullptr, nullptr))) {
            wchar_t guid[64]{};
            DWORD chars = ARRAYSIZE(guid);
            if (SUCCEEDED(assoc->GetString(ASSOCF_NOTRUNCATE, ASSOCSTR_SHELLEXTENSION,
                                           kPreviewHandlerIid, guid, &chars))) {
                try_parse_and_push(TrimWhitespace(guid));
            }
        }
    }
    for (const REGSAM view : {KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY}) {
        query_shellex_from_key(HKEY_CLASSES_ROOT, view, extension);
        std::wstring default_progid;
        if (ReadSubkeyStringValue(HKEY_CLASSES_ROOT, view, extension.c_str(), nullptr, default_progid)) {
            query_progid_handlers(default_progid);
        }
    }

    // The TXT previewer means the native text path is the faster preview of the
    // same content, so its extensions are not handed to a COM server at all.
    CLSID resolved{};
    bool activatable = false;
    if (candidates.empty() || !IsEqualCLSID(candidates.front(), kWindowsTxtPreviewer)) {
        for (const CLSID& candidate : candidates) {
            if (IsEqualCLSID(candidate, kWindowsTxtPreviewer)) continue;
            if (IsPreviewHandlerClsidActivatable(candidate)) {
                resolved = candidate;
                activatable = true;
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_association_mutex);
        if (activatable) {
            g_clsid_cache[extension] = resolved;
            g_clsid_negative.erase(extension);
        } else {
            g_clsid_negative[extension] = GetTickCount64();
        }
    }
    if (!activatable) {
        return false;
    }
    clsid = resolved;
    return true;
}

ComPtr<IClassFactory> FactoryFor(const CLSID& clsid, bool activate_32, HRESULT& hr_out) {
    auto& factories = activate_32 ? g_factories_32 : g_factories_64;
    if (auto it = factories.find(clsid); it != factories.end()) {
        hr_out = S_OK;
        return it->second;
    }
    ComPtr<IClassFactory> factory;
    hr_out = QueryIsolatedClassObject(clsid, activate_32, factory);
    if (FAILED(hr_out) || !factory) return {};
    factories[clsid] = factory;
    return factory;
}

void EvictFactory(const CLSID& clsid, bool activate_32) {
    auto& factories = activate_32 ? g_factories_32 : g_factories_64;
    factories.erase(clsid);
}

ComPtr<IUnknown> CreateHandler(const CLSID& clsid, HRESULT& result) {
#ifdef PULSE_PREVIEW_HANDLER_TESTING
    if (g_preview_handler_factory_for_test) {
        ComPtr<IUnknown> handler;
        handler.Attach(g_preview_handler_factory_for_test());
        result = handler ? S_OK : g_preview_handler_activation_hr_for_test;
        return handler;
    }
#endif
    HRESULT best_hr = E_FAIL;
    bool has_best_hr = false;
    auto record_hr = [&](HRESULT hr) {
        if (!has_best_hr) {
            best_hr = hr;
            has_best_hr = true;
            return;
        }
        if (best_hr == REGDB_E_CLASSNOTREG && hr != REGDB_E_CLASSNOTREG) {
            best_hr = hr;
        }
    };
    auto try_context = [&](bool activate_32) -> ComPtr<IUnknown> {
        const DWORD ctx = ActivationContextForBit(activate_32);
        for (int attempt = 0; attempt < 2; ++attempt) {
            HRESULT factory_hr = S_OK;
            ComPtr<IClassFactory> factory = FactoryFor(clsid, activate_32, factory_hr);
            if (factory) {
                ComPtr<IUnknown> unknown;
                const HRESULT hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&unknown));
                record_hr(hr);
                if (SUCCEEDED(hr) && unknown) return unknown;
                EvictFactory(clsid, activate_32);
                if (hr != kServerExecFailure && attempt == 0) break;
                continue;
            }
            record_hr(factory_hr);
            ComPtr<IUnknown> unknown;
            const HRESULT hr = CoCreateInstance(clsid, nullptr, ctx, IID_PPV_ARGS(&unknown));
            record_hr(hr);
            if (SUCCEEDED(hr) && unknown) return unknown;
            if (hr != kServerExecFailure) break;
        }
        return {};
    };

    if (ComPtr<IUnknown> unknown = try_context(false)) {
        result = S_OK;
        return unknown;
    }
#ifdef _WIN64
    if (ComPtr<IUnknown> unknown = try_context(true)) {
        result = S_OK;
        return unknown;
    }
#endif
    result = has_best_hr ? best_hr : E_FAIL;
    return {};
}

class PreviewFrame final : public IPreviewHandlerFrame {
public:
    explicit PreviewFrame(HWND hwnd) : hwnd_(hwnd) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPreviewHandlerFrame) {
            *ppv = static_cast<IPreviewHandlerFrame*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG refs = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (refs == 0) delete this;
        return refs;
    }
    IFACEMETHODIMP GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override {
        if (!info) return E_POINTER;
        info->haccel = nullptr;
        info->cAccelEntries = 0;
        return S_OK;
    }
    IFACEMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }

private:
    LONG refs_ = 1;
    HWND hwnd_ = nullptr;
};

HRESULT InitWithStream(IUnknown* handler, const std::wstring& path, IUnknown** kept_stream) {
    ComPtr<IInitializeWithStream> init;
    HRESULT hr = handler->QueryInterface(IID_PPV_ARGS(&init));
    if (FAILED(hr)) return hr;
    ComPtr<IStream> stream;
    hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE,
                                nullptr, &stream);
    if (FAILED(hr)) return hr;
    hr = init->Initialize(stream.Get(), STGM_READ);
    if (hr == E_NOTIMPL) return hr;
    if (FAILED(hr)) return hr;
    *kept_stream = stream.Detach();
    return S_OK;
}

HRESULT InitWithItem(IUnknown* handler, const std::wstring& path) {
    ComPtr<IInitializeWithItem> init;
    HRESULT hr = handler->QueryInterface(IID_PPV_ARGS(&init));
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem> item;
    hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    hr = init->Initialize(item.Get(), STGM_READ);
    return hr;
}

HRESULT InitWithFile(IUnknown* handler, const std::wstring& path) {
    ComPtr<IInitializeWithFile> init;
    HRESULT hr = handler->QueryInterface(IID_PPV_ARGS(&init));
    if (FAILED(hr)) return hr;
    hr = init->Initialize(path.c_str(), STGM_READ);
    return hr;
}

void RegisterClassOnce() {
    std::call_once(g_register_class_once, [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = PreviewHandlerHost::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    });
}

} // namespace

bool PreviewHandlerHost::CanHost(const std::wstring& path) {
    const std::wstring extension = ExtensionOf(path);
    if (extension.empty() || IsNativePreviewExtension(extension)) return false;
    // Plain text - the shell's own content type, or the built-in TXT previewer
    // below - renders instantly in the native text view; a handler process is
    // only started for previews the pane cannot draw itself.
    if (ExtensionHasTextContentType(extension)) return false;
    // A provider that just stalled or failed activation as unregistered/missing is
    // left alone so the caller falls back to the native/thumbnail path.
    if (ProviderCoolingDown(extension) || ProviderUnavailable(extension)) return false;
    CLSID clsid{};
    return FindPreviewHandlerClsid(extension, clsid);
}

struct PreviewHandlerHost::WorkerState {
    struct Command {
        HWND owner = nullptr;
        HWND notify = nullptr;
        RECT bounds{};
        std::wstring path;
        std::wstring identity;
        DWORD attrs = 0;
        bool enabled = false;
        bool app_active = true;
        bool covered = false;
        bool immediate = false;
    };

    ~WorkerState() {
        if (thread) CloseHandle(thread);
        if (wake) CloseHandle(wake);
    }

    static inline thread_local WorkerState* event_target = nullptr;

    static void CALLBACK OwnerEvent(HWINEVENTHOOK, DWORD, HWND window, LONG object,
                                    LONG child, DWORD, DWORD) {
        auto* self = event_target;
        if (!self || !window || object != OBJID_WINDOW || child != CHILDID_SELF) return;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            if (self->stop || !self->command.enabled) return;
        }
        if (window == self->owner || window == self->owner_root || IsChild(window, self->owner))
            self->PlaceOverlay();
    }

    bool EnsureWindow() {
        if (hwnd && IsWindow(hwnd)) return true;
        hwnd = nullptr;
        surface_cloaked = false;
        overlay.store(nullptr, std::memory_order_release);
        if (!owner || !IsWindow(owner)) return false;
        RegisterClassOnce();
        placed_clip = {LONG_MIN, LONG_MIN, LONG_MIN, LONG_MIN};
        owner_root = GetAncestor(owner, GA_ROOT);
        embedded = (GetWindowLongPtrW(owner, GWL_STYLE) & WS_CHILD) != 0;
        // A UI-thread HWND owner makes owner destruction and owner changes
        // wait for a blocked provider STA. Keep this tool window independent.
        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"",
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        // Published for the owner, which may need to hide it while this thread
        // is inside a provider call. Cleared on WM_DESTROY so it cannot go stale.
        overlay.store(hwnd, std::memory_order_release);
        return hwnd != nullptr;
    }

    bool OverlayOwnsForeground() const {
        HWND foreground = GetForegroundWindow();
        if (!foreground || !hwnd) return false;
        if (foreground == hwnd || IsChild(hwnd, foreground) ||
            GetAncestor(foreground, GA_ROOT) == hwnd) return true;
        for (HWND walk = foreground; walk; walk = GetWindow(walk, GW_OWNER)) {
            if (walk == hwnd) return true;
        }
        return false;
    }

    void SetPresented(bool visible) {
        if (presented.exchange(visible, std::memory_order_acq_rel) != visible && notify)
            InvalidateRect(notify, nullptr, FALSE);
    }

    void HideWindow() {
        pan.Disable();
        if (hwnd) {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        }
        SetPresented(false);
        shown = false;
        placed_x = INT_MIN;
        placed_y = INT_MIN;
    }

    void EnablePan() {
        const auto extension = ExtensionOf(path);
        if (extension == L".xlsx" || extension == L".xls" || extension == L".xlsm" ||
            extension == L".xlsb" || extension == L".csv" || extension == L".ods") {
            pan.Disable();
            return;
        }
        pan.Enable(hwnd);
    }

    bool SetSurfaceCloak(bool cloaked) {
        if (surface_cloaked == cloaked) return true;
        const BOOL value = cloaked ? TRUE : FALSE;
        if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value))))
            return false;
        surface_cloaked = cloaked;
        return true;
    }

    void PlaceOverlay() {
        // A provider can pump owner events while inside DoPreview. Keep the
        // staging surface stable and off screen until that call finishes.
        if (initializing_surface) return;
        if (!hwnd || !owner) {
            SetPresented(false);
            return;
        }
        if (!IsWindow(owner) || !IsWindowVisible(owner) || IsIconic(owner_root)) {
            HideWindow();
            return;
        }
        // Embedded panes do not receive WM_MOVE when an ancestor moves. Keep
        // provider windows on the STA and follow ancestor window events.
        if (handler && state.load(std::memory_order_acquire) == State::Shown) shown = true;
#ifdef PULSE_PREVIEW_HANDLER_TESTING
        g_test_place_calls.fetch_add(1, std::memory_order_relaxed);
        struct PlaceDone {
            ~PlaceDone() { g_test_place_done.fetch_add(1, std::memory_order_relaxed); }
        } place_done;
#endif
        POINT origin{bounds.left, bounds.top};
        if (!ClientToScreen(owner, &origin)) {
            HideWindow();
            return;
        }
        const int width = std::max(1L, bounds.right - bounds.left);
        const int height = std::max(1L, bounds.bottom - bounds.top);
        if (embedded) {
            RECT visible{origin.x, origin.y, origin.x + width, origin.y + height};
            for (HWND ancestor = owner; ancestor; ancestor = GetParent(ancestor)) {
                RECT client{};
                GetClientRect(ancestor, &client);
                MapWindowPoints(ancestor, nullptr, reinterpret_cast<POINT*>(&client), 2);
                IntersectRect(&visible, &visible, &client);
                if (!(GetWindowLongPtrW(ancestor, GWL_STYLE) & WS_CHILD)) break;
            }
            if (IsRectEmpty(&visible)) {
                HideWindow();
                return;
            }
            OffsetRect(&visible, -origin.x, -origin.y);
            if (!EqualRect(&visible, &placed_clip)) {
                HRGN region = CreateRectRgnIndirect(&visible);
                if (region) {
                    if (SetWindowRgn(hwnd, region, TRUE)) placed_clip = visible;
                    else DeleteObject(region);
                }
            }
        }
        // Our own popups (the toolbar picker) count as foreground we own, so a
        // topmost overlay would sit on top of them: covered hides it outright.
        if (shown && (covered || (!app_active && !OverlayOwnsForeground()))) {
            pan.Disable();
            SetWindowPos(hwnd, HWND_NOTOPMOST, origin.x, origin.y, width, height,
                         SWP_NOACTIVATE | SWP_HIDEWINDOW);
            SetPresented(false);
            return;
        }
        uint64_t pending_raise = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_raise = raise_serial;
        }
        const bool force_raise = shown && pending_raise != applied_raise_serial;
        const bool size_changed = width != placed_w || height != placed_h;
        const bool moved = origin.x != placed_x || origin.y != placed_y || size_changed;
        const bool visible = IsWindowVisible(hwnd) != FALSE;
        if (!moved && !force_raise && shown && visible && !surface_cloaked) {
            SetPresented(true);
            if (handler) EnablePan();
            return;
        }
        pan.Disable();
        // A hidden overlay may have yielded its z-order to a popup. Restore
        // its band when showing it again, even if the rectangle did not change.
        const bool reveal = shown && (!visible || surface_cloaked);
        const bool raise = !shown || reveal || size_changed || force_raise;
        if (!SetWindowPos(hwnd, raise ? (embedded ? HWND_TOP : HWND_TOPMOST) : nullptr,
                          origin.x, origin.y, width, height,
                          SWP_NOACTIVATE | (raise ? 0 : SWP_NOZORDER) |
                              (shown ? SWP_SHOWWINDOW : SWP_NOREDRAW))) {
            HideWindow();
            return;
        }
        if (force_raise) applied_raise_serial = pending_raise;
        placed_x = origin.x;
        placed_y = origin.y;
        placed_w = width;
        placed_h = height;
        if (shown && !SetSurfaceCloak(false)) {
            HideWindow();
            return;
        }
        SetPresented(shown && IsWindowVisible(hwnd));
        if (reveal) {
            // Do not erase content a handler painted directly into the HWND.
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        }
        if (handler && shown && size_changed) {
            ComPtr<IPreviewHandler> preview;
            if (SUCCEEDED(handler->QueryInterface(IID_PPV_ARGS(&preview)))) {
                RECT client{0, 0, width, height};
                preview->SetRect(&client);
            }
        }
        if (handler && shown) EnablePan();
    }

    void Unload() {
        pan.Disable();
        if (handler) {
            ComPtr<IPreviewHandler> preview;
            if (SUCCEEDED(handler->QueryInterface(IID_PPV_ARGS(&preview))))
                preview->Unload();
            handler->Release();
            handler = nullptr;
        }
        if (stream) {
            stream->Release();
            stream = nullptr;
        }
        if (site) {
            site->Release();
            site = nullptr;
        }
        provider_lease.Reset();
        shown = false;
    }

    bool IsCurrent(const std::wstring& opening_identity) {
        std::lock_guard<std::mutex> lock(mutex);
        return !stop && command.enabled && command.owner == owner &&
            command.identity == opening_identity;
    }

    bool OpenCurrent(const std::wstring& opening_identity) {
        const auto started = GetTickCount64();
        const auto request = diagnostics::runtime::NextId();
        uint64_t generation = 0;
        { std::lock_guard<std::mutex> lock(mutex); generation = command_version; }
        const auto report = [&](uint32_t stage, HRESULT hr) {
            const bool cancelled = !IsCurrent(opening_identity);
            diagnostics::runtime::Event(cancelled ? "preview_handler_cancelled" : "preview_handler_failure",
                {{"request", request}, {"generation", generation}, {"stage", stage},
                 {"hresult", static_cast<uint32_t>(hr)}, {"elapsed_ms", GetTickCount64() - started}},
                cancelled ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Error);
        };
        Unload();
        HideWindow();
        {
            std::lock_guard<std::mutex> lock(mutex);
            provider_path = path;
        }
#ifdef PULSE_PREVIEW_HANDLER_TESTING
        g_test_open_attempts.fetch_add(1, std::memory_order_relaxed);
        wchar_t delay_text[16]{};
        if (GetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS",
                                    delay_text, ARRAYSIZE(delay_text)) > 0) {
            const int delay = _wtoi(delay_text);
            if (delay > 0) Sleep(static_cast<DWORD>(std::min(delay, 10000)));
        }
#endif
        if (path.empty() || !IsCurrent(opening_identity) || !EnsureWindow()) return false;
        const std::wstring extension = ExtensionOf(path);
        CLSID clsid{};
        if (!FindPreviewHandlerClsid(extension, clsid)) return false;
        wchar_t provider_key[40]{};
        StringFromGUID2(clsid, provider_key, static_cast<int>(std::size(provider_key)));
        auto opening_lease = PreviewProviderBudget().TryAcquire(provider_key);
        if (!opening_lease) { report(9, HRESULT_FROM_WIN32(ERROR_BUSY)); return false; }
        HRESULT activation = E_FAIL;
        ComPtr<IUnknown> unknown = CreateHandler(clsid, activation);
        if (!unknown) {
            if (IsCurrent(opening_identity)) NoteUnavailableProvider(extension);
            report(2, activation);
            return false;
        }

        site = new PreviewFrame(hwnd);
        ComPtr<IObjectWithSite> object_with_site;
        if (SUCCEEDED(unknown.As(&object_with_site)) && object_with_site)
            object_with_site->SetSite(site);

        const std::wstring open_path = ShellPath(path);
        const HRESULT stream_hr = InitWithStream(unknown.Get(), open_path, &stream);
        const HRESULT item_hr = FAILED(stream_hr) ? InitWithItem(unknown.Get(), open_path) : S_OK;
        const HRESULT file_hr = (FAILED(stream_hr) && FAILED(item_hr))
            ? InitWithFile(unknown.Get(), open_path) : S_OK;
        if (FAILED(stream_hr) && FAILED(item_hr) && FAILED(file_hr)) {
            // Initialization and rendering errors belong to this document, not
            // its registered provider. Keep each attempted initialization HRESULT.
            report(3, file_hr); report(4, item_hr); report(5, stream_hr);
            Unload(); return false;
        }
        ComPtr<IPreviewHandler> preview;
        const HRESULT preview_hr = unknown.As(&preview);
        if (FAILED(preview_hr) || !preview) {
            if (IsCurrent(opening_identity)) NoteUnavailableProvider(extension);
            report(6, preview_hr);
            Unload(); return false;
        }
        handler = unknown.Detach();
        if (!IsCurrent(opening_identity)) { Unload(); return false; }
        // Some handlers skip creating content for a hidden HWND but still
        // report S_OK. Make it Win32-visible without exposing incomplete or
        // cancelled content on the desktop. If DWM fails, stay hidden.
        PlaceOverlay();
        pan.Disable();
        RECT client{};
        GetClientRect(hwnd, &client);
        if (client.right <= client.left || client.bottom <= client.top) {
            Unload(); HideWindow(); return false;
        }
        initializing_surface = true;
        if (SetSurfaceCloak(true)) {
            SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        const auto abandon_surface = [&] {
            HideWindow();
            initializing_surface = false;
            Unload();
            return false;
        };
        const HRESULT window_hr = preview->SetWindow(hwnd, &client);
        if (FAILED(window_hr)) {
            report(7, window_hr);
            return abandon_surface();
        }
        if (!IsCurrent(opening_identity)) {
            report(8, HRESULT_FROM_WIN32(ERROR_CANCELLED));
            return abandon_surface();
        }
        const HRESULT preview_result = preview->DoPreview();
        if (FAILED(preview_result)) {
            report(8, preview_result);
            return abandon_surface();
        }
        preview->SetRect(&client);
        if (!IsCurrent(opening_identity)) {
            report(8, HRESULT_FROM_WIN32(ERROR_CANCELLED));
            return abandon_surface();
        }
        initializing_surface = false;
        provider_lease = std::move(opening_lease);
        return true;
    }

    // Callers hold mutex. Bumping the version makes the apartment re-read the
    // latest command, and the tick lets the owner tell how long it has waited.
    void BumpLocked() {
        ++command_version;
        command_tick = GetTickCount64();
    }

    std::mutex mutex;
    Command command;
    uint64_t command_version = 0;
    uint64_t raise_serial = 0;
    uint64_t applied_raise_serial = 0;
    ULONGLONG command_tick = 0;
    bool stop = false;
    HANDLE wake = nullptr;
    PreviewResourceBudget::Lease apartment_lease;
    PreviewResourceBudget::Lease provider_lease;
    HANDLE thread = nullptr;
    std::atomic<State> state{State::Idle};
    // Read by the owner while this thread is inside a provider call.
    std::atomic<uint64_t> applied_version{0};
    std::atomic<uint64_t> opens_started{0};
    std::atomic<uint64_t> opens_finished{0};
    std::atomic<ULONGLONG> open_started_tick{0};
    std::atomic<HWND> overlay{nullptr};
    std::atomic<bool> presented{false};

    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND notify = nullptr;
    RECT bounds{};
    std::wstring path;
    std::wstring identity;
    std::wstring pending_identity;
    // The request the apartment is opening right now. Written by the worker and
    // read by the owner's watchdog, so both sides go through mutex. Empty while
    // the apartment has nothing in flight.
    std::wstring working_identity;
    // Retained through Unload: a completed open may still block while releasing
    // its provider, after the owner has already selected a different file.
    std::wstring provider_path;
    DWORD attrs = 0;
    bool app_active = true;
    bool covered = false;
    bool shown = false;
    bool surface_cloaked = false;
    bool initializing_surface = false;
    bool embedded = false;
    HWND owner_root = nullptr;
    RECT placed_clip{LONG_MIN, LONG_MIN, LONG_MIN, LONG_MIN};
    int placed_x = INT_MIN;
    int placed_y = INT_MIN;
    int placed_w = 0;
    int placed_h = 0;
    IUnknown* handler = nullptr;
    IUnknown* stream = nullptr;
    IUnknown* site = nullptr;
    PreviewHandlerPan pan;
};

namespace {
bool PreviewPathOnVolume(const std::wstring& path, const std::wstring& root) {
    return !root.empty() && path.size() >= root.size() &&
        _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 &&
        (root.back() == L'\\' || path.size() == root.size() || path[root.size()] == L'\\');
}
}

std::shared_future<void> PreviewHandlerHost::SuspendVolume(const std::wstring& root) {
    std::wstring normalized = root;
    for (auto& ch : normalized) ch = ch == L'/' ? L'\\' : static_cast<wchar_t>(towlower(ch));
    while (!normalized.empty() && normalized.back() == L'\\') normalized.pop_back();
    if (const auto it = suspended_volumes_.find(normalized); it != suspended_volumes_.end()) return it->second;
    std::vector<std::shared_ptr<WorkerState>> affected;
    auto stop_matching = [&](const std::shared_ptr<WorkerState>& worker) {
        if (!worker) return false;
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (!PreviewPathOnVolume(worker->provider_path, normalized) &&
            !PreviewPathOnVolume(worker->command.path, normalized)) return false;
        worker->stop = true;
        SetEvent(worker->wake);
        affected.push_back(worker);
        return true;
    };
    for (const auto& worker : retired_) stop_matching(worker);
    if (stop_matching(worker_)) {
        DetachOverlay(worker_->overlay.load(std::memory_order_acquire));
        retired_.push_back(worker_);
        worker_.reset();
        last_enabled_ = false;
        last_identity_.clear();
        last_path_.clear();
    }
    auto completion = std::make_shared<std::promise<void>>();
    auto release = completion->get_future().share();
    suspended_volumes_[normalized] = release;
    if (affected.empty()) completion->set_value();
    else std::thread([affected = std::move(affected), completion] {
        for (const auto& worker : affected) {
            if (!worker->thread || WaitForSingleObject(worker->thread, INFINITE) != WAIT_OBJECT_0) {
                completion->set_exception(std::make_exception_ptr(std::runtime_error("Preview release could not be confirmed")));
                return;
            }
        }
        completion->set_value();
    }).detach();
    return release;
}

void PreviewHandlerHost::ResumeVolume(const std::wstring& root) {
    std::wstring normalized = root;
    for (auto& ch : normalized) ch = ch == L'/' ? L'\\' : static_cast<wchar_t>(towlower(ch));
    while (!normalized.empty() && normalized.back() == L'\\') normalized.pop_back();
    suspended_volumes_.erase(normalized);
}

PreviewHandlerHost::PreviewHandlerHost() = default;

PreviewHandlerHost::~PreviewHandlerHost() {
    auto worker = worker_;
    worker_.reset();
    if (worker) {
        DetachOverlay(worker->overlay.load(std::memory_order_acquire));
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            worker->stop = true;
        }
        SetEvent(worker->wake);
        if (worker->thread) WaitForSingleObject(worker->thread, 100);
    }
    // Retired apartments stop themselves as soon as the provider call they are
    // inside returns; the references they hold keep them alive until then.
    for (const auto& retired : retired_) {
        DetachOverlay(retired->overlay.load(std::memory_order_acquire));
    }
    retired_.clear();
}

void PreviewHandlerHost::EnsureWorker() {
    if (worker_) return;
    const auto now = GetTickCount64();
    if (now < admission_retry_) return;
    // Resource or Win32 startup failure must surface as Failed rather than
    // leaving an Idle request that can never finish loading.
    apartment_busy_ = true;
    admission_retry_ = now + 1000;
    auto lease = PreviewApartmentBudget().TryAcquire();
    if (!lease) return;
    auto worker = std::make_shared<WorkerState>();
    worker->apartment_lease = std::move(lease);
    worker->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!worker->wake) return;
    auto* argument = new std::shared_ptr<WorkerState>(worker);
    worker->thread = CreateThread(nullptr, 0, WorkerMain, argument, 0, nullptr);
    if (!worker->thread) {
        delete argument;
        return;
    }
    worker_ = std::move(worker);
    apartment_busy_ = false;
    admission_retry_ = 0;
}

PreviewHandlerHost::State PreviewHandlerHost::state() const {
    return worker_ ? worker_->state.load(std::memory_order_acquire) : (apartment_busy_ ? State::Failed : State::Idle);
}

bool PreviewHandlerHost::Presented() const noexcept {
    const auto worker = worker_;
    return worker && last_enabled_ && !covered_ &&
           worker->state.load(std::memory_order_acquire) == State::Shown &&
           worker->presented.load(std::memory_order_acquire);
}

void PreviewHandlerHost::SetNotifyWindow(HWND hwnd) {
    notify_ = hwnd;
    if (!hwnd) {
        Reset();
        return;
    }
    if (!worker_) return;
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->command.notify = hwnd;
        worker_->BumpLocked();
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::Publish(bool enabled, HWND owner, const RECT& bounds,
                                 const std::wstring& path, const std::wstring& identity,
                                 DWORD attrs, bool immediate) {
    // A hide must not spawn an apartment of its own.
    if (!enabled && !worker_) return;
    EnsureWorker();
    if (!worker_) return;
    WorkerState::Command command;
    command.owner = owner;
    command.notify = notify_;
    command.bounds = bounds;
    command.path = path;
    command.identity = identity;
    command.attrs = attrs;
    command.enabled = enabled;
    command.app_active = app_active_;
    command.covered = covered_;
    command.immediate = immediate;
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->command = std::move(command);
        worker_->BumpLocked();
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::Hide() {
    if (!worker_) {
        last_enabled_ = false;
        last_identity_.clear();
        return;
    }
    RECT empty{};
    worker_->state.store(State::Idle, std::memory_order_release);
    Publish(false, nullptr, empty, {}, {}, 0, false);
    last_enabled_ = false;
    last_identity_.clear();
}

void PreviewHandlerHost::Reset() {
    Hide();
    last_owner_ = nullptr;
    last_bounds_ = {};
}

void PreviewHandlerHost::Reposition(bool raise_zorder) {
    auto worker = worker_;
    if (!worker || !last_enabled_) return;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (!worker->command.enabled) return;
        if (raise_zorder) ++worker->raise_serial;
        worker->BumpLocked();
    }
    SetEvent(worker->wake);
}

void PreviewHandlerHost::NotifyAppActivate(bool active) {
    if (app_active_ == active) return;
    app_active_ = active;
    if (!worker_) return;
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->command.app_active = active;
        worker_->BumpLocked();
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::SetCovered(bool covered) {
    if (covered_ == covered) return;
    covered_ = covered;
    if (!worker_) return;
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->command.covered = covered;
        worker_->BumpLocked();
    }
    SetEvent(worker_->wake);
}

void PreviewHandlerHost::Sync(HWND owner, const D2D1_RECT_F& bounds, const std::wstring& path,
                              DWORD attrs, uint64_t generation, uint64_t modified, uint64_t size,
                              bool dark, const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg,
                              bool enabled, bool immediate) {
    // Called on every paint: an apartment that stopped answering is retired
    // first, and this frame falls through to the caller's fallback preview.
    for (const auto& entry : suspended_volumes_)
        if (PreviewPathOnVolume(path, entry.first)) enabled = false;
    const std::wstring identity = path + L"\n" + std::to_wstring(generation) + L":" +
        std::to_wstring(modified) + L":" + std::to_wstring(size);
    if (RetireStalledApartment(identity, enabled)) return;
    (void)dark;
    (void)bg;
    (void)fg;
    RECT target{};
    target.left = static_cast<LONG>(std::lround(bounds.left));
    target.top = static_cast<LONG>(std::lround(bounds.top));
    target.right = static_cast<LONG>(std::lround(bounds.right));
    target.bottom = static_cast<LONG>(std::lround(bounds.bottom));

    const bool usable = enabled && owner && IsWindow(owner) && !IsIconic(owner) &&
        IsWindowVisible(owner) && !files::PreviewWouldRecall(attrs) &&
        target.right > target.left + 8 && target.bottom > target.top + 8;
    if (!usable) {
        if (last_enabled_) Hide();
        return;
    }

    const bool same_bounds = EqualRect(&target, &last_bounds_) != FALSE;
    if (last_enabled_ && owner == last_owner_ && identity == last_identity_ && same_bounds)
        return;
    const bool new_content = !last_enabled_ || identity != last_identity_;
    EnsureWorker();
    if (!worker_) return;
    last_enabled_ = true;
    last_owner_ = owner;
    last_bounds_ = target;
    last_identity_ = identity;
    last_path_ = path;
    if (new_content) worker_->state.store(State::Loading, std::memory_order_release);
    Publish(true, owner, target, path, identity, attrs, immediate);
}

uint64_t PreviewHandlerHost::NextWatchdogDeadline() const {
    const auto worker = worker_;
    if (!worker) return 0;
    std::lock_guard<std::mutex> lock(worker->mutex);
    const bool superseded = !last_enabled_ || worker->working_identity != last_identity_;
    const uint64_t budget = superseded ? CommandBudgetMs() : SlowOpenBudgetMs();
    uint64_t next = 0;
    if (worker->opens_started.load() > worker->opens_finished.load())
        next = worker->open_started_tick.load() + budget;
    if (worker->applied_version.load() != worker->command_version) {
        const auto command = worker->command_tick + budget;
        next = next ? (std::min)(next, command) : command;
    }
    return next;
}

void PreviewHandlerHost::ReapRetired() {
    for (auto it = retired_.begin(); it != retired_.end();) {
        const HANDLE thread = (*it)->thread;
        if (!thread || WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) it = retired_.erase(it);
        else ++it;
    }
}

uint64_t PreviewHandlerHost::NextDeadline() const {
    const auto worker = worker_;
    if (!worker) return 0;
    std::lock_guard<std::mutex> lock(worker->mutex);
    const auto budget = !last_enabled_ || worker->working_identity != last_identity_
        ? CommandBudgetMs() : SlowOpenBudgetMs();
    uint64_t deadline = 0;
    if (worker->opens_started.load(std::memory_order_acquire) >
        worker->opens_finished.load(std::memory_order_acquire))
        deadline = worker->open_started_tick.load(std::memory_order_acquire) + budget;
    if (worker->applied_version.load(std::memory_order_acquire) != worker->command_version) {
        const auto command = worker->command_tick + budget;
        if (!deadline || command < deadline) deadline = command;
    }
    // The worker may have accepted its delayed open but not yet entered COM.
    // Keep one wake scheduled across that gap; completed opens report Shown/Failed.
    if (!deadline && worker->state.load(std::memory_order_acquire) == State::Loading)
        deadline = GetTickCount64() + CommandBudgetMs();
    return deadline;
}

#ifdef PULSE_WITH_SELFTEST
void PreviewHandlerHost::PrimeStalledOpenForTest() {
    Reset();
    worker_ = std::make_shared<WorkerState>();
    worker_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker_->opens_started = 1;
    worker_->open_started_tick = GetTickCount64() - CommandBudgetMs() + 150;
    worker_->provider_path = L"private.pulse-deadline-test";
    worker_->state = State::Loading;
    last_enabled_ = false;
}
#endif

bool PreviewHandlerHost::RetireStalledApartment(const std::wstring& requested_identity,
                                               bool requested) {
    ReapRetired();
    const auto worker = worker_;
    if (!worker) return false;
    const ULONGLONG now = GetTickCount64();

    // The apartment serves one request at a time, so a request the pane has
    // moved on from must not wait for the provider that is still busy with the
    // previous one. Only the request the apartment is working on right now - the
    // one the pane is therefore still showing - keeps the longer budget above.
    bool superseded = !requested;
    std::wstring stalled_path;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (worker->working_identity != requested_identity) superseded = true;
        stalled_path = worker->provider_path.empty() ? last_path_ : worker->provider_path;
    }
    const ULONGLONG budget = superseded ? CommandBudgetMs() : SlowOpenBudgetMs();

    // An apartment is stuck when either the provider open it started never came
    // back, or a command sent to it was never applied.
    bool stalled = worker->opens_started.load(std::memory_order_acquire) >
            worker->opens_finished.load(std::memory_order_acquire) &&
        now - worker->open_started_tick.load(std::memory_order_acquire) >= budget;
    ULONGLONG sent = 0;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (!stalled && worker->applied_version.load(std::memory_order_relaxed) !=
                            worker->command_version) {
            stalled = true;
            sent = worker->command_tick;
        }
    }
    if (!stalled) return false;
    if (sent && now - sent < budget) return false;

    // Remember the provider, so the pane asks the thumbnail path instead of
    // queueing behind the same provider on a fresh apartment.
    NoteStalledProvider(ExtensionOf(stalled_path));
    DetachOverlay(worker->overlay.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->stop = true;
    }
    SetEvent(worker->wake);
    worker_.reset();
    // The next paint re-reads CanHost, which now reports the cooldown, and
    // publishes again only once the provider is allowed back. Ask for that frame
    // now: this one is already inside the caller's paint and still holds the
    // handler path it chose before the retire.
    const HWND wake = notify_ ? notify_ : last_owner_;
    if (wake && IsWindow(wake)) InvalidateRect(wake, nullptr, FALSE);
    last_enabled_ = false;
    last_identity_.clear();
    last_owner_ = nullptr;
    retired_.push_back(std::move(worker));
    return true;
}

LRESULT CALLBACK PreviewHandlerHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WorkerState* self = reinterpret_cast<WorkerState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WorkerState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (self->pan.HandleMessage(msg, wParam, lParam)) return 1;

    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_CONTEXTMENU:
        // A right-click the provider declined bubbles up to this owned popup.
        // Hand it to the owner asynchronously: the owner may run a modal menu,
        // and this apartment must never wait on it.
        if (self->owner && IsWindow(self->owner))
            PostMessageW(self->owner, WM_CONTEXTMENU,
                         reinterpret_cast<WPARAM>(self->owner), lParam);
        return 0;
    case WM_ERASEBKGND: {
        // Some Office previewers render into this host instead of an opaque
        // child. Erasing after DoPreview replaces valid content with white
        // until the next user input forces the handler to repaint.
        if (self->handler) return 1;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(reinterpret_cast<HDC>(wParam), &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_DESTROY:
        if (self->hwnd == hwnd) self->hwnd = nullptr;
        self->overlay.store(nullptr, std::memory_order_release);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI PreviewHandlerHost::WorkerMain(void* parameter) {
    std::unique_ptr<std::shared_ptr<WorkerState>> argument(
        static_cast<std::shared_ptr<WorkerState>*>(parameter));
    std::shared_ptr<WorkerState> self = *argument;
    auto apartment_lease = std::move(self->apartment_lease);
    const HRESULT com_result = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com_result)) {
        diagnostics::runtime::Event("preview_handler_failure",
            {{"stage", 1}, {"hresult", static_cast<uint32_t>(com_result)}},
            diagnostics::runtime::Level::Error);
        self->state.store(State::Failed, std::memory_order_release);
        return 0;
    }

    WorkerState::event_target = self.get();
    const HWINEVENTHOOK owner_events = SetWinEventHook(EVENT_OBJECT_DESTROY,
        EVENT_OBJECT_LOCATIONCHANGE, nullptr, WorkerState::OwnerEvent,
        GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    uint64_t applied_version = 0;
    std::optional<std::chrono::steady_clock::time_point> open_due;
    for (;;) {
        WorkerState::Command command;
        uint64_t version = 0;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            stop = self->stop;
            version = self->command_version;
            command = self->command;
        }
        if (stop) break;

        if (version != applied_version) {
            if (command.owner != self->owner && self->hwnd) {
                self->Unload();
                self->HideWindow();
                DestroyWindow(self->hwnd);
                self->hwnd = nullptr;
            }
            self->owner = command.owner;
            self->notify = command.notify;
            self->bounds = command.bounds;
            self->path = command.path;
            self->attrs = command.attrs;
            self->app_active = command.app_active;
            self->covered = command.covered;
            if (!command.enabled) {
                self->Unload();
                self->HideWindow();
                self->identity.clear();
                self->pending_identity.clear();
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    self->working_identity.clear();
                }
                open_due.reset();
                self->state.store(State::Idle, std::memory_order_release);
            } else if (command.identity == self->identity) {
                self->pending_identity.clear();
                open_due.reset();
                self->shown = self->handler != nullptr;
                self->state.store(self->shown ? State::Shown : State::Failed,
                                  std::memory_order_release);
                self->PlaceOverlay();
            } else if (command.identity != self->pending_identity) {
                self->HideWindow();
                self->pending_identity = command.identity;
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    self->working_identity = command.identity;
                }
                open_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                    command.immediate ? 0 : kOpenDelayMs);
                self->state.store(State::Loading, std::memory_order_release);
            } else {
                self->PlaceOverlay();
            }
            applied_version = version;
            self->applied_version.store(version, std::memory_order_release);
        }

        if (open_due && std::chrono::steady_clock::now() >= *open_due) {
            const std::wstring opening_identity = self->pending_identity;
            // Bracket the whole open, provider calls included, so the owner can
            // tell that this apartment is busy and since when.
            self->open_started_tick.store(GetTickCount64(), std::memory_order_release);
            self->opens_started.fetch_add(1, std::memory_order_release);

            const bool opened = self->OpenCurrent(opening_identity);
            self->opens_finished.fetch_add(1, std::memory_order_release);
            if (opened) ClearSlowProvider(ExtensionOf(self->path));
            bool current = false;
            bool placement_current = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->working_identity.clear();
                current = !self->stop && self->command.enabled && self->command.owner == self->owner &&
                    self->command.identity == opening_identity;
                placement_current = self->command_version == version;
            }
            if (current) {
                self->identity = opening_identity;
                self->pending_identity.clear();
                self->shown = opened;
                if (opened) {
                    // A cover/activation or resize change received during COM
                    // must be applied before putting the surface on screen.
                    if (placement_current) self->PlaceOverlay();
                } else {
                    self->HideWindow();
                }
                self->state.store(opened ? State::Shown : State::Failed,
                                  std::memory_order_release);
                if (self->notify) InvalidateRect(self->notify, nullptr, FALSE);
            } else {
                self->HideWindow();
                self->Unload();
            }
            open_due.reset();
            continue;
        }

        DWORD timeout = INFINITE;
        if (open_due) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *open_due - std::chrono::steady_clock::now()).count();
            timeout = static_cast<DWORD>(std::clamp<int64_t>(remaining, 0, 1000));
        }
        const DWORD wait = MsgWaitForMultipleObjectsEx(
            1, &self->wake, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0 + 1) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    if (owner_events) UnhookWinEvent(owner_events);
    WorkerState::event_target = nullptr;
    self->Unload();
    self->HideWindow();
    if (self->hwnd) DestroyWindow(self->hwnd);
    self->hwnd = nullptr;
    self->state.store(State::Idle, std::memory_order_release);
    g_factories_64.clear();
    g_factories_32.clear();
    CoUninitialize();
    return 0;
}

#ifdef PULSE_PREVIEW_HANDLER_TESTING
void ResetPreviewHandlerOpenAttemptsForTest() {
    g_test_open_attempts.store(0, std::memory_order_relaxed);
}

uint32_t PreviewHandlerOpenAttemptsForTest() {
    return g_test_open_attempts.load(std::memory_order_relaxed);
}

void ResetOverlayPlaceCountsForTest() {
    g_test_place_calls.store(0, std::memory_order_relaxed);
    g_test_place_done.store(0, std::memory_order_relaxed);
}

uint32_t OverlayPlaceCallsForTest() {
    return g_test_place_calls.load(std::memory_order_relaxed);
}

uint32_t OverlayPlaceDoneForTest() {
    return g_test_place_done.load(std::memory_order_relaxed);
}

bool PreviewProviderCoolingDownForTest(const std::wstring& path) {
    return ProviderCoolingDown(ExtensionOf(path));
}

void ResetSlowPreviewProvidersForTest() {
    ResetSlowProvidersForTest();
}

HWND PreviewHandlerHost::overlay_window_for_test() const {
    return worker_ ? worker_->overlay.load(std::memory_order_acquire) : nullptr;
}

bool PreviewHandlerCanActivateIsolatedForTest(const std::wstring& path) {
    CLSID clsid{};
    if (!FindPreviewHandlerClsid(ExtensionOf(path), clsid)) return false;
    ComPtr<IClassFactory> factory;
    if (SUCCEEDED(QueryIsolatedClassObject(clsid, false, factory)) && factory) return true;
#ifdef _WIN64
    return SUCCEEDED(QueryIsolatedClassObject(clsid, true, factory)) && factory;
#else
    return false;
#endif
}
#endif

} // namespace pulse::ui
