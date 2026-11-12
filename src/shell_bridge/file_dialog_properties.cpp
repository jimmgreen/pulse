#include "file_dialog_properties.h"
#include "property_value_text.h"
#include <propvarutil.h>
#include <algorithm>
#include <cwchar>

namespace pulse::shell_bridge {
using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t kMaxItems = 128;
constexpr size_t kMaxText = 32768;
constexpr size_t kMaxStoreItems = 1024;
constexpr size_t kMaxOpaqueBytes = 1024 * 1024;
struct Value {
    PROPVARIANT value{};
    ~Value() { PropVariantClear(&value); }
};
bool Text(const wchar_t* text) {
    return !text || wcsnlen_s(text, kMaxText + 1) <= kMaxText;
}
bool EditableScalar(VARTYPE type) {
    return type == VT_LPWSTR || type == VT_I1 || type == VT_UI1 || type == VT_I2 || type == VT_UI2 ||
        type == VT_I4 || type == VT_UI4 || type == VT_I8 || type == VT_UI8 || type == VT_BOOL || property_text::ExactScalar(type);
}
bool Safe(const PROPVARIANT& value) {
    switch (value.vt) {
    case VT_EMPTY: case VT_NULL: case VT_I1: case VT_UI1: case VT_I2: case VT_UI2:
    case VT_I4: case VT_UI4: case VT_I8: case VT_UI8: case VT_R4: case VT_R8:
    case VT_BOOL: case VT_DATE: case VT_FILETIME: return true;
    case VT_LPWSTR: return Text(value.pwszVal);
    // Preserve uncollected document payloads without exposing them as editable
    // text. Memory stores deep-copy these values; no COM object is retained.
    case VT_BLOB: return value.blob.cbSize <= kMaxOpaqueBytes && (!value.blob.cbSize || value.blob.pBlobData);
    case VT_VECTOR | VT_UI1: return value.caub.cElems <= kMaxOpaqueBytes && (!value.caub.cElems || value.caub.pElems);
    case VT_CF: return value.pclipdata && value.pclipdata->cbSize >= sizeof(value.pclipdata->ulClipFmt) &&
        value.pclipdata->cbSize <= kMaxOpaqueBytes &&
        (value.pclipdata->cbSize == sizeof(value.pclipdata->ulClipFmt) || value.pclipdata->pClipData);
    case VT_CLSID: return value.puuid != nullptr;
    case VT_VECTOR | VT_LPWSTR:
        if (value.calpwstr.cElems > kMaxItems || (value.calpwstr.cElems && !value.calpwstr.pElems)) return false;
        for (ULONG i = 0; i < value.calpwstr.cElems; ++i) if (!Text(value.calpwstr.pElems[i])) return false;
        return true;
    default: return false;
    }
}
}
HRESULT FileDialogProperties::Clone(IPropertyStore* source, IPropertyStore** result, CaptureStatus* status) {
    if (!result) return E_POINTER;
    *result = nullptr;
    ComPtr<IPropertyStore> copy;
    HRESULT hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&copy));
    if (FAILED(hr)) return hr;
    DWORD count = 0;
    if (source) { hr = source->GetCount(&count); if (FAILED(hr)) return hr; }
    if (status) status->count = count;
    if (count > kMaxStoreItems) return E_NOTIMPL;
    size_t total_text = 0, total_opaque = 0;
    for (DWORD i = 0; i < count; ++i) {
        PROPERTYKEY key{};
        Value value;
        if (status) { status->index = i; status->type = VT_EMPTY; }
        hr = source->GetAt(i, &key); if (FAILED(hr)) return hr;
        hr = source->GetValue(key, &value.value); if (FAILED(hr)) return hr;
        if (status) status->type = value.value.vt;
        if (!Safe(value.value)) return E_NOTIMPL;
        if (value.value.vt == VT_LPWSTR && value.value.pwszVal) total_text += wcslen(value.value.pwszVal);
        if (value.value.vt == (VT_VECTOR | VT_LPWSTR)) {
            for (ULONG j = 0; j < value.value.calpwstr.cElems; ++j)
                if (value.value.calpwstr.pElems[j]) total_text += wcslen(value.value.calpwstr.pElems[j]);
        }
        if (value.value.vt == VT_BLOB) total_opaque += value.value.blob.cbSize;
        if (value.value.vt == (VT_VECTOR | VT_UI1)) total_opaque += value.value.caub.cElems;
        if (value.value.vt == VT_CF) total_opaque += value.value.pclipdata->cbSize;
        if (total_text > 262144 || total_opaque > kMaxOpaqueBytes) return E_NOTIMPL;
        // Memory property stores own a deep PROPVARIANT copy after SetValue.
        hr = copy->SetValue(key, value.value); if (FAILED(hr)) return hr;
    }
    hr = copy->Commit(); if (FAILED(hr)) return hr;
    return copy.CopyTo(result);
}
HRESULT FileDialogProperties::MakeFields(IPropertyStore* store, const std::vector<Description>& descriptions,
    std::vector<DialogPropertyField>& result) const {
    for (const auto& description : descriptions) {
        DialogPropertyField field{ description.key, description.type, description.label, {} };
        field.read_only = description.read_only;
        field.group = description.group;
        if (description.group) { result.push_back(std::move(field)); continue; }
        Value value;
        HRESULT hr = store->GetValue(description.key, &value.value); if (FAILED(hr)) return hr;
        if (!Safe(value.value)) return E_NOTIMPL;
        if (value.value.vt != VT_EMPTY && value.value.vt != description.type) {
            // Project a private value through the Windows schema. Keep the store
            // unchanged until an explicit edit; never accept truncating coercion.
            hr = PSCoerceToCanonicalValue(description.key, &value.value);
            if (hr != S_OK || !Safe(value.value) || (value.value.vt != VT_EMPTY && value.value.vt != description.type)) return E_NOTIMPL;
        }
        if (description.read_only && description.type != (VT_VECTOR | VT_LPWSTR)) {
            PWSTR formatted = nullptr;
            if (value.value.vt != VT_EMPTY) {
                hr = PSFormatForDisplayAlloc(description.key, value.value, PDFF_DEFAULT, &formatted);
                if (FAILED(hr)) { CoTaskMemFree(formatted); return hr; }
            }
            try { field.values.emplace_back(formatted ? formatted : L""); }
            catch (...) { CoTaskMemFree(formatted); throw; }
            CoTaskMemFree(formatted);
        } else if (description.type == VT_LPWSTR) field.values.emplace_back(value.value.vt == VT_EMPTY || !value.value.pwszVal ? L"" : value.value.pwszVal);
        else if (property_text::ExactScalar(description.type)) {
            std::wstring text;
            if (value.value.vt != VT_EMPTY) {
                hr = property_text::Format(value.value, text);
                if (FAILED(hr)) return hr;
            }
            field.values.push_back(std::move(text));
        } else if (EditableScalar(description.type)) {
            PWSTR text = nullptr;
            if (value.value.vt != VT_EMPTY) {
                hr = PropVariantToStringAlloc(value.value, &text);
                if (FAILED(hr)) return hr;
            }
            try { field.values.emplace_back(text ? text : L""); }
            catch (...) { CoTaskMemFree(text); throw; }
            CoTaskMemFree(text);
        } else if (value.value.vt != VT_EMPTY) {
            for (ULONG i = 0; i < value.value.calpwstr.cElems; ++i)
                field.values.emplace_back(value.value.calpwstr.pElems[i] ? value.value.calpwstr.pElems[i] : L"");
        }
        result.push_back(std::move(field));
    }
    return S_OK;
}
HRESULT FileDialogProperties::SetStore(IPropertyStore* store) noexcept try {
    ComPtr<IPropertyStore> original, working;
    capture_status_ = {};
    HRESULT hr = Clone(store, &original, &capture_status_); if (FAILED(hr)) return hr;
    hr = Clone(original.Get(), &working); if (FAILED(hr)) return hr;
    std::vector<DialogPropertyField> fields;
    hr = MakeFields(working.Get(), descriptions_, fields); if (FAILED(hr)) return hr;
    original_ = std::move(original); working_ = std::move(working); fields_ = std::move(fields);
    invalid_edits_.clear();
    return S_OK;
} catch (...) { return E_OUTOFMEMORY; }
HRESULT FileDialogProperties::SetCollected(IPropertyDescriptionList* list) noexcept try {
    std::vector<Description> descriptions;
    UINT count = 0;
    HRESULT hr = list ? list->GetCount(&count) : S_OK; if (FAILED(hr)) return hr;
    if (count > kMaxItems) return E_NOTIMPL;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IPropertyDescription> description;
        hr = list->GetAt(i, IID_PPV_ARGS(&description)); if (FAILED(hr)) return hr;
        PROPDESC_TYPE_FLAGS flags = PDTF_DEFAULT;
        hr = description->GetTypeFlags(PDTF_MASK_ALL, &flags); if (FAILED(hr)) return hr;
        // ISINNATE means read-only; ISGROUP is a heading, not an editable value.
        // ISSYSTEMPROPERTY only denotes schema ownership and does not forbid editing.
        Description item;
        item.group = (flags & PDTF_ISGROUP) != 0;
        item.read_only = item.group || (flags & PDTF_ISINNATE) != 0;
        hr = description->GetPropertyKey(&item.key); if (FAILED(hr)) return hr;
        hr = description->GetPropertyType(&item.type); if (FAILED(hr)) return hr;
        if (!item.read_only && !EditableScalar(item.type) && item.type != (VT_VECTOR | VT_LPWSTR)) return E_NOTIMPL;
        PWSTR label = nullptr;
        hr = description->GetDisplayName(&label);
        if (FAILED(hr)) { CoTaskMemFree(label); return hr; }
        if (!label || !Text(label)) { CoTaskMemFree(label); return E_NOTIMPL; }
        try { item.label = label; } catch (...) { CoTaskMemFree(label); throw; }
        CoTaskMemFree(label);
        if (std::any_of(descriptions.begin(), descriptions.end(), [&](const auto& old) { return IsEqualPropertyKey(old.key, item.key); })) return E_INVALIDARG;
        descriptions.push_back(std::move(item));
    }
    ComPtr<IPropertyStore> working;
    hr = Clone(working_.Get(), &working); if (FAILED(hr)) return hr;
    std::vector<DialogPropertyField> fields;
    hr = MakeFields(working.Get(), descriptions, fields); if (FAILED(hr)) return hr;
    working_ = std::move(working); descriptions_ = std::move(descriptions); fields_ = std::move(fields);
    invalid_edits_.clear();
    return S_OK;
} catch (...) { return E_OUTOFMEMORY; }
HRESULT FileDialogProperties::Edit(REFPROPERTYKEY key, const std::vector<std::wstring>& values) noexcept try {
    const auto found = std::find_if(descriptions_.begin(), descriptions_.end(), [&](const auto& item) { return IsEqualPropertyKey(item.key, key); });
    if (found == descriptions_.end()) return E_INVALIDARG;
    if (found->read_only || found->group) return E_ACCESSDENIED;
    if (values.size() > kMaxItems || (EditableScalar(found->type) && values.size() != 1)) return E_INVALIDARG;
    for (const auto& text : values) if (text.size() > kMaxText || text.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    Value value;
    HRESULT hr;
    if (found->type == VT_LPWSTR) hr = InitPropVariantFromString(values[0].c_str(), &value.value);
    else if (EditableScalar(found->type)) {
        if (values[0].empty()) hr = S_OK; // VT_EMPTY clears the optional metadata value.
        else {
            if (!property_text::ExactScalar(found->type) && found->type != VT_BOOL) {
                const auto first = values[0][0] == L'-' || values[0][0] == L'+' ? 1u : 0u;
                if (first == values[0].size() || values[0].find_first_not_of(L"0123456789", first) != std::wstring::npos)
                    return E_INVALIDARG; // Do not round fractions or accept locale-dependent separators.
            }
            Value source;
            if (property_text::ExactScalar(found->type)) hr = property_text::Parse(found->type, values[0], value.value);
            else {
                hr = InitPropVariantFromString(values[0].c_str(), &source.value);
                if (SUCCEEDED(hr)) hr = PropVariantChangeType(&value.value, source.value, PVCHF_NOHEXSTRING, found->type);
            }
            if (SUCCEEDED(hr)) {
                Value canonical;
                hr = PropVariantCopy(&canonical.value, &value.value);
                if (SUCCEEDED(hr)) hr = PSCoerceToCanonicalValue(key, &canonical.value);
                if (hr != S_OK) return FAILED(hr) ? hr : E_INVALIDARG;
                if (hr == S_OK && (canonical.value.vt != value.value.vt ||
                    (property_text::ExactScalar(value.value.vt) ? !property_text::Same(canonical.value, value.value) :
                    PropVariantCompareEx(canonical.value, value.value, PVCU_DEFAULT, PVCF_DEFAULT) != 0))) hr = E_INVALIDARG;
            }
        }
    } else {
        std::vector<PCWSTR> pointers;
        for (const auto& text : values) pointers.push_back(text.c_str());
        hr = InitPropVariantFromStringVector(pointers.data(), static_cast<ULONG>(pointers.size()), &value.value);
    }
    if (FAILED(hr)) return hr;
    ComPtr<IPropertyStore> working;
    hr = Clone(working_.Get(), &working); if (FAILED(hr)) return hr;
    hr = working->SetValue(key, value.value); if (FAILED(hr)) return hr;
    hr = working->Commit(); if (FAILED(hr)) return hr;
    ComPtr<IPropertyStore> validated;
    hr = Clone(working.Get(), &validated); if (FAILED(hr)) return hr;
    working = std::move(validated);
    std::vector<DialogPropertyField> fields;
    hr = MakeFields(working.Get(), descriptions_, fields); if (FAILED(hr)) return hr;
    working_ = std::move(working); fields_ = std::move(fields);
    std::erase_if(invalid_edits_, [&](const auto& item) { return IsEqualPropertyKey(item, key); });
    return S_OK;
} catch (...) { return E_OUTOFMEMORY; }
HRESULT FileDialogProperties::EditDraft(REFPROPERTYKEY key, const std::vector<std::wstring>& values) noexcept try {
    const auto field = std::find_if(descriptions_.begin(), descriptions_.end(), [&](const auto& item) { return IsEqualPropertyKey(item.key, key); });
    if (field != descriptions_.end() && (field->read_only || field->group)) return E_ACCESSDENIED;
    if (field == descriptions_.end() || values.size() > kMaxItems ||
        (EditableScalar(field->type) && values.size() != 1)) return E_INVALIDARG;
    for (const auto& text : values)
        if (text.size() > kMaxText || text.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    const HRESULT hr = Edit(key, values);
    if (hr == S_OK) {
        std::erase_if(invalid_edits_, [&](const auto& item) { return IsEqualPropertyKey(item, key); });
        return S_OK;
    }
    if (hr != E_INVALIDARG && hr != DISP_E_TYPEMISMATCH && hr != DISP_E_OVERFLOW &&
        hr != HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && hr != HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW)) return hr;
    if (std::none_of(invalid_edits_.begin(), invalid_edits_.end(), [&](const auto& item) { return IsEqualPropertyKey(item, key); }))
        invalid_edits_.push_back(key);
    return S_FALSE;
} catch (...) { return E_OUTOFMEMORY; }
HRESULT FileDialogProperties::Snapshot(IPropertyStore** store) const noexcept try {
    return Clone(working_.Get(), store);
} catch (...) { if (store) *store = nullptr; return E_OUTOFMEMORY; }
HRESULT FileDialogProperties::ResetEdits() noexcept try {
    ComPtr<IPropertyStore> working;
    HRESULT hr = Clone(original_.Get(), &working); if (FAILED(hr)) return hr;
    std::vector<DialogPropertyField> fields;
    hr = MakeFields(working.Get(), descriptions_, fields); if (FAILED(hr)) return hr;
    working_ = std::move(working); fields_ = std::move(fields);
    invalid_edits_.clear();
    return S_OK;
} catch (...) { return E_OUTOFMEMORY; }
}



