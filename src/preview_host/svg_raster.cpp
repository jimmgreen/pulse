// svg_raster.cpp — Direct2D rasterization of SVG previews.
//
// The preview host decodes raster images through WIC, which has no SVG decoder.
// Direct2D can render SVG natively (ID2D1DeviceContext5::CreateSvgDocument), so
// this file owns that path: one D3D/D2D stack is built on first use and kept for
// the life of the process, a hardware device is preferred over the WARP software
// rasterizer, and documents that are too large to parse inside the UI's request
// budget are refused instead of risking a timeout.
#include "svg_raster.h"

#include <d2d1_3.h>
#include <d2d1svg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace pulse::preview {
namespace {

// Refused above this size: the UI allows one second per request, and a
// generated SVG can be orders of magnitude larger than a useful preview. There
// is no way to interrupt Direct2D once it starts parsing.
constexpr uint64_t kMaxSvgBytes = 8ull * 1024ull * 1024ull;
// Viewport of the first parse, before the document's own size is known. It is
// replaced by the intrinsic size right after, so it only has to be parseable.
constexpr float kProbeEdge = 1024.0f;
// Upper bound for a declared size, so a bogus width/height cannot ask for an
// absurd bitmap.
constexpr float kMaxDeclaredEdge = 32768.0f;

struct SvgStack {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext5> context;
    bool attempted = false;
    bool ready = false;
};

// Built at most once per process, and only once an SVG is actually requested:
// the preview host is long-lived (the UI reuses a single child for the whole
// session), so a user who never opens an SVG never pays for a device.
SvgStack& Stack() {
    static SvgStack stack;
    return stack;
}

void ResetStack() {
    SvgStack& stack = Stack();
    stack.context.Reset();
    stack.device.Reset();
    stack.factory.Reset();
    stack.d3d.Reset();
    stack.ready = false;
    stack.attempted = false;
}

// d3d11/d2d1/dxgi are resolved on demand rather than imported. An implicit
// import loads all three DLLs at every preview host start-up — that is, on the
// first preview of a session for *any* file type — even though only SVG needs
// them. Loading them here moves that cost to the first SVG the user opens.
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
    UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D2D1CreateFactoryFn = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID,
    const D2D1_FACTORY_OPTIONS*, void**);

D3D11CreateDeviceFn D3D11CreateDeviceProc() {
    static const D3D11CreateDeviceFn proc = [] {
        const HMODULE module = LoadLibraryW(L"d3d11.dll");
        return module ? reinterpret_cast<D3D11CreateDeviceFn>(
            GetProcAddress(module, "D3D11CreateDevice")) : nullptr;
    }();
    return proc;
}

D2D1CreateFactoryFn D2D1CreateFactoryProc() {
    static const D2D1CreateFactoryFn proc = [] {
        const HMODULE module = LoadLibraryW(L"d2d1.dll");
        return module ? reinterpret_cast<D2D1CreateFactoryFn>(
            GetProcAddress(module, "D2D1CreateFactory")) : nullptr;
    }();
    return proc;
}

bool CreateD3DDevice(D3D_DRIVER_TYPE driver, ComPtr<ID3D11Device>& out) {
    const D3D11CreateDeviceFn create_device = D3D11CreateDeviceProc();
    if (!create_device) return false;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL reached{};
    return SUCCEEDED(create_device(nullptr, driver, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, out.GetAddressOf(), &reached, nullptr));
}

bool EnsureStack() {
    SvgStack& stack = Stack();
    if (stack.ready) return true;
    // A failed attempt is remembered: a machine with no usable device should
    // not repeat the whole creation sequence for every SVG in a folder.
    if (stack.attempted) return false;
    stack.attempted = true;

    ComPtr<ID3D11Device> d3d;
    // Hardware first, then the software rasterizer, which also covers remote
    // sessions and machines without a usable GPU driver.
    if (!CreateD3DDevice(D3D_DRIVER_TYPE_HARDWARE, d3d) &&
        !CreateD3DDevice(D3D_DRIVER_TYPE_WARP, d3d))
        return false;

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d.As(&dxgi))) return false;

    const D2D1CreateFactoryFn create_factory = D2D1CreateFactoryProc();
    if (!create_factory) return false;
    D2D1_FACTORY_OPTIONS options{};
    ComPtr<ID2D1Factory1> factory;
    if (FAILED(create_factory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &options,
            reinterpret_cast<void**>(factory.GetAddressOf()))))
        return false;

    ComPtr<ID2D1Device> device;
    if (FAILED(factory->CreateDevice(dxgi.Get(), &device))) return false;

    ComPtr<ID2D1DeviceContext> context;
    if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)))
        return false;

    // SVG support arrives with the Windows 10 1703 interface set. On anything
    // older the previous behaviour (SVG as text) remains the fallback.
    ComPtr<ID2D1DeviceContext5> context5;
    if (FAILED(context.As(&context5))) return false;

    stack.d3d = std::move(d3d);
    stack.factory = std::move(factory);
    stack.device = std::move(device);
    stack.context = std::move(context5);
    stack.ready = true;
    return true;
}

bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart > 0 &&
        static_cast<uint64_t>(size.QuadPart) <= kMaxSvgBytes;
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != 0;
        if (ok) bytes.resize(read);
    }
    CloseHandle(file);
    if (!ok) bytes.clear();
    return ok;
}

bool MakeStream(const std::vector<unsigned char>& bytes, ComPtr<IStream>& stream) {
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return false;
    ULONG written = 0;
    if (FAILED(stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written)) ||
        written != bytes.size())
        return false;
    LARGE_INTEGER origin{};
    return SUCCEEDED(stream->Seek(origin, STREAM_SEEK_SET, nullptr));
}

