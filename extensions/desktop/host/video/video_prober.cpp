// video_prober.cpp - Media Foundation video probe, D3D11VA hardware decode check, and poster generator.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "host/render/wallpaper_backdrop.h"
#include "host/video/video_prober.h"
#include "host/video/video_decode_format_policy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

namespace pulse::desktop::video {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
std::atomic<uint64_t> device_creation_attempts{0};
std::atomic<uint64_t> jobs_holding_device{0};

std::array<GUID, 2> DecoderOutputSubtypes(REFGUID native_subtype, UINT32 profile) {
    if ((native_subtype == MFVideoFormat_H264 || native_subtype == MFVideoFormat_H264_ES) &&
        IsKnownEightBitAvcProfile(profile)) {
        return {MFVideoFormat_NV12, MFVideoFormat_P010};
    }
    return {MFVideoFormat_P010, MFVideoFormat_NV12};
}

// Standard DXVA/D3D11 AV1 Profile 0 GUID (may not be defined in older Windows SDK headers).
constexpr GUID kDecoderProfileAv1Main = {
    0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};

// Common codec subtypes not always exposed as macros across SDK versions.
constexpr GUID kVideoFormatVp9 = {
    0x30395056, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}; // 'VP90'
constexpr GUID kVideoFormatAv1 = {
    0x31305641, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}; // 'AV01'

// Scoped MFStartup / CoInitializeEx guard safe on any thread.
struct ScopedMf {
    HRESULT co_hr = E_FAIL;
    HRESULT mf_hr = E_FAIL;
    ScopedMf() noexcept {
        co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        mf_hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    }
    ~ScopedMf() {
        if (SUCCEEDED(mf_hr)) MFShutdown();
        if (SUCCEEDED(co_hr)) CoUninitialize();
    }
};

ComPtr<ID3D11Device> CreateTempVideoDevice() {
    device_creation_attempts.fetch_add(1, std::memory_order_relaxed);
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        return nullptr;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.As(&mt)) && mt) mt->SetMultithreadProtected(TRUE);
    return dev;
}

VideoCodecKind ClassifySubtype(const GUID& sub, std::wstring& out_name) {
    if (sub == MFVideoFormat_H264 || sub == MFVideoFormat_H264_ES) {
        out_name = L"H.264";
        return VideoCodecKind::H264;
    }
    if (sub == MFVideoFormat_HEVC || sub == MFVideoFormat_HEVC_ES) {
        out_name = L"HEVC";
        return VideoCodecKind::HEVC;
    }
    if (sub == kVideoFormatVp9 || sub == MFVideoFormat_VP90) {
        out_name = L"VP9";
        return VideoCodecKind::VP9;
    }
    if (sub == kVideoFormatAv1 || sub == MFVideoFormat_AV1) {
        out_name = L"AV1";
        return VideoCodecKind::AV1;
    }
    out_name = L"Video";
    return VideoCodecKind::Other;
}

bool HasMftForCategory(const GUID& category, const GUID& in_sub, const GUID& out_sub, bool hw_only) {
    MFT_REGISTER_TYPE_INFO in_t{MFMediaType_Video, in_sub};
    MFT_REGISTER_TYPE_INFO out_t{MFMediaType_Video, out_sub};
    const MFT_REGISTER_TYPE_INFO* p_in = (in_sub == GUID_NULL) ? nullptr : &in_t;
    const MFT_REGISTER_TYPE_INFO* p_out = (out_sub == GUID_NULL) ? nullptr : &out_t;
    const UINT32 flags = hw_only
                             ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER)
                             : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                                MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER);
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(category, flags, p_in, p_out, &acts, &count);
    if (acts) {
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    return SUCCEEDED(hr) && count > 0;
}

bool HasDecoderMftForSubtype(const GUID& sub) {
    if (HasMftForCategory(MFT_CATEGORY_VIDEO_DECODER, sub, GUID_NULL, false)) return true;
    if (sub == MFVideoFormat_HEVC) {
        return HasMftForCategory(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_HEVC_ES, GUID_NULL, false);
    }
    return false;
}

