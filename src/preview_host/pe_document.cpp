#include "pe_document.h"
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

namespace pulse::preview {
namespace {
constexpr size_t kEntryLimit = 1024;
constexpr size_t kMissing = static_cast<size_t>(-1);

std::wstring Hex(uint64_t value) {
    std::wostringstream out;
    out << L"0x" << std::uppercase << std::hex << value;
    return out.str();
}

struct Section { uint32_t rva, raw, size; };
struct Directory { uint32_t rva = 0, size = 0; };

class PeReader {
public:
    explicit PeReader(std::span<const uint8_t> bytes, size_t text_limit = 32000)
        : bytes_(bytes), text_limit_((std::min<size_t>)(text_limit, 32000)) {}

    bool Parse(std::wstring& text, bool& truncated, PeImports* imports = nullptr) {
        if (!Has(0, 64) || U16(0) != 0x5a4d) return false;
        const size_t nt = U32(60);
        if (!Has(nt, 24) || U32(nt) != 0x4550) return false;
        const auto count = U16(nt + 6);
        const auto optional_size = U16(nt + 20);
        const size_t opt = nt + 24;
        if (!Has(opt, optional_size) || optional_size < 2) return false;
        const auto magic = U16(opt);
        wide_ = magic == 0x20b;
        const size_t directory_offset = wide_ ? 112 : 96;
        if ((magic != 0x10b && !wide_) || optional_size < directory_offset ||
            count > 96 || !Has(opt + optional_size, static_cast<size_t>(count) * 40)) return false;
        headers_ = U32(opt + 60);
        image_base_ = wide_ ? U64(opt + 24) : U32(opt + 28);
        const auto directories = (std::min<size_t>)(16, U32(opt + directory_offset - 4));
        for (size_t i = 0; i < directories; ++i) {
            if (directory_offset + (i + 1) * 8 > optional_size) { damaged_ = true; break; }
            dirs_[i] = {U32(opt + directory_offset + i * 8), U32(opt + directory_offset + i * 8 + 4)};
        }
        const auto machine = U16(nt + 4);
        const wchar_t* architecture = machine == 0x8664 ? L"x64" : machine == 0x14c ? L"x86" :
            machine == 0xaa64 ? L"ARM64" : machine == 0xa641 ? L"ARM64EC" : machine == 0x1c4 ? L"ARM" : L"Unknown";
        Line(L"Portable Executable (" + std::wstring(wide_ ? L"PE32+" : L"PE32") + L")");
        Line(L"Architecture: " + std::wstring(architecture) + L" (" + Hex(machine) + L")");
        Line(L"Type: " + std::wstring((U16(nt + 22) & 0x2000) ? L"DLL" : L"Executable"));
        Line(L"Image base: " + Hex(image_base_));
        Line(L"Entry point RVA: " + Hex(U32(opt + 16)));
        Line(L"Image size: " + std::to_wstring(U32(opt + 56)) + L" bytes");
        Line(L"Subsystem: " + std::to_wstring(U16(opt + 68)) + L" (2 = GUI, 3 = Console)");
        Line(L"COFF timestamp: " + Hex(U32(nt + 8)));
        Line(L"Characteristics: " + Hex(U16(nt + 22)) + L"; DLL flags: " + Hex(U16(opt + 70)));
        Line(L"\nSections (" + std::to_wstring(count) + L")");
        for (size_t i = 0; i < count; ++i) {
            const size_t s = opt + optional_size + i * 40;
            sections_.push_back({U32(s + 12), U32(s + 20), U32(s + 16)});
            Line(L"  " + Ascii(s, 8, false) + L"  RVA " + Hex(U32(s + 12)) +
                 L"  virtual " + std::to_wstring(U32(s + 8)) + L"  raw " + Hex(U32(s + 20)) +
                 L" + " + std::to_wstring(U32(s + 16)) + L"  flags " + Hex(U32(s + 36)));
        }
        Dependencies(imports);
        if (imports) {
            imports->machine = machine;
            imports->incomplete = limited_ || damaged_;
            return true;
        }
        Version();
        Imports(false);
        Imports(true);
        Exports();
        if (damaged_) Line(L"\nSome metadata is invalid or unavailable in the captured bytes.");
        if (limited_) output_ += L"\n[Preview truncated]\n";
        text = std::move(output_);
        truncated = limited_ || damaged_;
        return true;
    }

private:
    bool Has(size_t offset, size_t size) const {
        return offset <= bytes_.size() && size <= bytes_.size() - offset;
    }
    uint16_t U16(size_t p) const {
        return Has(p, 2) ? static_cast<uint16_t>(bytes_[p] | (bytes_[p + 1] << 8)) : 0;
    }
    uint32_t U32(size_t p) const {
        return Has(p, 4) ? static_cast<uint32_t>(U16(p)) | (static_cast<uint32_t>(U16(p + 2)) << 16) : 0;
    }
    uint64_t U64(size_t p) const {
        return Has(p, 8) ? U32(p) | (static_cast<uint64_t>(U32(p + 4)) << 32) : 0;
    }
    size_t Rva(uint64_t rva, size_t size) {
        if (rva <= UINT32_MAX) {
            if (rva < headers_ && size <= headers_ - rva && Has(static_cast<size_t>(rva), size))
                return static_cast<size_t>(rva);
            for (const auto& s : sections_) {
                if (rva < s.rva) continue;
                const uint64_t delta = rva - s.rva;
                const uint64_t offset = s.raw + delta;
                if (delta <= s.size && size <= s.size - delta && offset <= bytes_.size() &&
                    Has(static_cast<size_t>(offset), size)) return static_cast<size_t>(offset);
            }
        }
        damaged_ = true;
        return kMissing;
    }
    std::wstring Ascii(size_t p, size_t cap = 256, bool terminated = true) {
        std::wstring result;
        for (size_t i = 0; i < cap; ++i) {
            if (!Has(p + i, 1)) { damaged_ = true; return result; }
            const auto c = bytes_[p + i];
            if (!c) return result;
            result += c >= 32 && c < 127 ? static_cast<wchar_t>(c) : L'.';
        }
        if (terminated) limited_ = true;
        return result;
    }
    std::wstring Name(uint64_t rva) {
        const size_t p = Rva(rva, 1);
        return p == kMissing ? L"<unavailable>" : Ascii(p);
    }
    void Line(const std::wstring& line) {
        if (output_.size() + line.size() + 1 <= text_limit_) output_ += line + L"\n";
        else limited_ = true;
    }
    void Thunks(uint64_t rva, bool va) {
        const size_t width = wide_ ? 8 : 4;
        for (size_t i = 0; i < kEntryLimit && !limited_; ++i) {
            const size_t p = Rva(rva + i * width, width);
            if (p == kMissing) return;
            const uint64_t value = wide_ ? U64(p) : U32(p);
            if (!value) return;
            if (value & (wide_ ? 0x8000000000000000ull : 0x80000000ull))
                Line(L"    ordinal " + std::to_wstring(value & 0xffff));
            else {
                if (va && value < image_base_) { damaged_ = true; return; }
                Line(L"    " + Name((va ? value - image_base_ : value) + 2));
            }
            if (i + 1 == kEntryLimit) limited_ = true;
        }
    }
    template <typename Visitor>
    void VisitImports(bool delay, Visitor visit) {
        const auto d = dirs_[delay ? 13 : 1];
        if (!d.rva || !d.size) return;
        const size_t stride = delay ? 32 : 20;
        const size_t count = (std::min<size_t>)(d.size / stride, 256);
        for (size_t i = 0; i < count && !limited_; ++i) {
            const size_t p = Rva(static_cast<uint64_t>(d.rva) + i * stride, stride);
            if (p == kMissing) return;
            bool empty = true;
            for (size_t j = 0; j < stride; j += 4) empty = empty && U32(p + j) == 0;
            if (empty) return;
            const bool va = delay && !(U32(p) & 1);
            uint64_t name = U32(p + (delay ? 4 : 12));
            uint64_t thunk = U32(p + (delay ? 16 : 0));
            if (!thunk) thunk = U32(p + (delay ? 12 : 16));
            if (va) {
                if (name < image_base_ || thunk < image_base_) { damaged_ = true; return; }
                name -= image_base_; thunk -= image_base_;
            }
            visit(Name(name), thunk, va);
        }
        if (count == 256) limited_ = true;
        else damaged_ = true; // No terminating descriptor within the directory.
    }
    void Dependencies(PeImports* imports) {
        struct Module { std::wstring name, key; bool normal = false, delay = false; };
        std::vector<Module> modules;
        for (const bool delay : {false, true}) {
            VisitImports(delay, [&](const std::wstring& name, uint64_t, bool) {
                std::wstring key = name;
                // PE import names are ASCII; DLL names are case-insensitive.
                for (auto& c : key) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + L'a' - L'A');
                auto it = std::find_if(modules.begin(), modules.end(),
                    [&](const Module& module) { return module.key == key; });
                if (it == modules.end()) {
                    modules.push_back({name, std::move(key)});
                    it = modules.end() - 1;
                }
                if (delay) it->delay = true;
                else it->normal = true;
            });
        }
        Line(L"\nDLL Dependencies");
        Line(L"  Direct module names recorded in this file:");
        if (modules.empty()) Line(L"  None recorded");
        for (const auto& module : modules) {
            if (imports) imports->modules.push_back({module.name, module.normal, module.delay});
            Line(L"  " + module.name + (module.normal && module.delay ? L" (imported, delay-loaded)" :
                module.delay ? L" (delay-loaded)" : L" (imported)"));
        }
    }
    void Imports(bool delay) {
        Line(delay ? L"\nDelay imports" : L"\nImports");
        bool any = false;
        VisitImports(delay, [&](const std::wstring& name, uint64_t thunk, bool va) {
            any = true;
            Line(L"  " + name);
            if (thunk) Thunks(thunk, va);
        });
        if (!any) Line(L"  None");
    }
    void Exports() {
        Line(L"\nExports");
        const auto d = dirs_[0];
        if (!d.rva || !d.size) { Line(L"  None"); return; }
        const size_t p = Rva(d.rva, 40);
        if (p == kMissing || d.size < 40) { damaged_ = true; return; }
        Line(L"  Module: " + Name(U32(p + 12)));
        const uint32_t functions = U32(p + 20), names = U32(p + 24), base = U32(p + 16);
        const size_t count = (std::min<size_t>)(functions, kEntryLimit);
        std::vector<std::wstring> labels(count);
        for (size_t i = 0; i < (std::min<size_t>)(names, kEntryLimit); ++i) {
            const size_t np = Rva(static_cast<uint64_t>(U32(p + 32)) + i * 4, 4);
            const size_t op = Rva(static_cast<uint64_t>(U32(p + 36)) + i * 2, 2);
            if (np == kMissing || op == kMissing) break;
            const auto index = U16(op);
            if (index < count) {
                if (!labels[index].empty()) labels[index] += L", ";
                labels[index] += Name(U32(np));
            } else if (index >= functions) damaged_ = true;
        }
        for (size_t i = 0; i < count && !limited_; ++i) {
            const size_t fp = Rva(static_cast<uint64_t>(U32(p + 28)) + i * 4, 4);
            if (fp == kMissing) break;
            const uint32_t target = U32(fp);
            if (!target) continue;
            std::wstring line = L"  " + std::to_wstring(static_cast<uint64_t>(base) + i) + L"  " +
                (labels[i].empty() ? L"(ordinal only)" : labels[i]);
            if (target >= d.rva && static_cast<uint64_t>(target) - d.rva < d.size)
                line += L" -> " + Name(target);
            else line += L"  RVA " + Hex(target);
            Line(line);
        }
        if (functions > kEntryLimit || names > kEntryLimit) limited_ = true;
    }

