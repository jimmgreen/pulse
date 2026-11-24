#pragma once
#include <windows.h>
#include <memory>
#include <string>
namespace pulse {
class DesktopPreviewBridge {
public:
    explicit DesktopPreviewBridge(HWND owner);
    ~DesktopPreviewBridge();
    bool Request(const std::wstring& path, unsigned mode, const RECT* origin);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