// The document's own notion of its size: viewBox wins, then width/height.
// Percentage units are ignored because they resolve against a viewport that we
// are choosing ourselves.
bool IntrinsicSize(ID2D1SvgDocument* document, float& width, float& height) {
    if (!document) return false;
    ComPtr<ID2D1SvgElement> root;
    document->GetRoot(&root);  // ID2D1SvgDocument::GetRoot returns void
    if (!root) return false;

    // D2D1_SVG_LENGTH has an inline GetAttributeValue overload but
    // D2D1_SVG_VIEWBOX does not, so the viewBox goes through the generic POD
    // entry point with an explicit type and size.
    D2D1_SVG_VIEWBOX viewbox{};
    if (SUCCEEDED(root->GetAttributeValue(L"viewBox",
            D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &viewbox, sizeof(viewbox))) &&
        viewbox.width > 0.0f && viewbox.height > 0.0f &&
        viewbox.width < kMaxDeclaredEdge && viewbox.height < kMaxDeclaredEdge) {
        width = viewbox.width;
        height = viewbox.height;
        return true;
    }

    D2D1_SVG_LENGTH declared_width{};
    D2D1_SVG_LENGTH declared_height{};
    const bool has_width = SUCCEEDED(root->GetAttributeValue(L"width", &declared_width)) &&
        declared_width.units == D2D1_SVG_LENGTH_UNITS_NUMBER &&
        declared_width.value > 0.0f && declared_width.value < kMaxDeclaredEdge;
    const bool has_height = SUCCEEDED(root->GetAttributeValue(L"height", &declared_height)) &&
        declared_height.units == D2D1_SVG_LENGTH_UNITS_NUMBER &&
        declared_height.value > 0.0f && declared_height.value < kMaxDeclaredEdge;
    if (has_width && has_height) {
        width = declared_width.value;
        height = declared_height.value;
        return true;
    }
    return false;
}

} // namespace

bool RasterizeSvgFile(const std::wstring& path, UINT max_edge,
                      std::vector<unsigned char>& pixels,
                      UINT& width, UINT& height, UINT& stride,
                      UINT& source_width, UINT& source_height,
                      std::wstring* error) {
    const auto fail = [error](const wchar_t* text) {
        if (error) *error = text;
        return false;
    };
    pixels.clear();
    if (max_edge == 0) max_edge = static_cast<UINT>(kProbeEdge);

    std::vector<unsigned char> bytes;
    if (!ReadWholeFile(path, bytes)) return fail(L"svg-unreadable");

    ComPtr<IStream> stream;
    if (!MakeStream(bytes, stream)) return fail(L"svg-stream-failed");

    SvgStack& stack = Stack();
    if (!EnsureStack()) return fail(L"svg-no-device");
    ID2D1DeviceContext5* dc = stack.context.Get();

    ComPtr<ID2D1SvgDocument> document;
    if (FAILED(dc->CreateSvgDocument(stream.Get(),
            D2D1::SizeF(kProbeEdge, kProbeEdge), &document)) || !document)
        return fail(L"svg-parse-failed");

    float document_width = kProbeEdge;
    float document_height = kProbeEdge;
    if (IntrinsicSize(document.Get(), document_width, document_height)) {
        // The probe viewport existed only to get a parseable document; the real
        // one keeps the intrinsic aspect ratio, so the raster has no letterbox.
        document->SetViewportSize(D2D1::SizeF(document_width, document_height));
    } else {
        document_width = document_height = kProbeEdge;
    }

    const float longest = (std::max)(document_width, document_height);
    const float scale = longest > static_cast<float>(max_edge)
        ? static_cast<float>(max_edge) / longest : 1.0f;
    const UINT out_width = (std::max)(1u, static_cast<UINT>(document_width * scale + 0.5f));
    const UINT out_height = (std::max)(1u, static_cast<UINT>(document_height * scale + 0.5f));

    const D2D1_PIXEL_FORMAT format =
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const D2D1_BITMAP_PROPERTIES1 target_properties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format);
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(out_width, out_height), nullptr, 0,
                                &target_properties, &target)))
        return fail(L"svg-target-failed");

    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->DrawSvgDocument(document.Get());
    const HRESULT drawn = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (FAILED(drawn)) {
        // A lost device must be rebuilt before anything else can be drawn.
        if (drawn == D2DERR_RECREATE_TARGET) ResetStack();
        return fail(L"svg-draw-failed");
    }

    const D2D1_BITMAP_PROPERTIES1 read_properties = D2D1::BitmapProperties1(
        static_cast<D2D1_BITMAP_OPTIONS>(D2D1_BITMAP_OPTIONS_CPU_READ |
                                         D2D1_BITMAP_OPTIONS_CANNOT_DRAW),
        format);
    ComPtr<ID2D1Bitmap1> staging;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(out_width, out_height), nullptr, 0,
                                &read_properties, &staging)) ||
        FAILED(staging->CopyFromBitmap(nullptr, target.Get(), nullptr)))
        return fail(L"svg-readback-failed");

    D2D1_MAPPED_RECT mapped{};
    if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
        return fail(L"svg-map-failed");

    stride = out_width * 4;
    pixels.resize(static_cast<size_t>(stride) * out_height);
    for (UINT row = 0; row < out_height; ++row) {
        std::memcpy(pixels.data() + static_cast<size_t>(row) * stride,
                    mapped.bits + static_cast<size_t>(row) * mapped.pitch, stride);
    }
    staging->Unmap();

    width = out_width;
    height = out_height;
    source_width = (std::max)(1u, static_cast<UINT>(document_width + 0.5f));
    source_height = (std::max)(1u, static_cast<UINT>(document_height + 0.5f));
    return true;
}

} // namespace pulse::preview