void ReadColorInfo(IMFMediaType* mt, VideoColorInfo& out) noexcept {
    if (!mt) return;
    UINT32 v = 0;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_YUV_MATRIX, &v))) out.yuv_matrix = v;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v))) out.nominal_range = v;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_VIDEO_PRIMARIES, &v))) out.primaries = v;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_TRANSFER_FUNCTION, &v))) out.transfer_func = v;

    // PQ (SMPTE ST 2084 = 15) or HLG (ARIB STD-B67 = 16) or BT.2020 primaries (9) with PQ/HLG.
    const bool is_pq = (out.transfer_func == MFVideoTransFunc_2084);
    const bool is_hlg = (out.transfer_func == MFVideoTransFunc_HLG);
    const bool is_bt2020 = (out.primaries == MFVideoPrimaries_BT2020) ||
                           (out.yuv_matrix == MFVideoTransferMatrix_BT2020_10) ||
                           (out.yuv_matrix == MFVideoTransferMatrix_BT2020_12);
    out.is_hdr = is_pq || is_hlg || is_bt2020;
}

bool SaveJpegBgraAtomic(const std::wstring& path, UINT w, UINT h, const uint8_t* bgra, UINT stride,
                       const VideoProbeJob::Cancelled& cancelled) {
    if (path.empty() || !bgra || w == 0 || h == 0) return false;
    if (cancelled && cancelled()) return false;
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    GUID id{};
    wchar_t suffix[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, suffix, 40)) return false;
    const std::wstring tmp = path + L"." + suffix + L".tmp.jpg";
    HANDLE reserved = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reserved == INVALID_HANDLE_VALUE) return false;
    CloseHandle(reserved);
    struct TempFileCleanup {
        const std::wstring& path;
        ~TempFileCleanup() { DeleteFileW(path.c_str()); }
    } cleanup{tmp};

    // Convert BGRA -> 24bpp BGR (opaque JPEG).
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, &props))) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_R4;
        var.fltVal = 0.88f;
        props->Write(1, &opt, &var);
    }
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(w, h))) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt))) {
        DeleteFileW(tmp.c_str());
        return false;
    }

    const UINT bgr_stride = (w * 3u + 3u) & ~3u;
    std::vector<uint8_t> bgr(static_cast<size_t>(bgr_stride) * h);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* src_row = bgra + static_cast<size_t>(y) * stride;
        uint8_t* dst_row = bgr.data() + static_cast<size_t>(y) * bgr_stride;
        for (UINT x = 0; x < w; ++x) {
            dst_row[x * 3 + 0] = src_row[x * 4 + 0];
            dst_row[x * 3 + 1] = src_row[x * 4 + 1];
            dst_row[x * 3 + 2] = src_row[x * 4 + 2];
        }
    }
    const bool ok = SUCCEEDED(frame->WritePixels(h, bgr_stride, static_cast<UINT>(bgr.size()), bgr.data())) &&
                    SUCCEEDED(frame->Commit()) &&
                    SUCCEEDED(enc->Commit());
    frame.Reset();
    enc.Reset();
    stream.Reset();
    if (!ok || (cancelled && cancelled())) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

uint64_t VideoProbeDeviceCreationAttempts() noexcept {
    return device_creation_attempts.load(std::memory_order_relaxed);
}

uint64_t VideoProbeJobsHoldingDevice() noexcept {
    return jobs_holding_device.load(std::memory_order_relaxed);
}

static GpuCapabilities ProbeGpuCapabilitiesImpl(ID3D11Device* existing_device, bool create_device) {
    ScopedMf mf;
    GpuCapabilities caps{};

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        caps.system_ram_bytes = ms.ullTotalPhys;
        caps.available_ram_bytes = ms.ullAvailPhys;
    }

    ComPtr<ID3D11Device> dev = existing_device ? ComPtr<ID3D11Device>(existing_device)
                                             : create_device ? CreateTempVideoDevice() : nullptr;
    if (dev) {
        ComPtr<IDXGIDevice> dxgi_dev;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dev.As(&dxgi_dev)) && SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && adapter) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                caps.dedicated_vram_bytes = desc.DedicatedVideoMemory;
                // Integrated GPUs typically report <= 512 MB dedicated VRAM (Intel UHD/Iris, AMD APU)
                // or vendor Intel (0x8086) with small dedicated pool.
                caps.is_integrated_gpu = (desc.DedicatedVideoMemory < 512ull * 1024ull * 1024ull);
            }
        }
    }

    caps.has_hevc_decoder = HasDecoderMftForSubtype(MFVideoFormat_HEVC);
    caps.has_hevc_hw_encoder = HasMftForCategory(MFT_CATEGORY_VIDEO_ENCODER, GUID_NULL, MFVideoFormat_HEVC, true);
    caps.has_h264_hw_encoder = HasMftForCategory(MFT_CATEGORY_VIDEO_ENCODER, GUID_NULL, MFVideoFormat_H264, true);
    caps.has_h264_sw_encoder = HasMftForCategory(MFT_CATEGORY_VIDEO_ENCODER, GUID_NULL, MFVideoFormat_H264, false);
    return caps;
}

