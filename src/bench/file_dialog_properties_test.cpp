#include "../shell_bridge/file_dialog_properties.h"
#include "../shell_bridge/property_value_text.h"
#include <clocale>
#include <cmath>
#include <limits>
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
        FileDialogProperties numeric;
        check(numeric.SetStore(source.Get()) == S_OK && numeric.SetCollected(unsupported.Get()) == S_OK &&
            numeric.Fields().size() == 1 && numeric.Fields()[0].values == std::vector<std::wstring>{L"42"}, "integer property exposes existing typed value");
        check(numeric.Edit(PKEY_Rating, {L"75"}) == S_OK, "integer property accepts exact numeric edit");
        ComPtr<IPropertyStore> numeric_snapshot;
        numeric.Snapshot(&numeric_snapshot);numeric_snapshot->GetValue(PKEY_Rating, &value);
        check(value.vt == VT_UI4 && value.ulVal == 75, "integer edit retains native property type");PropVariantClear(&value);
        check(FAILED(numeric.Edit(PKEY_Rating, {L"1.5"})) && FAILED(numeric.Edit(PKEY_Rating, {L"4294967296"})) &&
            FAILED(numeric.Edit(PKEY_Rating, {L"-1"})) && !numeric.Fields().empty() && numeric.Fields()[0].values[0] == L"75", "invalid numeric edits leave accepted value unchanged");
        check(numeric.Edit(PKEY_Rating, {L""}) == S_OK && !numeric.Fields().empty() && numeric.Fields()[0].values[0].empty(), "empty integer edit clears optional property");
        check(numeric.ResetEdits() == S_OK && !numeric.Fields().empty() && numeric.Fields()[0].values[0] == L"42", "cancel restores original typed value");
        ComPtr<IPropertyDescriptionList> bool_fields;
        check(PSGetPropertyDescriptionListFromString(L"prop:System.IsFlagged", IID_PPV_ARGS(&bool_fields)) == S_OK, "load boolean property schema");
        FileDialogProperties boolean;
        check(boolean.SetStore(source.Get()) == S_OK && boolean.SetCollected(bool_fields.Get()) == S_OK, "collect boolean property");
        check(boolean.Edit(PKEY_IsFlagged, {L"true"}) == S_OK, "edit boolean property");
        ComPtr<IPropertyStore> bool_snapshot;
        boolean.Snapshot(&bool_snapshot);bool_snapshot->GetValue(PKEY_IsFlagged, &value);
        check(value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE, "boolean edit retains native type");PropVariantClear(&value);
        check(FAILED(boolean.Edit(PKEY_IsFlagged, {L"not a boolean"})), "invalid boolean is rejected");
        check(numeric.EditDraft(PKEY_Rating, {L"-"}) == S_FALSE && numeric.HasInvalidEdits() &&
            numeric.Fields()[0].values[0] == L"42", "partial numeric draft does not overwrite committed value");
        check(numeric.EditDraft(PKEY_Rating, {L"25"}) == S_OK && !numeric.HasInvalidEdits(), "corrected numeric draft becomes valid");
        check(boolean.EditDraft(PKEY_IsFlagged, {L"t"}) == S_FALSE && boolean.HasInvalidEdits(), "partial boolean draft is a recoverable validation state");
        check(boolean.EditDraft(PKEY_IsFlagged, {L"true"}) == S_OK && !boolean.HasInvalidEdits(), "corrected boolean draft becomes valid");
        check(numeric.EditDraft(PKEY_Rating, {L"bad"}) == S_FALSE && numeric.ResetEdits() == S_OK &&
            !numeric.HasInvalidEdits() && numeric.Fields()[0].values[0] == L"42", "cancel discards invalid draft and restores original value");
        ComPtr<IPropertyDescriptionList> innate;
        check(SUCCEEDED(PSGetPropertyDescriptionListFromString(L"prop:System.Security.EncryptionOwners", IID_PPV_ARGS(&innate))), "load innate string-vector description");
        check(innate && FAILED(model.SetCollected(innate.Get())) && model.Fields().size() == 2,
            "innate schema without a native display label preserves the prior supported collection");
        ComPtr<IPropertyDescription> innate_description;
        if (innate && SUCCEEDED(innate->GetAt(0, IID_PPV_ARGS(&innate_description)))) {
            PROPERTYKEY innate_key{};
            innate_description->GetPropertyKey(&innate_key);
            check(FAILED(model.Edit(innate_key, { L"replacement" })), "unrepresented innate property has no editable field");
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
        ComPtr<IPropertyStore> rich;
        PSCreateMemoryPropertyStore(IID_PPV_ARGS(&rich));
        for (DWORD i = 0; i < 256; ++i) {
            PROPERTYKEY key = PKEY_Rating; key.pid = 10000 + i;
            value.vt = VT_UI4; value.ulVal = i;
            rich->SetValue(key, value); PropVariantClear(&value);
        }
        BYTE bytes[] = {0, 1, 127, 255};
        PROPERTYKEY blob_key = PKEY_Rating; blob_key.pid = 11000;
        value.vt = VT_BLOB; value.blob.cbSize = sizeof(bytes); value.blob.pBlobData = bytes;
        check(rich->SetValue(blob_key, value) == S_OK, "seed uncollected binary metadata");
        value = {};
        PROPERTYKEY vector_key = blob_key; ++vector_key.pid;
        value.vt = VT_VECTOR | VT_UI1; value.caub.cElems = sizeof(bytes); value.caub.pElems = bytes;
        check(rich->SetValue(vector_key, value) == S_OK, "seed uncollected byte vector");
        value = {};
        CLIPDATA clip{sizeof(bytes) + sizeof(LONG), CF_DIB, bytes};
        PROPERTYKEY clip_key = vector_key; ++clip_key.pid;
        value.vt = VT_CF; value.pclipdata = &clip;
        check(rich->SetValue(clip_key, value) == S_OK, "seed clipboard thumbnail metadata");
        value = {};
        FileDialogProperties rich_model;
        const auto rich_hr = rich_model.SetStore(rich.Get());
        check(rich_hr == S_OK, "large document property store captures without native fallback");
        if (rich_hr == S_OK) {
            check(rich_model.SetCollected(list.Get()) == S_OK && rich_model.Fields().size() == 2,
                "only requested text fields become controls");
            check(rich_model.Edit(PKEY_Title, {L"New document title"}) == S_OK, "edit text with opaque metadata retained");
            ComPtr<IPropertyStore> rich_result;
            check(rich_model.Snapshot(&rich_result) == S_OK, "snapshot rich document metadata");
            if (rich_result) {
                rich_result->GetValue(blob_key, &value);
                check(value.vt == VT_BLOB && value.blob.cbSize == sizeof(bytes) &&
                    memcmp(value.blob.pBlobData, bytes, sizeof(bytes)) == 0, "binary metadata survives edits byte for byte");
                PropVariantClear(&value);
                rich_result->GetValue(vector_key, &value);
                check(value.vt == (VT_VECTOR | VT_UI1) && value.caub.cElems == sizeof(bytes) &&
                    memcmp(value.caub.pElems, bytes, sizeof(bytes)) == 0, "byte vector survives edits byte for byte");
                PropVariantClear(&value);
                rich_result->GetValue(clip_key, &value);
                check(value.vt == VT_CF && value.pclipdata && value.pclipdata->ulClipFmt == CF_DIB &&
                    value.pclipdata->cbSize == clip.cbSize && memcmp(value.pclipdata->pClipData, bytes, sizeof(bytes)) == 0,
                    "clipboard thumbnail survives edits byte for byte");
                PropVariantClear(&value);
            }
            check(rich_model.ResetEdits() == S_OK, "cancel restores large metadata snapshot");
        }
        ComPtr<IPropertyStore> too_many;
        PSCreateMemoryPropertyStore(IID_PPV_ARGS(&too_many));
        for (DWORD i = 0; i < 1025; ++i) {
            PROPERTYKEY key = PKEY_Rating; key.pid = 20000 + i;
            value.vt = VT_UI4; value.ulVal = i;
            too_many->SetValue(key, value); PropVariantClear(&value);
        }
        check(model.SetStore(too_many.Get()) == E_NOTIMPL && model.Fields()[0].values[0] == L"Original",
            "property count bound remains transactional");
        ComPtr<IPropertyStore> aggregate;
        PSCreateMemoryPropertyStore(IID_PPV_ARGS(&aggregate));
        std::vector<BYTE> half(512 * 1024 + 1);
        value.vt = VT_BLOB; value.blob.cbSize = static_cast<ULONG>(half.size()); value.blob.pBlobData = half.data();
        aggregate->SetValue(blob_key, value); aggregate->SetValue(vector_key, value); value = {};
        check(model.SetStore(aggregate.Get()) == E_NOTIMPL && model.Fields()[0].values[0] == L"Original",
            "aggregate opaque payload bound remains transactional");
        ComPtr<IPropertyStore> bad;
        PSCreateMemoryPropertyStore(IID_PPV_ARGS(&bad));
        std::vector<BYTE> oversized(1024 * 1024 + 1);
        value.vt = VT_BLOB; value.blob.cbSize = static_cast<ULONG>(oversized.size()); value.blob.pBlobData = oversized.data();
        check(bad->SetValue(PKEY_Comment, value) == S_OK, "seed oversized metadata bound case"); value = {};
        check(FAILED(model.SetStore(bad.Get())) && model.Fields()[0].values[0] == L"Original", "unsupported data rejects transaction");
        check(FAILED(model.Edit(PKEY_Title, { L"a", L"b" })) && model.Fields()[0].values[0] == L"Original", "invalid edit preserves model");
    }
    {
        ComPtr<IPropertyDescription> floating_description;
        check(PSGetPropertyDescriptionByName(L"System.AppUserModel.Relevance", IID_PPV_ARGS(&floating_description)) == S_OK,
            "load real editable double schema");
        PROPERTYKEY floating_key{}; if (floating_description) floating_description->GetPropertyKey(&floating_key);
        ComPtr<IPropertyDescriptionList> float_list, date_list;
        PSGetPropertyDescriptionListFromString(L"prop:System.AppUserModel.Relevance", IID_PPV_ARGS(&float_list));
        PSGetPropertyDescriptionListFromString(L"prop:System.Photo.DateTaken", IID_PPV_ARGS(&date_list));
        ComPtr<IPropertyStore> source; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&source));
        PROPVARIANT initial{}; initial.vt = VT_R8; initial.dblVal = std::nextafter(1.0, 2.0);
        source->SetValue(floating_key, initial);
        FileDialogProperties floating;
        const HRESULT stored_float = floating.SetStore(source.Get());
        const HRESULT collected_float = floating.SetCollected(float_list.Get());
        std::printf("FLOAT_SCHEMA store=%08X collected=%08X list=%u\n", static_cast<unsigned>(stored_float), static_cast<unsigned>(collected_float), float_list ? 1u : 0u);
        check(stored_float == S_OK && collected_float == S_OK && floating.Fields().size() == 1,
            "collect real double without decimal display rounding");
        auto exact_value = [](FileDialogProperties& model, REFPROPERTYKEY key, const PROPVARIANT& expected) {
            ComPtr<IPropertyStore> snapshot; PROPVARIANT actual{};
            const bool result = model.Snapshot(&snapshot) == S_OK && snapshot->GetValue(key, &actual) == S_OK && property_text::Same(expected, actual);
            PropVariantClear(&actual); return result;
        };
        if (!floating.Fields().empty()) {
            check(floating.Edit(floating_key, floating.Fields()[0].values) == S_OK && exact_value(floating, floating_key, initial),
                "double displayed text roundtrips every original bit");
            check(floating.EditDraft(floating_key, {L"1e-"}) == S_FALSE && floating.HasInvalidEdits() && exact_value(floating, floating_key, initial),
                "partial floating draft retains committed bits and blocks acceptance");
            check(floating.Edit(floating_key, {L"1,25"}) == E_INVALIDARG && floating.Edit(floating_key, {L"1e9999"}) == E_INVALIDARG &&
                floating.Edit(floating_key, {L"nan"}) == E_INVALIDARG && floating.Edit(floating_key, {L"inf"}) == E_INVALIDARG,
                "locale separators overflow and nonfinite float drafts never coerce to a value");
            const std::string saved_locale = std::setlocale(LC_NUMERIC, nullptr);
            const bool locale_changed = std::setlocale(LC_NUMERIC, "de-DE") != nullptr;
            PROPVARIANT expected{}; expected.vt = VT_R8; expected.dblVal = 1.25;
            check(locale_changed && floating.EditDraft(floating_key, {L"1.25"}) == S_OK && !floating.HasInvalidEdits() && exact_value(floating, floating_key, expected),
                "decimal editing is invariant under German numeric locale");
            std::setlocale(LC_NUMERIC, saved_locale.c_str());
            check(floating.Edit(floating_key, {L""}) == S_OK && floating.Fields()[0].values[0].empty(), "empty double edit clears optional value");
            check(floating.ResetEdits() == S_OK && exact_value(floating, floating_key, initial), "double cancel restores original typed bits");
        }
        bool roundtrip = true;
        for (const double number : {0.0, -0.0, std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max(), -1.25}) {
            PROPVARIANT value{}, restored{}; value.vt = VT_R8; value.dblVal = number; std::wstring text;
            roundtrip = roundtrip && property_text::Format(value, text) == S_OK && property_text::Parse(VT_R8, text, restored) == S_OK && property_text::Same(value, restored);
        }
        for (const float number : {0.0f, -0.0f, std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(), -1.25f}) {
            PROPVARIANT value{}, restored{}; value.vt = VT_R4; value.fltVal = number; std::wstring text;
            roundtrip = roundtrip && property_text::Format(value, text) == S_OK && property_text::Parse(VT_R4, text, restored) == S_OK && property_text::Same(value, restored);
        }
        check(roundtrip, "float codec preserves signed zero subnormal and largest finite values bit for bit");
        PROPVARIANT timestamp{};
        check(property_text::Parse(VT_FILETIME, L"2024-02-29T23:59:59.1234567Z", timestamp) == S_OK, "parse exact UTC timestamp with 100ns precision");
        source->SetValue(PKEY_Photo_DateTaken, timestamp);
        FileDialogProperties dates;
        check(dates.SetStore(source.Get()) == S_OK && dates.SetCollected(date_list.Get()) == S_OK && dates.Fields().size() == 1,
            "collect real editable DateTaken schema");
        if (!dates.Fields().empty()) {
            check(dates.Fields()[0].values == std::vector<std::wstring>{L"2024-02-29T23:59:59.1234567Z"} &&
                dates.Edit(PKEY_Photo_DateTaken, dates.Fields()[0].values) == S_OK && exact_value(dates, PKEY_Photo_DateTaken, timestamp),
                "date text roundtrip preserves UTC and all seven fractional digits");
            check(dates.EditDraft(PKEY_Photo_DateTaken, {L"2024-"}) == S_FALSE && dates.HasInvalidEdits() && exact_value(dates, PKEY_Photo_DateTaken, timestamp),
                "partial date draft preserves committed timestamp");
            check(dates.Edit(PKEY_Photo_DateTaken, {L"2024-02-30T23:59:59.1234567Z"}) == E_INVALIDARG &&
                dates.Edit(PKEY_Photo_DateTaken, {L"2024-02-29T23:59:59.1234567+02:00"}) == E_INVALIDARG &&
                dates.Edit(PKEY_Photo_DateTaken, {L"29.02.2024"}) == E_INVALIDARG,
                "invalid day implicit timezone and localized ambiguous date never normalize silently");
            check(dates.EditDraft(PKEY_Photo_DateTaken, {L"2024-02-29T23:59:59.1234567Z"}) == S_OK && !dates.HasInvalidEdits(),
                "corrected exact date clears invalid draft");
            check(dates.Edit(PKEY_Photo_DateTaken, {L""}) == S_OK && dates.Fields()[0].values[0].empty(), "empty date edit clears optional timestamp");
            check(dates.ResetEdits() == S_OK && exact_value(dates, PKEY_Photo_DateTaken, timestamp), "date cancel restores exact original timestamp");
        }
    }
    {
        ComPtr<IPropertyStore> source; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&source));
        PROPVARIANT aperture{}; aperture.vt = VT_R8; aperture.dblVal = std::nextafter(1.4, 2.0);
        source->SetValue(PKEY_Photo_FNumber, aperture);
        PROPVARIANT modified{}; property_text::Parse(VT_FILETIME, L"2024-11-03T05:30:00.1234567Z", modified);
        source->SetValue(PKEY_DateModified, modified);
        ComPtr<IPropertyDescriptionList> list;
        check(PSGetPropertyDescriptionListFromString(L"prop:System.PropGroup.Camera;System.Photo.FNumber;System.DateModified", IID_PPV_ARGS(&list)) == S_OK,
            "load native group readonly float and readonly date schema");
        FileDialogProperties presentation;
        check(presentation.SetStore(source.Get()) == S_OK && presentation.SetCollected(list.Get()) == S_OK && presentation.Fields().size() == 3,
            "readonly values and group remain distinct presentation fields");
        if (presentation.Fields().size() == 3) {
            const auto& fields = presentation.Fields();
            check(fields[0].group && fields[0].read_only && fields[0].values.empty() && !fields[0].label.empty(), "group is a heading with no editable value");
            PWSTR formatted = nullptr;
            const HRESULT format_hr = PSFormatForDisplayAlloc(PKEY_Photo_FNumber, aperture, PDFF_DEFAULT, &formatted);
            check(format_hr == S_OK && fields[1].read_only && !fields[1].group && fields[1].values == std::vector<std::wstring>{formatted ? formatted : L""},
                "readonly floating display matches Windows property formatter"); CoTaskMemFree(formatted);
            PROPERTYKEY group_key = fields[0].key;
            check(presentation.Edit(group_key, {L"heading edit"}) == E_ACCESSDENIED && presentation.EditDraft(PKEY_DateModified, {L"bad"}) == E_ACCESSDENIED &&
                !presentation.HasInvalidEdits(), "group and readonly date reject writes without blocking valid dialog input");
            ComPtr<IPropertyStore> snapshot; presentation.Snapshot(&snapshot);
            PROPVARIANT actual{}; snapshot->GetValue(PKEY_Photo_FNumber, &actual);
            check(property_text::Same(actual, aperture), "human formatted readonly float retains all original store bits"); PropVariantClear(&actual);
            snapshot->GetValue(PKEY_DateModified, &actual);
            check(property_text::Same(actual, modified), "localized readonly date retains UTC and 100ns ticks through DST boundary"); PropVariantClear(&actual);
            DWORD count = 0; snapshot->GetCount(&count);
            check(count == 2 && presentation.ResetEdits() == S_OK, "group never becomes a stored property and cancel remains valid");
        }
    }
    CoUninitialize();
    return failures ? 1 : 0;
}

