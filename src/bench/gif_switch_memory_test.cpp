#include "preview_host_client.h"
#include "gif_fixture.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <psapi.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

using Microsoft::WRL::ComPtr;
using pulse_test::gif_fixture::Bytes;
using pulse_test::gif_fixture::Word;
using pulse_test::gif_fixture::Frame;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    failures += !ok;
}
Bytes Animation() {
    Bytes file{'G','I','F','8','9','a'}; Word(file, 2048); Word(file, 2048);
    file.insert(file.end(), {0x81,0,0,255,0,0,0,255,0,0,0,255,255,255,255});
    // Opaque red logical background; tiny first raster still allocates the full canvas.
    Frame(file, 0, 1, 1, 0, 1);
    Frame(file, 128, 32, 32, 1, 3, false, 320);
    Frame(file, 2032, 16, 16, 2, 1, false, 2032);
    file.push_back(0x3b);
    return file;
}
bool Write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}
bool SmallPng(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    Bytes pixels;
    for (int i = 0; i < 64; ++i) pixels.insert(pixels.end(), {30,20,10});
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(8, 8)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
        SUCCEEDED(frame->WritePixels(8, 24, static_cast<UINT>(pixels.size()), pixels.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
bool Pixels(const pulse_test::Result& result, unsigned frame, bool is_png) {
    const unsigned width = is_png ? 8 : 512;
    if (result.response.status != 0 || result.response.kind != pulse::ipc::PreviewContentKind::Bitmap ||
        result.response.width != width || result.response.height != width || result.response.stride < width * 4 ||
        result.pixels.size() < static_cast<size_t>(result.response.stride) * width ||
        result.response.frame_count != (is_png ? 1u : 3u)) return false;
    for (unsigned y = 0; y < width; ++y) for (unsigned x = 0; x < width; ++x) {
        unsigned expected[] = {0,0,255,255};
        if (is_png) { expected[0]=30; expected[1]=20; expected[2]=10; }
        else if (frame == 1 && x >= 32 && x < 40 && y >= 80 && y < 88) {
            expected[0]=0; expected[1]=255; expected[2]=0;
        } else if (frame == 2 && x >= 508 && y >= 508) {
            expected[0]=255; expected[1]=0; expected[2]=0;
        }
        for (unsigned c=0;c<4;++c) {
            const unsigned actual=result.pixels[static_cast<size_t>(y)*result.response.stride+x*4+c];
            if (actual != expected[c]) {
                std::cout << "  mismatch x=" << x << " y=" << y << " channel=" << c
                    << " actual=" << actual << " expected=" << expected[c] << std::endl;
                return false;
            }
        }
    }
    return true;
}
}
int main() {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(com)) return 2;
    const auto parent=std::filesystem::absolute("bench_data"); std::filesystem::create_directories(parent);
    const auto root=parent/("gif-switch-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    if (!CreateDirectoryW(root.c_str(),nullptr)) { CoUninitialize(); return 2; }
    Check(Write(root/"large.gif",Animation()),"write isolated large-canvas GIF fixture");
    Check(SmallPng(root/"small.png"),"encode isolated small PNG fixture");
    pulse_test::Host host;
    const bool started=host.Start(); Check(started,"start isolated real GIF preview host");
    if (started) {
        const DWORD host_pid=host.ProcessId();
        HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,host_pid);
        const auto sample=[&](const char* stage) {
            Sleep(200);
            PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb=sizeof(counters);
            bool valid=process && GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters));
            std::vector<unsigned char> buffer(4*1024*1024);
            valid=valid && QueryWorkingSet(process,buffer.data(),static_cast<DWORD>(buffer.size()));
            size_t private_ws=0;
            if (valid) {
                SYSTEM_INFO system{}; GetSystemInfo(&system);
                const auto* ws=reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
                for (ULONG_PTR i=0;i<ws->NumberOfEntries;++i)
                    if (!ws->WorkingSetInfo[i].Shared) private_ws+=system.dwPageSize;
            }
            std::cout << "MEMORY stage=" << stage << " pid=" << host_pid << " private_commit=" << counters.PrivateUsage
                << " private_ws=" << private_ws << " valid=" << valid << std::endl;
            Check(valid,"sample own child private commit and resident pages");
        };
        const auto request=[&](unsigned frame,bool is_png,const char* label) {
            pulse_test::Result result;
            const auto begin=std::chrono::steady_clock::now();
            const bool received=host.Request((root/(is_png?"small.png":"large.gif")).wstring(),result,
                MAXDWORD,1024,pulse::ipc::PreviewRequestKind::Content,0,frame);
            const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            uint64_t checksum=1469598103934665603ull;
            for (const auto byte:result.pixels) { checksum^=byte; checksum*=1099511628211ull; }
            std::cout << "REQUEST " << label << " frame=" << frame << " received=" << received << " ms=" << elapsed
                << " status=" << result.response.status << " kind=" << static_cast<unsigned>(result.response.kind)
                << " width=" << result.response.width << " height=" << result.response.height
                << " count=" << result.response.frame_count << " checksum=" << checksum << std::endl;
            if (!result.error.empty()) std::wcout << L"  error=" << result.error << std::endl;
            Check(received && Pixels(result,frame,is_png),label);
        };
        sample("startup-idle");
        request(0,false,"initial logical background");
        request(1,false,"local disposal-3 patch");
        sample("gif-patch-idle");
        request(2,false,"forward playback restores previous patch");
        request(0,false,"backward playback restores background");
        request(2,false,"distant forward seek preserves all pixels");
        request(1,false,"reverse seek restores local patch");
        sample("gif-reused-idle");
        request(0,true,"switch to small PNG");
        sample("png-idle");
        request(0,true,"repeat small PNG");
        sample("png-reused-idle");
        request(2,false,"return to GIF with distant seek");
        request(0,false,"returned GIF backward seek");
        sample("gif-returned-idle");
        if (process) CloseHandle(process);
        host.Stop();
    }
    std::error_code error; std::filesystem::remove_all(root,error);
    Check(!error,"clean only owned GIF switch fixtures");
    CoUninitialize();
    std::cout << "Failures: " << failures << std::endl;
    return failures?1:0;
}
