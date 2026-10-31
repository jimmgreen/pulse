#include "../shell_bridge/file_dialog_properties.h"
#include <propkey.h>
#include <propvarutil.h>
#include <cstdio>
using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
int wmain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; };
    {
        ComPtr<IPropertyStore> source;
        check(SUCCEEDED(PSCreateMemoryPropertyStore(IID_PPV_ARGS(&source))), "memory store");
        if (!source) { CoUninitialize(); return 1; }
        PROPVARIANT value{};
        InitPropVariantFromString(L"Original", &value);
        source->SetValue(PKEY_Title, value); PropVariantClear(&value);
        PCWSTR authors[] = { L"One; literal semicolon", L"Two" };
        InitPropVariantFromStringVector(authors, 2, &value);
        source->SetValue(PKEY_Author, value); PropVariantClear(&value);
        value.vt = VT_UI4; value.ulVal = 42;
        source->SetValue(PKEY_Rating, value); PropVariantClear(&value);
        FileDialogProperties model;
        check(SUCCEEDED(model.SetStore(source.Get())), "capture source");
        ComPtr<IPropertyDescriptionList> list;
        check(SUCCEEDED(PSGetPropertyDescriptionListFromString(L"prop:System.Title;System.Author", IID_PPV_ARGS(&list))), "real property descriptions");
        check(SUCCEEDED(model.SetCollected(list.Get())) && model.Fields().size() == 2, "capture scalar and vector labels");
        if (model.Fields().size() == 2) check(!model.Fields()[0].label.empty() && model.Fields()[1].values.size() == 2 && model.Fields()[1].values[0] == authors[0], "preserve vector delimiters");
        check(SUCCEEDED(model.Edit(PKEY_Title, { L"Edited" })), "edit scalar");
        check(SUCCEEDED(model.Edit(PKEY_Author, { L"New;still one", L"Another" })), "edit vector entries");
        ComPtr<IPropertyStore> snapshot;
        check(SUCCEEDED(model.Snapshot(&snapshot)), "independent snapshot");
        snapshot->GetValue(PKEY_Rating, &value);
        check(value.vt == VT_UI4 && value.ulVal == 42, "uncollected type and value preserved"); PropVariantClear(&value);
        source->GetValue(PKEY_Title, &value);
        check(value.vt == VT_LPWSTR && std::wstring(value.pwszVal) == L"Original", "source isolated from edits"); PropVariantClear(&value);
        check(SUCCEEDED(model.Edit(PKEY_Author, {})), "clear vector");
        check(SUCCEEDED(model.Edit(PKEY_Title, { L"" })), "clear scalar");
        check(SUCCEEDED(model.ResetEdits()), "cancel edits");
        check(model.Fields()[0].values[0] == L"Original" && model.Fields()[1].values[0] == authors[0], "cancel restores original");
        snapshot->GetValue(PKEY_Title, &value);
        check(value.vt == VT_LPWSTR && std::wstring(value.pwszVal) == L"Edited", "snapshot survives reset"); PropVariantClear(&value);
        ComPtr<IPropertyDescriptionList> unsupported;
        PSGetPropertyDescriptionListFromString(L"prop:System.Rating", IID_PPV_ARGS(&unsupported));
        check(FAILED(model.SetCollected(unsupported.Get())) && model.Fields().size() == 2, "unsupported collected type preserves model");
        ComPtr<IPropertyDescriptionList> innate;
        check(SUCCEEDED(PSGetPropertyDescriptionListFromString(L"prop:System.Security.EncryptionOwners", IID_PPV_ARGS(&innate))), "load innate string-vector description");
        check(innate && FAILED(model.SetCollected(innate.Get())) && model.Fields().size() == 2,
            "readonly innate collection cannot become editable after SetStore");
        ComPtr<IPropertyDescription> innate_description;
        if (innate && SUCCEEDED(innate->GetAt(0, IID_PPV_ARGS(&innate_description)))) {
            PROPERTYKEY innate_key{};
            innate_description->GetPropertyKey(&innate_key);
            check(FAILED(model.Edit(innate_key, { L"replacement" })), "rejected innate property has no editable field");
        }
        FileDialogProperties document_model;
        ComPtr<IPropertyDescriptionList> document_fields;
        check(SUCCEEDED(PSGetPropertyDescriptionListFromString(
            L"prop:System.Author;System.Keywords;System.Title;System.Subject;System.Document.Manager;System.Company;System.Category;System.Comment",
            IID_PPV_ARGS(&document_fields))), "load eight document metadata descriptions");
        check(SUCCEEDED(document_model.SetStore(source.Get())) && document_fields &&
            SUCCEEDED(document_model.SetCollected(document_fields.Get())) && document_model.Fields().size() == 8,
            "all eight editable document fields remain admitted");
        // Real callers may provide noncanonical values even for a typed schema.
        ComPtr<IPropertyStore> noncanonical;
        PSCreateMemoryPropertyStore(IID_PPV_ARGS(&noncanonical));
        value.vt = VT_NULL;
        noncanonical->SetValue(PKEY_Title, value); PropVariantClear(&value);
        InitPropVariantFromString(L"Alice; Bob", &value);
        noncanonical->SetValue(PKEY_Author, value); PropVariantClear(&value);
        FileDialogProperties projected;
        check(projected.SetStore(noncanonical.Get()) == S_OK, "capture null and scalar source values");
        const HRESULT projected_hr = projected.SetCollected(list.Get());
        check(projected_hr == S_OK, "noncanonical text fields admit native schema conversion");
        if (projected_hr == S_OK) {
            PROPVARIANT expected{};
            InitPropVariantFromString(L"Alice; Bob", &expected);
            const HRESULT canonical_hr = PSCoerceToCanonicalValue(PKEY_Author, &expected);
            const auto& fields = projected.Fields();
            bool same = canonical_hr == S_OK && expected.vt == (VT_VECTOR | VT_LPWSTR) && fields.size() == 2 &&
                fields[0].values.size() == 1 && fields[0].values[0].empty() && fields[1].values.size() == expected.calpwstr.cElems;
            if (same) for (size_t i = 0; i < fields[1].values.size(); ++i) same = same && fields[1].values[i] == expected.calpwstr.pElems[i];
            check(same, "display projection matches Windows canonical conversion");
            PropVariantClear(&expected);
            ComPtr<IPropertyStore> untouched;
            check(projected.Snapshot(&untouched) == S_OK, "snapshot noncanonical original");
            untouched->GetValue(PKEY_Title, &value);
            check(value.vt == VT_NULL, "unedited null stays null in returned store"); PropVariantClear(&value);
            untouched->GetValue(PKEY_Author, &value);
            check(value.vt == VT_LPWSTR && std::wstring(value.pwszVal) == L"Alice; Bob", "unedited scalar source remains exact"); PropVariantClear(&value);
            check(projected.Edit(PKEY_Author, {L"Literal; one entry", L"Second"}) == S_OK, "edit converted field uses explicit vector values");
            check(projected.ResetEdits() == S_OK && projected.Snapshot(&untouched) == S_OK, "cancel converted edits restores initial source");
            untouched->GetValue(PKEY_Author, &value);
            check(value.vt == VT_LPWSTR && std::wstring(value.pwszVal) == L"Alice; Bob", "cancel restores original scalar type and text"); PropVariantClear(&value);
        }
        ComPtr<IPropertyStore> bad;
        PSCreateMemoryPropertyStore(IID_PPV_ARGS(&bad));
        value.vt = VT_BLOB; value.blob.cbSize = 0; value.blob.pBlobData = nullptr;
        bad->SetValue(PKEY_Comment, value); PropVariantClear(&value);
        check(FAILED(model.SetStore(bad.Get())) && model.Fields()[0].values[0] == L"Original", "unsupported data rejects transaction");
        check(FAILED(model.Edit(PKEY_Title, { L"a", L"b" })) && model.Fields()[0].values[0] == L"Original", "invalid edit preserves model");
    }
    CoUninitialize();
    return failures ? 1 : 0;
}

