#pragma once
#include <string>
#include <dwrite.h>

namespace pulse {
void CaptureFramePerfFont(IDWriteTextFormat* format, int text_mode, int font_percent, int scale_milli) noexcept;
// Synchronous disk work: call only during diagnostic export, never while drawing.
bool WriteFramePerfEnvironment(const std::wstring& path) noexcept;
}