GpuCapabilities ProbeGpuCapabilities(ID3D11Device* existing_device) {
    return ProbeGpuCapabilitiesImpl(existing_device, true);
}

bool CheckHardwareDecodeSupport(ID3D11Device* d3d_device,
                                VideoCodecKind codec,
                                uint8_t bit_depth,
                                uint32_t width,
                                uint32_t height,
                                bool* out_profile_found,
                                uint32_t* out_config_count) {
    if (out_profile_found) *out_profile_found = false;
    if (out_config_count) *out_config_count = 0;
    if (!d3d_device || width == 0 || height == 0) return false;

    ComPtr<ID3D11VideoDevice> vdev;
    if (FAILED(d3d_device->QueryInterface(IID_PPV_ARGS(&vdev))) || !vdev) return false;

    GUID wanted = GUID_NULL;
    DXGI_FORMAT fmt = (bit_depth >= 10) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    switch (codec) {
    case VideoCodecKind::H264:
        if (bit_depth >= 10) return false; // Standard DXVA H.264 HW decoders are 8-bit only.
        wanted = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
        fmt = DXGI_FORMAT_NV12;
        break;
    case VideoCodecKind::HEVC:
        wanted = (bit_depth >= 10) ? D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10
                                   : D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
        break;
    case VideoCodecKind::VP9:
        wanted = (bit_depth >= 10) ? D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2
                                   : D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0;
        break;
    case VideoCodecKind::AV1:
        wanted = kDecoderProfileAv1Main;
        break;
    default:
        return false;
    }

    const UINT profile_count = vdev->GetVideoDecoderProfileCount();
    bool found = false;
    for (UINT i = 0; i < profile_count; ++i) {
        GUID g{};
        if (SUCCEEDED(vdev->GetVideoDecoderProfile(i, &g)) && g == wanted) {
            found = true;
            break;
        }
    }
    if (out_profile_found) *out_profile_found = found;
    if (!found) return false;

    BOOL fmt_ok = FALSE;
    if (FAILED(vdev->CheckVideoDecoderFormat(&wanted, fmt, &fmt_ok)) || !fmt_ok) {
        return false;
    }

    D3D11_VIDEO_DECODER_DESC desc{};
    desc.Guid = wanted;
    desc.SampleWidth = width;
    desc.SampleHeight = height;
    desc.OutputFormat = fmt;

    UINT cfg_count = 0;
    if (FAILED(vdev->GetVideoDecoderConfigCount(&desc, &cfg_count))) {
        return false;
    }
    if (out_config_count) *out_config_count = cfg_count;
    if (cfg_count == 0) return false;

    // Driver verification: some drivers ignore SampleWidth/SampleHeight in GetVideoDecoderConfigCount
    // and only enforce hardware resolution limits (e.g., 4096x4096 H.264 on NVIDIA) inside
    // CreateVideoDecoder or MF's decoder caps. Check explicit D3D11_VIDEO_DECODER_CONFIG + H.264
    // >4096 verification via lightweight CreateVideoDecoder probe when resolution > 4096.
    if (width > 4096u || height > 4096u) {
        D3D11_VIDEO_DECODER_CONFIG cfg{};
        if (SUCCEEDED(vdev->GetVideoDecoderConfig(&desc, 0, &cfg))) {
            ComPtr<ID3D11VideoDecoder> test_dec;
            if (FAILED(vdev->CreateVideoDecoder(&desc, &cfg, &test_dec)) || !test_dec) {
                return false;
            }
        }
    }
    return true;
}

