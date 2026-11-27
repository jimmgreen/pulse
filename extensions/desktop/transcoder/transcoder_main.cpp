// transcoder_main.cpp - Isolated background video wallpaper transcoder process
// (`pulse_wallpaper_transcoder.exe`). Uses pure Windows Media Foundation + D3D11 VideoProcessor
// (zero FFmpeg dependency). Runs inside a Job Object with memory cap and low CPU/IO priority.
//
// Usage:
//   pulse_wallpaper_transcoder.exe --input <src> --output <tmp.mp4> --width <W> --height <H>
//                                  [--codec hevc_main10|hevc_main|h264_hw|h264_sw|auto]
//                                  [--pipe <pipe_name>]
//
// Exit codes:
//   0 = OK (Finalized)
//   2 = Bad arguments
//   3 = Cancelled
//   4 = Out of memory / job limit exceeded
//   5 = Missing codec / unsupported stream
//   6 = General transcode failure
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <codecapi.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "host/video/video_color_space.h"
#include "host/video/video_wallpaper_types.h"
#include "ipc/desktop_ipc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitBadArgs = 2;
constexpr int kExitCancelled = 3;
constexpr int kExitOom = 4;
constexpr int kExitCodecMissing = 5;
constexpr int kExitError = 6;

bool IsOomHresult(HRESULT hr) noexcept {
    return hr == E_OUTOFMEMORY ||
           hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY) ||
           hr == HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY) ||
           hr == HRESULT_FROM_WIN32(ERROR_COMMITMENT_LIMIT) ||
           hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
}

void LowerProcessIoPriority() noexcept {
    // NtSetInformationProcess(ProcessIoPriority = 33, IoPriorityLow = 1) lowers disk I/O contention
    // without throttling GPU NVENC scheduling (unlike PROCESS_MODE_BACKGROUND_BEGIN).
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    using NtSetInfoFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
    auto fn = reinterpret_cast<NtSetInfoFn>(GetProcAddress(ntdll, "NtSetInformationProcess"));
    if (!fn) return;
    ULONG io_prio = 1u; // IoPriorityLow
    fn(GetCurrentProcess(), 33u, &io_prio, sizeof(io_prio));
}

void TagBt709Studio(IMFMediaType* mt) noexcept {
    if (!mt) return;
    mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
}

