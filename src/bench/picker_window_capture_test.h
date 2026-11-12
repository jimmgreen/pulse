#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
inline bool CapturePickerWindow(HWND window, const std::wstring& path) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return false;
    using SetDpi = HANDLE (WINAPI*)(HANDLE);
    const auto set_dpi = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    const HANDLE previous_dpi = set_dpi ? set_dpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))) : nullptr;
    RECT rect{}; GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetWindowDC(window), memory = dc ? CreateCompatibleDC(dc) : nullptr;
    HBITMAP bitmap = memory && width > 0 && height > 0 ? CreateCompatibleBitmap(dc, width, height) : nullptr;
    bool saved = false;
    if (bitmap) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        const BOOL captured = PrintWindow(window, memory, 2);
        SelectObject(memory, previous);
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICBitmap> source;
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (captured && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source)) &&
            SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) saved = true;
        DeleteObject(bitmap);
    }
    if (memory) DeleteDC(memory);
    if (dc) ReleaseDC(window, dc);
    if (previous_dpi) set_dpi(previous_dpi);
    CoUninitialize();
    return saved;
}