static VideoProbeInfo ProbeVideoFileImpl(const std::wstring& path, ID3D11Device* d3d_device, bool create_device) {
    ScopedMf mf;
    VideoProbeInfo info{};
    info.source_path = path;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        info.file_exists = false;
        info.error_detail = L"file not found";
        return info;
    }
    info.file_exists = true;
    info.file_size_bytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    info.file_mtime_100ns = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                            fad.ftLastWriteTime.dwLowDateTime;

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr) || !reader) {
        info.error_detail = L"MFCreateSourceReaderFromURL failed";
        return info;
    }

    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> native;
    hr = reader->GetNativeMediaType(vs, 0, &native);
    if (FAILED(hr) || !native) {
        info.error_detail = L"no video stream";
        return info;
    }

    GUID sub{};
    native->GetGUID(MF_MT_SUBTYPE, &sub);
    info.codec = ClassifySubtype(sub, info.codec_name);
    if (info.codec == VideoCodecKind::HEVC) {
        info.store_uri = L"ms-windows-store://pdp/?ProductId=9NMZLZ57R3T7";
    } else if (info.codec == VideoCodecKind::AV1) {
        info.store_uri = L"ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V";
    }

    UINT32 cw = 0, ch = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
    info.coded_width = cw;
    info.coded_height = ch;
    info.display_width = cw;
    info.display_height = ch;
    info.crop_left = 0;
    info.crop_top = 0;
    info.crop_right = static_cast<int32_t>(cw);
    info.crop_bottom = static_cast<int32_t>(ch);

    MFVideoArea area{};
    if (SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
        info.crop_left = static_cast<int32_t>(area.OffsetX.value);
        info.crop_top = static_cast<int32_t>(area.OffsetY.value);
        info.display_width = static_cast<uint32_t>(area.Area.cx);
        info.display_height = static_cast<uint32_t>(area.Area.cy);
        info.crop_right = info.crop_left + static_cast<int32_t>(area.Area.cx);
        info.crop_bottom = info.crop_top + static_cast<int32_t>(area.Area.cy);
    }

    UINT32 num = 30, den = 1;
    if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0) {
        info.fps_num = num;
        info.fps_den = den;
    }

    PROPVARIANT dur{};
    PropVariantInit(&dur);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &dur))) {
        if (dur.vt == VT_UI8) info.duration_100ns = dur.uhVal.QuadPart;
        else if (dur.vt == VT_I8 && dur.hVal.QuadPart > 0) info.duration_100ns = static_cast<uint64_t>(dur.hVal.QuadPart);
    }
    PropVariantClear(&dur);

    ReadColorInfo(native.Get(), info.color);

    // Detect 10-bit profile from native metadata before attempting decoder negotiation.
    UINT32 profile = 0;
    if (SUCCEEDED(native->GetUINT32(MF_MT_MPEG2_PROFILE, &profile)) ||
        SUCCEEDED(native->GetUINT32(MF_MT_VIDEO_PROFILE, &profile))) {
        if (info.codec == VideoCodecKind::HEVC && profile == 2u) { // Main10
            info.bit_depth = 10;
        } else if (info.codec == VideoCodecKind::H264 && profile == 110u) { // High 10
            info.bit_depth = 10;
        }
    }

    info.has_mf_decoder = HasDecoderMftForSubtype(sub);
    info.open_ok = (info.display_width > 0 && info.display_height > 0);

    // If a decoder MFT exists, also check whether negotiating P010 vs NV12 reveals 10-bit or
    // refined color / aperture attributes (without reading/decoding frames).
    if (info.has_mf_decoder && info.open_ok) {
        ComPtr<ID3D11Device> dev = d3d_device ? ComPtr<ID3D11Device>(d3d_device)
                                           : create_device ? CreateTempVideoDevice() : nullptr;
        ComPtr<IMFDXGIDeviceManager> mgr;
        UINT token = 0;
        ComPtr<IMFAttributes> ra;
        MFCreateAttributes(&ra, 2);
        if (dev && SUCCEEDED(MFCreateDXGIDeviceManager(&token, &mgr)) &&
            SUCCEEDED(mgr->ResetDevice(dev.Get(), token))) {
            ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr.Get());
        }
        ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        ComPtr<IMFSourceReader> probe_reader;
        if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), ra.Get(), &probe_reader)) && probe_reader) {
            probe_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            probe_reader->SetStreamSelection(vs, TRUE);
            bool negotiated = false;
            for (const GUID& want : DecoderOutputSubtypes(sub, profile)) {
                ComPtr<IMFMediaType> t;
                MFCreateMediaType(&t);
                t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                t->SetGUID(MF_MT_SUBTYPE, want);
                if (SUCCEEDED(probe_reader->SetCurrentMediaType(vs, nullptr, t.Get()))) {
                    if (want == MFVideoFormat_P010) info.bit_depth = 10;
                    negotiated = true;
                    break;
                }
            }
            if (!negotiated) {
                info.has_mf_decoder = false;
            } else {
                ComPtr<IMFMediaType> cur;
                if (SUCCEEDED(probe_reader->GetCurrentMediaType(vs, &cur)) && cur) {
                    if (info.color.yuv_matrix == 0 || !info.color.is_hdr) {
                        VideoColorInfo refined = info.color;
                        ReadColorInfo(cur.Get(), refined);
                        if (info.color.yuv_matrix == 0) info.color.yuv_matrix = refined.yuv_matrix;
                        if (info.color.nominal_range == 0) info.color.nominal_range = refined.nominal_range;
                        if (info.color.primaries == 0) info.color.primaries = refined.primaries;
                        if (info.color.transfer_func == 0) info.color.transfer_func = refined.transfer_func;
                        info.color.is_hdr = info.color.is_hdr || refined.is_hdr;
                    }
                    MFVideoArea cur_area{};
                    if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                               reinterpret_cast<UINT8*>(&cur_area), sizeof(cur_area), nullptr)) &&
                        cur_area.Area.cx > 0 && cur_area.Area.cy > 0) {
                        info.crop_left = static_cast<int32_t>(cur_area.OffsetX.value);
                        info.crop_top = static_cast<int32_t>(cur_area.OffsetY.value);
                        info.display_width = static_cast<uint32_t>(cur_area.Area.cx);
                        info.display_height = static_cast<uint32_t>(cur_area.Area.cy);
                        info.crop_right = info.crop_left + static_cast<int32_t>(cur_area.Area.cx);
                        info.crop_bottom = info.crop_top + static_cast<int32_t>(cur_area.Area.cy);
                    }
                }
            }
        }

        if (dev && info.has_mf_decoder) {
            info.hw_decode_supported = CheckHardwareDecodeSupport(
                dev.Get(), info.codec, info.bit_depth,
                info.coded_width ? info.coded_width : info.display_width,
                info.coded_height ? info.coded_height : info.display_height);
        }
    }

    return info;
}

