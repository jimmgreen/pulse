// ai_vision.cpp - WIC image resize + JPEG/PNG base64 data URL encoder (ai_vision.h).
#include "ai_vision.h"

#include "ai_tools_fs.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse::app::ai {
namespace {

using Microsoft::WRL::ComPtr;

struct ComScope {
    bool uninit = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninit = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (uninit) CoUninitialize();
    }
};

bool EncodeWicSourceToBytes(IWICImagingFactory* factory, IWICBitmapSource* src, int target_w, int target_h,
                            float jpeg_quality, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    ComPtr<IWICBitmapSource> current = src;
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    if (static_cast<int>(w) != target_w || static_cast<int>(h) != target_h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
        if (FAILED(scaler->Initialize(src, static_cast<UINT>(target_w), static_cast<UINT>(target_h),
                                      WICBitmapInterpolationModeFant)))
            return false;
        current = scaler;
    }
    // Convert to 24bpp BGR for JPEG encoding (strips alpha cleanly onto black/white).
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(current.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;

    ComPtr<IStream> mem_stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem_stream))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(mem_stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
    if (props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT var{};
        VariantInit(&var);
        var.vt = VT_R4;
        var.fltVal = jpeg_quality;
        props->Write(1, &opt, &var);
    }
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(target_w), static_cast<UINT>(target_h)))) return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&pf);
    if (FAILED(frame->WriteSource(conv.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;

    HGLOBAL hglobal = nullptr;
    if (FAILED(GetHGlobalFromStream(mem_stream.Get(), &hglobal)) || !hglobal) return false;
    STATSTG st{};
    if (FAILED(mem_stream->Stat(&st, STATFLAG_NONAME))) return false;
    const size_t len = static_cast<size_t>(st.cbSize.QuadPart);
    if (len == 0) return false;
    const void* ptr = GlobalLock(hglobal);
    if (!ptr) return false;
    out_bytes.assign(static_cast<const uint8_t*>(ptr), static_cast<const uint8_t*>(ptr) + len);
    GlobalUnlock(hglobal);
    return true;
}

EncodedImage EncodeSourceBounded(IWICImagingFactory* factory, IWICBitmapSource* src, int max_edge, size_t max_bytes) {
    EncodedImage out;
    UINT w = 0, h = 0;
    if (!factory || !src || FAILED(src->GetSize(&w, &h)) || w == 0 || h == 0) {
        out.error_message = "Could not read image dimensions.";
        return out;
    }
    const int capped_edge = std::clamp(max_edge, 256, 2048);
    int tw = static_cast<int>(w);
    int th = static_cast<int>(h);
    if (tw > capped_edge || th > capped_edge) {
        const double scale = static_cast<double>(capped_edge) / static_cast<double>((std::max)(tw, th));
        tw = (std::max)(1, static_cast<int>(std::lround(tw * scale)));
        th = (std::max)(1, static_cast<int>(std::lround(th * scale)));
    }
    std::vector<uint8_t> bytes;
    static constexpr float kQualities[] = {0.85f, 0.72f, 0.58f};
    for (float q : kQualities) {
        if (!EncodeWicSourceToBytes(factory, src, tw, th, q, bytes)) {
            out.error_message = "Failed to encode image.";
            return out;
        }
        if (bytes.size() <= max_bytes) break;
        tw = (std::max)(256, tw * 3 / 4);
        th = (std::max)(256, th * 3 / 4);
    }
    if (bytes.empty() || bytes.size() > max_bytes * 2) {
        out.error_message = "Encoded image exceeds size budget.";
        return out;
    }
    out.ok = true;
    out.mime = "image/jpeg";
    out.width = tw;
    out.height = th;
    out.encoded_bytes = bytes.size();
    out.data_url = "data:image/jpeg;base64," + Base64Encode(bytes.data(), bytes.size());
    return out;
}

} // namespace

std::string Base64Encode(const uint8_t* data, size_t length) {
    static constexpr char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= length) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTab[(v >> 18) & 63]);
        out.push_back(kTab[(v >> 12) & 63]);
        out.push_back(kTab[(v >> 6) & 63]);
        out.push_back(kTab[v & 63]);
        i += 3;
    }
    if (i < length) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (i + 1 < length ? (static_cast<uint32_t>(data[i + 1]) << 8) : 0u);
        out.push_back(kTab[(v >> 18) & 63]);
        out.push_back(kTab[(v >> 12) & 63]);
        out.push_back(i + 1 < length ? kTab[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

bool ClipboardHasImage() {
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) return false;
    return IsClipboardFormatAvailable(CF_BITMAP) != FALSE || IsClipboardFormatAvailable(CF_DIB) != FALSE;
}

EncodedImage EncodeImageFileForVision(const std::wstring& path, int max_edge, size_t max_bytes) {
    EncodedImage out;
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        out.error_message = "WIC imaging factory unavailable.";
        return out;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    const std::wstring win_path = Win32Path(path);
    if (FAILED(factory->CreateDecoderFromFilename(win_path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        out.error_message = "Could not decode image file.";
        return out;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        out.error_message = "Image has no decodable frame.";
        return out;
    }
    return EncodeSourceBounded(factory.Get(), frame.Get(), max_edge, max_bytes);
}

EncodedImage EncodeClipboardImageForVision(HWND hwnd, int max_edge, size_t max_bytes) {
    EncodedImage out;
    if (!OpenClipboard(hwnd)) {
        out.error_message = "Could not open clipboard.";
        return out;
    }
    HBITMAP bmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (!bmp) {
        CloseClipboard();
        out.error_message = "Clipboard does not contain a bitmap.";
        return out;
    }
    // Clone bitmap before closing clipboard so WIC reads safely.
    HBITMAP copy = static_cast<HBITMAP>(CopyImage(bmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    CloseClipboard();
    if (!copy) {
        out.error_message = "Failed to copy clipboard bitmap.";
        return out;
    }
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> wic_bmp;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateBitmapFromHBITMAP(copy, nullptr, WICBitmapIgnoreAlpha, &wic_bmp))) {
        out = EncodeSourceBounded(factory.Get(), wic_bmp.Get(), max_edge, max_bytes);
    } else {
        out.error_message = "Could not convert clipboard bitmap.";
    }
    DeleteObject(copy);
    return out;
}

} // namespace pulse::app::ai
