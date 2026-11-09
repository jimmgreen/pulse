#include "../ui/native_preview_view.h"
#include <atomic>

// The in-process picker runs inside third-party processes and never hosts the
// extension runtime. Quick Look falls back to its built-in preview paths.
namespace pulse::ui {
namespace {
std::atomic<uint64_t> next_surface{1};
}
NativePreviewView::NativePreviewView() : surface_(next_surface.fetch_add(1)) {}
NativePreviewView::~NativePreviewView() = default;
void NativePreviewView::Reset() {
    owner_ = nullptr;
    result_.reset(); bitmap_.reset(); device_ = nullptr;
}
bool NativePreviewView::Available(HWND, const std::wstring&) { Reset(); return false; }
bool NativePreviewView::CaptureModel(const wchar_t*) { return false; }
std::optional<PreviewDrawResult> NativePreviewView::Draw(HWND, HWND, ID2D1DeviceContext*,
    const D2D1_RECT_F&, const std::wstring&, uint64_t, uint64_t, uint32_t,
    std::shared_ptr<const std::wstring>&, bool&, preview::Integrity&, uint32_t&, uint32_t&,
    bool, D2D1_COLOR_F, float) {
    return std::nullopt;
}
}