VideoProbeInfo ProbeVideoFile(const std::wstring& path, ID3D11Device* d3d_device) {
    return ProbeVideoFileImpl(path, d3d_device, true);
}

static bool ExtractPosterJpegImpl(const std::wstring& video_path,
                       const std::wstring& poster_jpg_path,
                       uint32_t max_w,
                       uint32_t max_h,
                       uint32_t* out_accent_argb,
                       ID3D11Device* d3d_device, bool create_device,
                       const VideoProbeJob::Cancelled& cancelled) {
    ScopedMf mf;
    if (video_path.empty() || poster_jpg_path.empty() || max_w < 2 || max_h < 2) return false;
    if (cancelled && cancelled()) return false;

    ComPtr<ID3D11Device> dev = d3d_device ? ComPtr<ID3D11Device>(d3d_device)
                                       : create_device ? CreateTempVideoDevice() : nullptr;
    if (!dev) return false;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    if (FAILED(dev.As(&vdev)) || FAILED(ctx.As(&vctx))) return false;

    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> mgr;
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 2);
    if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &mgr)) && SUCCEEDED(mgr->ResetDevice(dev.Get(), token))) {
        ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr.Get());
    }
    ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(video_path.c_str(), ra.Get(), &reader)) || !reader) return false;
    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(vs, TRUE);

    ComPtr<IMFMediaType> native;
    GUID native_subtype{};
    UINT32 native_profile = 0;
    if (SUCCEEDED(reader->GetNativeMediaType(vs, 0, &native)) && native) {
        native->GetGUID(MF_MT_SUBTYPE, &native_subtype);
        if (FAILED(native->GetUINT32(MF_MT_MPEG2_PROFILE, &native_profile))) {
            native->GetUINT32(MF_MT_VIDEO_PROFILE, &native_profile);
        }
    }
    DXGI_FORMAT in_fmt = DXGI_FORMAT_NV12;
    bool typed = false;
    for (const GUID& want : DecoderOutputSubtypes(native_subtype, native_profile)) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, want);
        if (SUCCEEDED(reader->SetCurrentMediaType(vs, nullptr, t.Get()))) {
            in_fmt = (want == MFVideoFormat_P010) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
            typed = true;
            break;
        }
    }
    if (!typed) return false;

    ComPtr<IMFSample> sample;
    for (int attempt = 0; attempt < 32 && !sample; ++attempt) {
        if (cancelled && cancelled()) return false;
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(reader->ReadSample(vs, 0, &idx, &flags, &ts, &s))) return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (s) sample = std::move(s);
    }
    if (!sample) return false;
    if (cancelled && cancelled()) return false;

    ComPtr<IMFMediaType> cur;
    if (FAILED(reader->GetCurrentMediaType(vs, &cur)) || !cur) return false;
    UINT32 cw = 0, ch = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
    if (cw == 0 || ch == 0) return false;

    RECT src_rect{0, 0, static_cast<LONG>(cw), static_cast<LONG>(ch)};
    UINT vw = cw, vh = ch;
    MFVideoArea area{};
    if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
        src_rect.left = static_cast<LONG>(area.OffsetX.value);
        src_rect.top = static_cast<LONG>(area.OffsetY.value);
        src_rect.right = src_rect.left + area.Area.cx;
        src_rect.bottom = src_rect.top + area.Area.cy;
        vw = static_cast<UINT>(area.Area.cx);
        vh = static_cast<UINT>(area.Area.cy);
    }

    VideoColorInfo cinfo{};
    ReadColorInfo(cur.Get(), cinfo);
    uint32_t ow = 0, oh = 0;
    ComputeAspectFitEvenSize(vw, vh, max_w, max_h, ow, oh);

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf)) || !buf) return false;
    ComPtr<ID3D11Texture2D> in_tex;
    UINT slice = 0;
    ComPtr<IMFDXGIBuffer> dxgi_buf;
    if (SUCCEEDED(buf.As(&dxgi_buf)) && SUCCEEDED(dxgi_buf->GetResource(IID_PPV_ARGS(&in_tex))) && in_tex) {
        dxgi_buf->GetSubresourceIndex(&slice);
    } else {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = cw;
        td.Height = ch;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = in_fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &in_tex))) return false;
        ComPtr<IMF2DBuffer> b2;
        BYTE* data = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch))) {
            ctx->UpdateSubresource(in_tex.Get(), 0, nullptr, data, static_cast<UINT>(pitch), 0);
            b2->Unlock2D();
        } else {
            DWORD len = 0;
            if (FAILED(buf->Lock(&data, nullptr, &len))) return false;
            ctx->UpdateSubresource(in_tex.Get(), 0, nullptr, data, cw * (in_fmt == DXGI_FORMAT_P010 ? 2u : 1u), 0);
            buf->Unlock();
        }
    }

    D3D11_TEXTURE2D_DESC in_desc{};
    in_tex->GetDesc(&in_desc);
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = in_desc.Width;
    cd.InputHeight = in_desc.Height;
    cd.OutputWidth = ow;
    cd.OutputHeight = oh;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;

    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &venum)) ||
        FAILED(vdev->CreateVideoProcessor(venum.Get(), 0, &vp))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC out_desc{};
    out_desc.Width = ow;
    out_desc.Height = oh;
    out_desc.MipLevels = 1;
    out_desc.ArraySize = 1;
    out_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    out_desc.SampleDesc.Count = 1;
    out_desc.Usage = D3D11_USAGE_DEFAULT;
    out_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> out_tex;
    if (FAILED(dev->CreateTexture2D(&out_desc, nullptr, &out_tex))) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = slice;
    ComPtr<ID3D11VideoProcessorInputView> iv;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> ov;
    if (FAILED(vdev->CreateVideoProcessorInputView(in_tex.Get(), venum.Get(), &ivd, &iv)) ||
        FAILED(vdev->CreateVideoProcessorOutputView(out_tex.Get(), venum.Get(), &ovd, &ov))) {
        return false;
    }

    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(vctx.As(&vctx1)) && vctx1) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, MapMfToDxgiColorSpace(cinfo, vw, vh));
        vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
    const RECT dst_rect{0, 0, static_cast<LONG>(ow), static_cast<LONG>(oh)};
    vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src_rect);
    vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst_rect);
    vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst_rect);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);

    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = iv.Get();
    if (FAILED(vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, &st))) return false;

    D3D11_TEXTURE2D_DESC stg_desc = out_desc;
    stg_desc.Usage = D3D11_USAGE_STAGING;
    stg_desc.BindFlags = 0;
    stg_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&stg_desc, nullptr, &staging))) return false;
    ctx->CopyResource(staging.Get(), out_tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) return false;
    const auto* pixels = static_cast<const uint8_t*>(mapped.pData);
    const uint32_t accent = out_accent_argb
        ? render::ExtractVibrantAccentArgb(pixels, ow, oh, mapped.RowPitch) : 0;
    const bool saved = SaveJpegBgraAtomic(poster_jpg_path, ow, oh, pixels, mapped.RowPitch, cancelled);
    ctx->Unmap(staging.Get(), 0);
    if (saved && out_accent_argb) *out_accent_argb = accent;
    return saved;
}

