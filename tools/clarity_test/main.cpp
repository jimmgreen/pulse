// Finite, headless Windows experiment. Uses the shipped SDK and production wrapper.
#include "../../src/ui/lumatext_renderer.h"
#include "../../src/ui/typography.h"
#include "../../src/common/localization.h"
#include <lumatext/lumatext.hpp>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT kColumn = 850, kWidth = kColumn * 4, kRow = 72, kHeight = kRow * 12 + 44;
void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void Hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) { std::ostringstream s; s << what << " HRESULT=0x" << std::hex << hr; throw std::runtime_error(s.str()); }
}
std::string Json(const std::wstring& input) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), utf8.data(), size, nullptr, nullptr);
    std::string out = "\"";
    for (unsigned char c : utf8) { if (c == '\\' || c == '"') out += '\\'; if (c < 32) out += ' '; else out += static_cast<char>(c); }
    return out + '"';
}
std::wstring FontFile(IDWriteFontFace* face) {
    UINT32 count = 0, key_size = 0, length = 0;
    ComPtr<IDWriteFontFile> file; ComPtr<IDWriteFontFileLoader> loader; ComPtr<IDWriteLocalFontFileLoader> local;
    const void* key = nullptr;
    Hr(face->GetFiles(&count, nullptr), "count font files"); Check(count == 1, "one font file");
    Hr(face->GetFiles(&count, &file), "font file"); Hr(file->GetReferenceKey(&key, &key_size), "font key");
    Hr(file->GetLoader(&loader), "font loader"); Hr(loader.As(&local), "local font loader");
    Hr(local->GetFilePathLengthFromKey(key, key_size, &length), "font path length");
    std::wstring path(length + 1, L'\0'); Hr(local->GetFilePathFromKey(key, key_size, path.data(), length + 1), "font path");
    path.resize(length); return path;
}
struct Verifier final : IDWriteTextRenderer {
    std::wstring expected;
    struct FaceInfo { std::wstring path; UINT32 index; DWRITE_FONT_SIMULATIONS simulations; };
    std::vector<FaceInfo> files;
    UINT runs = 0, glyphs = 0;
    explicit Verifier(std::wstring file = {}) : expected(std::move(file)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
        if (!p) return E_POINTER; *p = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextRenderer) || id == __uuidof(IDWritePixelSnapping)) { *p = this; return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* v) override { *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override { *m = {1,0,0,1,0,0}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* v) override { *v = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        if (!run || !run->fontFace) return E_FAIL;
        try {
            const auto path = FontFile(run->fontFace);
            if (!expected.empty() && (_wcsicmp(path.c_str(), expected.c_str()) || run->fontFace->GetIndex() != 0 ||
                run->fontFace->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE)) return E_FAIL;
            for (UINT32 i=0; i<run->glyphCount; ++i) if (!run->glyphIndices[i]) return E_FAIL;
            files.push_back({path, run->fontFace->GetIndex(), run->fontFace->GetSimulations()}); ++runs; glyphs += run->glyphCount; return S_OK;
        } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return E_NOTIMPL; }
};
struct Test {
    ComPtr<ID3D11Device> d3d; ComPtr<ID3D11DeviceContext> d3d_context;
    ComPtr<IDXGIDevice> dxgi; ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device; ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> target, readback;
    ComPtr<IDWriteFactory2> write; ComPtr<IDWriteFactory3> write3;
    ComPtr<IDWriteRenderingParams2> params;
    ComPtr<IDWriteFontCollection1> collection;
    ComPtr<IWICImagingFactory> wic;
    std::array<std::wstring,2> font_paths, font_families;
    LumaText::Context context;
    std::array<LumaText::FontFace,2> faces;
    LumaText::FontCascade cascade;
    pulse::ui::LumaTextRenderer mitchell, direct;
    std::ofstream metadata;
    void Init() {
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, &d3d_context), "WARP device");
        Hr(d3d.As(&dxgi), "DXGI device"); Hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()), "D2D factory");
        Hr(factory->CreateDevice(dxgi.Get(), &device), "D2D device");
        Hr(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc), "D2D context"); dc->SetDpi(96,96);
        const auto pixel = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        Hr(dc->CreateBitmap(D2D1::SizeU(kWidth,kHeight), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixel,96,96), &target), "target");
        Hr(dc->CreateBitmap(D2D1::SizeU(kWidth,kHeight), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,pixel,96,96), &readback), "readback");
        dc->SetTarget(target.Get());
        Hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(write.GetAddressOf())), "DWrite");
        Hr(write.As(&write3), "DWrite3");
        Hr(pulse::ui::typography::CreateRenderingParams(write.Get(), MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY), &params), "Pulse native params");
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); dc->SetTextRenderingParams(params.Get());
        Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)), "WIC");
        pulse::l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
        auto c=LumaText::Descriptor<lt_context_desc>(); c.dwrite_factory=write.Get();
        Check(lt_context_create(&c,context.put())==LT_OK,"measurement context");
        wchar_t windows[MAX_PATH]{}; Check(GetWindowsDirectoryW(windows,MAX_PATH)>0,"Windows path");
        ComPtr<IDWriteFontSetBuilder> builder; Hr(write3->CreateFontSetBuilder(&builder),"font builder");
        for (size_t i=0;i<2;++i) {
            font_paths[i]=std::wstring(windows)+L"\\Fonts\\"+(i?L"msyhbd.ttc":L"msyh.ttc");
            auto f=LumaText::Descriptor<lt_font_source_desc>(); f.file_path=font_paths[i].c_str();
            Check(lt_font_face_create(context.get(),&f,faces[i].put())==LT_OK,"YaHei face0");
            ComPtr<IDWriteFontFaceReference> ref; Hr(write3->CreateFontFaceReference(font_paths[i].c_str(),nullptr,0,DWRITE_FONT_SIMULATIONS_NONE,&ref),"native face0");
            Hr(builder->AddFontFaceReference(ref.Get()),"add native face");
        }
        lt_font_cascade_entry entries[]={{faces[0].get(),400,0},{faces[1].get(),600,0},{faces[1].get(),700,0}};
        auto cd=LumaText::Descriptor<lt_font_cascade_desc>(); cd.entries=entries; cd.entry_count=3;
        Check(lt_font_cascade_create(context.get(),&cd,cascade.put())==LT_OK,"measurement cascade");
        ComPtr<IDWriteFontSet> set; Hr(builder->CreateFontSet(&set),"font set");
        Hr(write3->CreateFontCollectionFromFontSet(set.Get(),&collection),"native custom collection");
        for (size_t i=0;i<2;++i) {
            ComPtr<IDWriteFontFaceReference> ref; ComPtr<IDWriteFontFace3> face;
            Hr(write3->CreateFontFaceReference(font_paths[i].c_str(),nullptr,0,DWRITE_FONT_SIMULATIONS_NONE,&ref),"reference");
            Hr(ref->CreateFontFace(&face),"native face");
            ComPtr<IDWriteFont> font; ComPtr<IDWriteFontFamily> family; ComPtr<IDWriteLocalizedStrings> names;
            Hr(collection->GetFontFromFontFace(face.Get(),&font),"font from face"); Hr(font->GetFontFamily(&family),"family");
            Hr(family->GetFamilyNames(&names),"family names"); UINT32 length=0; Hr(names->GetStringLength(0,&length),"name length");
            font_families[i].resize(length+1); Hr(names->GetString(0,font_families[i].data(),length+1),"family name"); font_families[i].resize(length);
        }
        SetEnvironmentVariableW(L"PULSE_LUMATEXT_FILTER",L"mitchell"); Check(mitchell.Init(write.Get(),dc.Get()),"actual Mitchell wrapper");
        SetEnvironmentVariableW(L"PULSE_LUMATEXT_FILTER",L"direct"); Check(direct.Init(write.Get(),dc.Get()),"actual Direct wrapper");
        SetEnvironmentVariableW(L"PULSE_LUMATEXT_FILTER",nullptr);
    }
    lt_text_metrics Metrics(const std::wstring& text,float px,UINT weight) {
        auto style=LumaText::Descriptor<lt_text_style>(); style.cascade=cascade.get(); style.font_size=px; style.weight=static_cast<uint16_t>(weight);
        auto desc=LumaText::Descriptor<lt_text_layout_desc>(); desc.text=text.c_str(); desc.text_length=static_cast<uint32_t>(text.size()); desc.base_style=style; desc.locale="zh-CN"; desc.max_width=10000;
        LumaText::TextLayout layout; Check(lt_text_layout_create(context.get(),&desc,layout.put())==LT_OK,"measurement layout");
        auto metrics=LumaText::Descriptor<lt_text_metrics>(); Check(lt_text_layout_get_metrics(layout.get(),&metrics)==LT_OK,"measurement metrics"); return metrics;
    }
    std::vector<BYTE> Pixels() {
        dc->SetTarget(nullptr); Hr(readback->CopyFromBitmap(nullptr,target.Get(),nullptr),"copy readback");
        D2D1_MAPPED_RECT map{}; Hr(readback->Map(D2D1_MAP_OPTIONS_READ,&map),"map readback");
        std::vector<BYTE> result(static_cast<size_t>(kWidth)*kHeight*4);
        for (UINT y=0;y<kHeight;++y) memcpy(result.data()+static_cast<size_t>(y)*kWidth*4,map.bits+static_cast<size_t>(y)*map.pitch,kWidth*4);
        Hr(readback->Unmap(),"unmap"); dc->SetTarget(target.Get()); return result;
    }
    void Save(const std::filesystem::path& path,const std::vector<BYTE>& pixels) {
        ComPtr<IWICBitmap> bitmap; Hr(wic->CreateBitmapFromMemory(kWidth,kHeight,GUID_WICPixelFormat32bppPBGRA,kWidth*4,
            static_cast<UINT>(pixels.size()),const_cast<BYTE*>(pixels.data()),&bitmap),"WIC bitmap");
        ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
        Hr(wic->CreateStream(&stream),"stream"); Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG file");
        Hr(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder"); Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"encoder init");
        Hr(encoder->CreateNewFrame(&frame,nullptr),"frame"); Hr(frame->Initialize(nullptr),"frame init"); Hr(frame->SetSize(kWidth,kHeight),"frame size");
        WICPixelFormatGUID pixel=GUID_WICPixelFormat32bppBGRA; Hr(frame->SetPixelFormat(&pixel),"frame pixel");
        Hr(frame->WriteSource(bitmap.Get(),nullptr),"write pixels"); Hr(frame->Commit(),"frame commit"); Hr(encoder->Commit(),"PNG commit");
    }
    void Native(const std::wstring& text,IDWriteTextFormat* format,D2D1_RECT_F rc,ID2D1SolidColorBrush* brush,
        bool matched,const std::wstring& expected,const lt_text_metrics& metrics,bool log) {
        ComPtr<IDWriteTextLayout> layout; Hr(write->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format,kColumn-32,64,&layout),"native layout");
        Hr(layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP),"no wrap");
        Verifier verify(matched?expected:L""); Hr(layout->Draw(nullptr,&verify,0,0),"verify actual native glyph faces"); Check(verify.runs>0,"native actual glyph runs");
        DWRITE_LINE_METRICS line{}; UINT32 count=0; Hr(layout->GetLineMetrics(&line,1,&count),"native line metrics"); Check(count==1,"one native line");
        DWRITE_TEXT_METRICS native_metrics{}; DWRITE_OVERHANG_METRICS overhang{};
        Hr(layout->GetMetrics(&native_metrics),"native width"); Hr(layout->GetOverhangMetrics(&overhang),"native ink bounds");
        Check(native_metrics.width + std::max(0.f,overhang.right) + 8 < kColumn-32,"native ink fits host clip");
        const float origin_y=matched?rc.top+(rc.bottom-rc.top-metrics.height)*.5f+metrics.ascent-line.baseline:
            rc.top+(rc.bottom-rc.top-line.height)*.5f;
        dc->DrawTextLayout(D2D1::Point2F(rc.left,origin_y),layout.Get(),brush,D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (log) {
            metadata << ",\"native_" << (matched?"matched":"shipped") << "\":{\"baseline_y\":" << origin_y+line.baseline << ",\"files\":[";
            for(size_t i=0;i<verify.files.size();++i) { if(i) metadata<<','; metadata<<"{\"path\":"<<Json(verify.files[i].path)<<",\"face_index\":"<<verify.files[i].index<<",\"simulations\":"<<verify.files[i].simulations<<'}'; }
            metadata<<"],\"exact_face0_no_simulations_verified\":"<<(matched?"true":"false")<<'}';
        }
    }
    void Page(float dip,float scale,bool dark,bool log) {
        const auto bg=D2D1::ColorF(dark?0x202020:0xffffff), fg=D2D1::ColorF(dark?0xf2f2f2:0x202020);
        const float px=dip*scale; dc->BeginDraw(); dc->Clear(bg);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); dc->SetTextRenderingParams(params.Get());
        ComPtr<ID2D1SolidColorBrush> brush; Hr(dc->CreateSolidColorBrush(fg,&brush),"brush");
        ComPtr<IDWriteTextFormat> label; Hr(write->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,13,L"en-US",&label),"label");
        const wchar_t* labels[]={L"Pulse native as shipped (YaHei UI/600)",L"Native exact Luma font (face0; 600 maps bold)",L"Actual Pulse Luma wrapper: Mitchell",L"Actual Pulse Luma wrapper: Direct"};
        for(UINT col=0;col<4;++col) dc->DrawTextW(labels[col],static_cast<UINT32>(wcslen(labels[col])),label.Get(),D2D1::RectF(col*kColumn+16.f,8,col*kColumn+kColumn-16.f,36),brush.Get());
        const std::array<std::wstring,3> texts={L"Hamburgefontsiv Il1 O0 AV To fi fl 0123456789",L"微软雅黑 清晰的文字 图纸设计 阅读与文件管理",L"图纸设计2.dwg Windows macOS 文件 0123"};
        UINT row=0;
        for(UINT weight:{400u,600u}) for(float phase:{0.f,.5f}) for(size_t kind=0;kind<texts.size();++kind,++row) {
            const auto& text=texts[kind]; const size_t face=weight>=600?1:0;
            ComPtr<IDWriteTextFormat> host,matched,shipped;
            Hr(write->CreateTextFormat(L"Microsoft YaHei",nullptr,static_cast<DWRITE_FONT_WEIGHT>(weight),DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,px,L"zh-CN",&host),"host format");
            Hr(write->CreateTextFormat(font_families[face].c_str(),collection.Get(),face?DWRITE_FONT_WEIGHT_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,px,L"zh-CN",&matched),"exact font format");
            Hr(pulse::ui::typography::CreateTextFormat(write.Get(),{weight>=600?pulse::ui::typography::FontRole::Display:pulse::ui::typography::FontRole::Text,px,static_cast<DWRITE_FONT_WEIGHT>(weight)},&shipped),"shipped native format");
            const auto metrics=Metrics(text,px,weight);
            Check(metrics.width + pulse::ui::typography::InkPad(host.Get()) < kColumn-32,"Luma ink fits host clip without ellipsis");
            if(log) metadata<<"{\"row\":"<<row<<",\"dip\":"<<dip<<",\"scale\":"<<scale<<",\"physical_em\":"<<px<<",\"weight\":"<<weight<<",\"phase\":"<<phase<<",\"dark\":"<<(dark?"true":"false")<<",\"kind\":"<<kind<<",\"text\":"<<Json(text)<<",\"rect_y\":"<<44+row*kRow<<",\"rect_height\":64,\"font_file\":"<<Json(font_paths[face]);
            for(UINT col=0;col<4;++col) {
                const float x=col*kColumn+16.f+phase, y=44.f+row*kRow+phase;
                const auto rc=D2D1::RectF(x,y,x+kColumn-32,y+64);
                if(col<2) Native(text,col?matched.Get():shipped.Get(),rc,brush.Get(),col==1,font_paths[face],metrics,log);
                else {
                    auto& renderer=col==2?mitchell:direct; const auto before=renderer.Stats().draw_calls;
                    Check(renderer.Draw(text,host.Get(),rc,fg,bg),"actual wrapper used Luma, no fallback");
                    Check(renderer.Stats().draw_calls==before+1,"actual wrapper draw recorded");
                }
            }
            if(log) metadata<<",\"luma_actual_path\":\"lt_frame_draw_text_layout; command-list replay\",\"luma_baseline_y\":"<<44.f+row*kRow+phase+(64-metrics.height)*.5f+metrics.ascent<<",\"flags\":0,\"coverage_gamma\":0.85,\"contrast\":1,\"optical_weight\":0}\n";
        }
        Hr(dc->EndDraw(),"finish page");
    }
};
}
int wmain(int argc,wchar_t** argv) {
    try {
        Check(argc==2,"usage: pulse_clarity OUTPUT_DIRECTORY"); Hr(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"COM");
        const std::filesystem::path output=argv[1]; std::filesystem::create_directories(output);
        Test test; test.Init(); test.metadata.open(output/L"rows.jsonl"); Check(test.metadata.good(),"metadata file");
        std::ofstream config(output/L"config.json");
        config<<"{\"target\":\"D3D11 WARP / D2D1DeviceContext / PREMULTIPLIED\",\"target_dpi\":96,\"frame_dpi\":96,\"column_width\":"<<kColumn<<",\"native_gray\":true,\"native_gamma\":"<<test.params->GetGamma()<<",\"native_gray_contrast\":"<<test.params->GetGrayscaleEnhancedContrast()<<",\"native_mode\":"<<test.params->GetRenderingMode()<<",\"native_geometry\":"<<test.params->GetPixelGeometry()<<",\"native_gridfit\":"<<test.params->GetGridFitMode()<<",\"axes\":[],\"luma_face_index\":0,\"scope\":\"YaHei family only; actual Pulse wrapper and bundled DLL. Native matched column aligns baseline and exact face; native shipped column uses actual typography policy.\"}";
        for(const auto [dip,scale]:std::array<std::pair<float,float>,6>{{{12.f,1.f},{13.f,1.f},{14.f,1.f},{13.f,1.25f},{13.f,1.5f},{13.f,2.f}}}) for(bool dark:{false,true}) {
            const auto name=std::to_wstring(static_cast<int>(dip))+L"dip-"+std::to_wstring(static_cast<int>(scale*100))+(dark?L"-dark":L"-light");
            test.Page(dip,scale,dark,true); const auto cold=test.Pixels(); test.Save(output/(name+L".png"),cold);
            const auto before_m=test.mitchell.Stats().surface_cache_hits,before_d=test.direct.Stats().surface_cache_hits;
            test.Page(dip,scale,dark,false); const auto warm=test.Pixels();
            Check(cold==warm,"cold/warm image equality");
            Check(test.mitchell.Stats().surface_cache_hits>=before_m+12 && test.direct.Stats().surface_cache_hits>=before_d+12,"both wrappers hit cached command lists");
            std::cout<<"PASS "<<Json(name)<<" cold=warm; 12 real draws per filter; exact-font native verified\n";
        }
        test.metadata.close(); Check(test.metadata.good(),"metadata written");
        std::cout<<"PASS all 144 rows; 288 filter draws plus 288 native references; 288 warm-cache filter replays\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