bool TrySetupWriter(const std::wstring& out_path,
                    IMFDXGIDeviceManager* mgr,
                    bool hw_encoder,
                    const GUID& out_sub,
                    UINT32 profile,
                    const GUID& in_sub,
                    UINT ow, UINT oh,
                    UINT32 fps_num, UINT32 fps_den,
                    UINT32 bitrate,
                    ComPtr<IMFSinkWriter>& writer,
                    DWORD& stream_idx) {
    writer.Reset();
    DeleteFileW(out_path.c_str());

    ComPtr<IMFAttributes> wa;
    if (FAILED(MFCreateAttributes(&wa, 3))) return false;
    if (hw_encoder && mgr) {
        wa->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, mgr);
        wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    } else {
        wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    }

    if (FAILED(MFCreateSinkWriterFromURL(out_path.c_str(), nullptr, wa.Get(), &writer)) || !writer) {
        return false;
    }

    ComPtr<IMFMediaType> ot;
    MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, out_sub);
    ot->SetUINT32(MF_MT_AVG_BITRATE, out_sub == MFVideoFormat_H264 ? (bitrate * 3u / 2u) : bitrate);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, ow, oh);
    MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
    MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (profile != 0) ot->SetUINT32(MF_MT_VIDEO_PROFILE, profile);
    TagBt709Studio(ot.Get());
    if (FAILED(writer->AddStream(ot.Get(), &stream_idx))) return false;

    ComPtr<IMFMediaType> it;
    MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, in_sub);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, ow, oh);
    MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
    MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    TagBt709Studio(it.Get());
    if (FAILED(writer->SetInputMediaType(stream_idx, it.Get(), nullptr))) return false;

    return SUCCEEDED(writer->BeginWriting());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring input_path;
    std::wstring output_path;
    std::wstring codec_pref = L"auto";
    std::wstring pipe_name;
    UINT target_w = 0;
    UINT target_h = 0;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--input" && i + 1 < argc) input_path = argv[++i];
        else if (a == L"--output" && i + 1 < argc) output_path = argv[++i];
        else if (a == L"--width" && i + 1 < argc) target_w = static_cast<UINT>(_wtoi(argv[++i]));
        else if (a == L"--height" && i + 1 < argc) target_h = static_cast<UINT>(_wtoi(argv[++i]));
        else if (a == L"--codec" && i + 1 < argc) codec_pref = argv[++i];
        else if (a == L"--pipe" && i + 1 < argc) pipe_name = argv[++i];
    }
    if (input_path.empty() || output_path.empty() || target_w < 2 || target_h < 2) {
        return kExitBadArgs;
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    LowerProcessIoPriority();

    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mf_hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    std::atomic<bool> cancelled{false};
    std::atomic<bool> paused{false};
    HANDLE resume_event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    pulse::desktop::ipc::AsyncPipeEndpoint pipe;
    uint64_t next_seq = 1;
    if (!pipe_name.empty()) {
        pipe.StartClient(
            pipe_name,
            [&](pulse::desktop::ipc::Frame&& f) {
                std::string cmd;
                pulse::desktop::ipc::JsonFindString(f.PayloadText(), "cmd", cmd);
                if (cmd == "cancel") {
                    cancelled.store(true, std::memory_order_release);
                    SetEvent(resume_event);
                } else if (cmd == "pause") {
                    paused.store(true, std::memory_order_release);
                    ResetEvent(resume_event);
                } else if (cmd == "resume") {
                    paused.store(false, std::memory_order_release);
                    SetEvent(resume_event);
                }
            },
            [&]() {
                // Host pipe disconnected -> cancel work immediately.
                cancelled.store(true, std::memory_order_release);
                SetEvent(resume_event);
            },
            3000);
    }

    auto send_progress = [&](double progress, int64_t processed_100ns, int64_t duration_100ns,
                             const char* state_str, const char* encoder_str = "") {
        if (!pipe.IsConnected()) return;
        char buf[384]{};
        std::snprintf(buf, sizeof(buf),
                      "{\"state\":\"%s\",\"progress\":%.4f,\"processed_100ns\":%lld,"
                      "\"duration_100ns\":%lld,\"encoder\":\"%s\"}",
                      state_str, std::clamp(progress, 0.0, 1.0),
                      static_cast<long long>(processed_100ns),
                      static_cast<long long>(duration_100ns),
                      encoder_str);
        pipe.Send(pulse::desktop::ipc::MessageType::CommandActionMsg, next_seq++, buf);
    };

    int exit_code = kExitError;
    do {
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                     levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
            break;
        }
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt)) && mt) mt->SetMultithreadProtected(TRUE);

        UINT token = 0;
        ComPtr<IMFDXGIDeviceManager> mgr;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &mgr)) || FAILED(mgr->ResetDevice(dev.Get(), token))) {
            break;
        }

        ComPtr<IMFAttributes> ra;
        MFCreateAttributes(&ra, 2);
        ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr.Get());
        ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        ComPtr<IMFSourceReader> reader;
        HRESULT hr = MFCreateSourceReaderFromURL(input_path.c_str(), ra.Get(), &reader);
        if (FAILED(hr) || !reader) {
            exit_code = IsOomHresult(hr) ? kExitOom : kExitCodecMissing;
            break;
        }

        const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(vs, TRUE);

        DXGI_FORMAT in_fmt = DXGI_FORMAT_NV12;
        bool typed = false;
        for (const GUID& want : {MFVideoFormat_P010, MFVideoFormat_NV12}) {
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
        if (!typed) {
            exit_code = kExitCodecMissing;
            break;
        }

        ComPtr<IMFMediaType> cur;
        if (FAILED(reader->GetCurrentMediaType(vs, &cur)) || !cur) break;
        UINT32 cw = 0, ch = 0, fps_num = 30, fps_den = 1;
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
        if (FAILED(MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den)) || !fps_num || !fps_den) {
            fps_num = 30;
            fps_den = 1;
        }

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

        pulse::desktop::video::VideoColorInfo cinfo{};
        UINT32 cv = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_YUV_MATRIX, &cv))) cinfo.yuv_matrix = cv;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &cv))) cinfo.nominal_range = cv;
        if (cinfo.yuv_matrix == 0 || cinfo.nominal_range == 0) {
            ComPtr<IMFMediaType> nat;
            if (SUCCEEDED(reader->GetNativeMediaType(vs, 0, &nat)) && nat) {
                if (cinfo.yuv_matrix == 0 && SUCCEEDED(nat->GetUINT32(MF_MT_YUV_MATRIX, &cv))) cinfo.yuv_matrix = cv;
                if (cinfo.nominal_range == 0 && SUCCEEDED(nat->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &cv))) cinfo.nominal_range = cv;
            }
        }

        int64_t duration_100ns = 0;
        PROPVARIANT dur_pv{};
        PropVariantInit(&dur_pv);
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                       MF_PD_DURATION, &dur_pv))) {
            if (dur_pv.vt == VT_UI8) duration_100ns = static_cast<int64_t>(dur_pv.uhVal.QuadPart);
            else if (dur_pv.vt == VT_I8 && dur_pv.hVal.QuadPart > 0) duration_100ns = dur_pv.hVal.QuadPart;
        }
        PropVariantClear(&dur_pv);

        uint32_t ow = 0, oh = 0;
        pulse::desktop::video::ComputeAspectFitEvenSize(vw, vh, target_w, target_h, ow, oh);
        const double fps = std::clamp(static_cast<double>(fps_num) / static_cast<double>(fps_den), 1.0, 240.0);
        const UINT32 bitrate = static_cast<UINT32>(
            std::clamp(static_cast<double>(ow) * static_cast<double>(oh) * fps * 0.10, 1500000.0, 80000000.0));

        // Encoder negotiation: HEVC (Main10 if 10-bit else Main) -> H.264 HW -> H.264 SW.
        ComPtr<IMFSinkWriter> writer;
        DWORD stream_idx = 0;
        DXGI_FORMAT enc_in_fmt = in_fmt;
        bool enc_uses_d3d = true;
        const char* chosen_codec = "none";

        const bool want_hevc = (codec_pref == L"auto" || codec_pref == L"hevc_main10" || codec_pref == L"hevc_main");
        const bool ten_bit = (in_fmt == DXGI_FORMAT_P010);

        if (want_hevc && ten_bit &&
            TrySetupWriter(output_path, mgr.Get(), true, MFVideoFormat_HEVC, eAVEncH265VProfile_Main_420_10,
                           MFVideoFormat_P010, ow, oh, fps_num, fps_den, bitrate, writer, stream_idx)) {
            enc_in_fmt = DXGI_FORMAT_P010;
            enc_uses_d3d = true;
            chosen_codec = "hevc_main10_hw";
        } else if (want_hevc && !ten_bit &&
                   TrySetupWriter(output_path, mgr.Get(), true, MFVideoFormat_HEVC, eAVEncH265VProfile_Main_420_8,
                                  MFVideoFormat_NV12, ow, oh, fps_num, fps_den, bitrate, writer, stream_idx)) {
            enc_in_fmt = DXGI_FORMAT_NV12;
            enc_uses_d3d = true;
            chosen_codec = "hevc_main_hw";
        } else if (codec_pref != L"h264_sw" &&
                   TrySetupWriter(output_path, mgr.Get(), true, MFVideoFormat_H264, eAVEncH264VProfile_High,
                                  MFVideoFormat_NV12, ow, oh, fps_num, fps_den, bitrate, writer, stream_idx)) {
            enc_in_fmt = DXGI_FORMAT_NV12;
            enc_uses_d3d = true;
            chosen_codec = "h264_high_hw";
        } else if (TrySetupWriter(output_path, nullptr, false, MFVideoFormat_H264, eAVEncH264VProfile_High,
                                  MFVideoFormat_NV12, ow, oh, fps_num, fps_den, bitrate, writer, stream_idx)) {
            enc_in_fmt = DXGI_FORMAT_NV12;
            enc_uses_d3d = false;
            chosen_codec = "h264_high_sw";
        } else {
            exit_code = kExitCodecMissing;
            break;
        }

        // D3D11 VideoProcessor scaler (accurate input color space -> BT.709 studio output).
        ComPtr<ID3D11VideoDevice> vdev;
        ComPtr<ID3D11VideoContext> vctx;
        if (FAILED(dev.As(&vdev)) || FAILED(ctx.As(&vctx))) break;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = cw;
        cd.InputHeight = ch;
        cd.OutputWidth = ow;
        cd.OutputHeight = oh;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;

        ComPtr<ID3D11VideoProcessorEnumerator> venum;
        ComPtr<ID3D11VideoProcessor> vp;
        UINT support = 0;
        if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &venum)) ||
            FAILED(vdev->CreateVideoProcessor(venum.Get(), 0, &vp)) ||
            FAILED(venum->CheckVideoProcessorFormat(enc_in_fmt, &support)) ||
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            break;
        }

        const RECT dst_rect{0, 0, static_cast<LONG>(ow), static_cast<LONG>(oh)};
        vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src_rect);
        vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst_rect);
        vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst_rect);
        vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
        ComPtr<ID3D11VideoContext1> vctx1;
        if (SUCCEEDED(vctx.As(&vctx1)) && vctx1) {
            vctx1->VideoProcessorSetStreamColorSpace1(
                vp.Get(), 0, pulse::desktop::video::MapMfToDxgiColorSpace(cinfo, vw, vh));
            vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        }

        send_progress(0.0, 0, duration_100ns, "running", chosen_codec);

        ComPtr<ID3D11Texture2D> upload_tex;
        ComPtr<ID3D11Texture2D> sw_enc_gpu_tex;
        ComPtr<ID3D11VideoProcessorOutputView> sw_enc_ov;
        ComPtr<ID3D11Texture2D> sw_enc_staging;
        std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> in_views;

        LONGLONG first_ts = -1;
        uint64_t frames = 0;
        ULONGLONG last_report_ms = GetTickCount64();
        bool loop_ok = true;

        while (!cancelled.load(std::memory_order_acquire)) {
            if (paused.load(std::memory_order_acquire)) {
                send_progress(duration_100ns > 0 ? static_cast<double>(frames) / (fps * (duration_100ns / 1e7)) : 0.0,
                              static_cast<int64_t>(frames * (1e7 / fps)), duration_100ns, "paused", chosen_codec);
                WaitForSingleObject(resume_event, INFINITE);
                if (cancelled.load(std::memory_order_acquire)) break;
            }

            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            hr = reader->ReadSample(vs, 0, &idx, &flags, &ts, &smp);
            if (FAILED(hr)) {
                exit_code = IsOomHresult(hr) ? kExitOom : kExitError;
                loop_ok = false;
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!smp) continue;
            if (first_ts < 0) first_ts = ts;

            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(smp->GetBufferByIndex(0, &buf)) || !buf) continue;

            ComPtr<ID3D11Texture2D> in_tex;
            UINT slice = 0;
            ComPtr<IMFDXGIBuffer> dxgi_buf;
            if (SUCCEEDED(buf.As(&dxgi_buf)) && SUCCEEDED(dxgi_buf->GetResource(IID_PPV_ARGS(&in_tex))) && in_tex) {
                dxgi_buf->GetSubresourceIndex(&slice);
            } else {
                if (!upload_tex) {
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = cw;
                    td.Height = ch;
                    td.MipLevels = td.ArraySize = 1;
                    td.Format = in_fmt;
                    td.SampleDesc.Count = 1;
                    td.BindFlags = D3D11_BIND_RENDER_TARGET;
                    hr = dev->CreateTexture2D(&td, nullptr, &upload_tex);
                    if (FAILED(hr)) {
                        exit_code = IsOomHresult(hr) ? kExitOom : kExitError;
                        loop_ok = false;
                        break;
                    }
                }
                ComPtr<IMF2DBuffer> b2;
                BYTE* data = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch))) {
                    ctx->UpdateSubresource(upload_tex.Get(), 0, nullptr, data, static_cast<UINT>(pitch), 0);
                    b2->Unlock2D();
                } else {
                    DWORD len = 0;
                    if (FAILED(buf->Lock(&data, nullptr, &len))) {
                        loop_ok = false;
                        break;
                    }
                    ctx->UpdateSubresource(upload_tex.Get(), 0, nullptr, data,
                                           cw * (in_fmt == DXGI_FORMAT_P010 ? 2u : 1u), 0);
                    buf->Unlock();
                }
                in_tex = upload_tex;
            }

            auto key = std::make_pair(in_tex.Get(), slice);
            ID3D11VideoProcessorInputView* iv = nullptr;
            if (auto it = in_views.find(key); it != in_views.end()) {
                iv = it->second.Get();
            } else {
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
                ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                ivd.Texture2D.ArraySlice = slice;
                ComPtr<ID3D11VideoProcessorInputView> created;
                hr = vdev->CreateVideoProcessorInputView(in_tex.Get(), venum.Get(), &ivd, &created);
                if (FAILED(hr)) {
                    exit_code = IsOomHresult(hr) ? kExitOom : kExitError;
                    loop_ok = false;
                    break;
                }
                iv = created.Get();
                in_views[key] = std::move(created);
            }

            ComPtr<IMFSample> out_sample;
            if (enc_uses_d3d) {
                D3D11_TEXTURE2D_DESC od{};
                od.Width = ow;
                od.Height = oh;
                od.MipLevels = od.ArraySize = 1;
                od.Format = enc_in_fmt;
                od.SampleDesc.Count = 1;
                od.BindFlags = D3D11_BIND_RENDER_TARGET;
                ComPtr<ID3D11Texture2D> out_tex;
                ComPtr<ID3D11VideoProcessorOutputView> ov;
                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
                ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                hr = dev->CreateTexture2D(&od, nullptr, &out_tex);
                if (FAILED(hr) || FAILED(vdev->CreateVideoProcessorOutputView(out_tex.Get(), venum.Get(), &ovd, &ov))) {
                    exit_code = IsOomHresult(hr) ? kExitOom : kExitError;
                    loop_ok = false;
                    break;
                }
                D3D11_VIDEO_PROCESSOR_STREAM st{};
                st.Enable = TRUE;
                st.pInputSurface = iv;
                vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, &st);

                ComPtr<IMFMediaBuffer> ob;
                if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), out_tex.Get(), 0, FALSE, &ob)) ||
                    FAILED(MFCreateSample(&out_sample))) {
                    loop_ok = false;
                    break;
                }
                DWORD len = 0;
                ComPtr<IMF2DBuffer> ob2;
                if (SUCCEEDED(ob.As(&ob2))) ob2->GetContiguousLength(&len);
                ob->SetCurrentLength(len);
                out_sample->AddBuffer(ob.Get());
            } else {
                // Software encoder fallback: scale on GPU -> readback NV12 -> MF memory buffer.
                if (!sw_enc_gpu_tex) {
                    D3D11_TEXTURE2D_DESC od{};
                    od.Width = ow;
                    od.Height = oh;
                    od.MipLevels = od.ArraySize = 1;
                    od.Format = DXGI_FORMAT_NV12;
                    od.SampleDesc.Count = 1;
                    od.BindFlags = D3D11_BIND_RENDER_TARGET;
                    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
                    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                    if (FAILED(dev->CreateTexture2D(&od, nullptr, &sw_enc_gpu_tex)) ||
                        FAILED(vdev->CreateVideoProcessorOutputView(sw_enc_gpu_tex.Get(), venum.Get(), &ovd, &sw_enc_ov))) {
                        loop_ok = false;
                        break;
                    }
                    od.Usage = D3D11_USAGE_STAGING;
                    od.BindFlags = 0;
                    od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (FAILED(dev->CreateTexture2D(&od, nullptr, &sw_enc_staging))) {
                        loop_ok = false;
                        break;
                    }
                }
                D3D11_VIDEO_PROCESSOR_STREAM st{};
                st.Enable = TRUE;
                st.pInputSurface = iv;
                vctx->VideoProcessorBlt(vp.Get(), sw_enc_ov.Get(), 0, 1, &st);
                ctx->CopyResource(sw_enc_staging.Get(), sw_enc_gpu_tex.Get());

                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(ctx->Map(sw_enc_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
                    loop_ok = false;
                    break;
                }
                const DWORD nv12_bytes = static_cast<DWORD>(ow) * oh * 3u / 2u;
                ComPtr<IMFMediaBuffer> mb;
                BYTE* dst_ptr = nullptr;
                if (FAILED(MFCreateMemoryBuffer(nv12_bytes, &mb)) || FAILED(mb->Lock(&dst_ptr, nullptr, nullptr))) {
                    ctx->Unmap(sw_enc_staging.Get(), 0);
                    loop_ok = false;
                    break;
                }
                const auto* src_bytes = static_cast<const uint8_t*>(mapped.pData);
                for (UINT y = 0; y < oh + oh / 2u; ++y) {
                    std::memcpy(dst_ptr + static_cast<size_t>(y) * ow,
                                src_bytes + static_cast<size_t>(y) * mapped.RowPitch, ow);
                }
                mb->Unlock();
                ctx->Unmap(sw_enc_staging.Get(), 0);
                mb->SetCurrentLength(nv12_bytes);
                if (FAILED(MFCreateSample(&out_sample))) {
                    loop_ok = false;
                    break;
                }
                out_sample->AddBuffer(mb.Get());
            }

            const int64_t rel_ts = ts - first_ts;
            out_sample->SetSampleTime(rel_ts);
            out_sample->SetSampleDuration(static_cast<LONGLONG>(1e7 / fps + 0.5));

            hr = writer->WriteSample(stream_idx, out_sample.Get());
            if (FAILED(hr)) {
                exit_code = IsOomHresult(hr) ? kExitOom : kExitError;
                loop_ok = false;
                break;
            }
            ++frames;

            const ULONGLONG now_tick = GetTickCount64();
            if (now_tick - last_report_ms >= 500ull) {
                last_report_ms = now_tick;
                const double prog = (duration_100ns > 0)
                                        ? std::clamp(static_cast<double>(rel_ts) / static_cast<double>(duration_100ns),
                                                     0.0, 0.99)
                                        : 0.0;
                send_progress(prog, rel_ts, duration_100ns, "running", chosen_codec);
            }
        }

        if (cancelled.load(std::memory_order_acquire)) {
            exit_code = kExitCancelled;
            writer.Reset();
            DeleteFileW(output_path.c_str());
            break;
        }
        if (!loop_ok || frames == 0) {
            writer.Reset();
            DeleteFileW(output_path.c_str());
            break;
        }
        hr = writer->Finalize();
        if (FAILED(hr)) {
            exit_code = IsOomHresult(hr) ? kExitOom : kExitError;
            writer.Reset();
            DeleteFileW(output_path.c_str());
            break;
        }

        send_progress(1.0, duration_100ns, duration_100ns, "done", chosen_codec);
        exit_code = kExitOk;
    } while (false);

    pipe.Stop();
    if (resume_event) CloseHandle(resume_event);
    if (SUCCEEDED(mf_hr)) MFShutdown();
    if (SUCCEEDED(co_hr)) CoUninitialize();
    return exit_code;
}
