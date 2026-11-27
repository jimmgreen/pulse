#include "../common/windows_compat.h"
#include <dwmapi.h>
#include <unordered_map>
#include "confirm_caption_trace.h"

namespace pulse::ui::test {
namespace { std::unordered_map<HWND, CaptionWrite> writes; }
void ClearCaptionWrites() { writes.clear(); }
CaptionWrite LastCaptionWrite(HWND hwnd) {
    const auto found = writes.find(hwnd);
    return found == writes.end() ? CaptionWrite{} : found->second;
}
}

HRESULT WINAPI ConfirmTestDwmSetWindowAttribute(HWND hwnd, DWORD attribute, LPCVOID value, DWORD bytes) {
    const HRESULT result = ::DwmSetWindowAttribute(hwnd, attribute, value, bytes);
    if (attribute == DWMWA_CAPTION_COLOR && value && bytes == sizeof(COLORREF)) {
        auto& call = pulse::ui::test::writes[hwnd];
        call.value = *static_cast<const COLORREF*>(value);
        call.result = result;
        ++call.calls;
    }
    return result;
}

// This target observes real successful DWM writes, not a simulated OS. Caption
// color is set-only on Windows 11; DwmGetWindowAttribute rejects that attribute.
#define DwmSetWindowAttribute ConfirmTestDwmSetWindowAttribute
#include "../ui/window_helpers.cpp"
#undef DwmSetWindowAttribute
