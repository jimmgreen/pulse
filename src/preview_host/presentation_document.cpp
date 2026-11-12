#include "presentation_document.h"
#include "../ipc/preview_protocol.h"
#include "compound_file.h"
#include "doc_payload.h"
#include "zip_entry.h"
#include <functional>
#include <map>
#include <objidl.h>
#include <set>
#include <shlwapi.h>
#include <wrl/client.h>
#include <xmllite.h>

namespace pulse::preview {
namespace {
constexpr size_t kMaxSlides = 512;
constexpr size_t kPartLimit = 8u << 20;
constexpr size_t kTotalLimit = 64u << 20;
struct Slide {
    std::vector<std::wstring> text;
    // Legacy placeholder text points into the current slide's text-header list.
    std::vector<std::pair<size_t, uint32_t>> outline_refs;
};
using Microsoft::WRL::ComPtr;
using XmlVisitor = std::function<void(IXmlReader*, XmlNodeType, std::wstring_view)>;
std::wstring Attr(IXmlReader* r, const wchar_t* name, const wchar_t* ns = nullptr) {
    std::wstring result;
    if (r->MoveToAttributeByName(name, ns) == S_OK) {
        const wchar_t* p = nullptr;
        UINT n = 0;
        if (SUCCEEDED(r->GetValue(&p, &n)) && p) result.assign(p, n);
    }
    r->MoveToElement();
    return result;
}
bool Xml(const std::vector<uint8_t>& bytes, const XmlVisitor& visit) {
    using Factory = HRESULT(WINAPI*)(REFIID, void**, IMalloc*);
    static const Factory create = [] {
        auto module = LoadLibraryExW(L"xmllite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<Factory>(GetProcAddress(module, "CreateXmlReader")) : nullptr;
    }();
    if (!create || bytes.empty()) return false;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    ComPtr<IXmlReader> reader;
    if (!stream ||
        FAILED(create(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)))
        return false;
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    reader->SetProperty(XmlReaderProperty_MaxElementDepth, 128);
    if (FAILED(reader->SetInput(stream.Get()))) return false;
    XmlNodeType type{};
    HRESULT hr;
    while ((hr = reader->Read(&type)) == S_OK) {
        const wchar_t* p = nullptr;
        UINT n = 0;
        reader->GetLocalName(&p, &n);
        visit(reader.Get(), type, p ? std::wstring_view(p, n) : std::wstring_view{});
    }
    return hr == S_FALSE;
}
struct Rel {
    std::wstring target, type;
};
bool Rels(const std::vector<uint8_t>& bytes, std::map<std::wstring, Rel>& rels) {
    bool valid = true;
    return Xml(bytes,
               [&](IXmlReader* r, XmlNodeType t, std::wstring_view n) {
                   if (t != XmlNodeType_Element || n != L"Relationship" ||
                       Attr(r, L"TargetMode") == L"External")
                       return;
                   auto id = Attr(r, L"Id");
                   if (id.empty() || !rels.emplace(id, Rel{Attr(r, L"Target"), Attr(r, L"Type")}).second)
                       valid = false;
               }) &&
           valid;
}
bool Pptx(const std::wstring& path, std::vector<Slide>& slides, size_t& total, uint32_t& read, bool& cut) {
    std::vector<uint8_t> bytes;
    auto part = [&](const std::wstring& name) {
        if (!ReadZipEntry(path, ToUtf8(name), kPartLimit, bytes, nullptr)) return false;
        if (bytes.size() > kTotalLimit - read) return false;
        read += static_cast<uint32_t>(bytes.size());
        return true;
    };
    std::map<std::wstring, Rel> roots;
    if (!part(L"_rels/.rels") || !Rels(bytes, roots)) return false;
    std::wstring main;
    for (const auto& [id, r] : roots)
        if (r.type.ends_with(L"/officeDocument")) main = ResolvePartName(L"", r.target);
    if (main.empty() || !part(main)) return false;
    std::vector<std::wstring> ids;
    bool presentation = false;
    if (!Xml(bytes,
             [&](IXmlReader* r, XmlNodeType t, std::wstring_view n) {
                 if (t != XmlNodeType_Element) return;
                 if (n == L"presentation") presentation = true;
                 if (n == L"sldId") {
                     ++total;
                     if (ids.size() < kMaxSlides) {
                         auto id =
                             Attr(r, L"id",
                                  L"http://schemas.openxmlformats.org/officeDocument/2006/relationships");
                         if (id.empty())
                             id = Attr(r, L"id", L"http://purl.oclc.org/ooxml/officeDocument/relationships");
                         ids.push_back(std::move(id));
                     } else
                         cut = true;
                 }
             }) ||
        !presentation || ids.empty())
        return false;
    auto slash = main.find_last_of(L'/');
    std::wstring dir = slash == std::wstring::npos ? L"" : main.substr(0, slash + 1);
    std::wstring filename = main.substr(slash == std::wstring::npos ? 0 : slash + 1);
    std::map<std::wstring, Rel> rels;
    if (!part(dir + L"_rels/" + filename + L".rels") || !Rels(bytes, rels)) return false;
    for (const auto& id : ids) {
        auto found = rels.find(id);
        if (found == rels.end() || !found->second.type.ends_with(L"/slide")) return false;
        if (!part(ResolvePartName(dir, found->second.target))) return false;
        Slide slide;
        bool in_text = false, root = false;
        std::wstring paragraph;
        if (!Xml(bytes,
                 [&](IXmlReader* r, XmlNodeType t, std::wstring_view n) {
                     if (t == XmlNodeType_Element) {
                         if (n == L"sld") root = true;
                         if (n == L"t") in_text = !r->IsEmptyElement();
                         if (n == L"br") paragraph += L'\n';
                         if (n == L"pic" || n == L"blip" || n == L"graphicFrame" || n == L"oleObj")
                             cut = true;
                     } else if (t == XmlNodeType_EndElement) {
                         if (n == L"t") in_text = false;
                         if (n == L"p") {
                             if (!paragraph.empty()) slide.text.push_back(std::move(paragraph));
                             paragraph.clear();
                         }
                     } else if (in_text && (t == XmlNodeType_Text || t == XmlNodeType_CDATA ||
                                            t == XmlNodeType_Whitespace)) {
                         const wchar_t* p = nullptr;
                         UINT len = 0;
                         if (SUCCEEDED(r->GetValue(&p, &len)) && p) paragraph.append(p, len);
                     }
                 }) ||
            !root)
            return false;
        if (!paragraph.empty()) slide.text.push_back(std::move(paragraph));
        slides.push_back(std::move(slide));
    }
    return true;
}
struct Rec {
    uint16_t type = 0, instance = 0;
    bool container = false;
    size_t body = 0, end = 0;
};
bool Record(const std::vector<uint8_t>& s, size_t at, size_t end, Rec& r) {
    if (at > end || end > s.size() || end - at < 8) return false;
    r.type = CfbU16(s, at + 2);
    r.instance = CfbU16(s, at) >> 4;
    r.container = (CfbU16(s, at) & 15) == 15;
    r.body = at + 8;
    size_t len = CfbU32(s, at + 4);
    if (len > end - r.body) return false;
    r.end = r.body + len;
    return true;
}
std::wstring AtomText(const std::vector<uint8_t>& s, const Rec& r, size_t& budget, bool& cut) {
    std::wstring out;
    for (size_t p = r.body; p < r.end; p += r.type == 4000 ? 2 : 1) {
        wchar_t c = r.type == 4000 ? static_cast<wchar_t>(CfbU16(s, p)) : static_cast<wchar_t>(s[p]);
        if (c == L'\r' || c == 11) c = L'\n';
        if (c) {
            if (!budget) {
                cut = true;
                break;
            }
            out += c;
            --budget;
        }
    }
    return out;
}
bool WalkText(const std::vector<uint8_t>& s, size_t at, size_t end, Slide& slide, bool& cut, size_t& budget,
              size_t& nodes, unsigned depth = 0) {
    if (depth > 32) return false;
    while (at < end) {
        if (!budget || ++nodes > 1000000) {
            cut = true;
            return true;
        }
        Rec r;
        if (!Record(s, at, end, r)) return false;
        at = r.end;
        if (r.type == 3998) {
            if (r.end - r.body < 4) return false;
            slide.outline_refs.emplace_back(slide.text.size(), CfbU32(s, r.body));
            slide.text.emplace_back();
        } else if (r.type == 4000 || r.type == 4008) {
            if (r.type == 4000 && (r.end - r.body) % 2) return false;
            auto text = AtomText(s, r, budget, cut);
            if (!text.empty()) slide.text.push_back(std::move(text));
        } else if (r.container && !WalkText(s, r.body, r.end, slide, cut, budget, nodes, depth + 1))
            return false;
        // Drawing records can include artwork that the text reader does not render.
        if (r.type == 0xF00B) cut = true;
    }
    return true;
}
bool Ppt(const std::wstring& path, std::vector<Slide>& slides, size_t& total, uint32_t& read, bool& cut) {
    CompoundFile cfb;
    if (!cfb.Open(path)) return false;
    std::vector<uint8_t> s, current;
    if (!cfb.ReadStream(L"Current User", 4096, current) || current.size() < 28 ||
        CfbU16(current, 2) != 4086 || CfbU32(current, 12) != 0xE391C05F ||
        !cfb.ReadStream(L"PowerPoint Document", kTotalLimit, s))
        return false;
    read = static_cast<uint32_t>(s.size() + current.size());
    std::map<uint32_t, uint32_t> persist;
    std::set<uint32_t> edits;
    uint32_t edit = CfbU32(current, 16), doc_id = 0;
    size_t persist_entries = 0;
    while (edit) {
        if (!edits.insert(edit).second || edits.size() > 4096) return false;
        Rec e;
        if (!Record(s, edit, s.size(), e) || e.type != 4085 || e.end - e.body < 28) return false;
        if (e.end - e.body >= 32 && CfbU32(s, e.body + 28) != 0) return false;
        if (!doc_id) doc_id = CfbU32(s, e.body + 16);
        Rec p;
        if (!Record(s, CfbU32(s, e.body + 12), s.size(), p) || (p.type != 6002 && p.type != 6001))
            return false;
        size_t pos = p.body;
        while (pos < p.end) {
            if (p.end - pos < 4) return false;
            uint32_t bits = CfbU32(s, pos), id = bits & 0xFFFFF, count = bits >> 20;
            pos += 4;
            if (!count || count > (p.end - pos) / 4 || count > 0x100000u - id ||
                count > 1000000 - persist_entries)
                return false;
            persist_entries += count;
            for (uint32_t i = 0; i < count; ++i, pos += 4)
                persist.emplace(id + i, CfbU32(s, pos));
        }
        edit = CfbU32(s, e.body + 8);
    }
    auto doc = persist.find(doc_id);
    Rec d;
    if (doc == persist.end() || !Record(s, doc->second, s.size(), d) || d.type != 1000) return false;
    bool found_list = false;
    size_t budget = ipc::kPreviewMaxTableChars, nodes = 0;
    size_t resolved_budget = ipc::kPreviewMaxTableChars;
    for (size_t pos = d.body; pos < d.end;) {
        Rec list;
        if (!Record(s, pos, d.end, list)) return false;
        pos = list.end;
        if (list.type != 4080 || list.instance != 0) continue;
        if (found_list) return false;
        found_list = true;
        Slide* active = nullptr;
        std::vector<std::wstring> list_texts;
        auto finish_slide = [&]() {
            if (!active) return true;
            // Do not append a second copy of placeholder text. Resolve only the
            // actual OutlineTextRefAtom slots, or use the list for list-only files.
            for (const auto& [slot, index] : active->outline_refs) {
                if (index >= list_texts.size()) return false;
                const size_t length = (std::min)(resolved_budget, list_texts[index].size());
                active->text[slot] = list_texts[index].substr(0, length);
                resolved_budget -= length;
                if (length < list_texts[index].size()) cut = true;
            }
            if (active->text.empty()) active->text = std::move(list_texts);
            list_texts.clear();
            return true;
        };
        for (size_t p = list.body; p < list.end;) {
            if (++nodes > 1000000) return false;
            Rec r;
            if (!Record(s, p, list.end, r)) return false;
            p = r.end;
            if (r.type == 1011) {
                if (r.end - r.body < 16) return false;
                if (!finish_slide()) return false;
                ++total;
                active = nullptr;
                if (slides.size() >= kMaxSlides) {
                    cut = true;
                    continue;
                }
                auto entry = persist.find(CfbU32(s, r.body));
                Rec slide;
                if (entry == persist.end() || !Record(s, entry->second, s.size(), slide) ||
                    slide.type != 1006)
                    return false;
                slides.emplace_back();
                active = &slides.back();
                if (!WalkText(s, slide.body, slide.end, *active, cut, budget, nodes)) return false;
            } else if (active && r.type == 3999) {
                if (r.end - r.body < 4) return false;
                list_texts.emplace_back();
            } else if (active && (r.type == 4000 || r.type == 4008)) {
                if (r.type == 4000 && (r.end - r.body) % 2) return false;
                auto text = AtomText(s, r, budget, cut);
                if (list_texts.empty()) return false;
                list_texts.back() += text;
            }
        }
        if (!finish_slide()) return false;
    }
    return found_list && !slides.empty();
}
} // namespace
bool MakePresentationDocument(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                              bool& truncated) {
    payload.clear();
    bytes_read = 0;
    truncated = false;
    File file;
    uint8_t magic[8]{};
    if (!file.Open(path) || !file.ReadAt(0, magic, sizeof(magic))) return false;
    bool zip = magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
    static constexpr uint8_t cfb[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (!zip && memcmp(magic, cfb, 8) != 0) return false;
    std::vector<Slide> slides;
    size_t total = 0;
    if (!(zip ? Pptx(path, slides, total, bytes_read, truncated)
              : Ppt(path, slides, total, bytes_read, truncated)))
        return false;
    DocPayload out(ipc::kPreviewMaxTableChars - 256);
    out.Record(std::wstring(L"M\t") + (zip ? L"pptx" : L"ppt") + L"\t" + std::to_wstring(total));
    size_t loaded = 0;
    for (size_t i = 0; i < slides.size() && !out.full(); ++i) {
        const std::wstring title = std::to_wstring(i + 1);
        out.Record(L"P\t" + title);
        out.Record(L"C\t1\t" + std::to_wstring(i) + L"\t0\t" + title);
        for (const auto& text : slides[i].text) {
            out.Begin(L'p');
            out.Text(text);
            out.End();
            if (out.full()) break;
        }
        if (!out.full()) ++loaded;
    }
    truncated = truncated || out.full() || loaded < total;
    payload = std::move(out.str());
    payload += L"V\tchapters\t" + std::to_wstring(loaded) + L"\t" + std::to_wstring(total) + L"\n";
    return !slides.empty();
}
} // namespace pulse::preview
