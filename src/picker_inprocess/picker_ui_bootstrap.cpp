#include "picker_ui_environment.h"
#include "../common/picker_owner_guard.h"
#include <thread>
#include <chrono>
#include "picker_ui_api.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/localization.h"
#include "../app/picker_appearance_ui.h"
#include "../ui/typography.h"
#include <objbase.h>
#include <algorithm>
#include <string>
#include <new>

namespace {
SRWLOCK initialize_lock = SRWLOCK_INIT;
thread_local bool initialized = false;
thread_local bool reserved_v2 = false;
thread_local bool lifecycle_busy = false;
struct LifecycleBusyScope {
    LifecycleBusyScope() { lifecycle_busy = true; }
    ~LifecycleBusyScope() { lifecycle_busy = false; }
};
thread_local HMODULE module = nullptr;
thread_local HANDLE activation = INVALID_HANDLE_VALUE;
thread_local LANGID prior_language = 0;
thread_local pulse::app::PickerAppearanceSnapshot appearance;
struct Frame {
    HWND window = nullptr;
    HANDLE cancel = nullptr;
    PulsePickerUiCallbacks callbacks{};
    HRESULT callback_error = S_OK;
    ~Frame() { if (cancel) CloseHandle(cancel); }
};
thread_local Frame* active = nullptr;
struct ThreadDpiScope {
    using SetContext = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetContext set = nullptr;
    DPI_AWARENESS_CONTEXT prior = nullptr;
    DWORD Enter() noexcept {
        const HMODULE user = GetModuleHandleW(L"user32.dll");
        set = user ? reinterpret_cast<SetContext>(GetProcAddress(user, "SetThreadDpiAwarenessContext")) : nullptr;
        if (!set) return ERROR_CALL_NOT_IMPLEMENTED;
        SetLastError(ERROR_SUCCESS);
        prior = set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!prior) { const DWORD error = GetLastError(); return error ? error : ERROR_GEN_FAILURE; }
        return ERROR_SUCCESS;
    }
    ~ThreadDpiScope() { if (prior) set(prior); }
};
HRESULT WINAPI Initialize(uint32_t flags) {
    if (reserved_v2 || lifecycle_busy) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (flags) return E_INVALIDARG;
    if (initialized) return S_FALSE;
    APTTYPE type{}; APTTYPEQUALIFIER qualifier{};
    HRESULT hr = CoGetApartmentType(&type, &qualifier);
    if (FAILED(hr)) return hr;
    if (type != APTTYPE_STA && type != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    if (!TryAcquireSRWLockExclusive(&initialize_lock)) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Initialize), &module)) {
        const DWORD error = GetLastError();
        ReleaseSRWLockExclusive(&initialize_lock); return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    hr = OleInitialize(nullptr);
    if (FAILED(hr)) { ReleaseSRWLockExclusive(&initialize_lock); return hr; }
    ACTCTXW context{}; context.cbSize = sizeof(context);
    context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    context.hModule = module; context.lpResourceName = MAKEINTRESOURCEW(module == GetModuleHandleW(nullptr) ? 1 : 2);
    activation = CreateActCtxW(&context);
    if (activation == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError(); OleUninitialize(); ReleaseSRWLockExclusive(&initialize_lock);
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    prior_language = GetThreadUILanguage();
    try {
        appearance = pulse::app::ReadUserPickerAppearanceReadOnly().snapshot;
        pulse::l10n::Initialize(module, appearance.language);
    }
    catch (...) {
        ReleaseActCtx(activation); activation = INVALID_HANDLE_VALUE;
        SetThreadUILanguage(prior_language); OleUninitialize(); ReleaseSRWLockExclusive(&initialize_lock);
        return E_OUTOFMEMORY;
    }
    initialized = true; return S_OK;
}
HRESULT WINAPI Shutdown() {
    if (reserved_v2 || lifecycle_busy) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (!initialized) return S_FALSE;
    if (active) return HRESULT_FROM_WIN32(ERROR_BUSY);
    LifecycleBusyScope busy;
    ReleaseActCtx(activation); activation = INVALID_HANDLE_VALUE;
    SetThreadUILanguage(prior_language); OleUninitialize(); initialized = false;
    ReleaseSRWLockExclusive(&initialize_lock);
    return S_OK;
}
HRESULT WINAPI GetWindow(HWND* output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!active || !active->window || !IsWindow(active->window)) return E_UNEXPECTED;
    DWORD pid = 0;
    if (GetWindowThreadProcessId(active->window, &pid) != GetCurrentThreadId() || pid != GetCurrentProcessId()) return E_UNEXPECTED;
    *output = active->window; return S_OK;
}
HRESULT WINAPI Close() {
    if (!active || !active->cancel) return E_UNEXPECTED;
    if (!SetEvent(active->cancel)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}
bool CopyString(const wchar_t* text, uint32_t count, std::wstring& value) {
    if (count > PULSE_PICKER_UI_MAX_UNITS || (count && !text)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto c = static_cast<uint16_t>(text[i]);
        if (!c) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == count || text[i] < 0xdc00 || text[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    if (count) value.assign(text, count); else value.clear();
    return true;
}
HRESULT Notify(Frame& frame, uint32_t kind, HWND window, const pulse::ui::FilePickerResult* candidate = nullptr) {
    frame.window = window;
    if (!frame.callbacks.callback) return S_OK;
    PulsePickerUiEvent event{}; event.size = sizeof(event); event.kind = kind; event.window = window;
    if (candidate) {
        if (candidate->paths.size() != 1 || candidate->paths.front().size() > PULSE_PICKER_UI_MAX_UNITS) return E_UNEXPECTED;
        event.path = candidate->paths.front().c_str(); event.path_units = static_cast<uint32_t>(candidate->paths.front().size());
        event.filter_index = static_cast<uint32_t>(candidate->filter_index);
    }
    HRESULT hr = E_FAIL;
    try { hr = frame.callbacks.callback(frame.callbacks.context, &event); }
    catch (...) { hr = E_FAIL; }
    if (hr != S_OK && !(kind == PULSE_PICKER_UI_FILE_OK && hr == S_FALSE)) {
        frame.callback_error = FAILED(hr) ? hr : E_UNEXPECTED;
        return frame.callback_error;
    }
    return hr;
}
HRESULT WINAPI Show(const PulsePickerUiConfig* config, const PulsePickerUiCallbacks* callbacks, PulsePickerUiResult* result) {
    if (reserved_v2 || lifecycle_busy) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (!initialized) return CO_E_NOTINITIALIZED;
    if (active) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (!config || !callbacks || !result) return E_POINTER;
    // Test size before reading any later member (including version/pointers).
    if (config->size != sizeof(*config) || callbacks->size != sizeof(*callbacks) || result->size != sizeof(*result)) return E_INVALIDARG;
    if (config->version != PULSE_PICKER_UI_ABI || config->reserved || callbacks->reserved ||
        (config->flags & ~PULSE_PICKER_UI_DARK) || !result->path || result->path_capacity < PULSE_PICKER_UI_MAX_UNITS + 1) return E_INVALIDARG;
    result->status = PULSE_PICKER_UI_FAILED; result->win32_error = ERROR_GEN_FAILURE; result->path_units = 0; result->path[0] = 0;
    LifecycleBusyScope busy;
    ThreadDpiScope dpi_scope;
    const DWORD dpi_error = dpi_scope.Enter();
    if (dpi_error) { result->win32_error = dpi_error; return HRESULT_FROM_WIN32(dpi_error); }
    // Proof callbacks currently run in the picker PMv2 context. A production
    // event adapter must restore the caller's context around external callbacks.
    ULONG_PTR cookie = 0;
    if (!ActivateActCtx(activation, &cookie)) return HRESULT_FROM_WIN32(GetLastError());
    struct ActivationScope { ULONG_PTR cookie; ~ActivationScope() { DeactivateActCtx(0, cookie); } } scope{cookie};
    Frame frame; frame.callbacks = *callbacks;
    try {
        pulse::PickerOwnerGuard owner_guard(config->owner);
        if (owner_guard.Error()) { result->win32_error = owner_guard.Error(); return HRESULT_FROM_WIN32(owner_guard.Error()); }
        pulse::ui::FolderPickerSpec spec; spec.mode = pulse::ui::PickerMode::File;
        pulse::app::ApplyPickerAppearanceToSpec(appearance, spec);
        struct TypographyScope {
            int scale = pulse::ui::typography::UiFontScalePercent();
            pulse::ui::typography::TextRenderMode mode = pulse::ui::typography::CurrentTextRenderMode();
            ~TypographyScope() {
                pulse::ui::typography::SetUiFontScale(scale);
                pulse::ui::typography::SetTextRenderMode(mode);
            }
        } typography_scope;
        pulse::ui::typography::SetUiFontScale(appearance.ui_font_scale);
        pulse::ui::typography::SetTextRenderMode(static_cast<pulse::ui::typography::TextRenderMode>(appearance.text_render));
        auto request_appearance = appearance;
        if (config->flags & PULSE_PICKER_UI_DARK) request_appearance.theme_mode = 2;
        const auto colors = pulse::app::ResolveSystemPickerAppearanceColors(request_appearance);
        if (!CopyString(config->directory, config->directory_units, spec.initial_path) ||
            !CopyString(config->filename, config->filename_units, spec.filename) || spec.initial_path.empty() ||
            spec.filename.find_first_of(L"\\/:*?\"<>|") != spec.filename.npos) return E_INVALIDARG;
        frame.cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!frame.cancel) return HRESULT_FROM_WIN32(GetLastError());
        spec.owner_guard = &owner_guard;
        std::jthread owner_monitor([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (!owner_guard.Alive()) { SetEvent(frame.cancel); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
        spec.cancel_event = frame.cancel; spec.module_instance = module;
        spec.pulse_painted_plain_child = true;
        spec.on_ready = [&](HWND window) { return Notify(frame, PULSE_PICKER_UI_READY, window); };
        spec.on_file_ok = [&](HWND window, const pulse::ui::FilePickerResult& candidate) { return Notify(frame, PULSE_PICKER_UI_FILE_OK, window, &candidate); };
        active = &frame;
        struct ActiveScope { ~ActiveScope() { active = nullptr; } } live;
        const auto outcome = pulse::ui::ShowFilePickerDetailed(config->owner, spec, colors.dark, pulse::ui::HexColor(colors.accent_rgb));
        frame.window = nullptr;
        if (FAILED(frame.callback_error)) { result->win32_error = ERROR_FUNCTION_FAILED; return frame.callback_error; }
        if (!owner_guard.Alive() || outcome.status == pulse::ui::FilePickerStatus::Cancelled) {
            result->status = PULSE_PICKER_UI_CANCELLED; result->win32_error = ERROR_SUCCESS;
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        if (outcome.status != pulse::ui::FilePickerStatus::Accepted || outcome.result.paths.size() != 1) {
            result->win32_error = outcome.error ? outcome.error : ERROR_INVALID_DATA;
            return HRESULT_FROM_WIN32(result->win32_error);
        }
        const auto& path = outcome.result.paths.front();
        if (path.size() >= result->path_capacity) { result->win32_error = ERROR_INSUFFICIENT_BUFFER; return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
        std::copy(path.begin(), path.end(), result->path); result->path[path.size()] = 0;
        result->path_units = static_cast<uint32_t>(path.size()); result->status = PULSE_PICKER_UI_ACCEPTED; result->win32_error = ERROR_SUCCESS;
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
}
namespace pulse::picker_inprocess {
HRESULT AcquireEnvironment(Environment& output) {
    if (initialized || reserved_v2) return HRESULT_FROM_WIN32(ERROR_BUSY);
    const LANGID caller_language = GetThreadUILanguage();
    HRESULT hr = Initialize(0);
    if (FAILED(hr)) return hr;
    try {
        output.module = module; output.activation = activation;
        output.ui_language = GetThreadUILanguage(); output.appearance = appearance;
        reserved_v2 = true;
        SetThreadUILanguage(caller_language);
        return S_OK;
    } catch (...) {
        Shutdown(); SetThreadUILanguage(caller_language); return E_OUTOFMEMORY;
    }
}
HRESULT ReleaseEnvironment() {
    if (!reserved_v2) return E_UNEXPECTED;
    const LANGID caller_language = GetThreadUILanguage();
    reserved_v2 = false;
    const HRESULT hr = Shutdown();
    SetThreadUILanguage(caller_language);
    return hr;
}
}

extern "C" HRESULT WINAPI PulsePickerUiGetApi(uint32_t version, uint32_t size, PulsePickerUiApi* api) {
    if (!api) return E_POINTER;
    if (version != PULSE_PICKER_UI_ABI || size != sizeof(*api)) return E_INVALIDARG;
    *api = {sizeof(*api), PULSE_PICKER_UI_ABI, Initialize, Show, GetWindow, Close, Shutdown};
    return S_OK;
}
