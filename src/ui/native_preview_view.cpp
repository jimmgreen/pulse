#include "native_preview_view.h"
#include "extension_panel.h"
#include "../app/extensions/native/previews_adapter.h"
#include "../app/extensions/native/session.h"
#include "../app/app_runtime.h"
#include <atomic>

namespace pulse::ui {
namespace {
std::atomic<uint64_t> next_surface{1};
}
NativePreviewView::NativePreviewView() : surface_(next_surface.fetch_add(1)) {}
NativePreviewView::~NativePreviewView() { Reset(); }
void NativePreviewView::Reset() {
    if (owner_) extensions::native::PreviewsAdapter::Cancel(owner_, surface_);
    owner_ = nullptr;
    if (model_) model_->Close();
    result_.reset(); bitmap_.reset(); device_ = nullptr;
}
bool NativePreviewView::Available(HWND owner, const std::wstring& path) {
    if (auto* state = GetAppState(owner)) extensions::native::PrepareCommandPackages(*state);
    const bool available = extensions::native::PreviewsAdapter::Available(owner, path);
    if (!available) Reset();
    return available;
}
bool NativePreviewView::CaptureModel(const wchar_t* path) { return model_ && model_->SaveCurrentFrame(path); }
std::optional<PreviewDrawResult> NativePreviewView::Draw(HWND owner, HWND notify,
    ID2D1DeviceContext* dc, const D2D1_RECT_F& bounds, const std::wstring& path,
    uint64_t modified, uint64_t size, uint32_t pixels,
    std::shared_ptr<const std::wstring>& text, bool& truncated,
    preview::Integrity& integrity, uint32_t& width, uint32_t& height,
    bool dark, D2D1_COLOR_F accent, float scale) {
    using namespace extensions::native;
    if (owner_ && owner_ != owner) Reset();
    owner_ = owner;
    const auto response = PreviewsAdapter::Request(owner, path, modified, size, pixels, surface_, notify);
    if (response.status == PreviewStatus::Unavailable) { Reset(); return std::nullopt; }
    text.reset(); truncated = false; width = height = 0;
    integrity = {}; integrity.state = preview::IntegrityState::Loading;
    if (response.status == PreviewStatus::Pending) {
        if (model_) model_->Close();
        result_.reset(); bitmap_.reset(); return PreviewDrawResult::Pending;
    }
    if (response.status != PreviewStatus::Ready || !response.result) {
        if (model_) model_->Close();
        result_.reset(); bitmap_.reset(); integrity.state = preview::IntegrityState::Failed;
        return PreviewDrawResult::Failed;
    }
    const auto& result = *response.result;
    truncated = result.incomplete;
    integrity.state = result.incomplete ? preview::IntegrityState::Partial : preview::IntegrityState::Complete;
    if (result.kind == PULSE_PREVIEW_UI_MODEL) {
        const RECT rect{static_cast<LONG>(bounds.left), static_cast<LONG>(bounds.top),
            static_cast<LONG>(bounds.right), static_cast<LONG>(bounds.bottom)};
        if (!model_) model_ = std::make_shared<ExtensionPanel>();
        if (result_ != response.result || !model_->IsOpen()) {
            model_->Close();
            ExtensionPanelSpec spec; spec.tree = *result.model; spec.content_only = true;
            if (!model_->ShowDocked(notify, rect, std::move(spec), dark, accent, scale)) return PreviewDrawResult::Failed;
            result_ = response.result;
        } else model_->UpdateDocked(rect, dark, accent, scale);
        bitmap_.reset();
        return PreviewDrawResult::Custom;
    }
    if (model_) model_->Close();
    if (result.kind == PULSE_PREVIEW_ARCHIVE) {
        result_ = response.result; bitmap_.reset();
        text = result.archive_text;
        return PreviewDrawResult::Archive;
    }
    if (result_ != response.result || device_ != dc) {
        bitmap_.reset(); result_ = response.result; device_ = dc;
    }
    if (!bitmap_.get()) {
        const auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (!dc || FAILED(dc->CreateBitmap(D2D1::SizeU(result.width, result.height),
                result.pixels.data(), result.stride, &properties, &bitmap_))) return PreviewDrawResult::Failed;
    }
    width = result.width; height = result.height;
    const float fit = (std::min)((bounds.right - bounds.left) / width, (bounds.bottom - bounds.top) / height);
    const float x = (bounds.left + bounds.right - width * fit) * .5f;
    const float y = (bounds.top + bounds.bottom - height * fit) * .5f;
    const auto fitted = D2D1::RectF(x, y, x + width * fit, y + height * fit);
    dc->DrawBitmap(bitmap_.get(), fitted, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    return PreviewDrawResult::Bitmap;
}
}