bool ExtractPosterJpeg(const std::wstring& video_path, const std::wstring& poster_jpg_path,
                       uint32_t max_w, uint32_t max_h, uint32_t* out_accent_argb,
                       ID3D11Device* d3d_device) {
    return ExtractPosterJpegImpl(video_path, poster_jpg_path, max_w, max_h, out_accent_argb,
                                 d3d_device, true, {});
}

VideoProbeJob::VideoProbeJob(Cancelled cancelled, DeviceFactory factory)
    : cancelled_(std::move(cancelled)) {
    if (IsCancelled()) { closed_ = true; return; }
    ++device_creation_attempts_;
    device_ = factory ? factory() : CreateTempVideoDevice();
    if (device_) jobs_holding_device.fetch_add(1, std::memory_order_relaxed);
}

VideoProbeJob::~VideoProbeJob() { Reset(); }

bool VideoProbeJob::IsCancelled() const {
    return closed_ || (cancelled_ && cancelled_());
}

GpuCapabilities VideoProbeJob::ProbeCapabilities() {
    if (IsCancelled()) return {};
    return ProbeGpuCapabilitiesImpl(device_.Get(), false);
}

VideoProbeInfo VideoProbeJob::ProbeFile(const std::wstring& path) {
    if (IsCancelled()) return {};
    return ProbeVideoFileImpl(path, device_.Get(), false);
}

bool VideoProbeJob::ExtractPoster(const std::wstring& path, const std::wstring& destination,
                                 uint32_t max_w, uint32_t max_h, uint32_t* accent) {
    if (IsCancelled() || !device_) return false;
    return ExtractPosterJpegImpl(path, destination, max_w, max_h, accent, device_.Get(), false,
                                 [this] { return IsCancelled(); });
}

void VideoProbeJob::Reset() noexcept {
    closed_ = true;
    const bool held_device = !!device_;
    if (device_) {
        // This job exclusively owns its device; all reader/processor locals have
        // already gone out of scope. Retire deferred objects and driver caches once,
        // rather than retaining them after the background job has finished.
        ComPtr<ID3D11DeviceContext> context;
        device_->GetImmediateContext(&context);
        if (context) {
            context->ClearState();
            context->Flush();
        }
        ComPtr<IDXGIDevice3> dxgi;
        if (SUCCEEDED(device_.As(&dxgi))) dxgi->Trim();
    }
    device_.Reset();
    if (held_device) jobs_holding_device.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace pulse::desktop::video
