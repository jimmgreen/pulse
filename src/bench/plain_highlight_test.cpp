#include "../ui/plain_highlight_lines.h"
#include <windows.h>
#include <string>
#include <cstdio>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
int main() {
    ComPtr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) return 2;
    int checked=0;
    for (float scale : {1.0f, 1.5f}) {
        ComPtr<IDWriteTextFormat> format;
        factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14*scale, L"en-us", &format);
        std::wstring text;
        for (int i=0;i<500;++i) text += L"abc \x05d0\x05d1\x05d2 abc\r\n\xD83D\xDE00 abc long wrapped line abc abc abc abc\n";
        ComPtr<IDWriteTextLayout> layout;
        factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(), 170*scale, 100000, &layout);
        layout->SetFontSize(32*scale, {0, 25});
        pulse::ui::PlainHighlightLines index;
        for (float top : {0.f, 1.f, 48.f, 999.f, 10000.f}) {
            const float bottom=top+200*scale;
            UINT32 begin=0,end=0;
            if (!index.Range(layout.Get(), top, bottom, begin, end)) return 3;
            for (UINT32 start=0;start<text.size();start+=3) {
                const UINT32 length=(std::min)(29u, static_cast<UINT32>(text.size())-start);
                DWRITE_HIT_TEST_METRICS metrics[96]; UINT32 actual=0;
                if (FAILED(layout->HitTestTextRange(start,length,0,0,metrics,96,&actual))) continue;
                bool visible=false;
                for(UINT32 i=0;i<actual;++i) visible |= metrics[i].top < bottom && metrics[i].top+metrics[i].height > top;
                if (visible && !(start<end && static_cast<uint64_t>(start)+length>begin)) { std::printf("[FAIL] culled visible hit %u %u %u\n",start,begin,end); return 1; }
                ++checked;
            }
        }
        index.Reset();
    }
    ComPtr<IDWriteTextFormat> cap_format;
    factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"en-us", &cap_format);
    std::wstring huge(65537, L'\n');
    ComPtr<IDWriteTextLayout> huge_layout;
    factory->CreateTextLayout(huge.data(), static_cast<UINT32>(huge.size()), cap_format.Get(), 170, 2000000, &huge_layout);
    pulse::ui::PlainHighlightLines cap_index;
    uint32_t cap_begin=0,cap_end=0;
    if(cap_index.Range(huge_layout.Get(),0,200,cap_begin,cap_end) || cap_index.Range(huge_layout.Get(),0,200,cap_begin,cap_end)) return 4;
    cap_index.Reset();
    ComPtr<IDWriteTextLayout> small_layout;
    factory->CreateTextLayout(L"abc",3,cap_format.Get(),170,200,&small_layout);
    small_layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if(!cap_index.Range(small_layout.Get(),90,110,cap_begin,cap_end) || cap_begin!=0 || cap_end!=3) return 5;
    std::printf("[PASS] line-cap fallback, repeated fallback, reset to different centered layout\n");
    std::printf("[PASS] %d real DWrite ranges: CRLF, wrapping, bidi, multiline, mixed size, 100/150%%\n",checked);
}