    void VersionBlock(size_t p, size_t end, unsigned depth) {
        if (depth > 5 || ++version_nodes_ > 256) { limited_ = true; return; }
        if (!Has(p, 6) || end - p < 6) { damaged_ = true; return; }
        const size_t length = U16(p), value_length = U16(p + 2);
        if (length < 6 || length > end - p) { damaged_ = true; return; }
        const size_t stop = p + length;
        size_t cursor = p + 6;
        std::wstring key;
        while (cursor + 2 <= stop && U16(cursor) && key.size() < 128) {
            key += static_cast<wchar_t>(U16(cursor)); cursor += 2;
        }
        if (cursor + 2 > stop || U16(cursor)) { damaged_ = true; return; }
        cursor = (cursor + 2 + 3) & ~size_t{3};
        const size_t value_bytes = value_length * (U16(p + 4) == 1 ? 2 : 1);
        if (cursor > stop || value_bytes > stop - cursor) { damaged_ = true; return; }
        if (key == L"VS_VERSION_INFO" && value_bytes >= 52 && U32(cursor) == 0xfeef04bd) {
            Line(L"  File version: " + std::to_wstring(U16(cursor + 10)) + L"." +
                std::to_wstring(U16(cursor + 8)) + L"." + std::to_wstring(U16(cursor + 14)) +
                L"." + std::to_wstring(U16(cursor + 12)));
            Line(L"  Product version: " + std::to_wstring(U16(cursor + 18)) + L"." +
                std::to_wstring(U16(cursor + 16)) + L"." + std::to_wstring(U16(cursor + 22)) +
                L"." + std::to_wstring(U16(cursor + 20)));
        } else if (U16(p + 4) == 1 && value_length) {
            std::wstring value;
            for (size_t i = 0; i < (std::min<size_t>)(value_length, 512); ++i) {
                const auto c = U16(cursor + i * 2);
                if (!c) break;
                value += c >= 32 ? static_cast<wchar_t>(c) : L' ';
            }
            Line(L"  " + key + L": " + value);
            if (value_length > 512) limited_ = true;
        }
        cursor = (cursor + value_bytes + 3) & ~size_t{3};
        while (cursor + 6 <= stop && !limited_) {
            const size_t child = U16(cursor);
            if (child < 6 || child > stop - cursor) { damaged_ = true; break; }
            VersionBlock(cursor, stop, depth + 1);
            cursor = (cursor + child + 3) & ~size_t{3};
        }
    }
    void Resource(uint32_t relative, unsigned depth) {
        const auto d = dirs_[2];
        if (depth > 2 || ++resource_nodes_ > 256) { limited_ = true; return; }
        if (relative > d.size || d.size - relative < 16) { damaged_ = true; return; }
        const size_t p = Rva(static_cast<uint64_t>(d.rva) + relative, 16);
        if (p == kMissing) return;
        const size_t count = static_cast<size_t>(U16(p + 12)) + U16(p + 14);
        if (count > (d.size - relative - 16) / 8) { damaged_ = true; return; }
        for (size_t i = 0; i < (std::min<size_t>)(count, 256) && !limited_; ++i) {
            const size_t e = Rva(static_cast<uint64_t>(d.rva) + relative + 16 + i * 8, 8);
            if (e == kMissing) return;
            if (depth == 0 && U32(e) != 16) continue; // RT_VERSION only.
            const uint32_t target = U32(e + 4);
            if (target & 0x80000000u) Resource(target & 0x7fffffffu, depth + 1);
            else {
                if (target > d.size || d.size - target < 16) { damaged_ = true; continue; }
                const size_t data = Rva(static_cast<uint64_t>(d.rva) + target, 16);
                if (data == kMissing) continue;
                const uint32_t size = U32(data + 4);
                if (size > 65536) { limited_ = true; continue; }
                const size_t start = Rva(U32(data), size);
                if (start != kMissing) VersionBlock(start, start + size, 0);
            }
        }
        if (count > 256) limited_ = true;
    }
    void Version() {
        Line(L"\nVersion information");
        if (dirs_[2].rva && dirs_[2].size) Resource(0, 0);
        if (!version_nodes_) Line(L"  Not present");
    }

    std::span<const uint8_t> bytes_;
    size_t text_limit_ = 32000;
    std::vector<Section> sections_;
    std::array<Directory, 16> dirs_{};
    std::wstring output_;
    uint64_t image_base_ = 0;
    uint32_t headers_ = 0;
    unsigned resource_nodes_ = 0, version_nodes_ = 0;
    bool wide_ = false, damaged_ = false, limited_ = false;
};
} // namespace

bool MakePeDocument(std::span<const uint8_t> bytes, std::wstring& text, bool& truncated, size_t text_limit) {
    text.clear();
    truncated = false;
    return PeReader(bytes, text_limit).Parse(text, truncated);
}
bool ReadPeImports(std::span<const uint8_t> bytes, PeImports& imports) {
    imports = {};
    std::wstring unused;
    bool truncated = false;
    return PeReader(bytes).Parse(unused, truncated, &imports);
}
} // namespace pulse::preview
