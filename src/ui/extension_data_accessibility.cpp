#include "extension_data_accessibility.h"
#include <atomic>
#include <new>
#include <utility>
namespace pulse::ui {
namespace {
class DataAccessible final : public IAccessible {
  public:
    DataAccessible(HWND window, DataAccessibleCallbacks callbacks)
        : window_(window), callbacks_(std::move(callbacks)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible) {
            *result = static_cast<IAccessible*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references_;
        if (!count) {
            delete this;
        }
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        if (!count) {
            return E_POINTER;
        }
        *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*,
                                     UINT*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        DataAccessibleSnapshot snapshot;
        const auto hr = Read(snapshot);
        if (FAILED(hr)) {
            return hr;
        }
        const HWND parent = GetParent(window_);
        if (!parent) {
            return S_FALSE;
        }
        return AccessibleObjectFromWindow(parent, OBJID_WINDOW, IID_IDispatch,
                                          reinterpret_cast<void**>(result));
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
        if (!count) {
            return E_POINTER;
        }
        *count = 0;
        DataAccessibleSnapshot s;
        const auto hr = Read(s);
        if (FAILED(hr)) {
            return hr;
        }
        *count = static_cast<long>(s.rows.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        return FAILED(hr) ? hr : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        if (FAILED(hr)) {
            return hr;
        }
        return String(child.lVal ? s.rows[child.lVal - 1].name : s.name, result);
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* result) override {
        return Empty(child, result);
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* result) override {
        return Empty(child, result);
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* result) override {
        if (!result) {
            return E_POINTER;
        }
        VariantInit(result);
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        if (FAILED(hr)) {
            return hr;
        }
        result->vt = VT_I4;
        result->lVal = s.type == extensions::UiNodeType::Tree
                           ? (child.lVal ? ROLE_SYSTEM_OUTLINEITEM : ROLE_SYSTEM_OUTLINE)
                       : s.type == extensions::UiNodeType::DataTable
                           ? (child.lVal ? ROLE_SYSTEM_ROW : ROLE_SYSTEM_TABLE)
                           : (child.lVal ? ROLE_SYSTEM_LISTITEM : ROLE_SYSTEM_LIST);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* result) override {
        if (!result) {
            return E_POINTER;
        }
        VariantInit(result);
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        if (FAILED(hr)) {
            return hr;
        }
        result->vt = VT_I4;
        result->lVal = STATE_SYSTEM_FOCUSABLE;
        if (!s.enabled) {
            result->lVal |= STATE_SYSTEM_UNAVAILABLE;
        }
        if (!child.lVal) {
            if (s.focused) {
                result->lVal |= STATE_SYSTEM_FOCUSED;
            }
            return S_OK;
        }
        const auto& row = s.rows[child.lVal - 1];
        result->lVal |= STATE_SYSTEM_SELECTABLE;
        if (!row.enabled) {
            result->lVal |= STATE_SYSTEM_UNAVAILABLE;
        }
        if (row.selected) {
            result->lVal |= STATE_SYSTEM_SELECTED;
        }
        if (row.focused && s.focused) {
            result->lVal |= STATE_SYSTEM_FOCUSED;
        }
        if (row.offscreen) {
            result->lVal |= STATE_SYSTEM_OFFSCREEN;
        }
        if (s.type == extensions::UiNodeType::Tree && row.has_children) {
            result->lVal |= row.expanded ? STATE_SYSTEM_EXPANDED : STATE_SYSTEM_COLLAPSED;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT child, BSTR* result) override {
        return Empty(child, result);
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT child, long* topic) override {
        if (!file || !topic) {
            return E_POINTER;
        }
        *file = nullptr;
        *topic = 0;
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        return FAILED(hr) ? hr : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child, BSTR* result) override {
        return Empty(child, result);
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* result) override {
        if (!result) {
            return E_POINTER;
        }
        VariantInit(result);
        DataAccessibleSnapshot s;
        const auto hr = Read(s);
        if (FAILED(hr)) {
            return hr;
        }
        if (!s.focused) {
            return S_FALSE;
        }
        result->vt = VT_I4;
        result->lVal = CHILDID_SELF;
        for (size_t i = 0; i < s.rows.size(); ++i) {
            if (s.rows[i].focused) {
                result->lVal = static_cast<LONG>(i + 1);
                break;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* result) override {
        if (!result) {
            return E_POINTER;
        }
        VariantInit(result);
        DataAccessibleSnapshot s;
        const auto hr = Read(s);
        if (FAILED(hr)) {
            return hr;
        }
        for (size_t i = 0; i < s.rows.size(); ++i) {
            if (s.rows[i].selected) {
                result->vt = VT_I4;
                result->lVal = static_cast<LONG>(i + 1);
                return S_OK;
            }
        }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        if (FAILED(hr)) {
            return hr;
        }
        if (!child.lVal || !s.enabled || !s.rows[child.lVal - 1].enabled || !callbacks_.activate) {
            return S_FALSE;
        }
        return s.default_action_name.empty() ? S_FALSE : String(s.default_action_name, result);
    }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override {
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        if (FAILED(hr)) {
            return hr;
        }
        constexpr long supported = SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION;
        if (!child.lVal || !flags || (flags & ~supported)) {
            return E_INVALIDARG;
        }
        if (!s.enabled || !s.rows[child.lVal - 1].enabled || !callbacks_.select) {
            return E_ACCESSDENIED;
        }
        const bool selected = callbacks_.select(s.rows[child.lVal - 1].id);
        DataAccessibleSnapshot after;
        const auto connected = Read(after);
        if (FAILED(connected)) {
            return connected;
        }
        if (!selected) {
            return E_FAIL;
        }
        if (flags & SELFLAG_TAKEFOCUS) {
            SetFocus(window_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height,
                                          VARIANT child) override {
        if (!left || !top || !width || !height) {
            return E_POINTER;
        }
        *left = *top = *width = *height = 0;
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        if (FAILED(hr)) {
            return hr;
        }
        const auto r = child.lVal ? s.rows[child.lVal - 1].screen_bounds : s.screen_bounds;
        *left = r.left;
        *top = r.top;
        *width = r.right - r.left;
        *height = r.bottom - r.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT child, VARIANT* result) override {
        if (!result) {
            return E_POINTER;
        }
        VariantInit(result);
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        if (FAILED(hr)) {
            return hr;
        }
        long target = 0;
        if (!child.lVal && direction == NAVDIR_FIRSTCHILD) {
            target = 1;
        } else if (!child.lVal && direction == NAVDIR_LASTCHILD) {
            target = static_cast<long>(s.rows.size());
        } else if (child.lVal && (direction == NAVDIR_NEXT || direction == NAVDIR_DOWN)) {
            target = child.lVal + 1;
        } else if (child.lVal && (direction == NAVDIR_PREVIOUS || direction == NAVDIR_UP)) {
            target = child.lVal - 1;
        } else {
            return S_FALSE;
        }
        if (target < 1 || static_cast<size_t>(target) > s.rows.size()) {
            return S_FALSE;
        }
        result->vt = VT_I4;
        result->lVal = target;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* result) override {
        if (!result) {
            return E_POINTER;
        }
        VariantInit(result);
        DataAccessibleSnapshot s;
        const auto hr = Read(s);
        if (FAILED(hr)) {
            return hr;
        }
        const POINT point{x, y};
        if (!PtInRect(&s.screen_bounds, point)) {
            return S_FALSE;
        }
        result->vt = VT_I4;
        result->lVal = CHILDID_SELF;
        for (size_t i = 0; i < s.rows.size(); ++i) {
            if (!s.rows[i].offscreen && PtInRect(&s.rows[i].screen_bounds, point)) {
                result->lVal = static_cast<long>(i + 1);
                break;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        if (FAILED(hr)) {
            return hr;
        }
        if (!child.lVal) {
            return E_INVALIDARG;
        }
        if (!s.enabled || !s.rows[child.lVal - 1].enabled || !callbacks_.activate) {
            return E_ACCESSDENIED;
        }
        const bool activated = callbacks_.activate(s.rows[child.lVal - 1].id);
        DataAccessibleSnapshot after;
        const auto connected = Read(after);
        return FAILED(connected) ? connected : activated ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT child, BSTR) override {
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        return FAILED(hr) ? hr : E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT child, BSTR) override {
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        return FAILED(hr) ? hr : E_ACCESSDENIED;
    }

  private:
    std::atomic<ULONG> references_{1};
    HWND window_;
    DataAccessibleCallbacks callbacks_;
    bool disconnected_ = false;
    HRESULT Read(DataAccessibleSnapshot& result) {
        if (disconnected_ || (window_ && !IsWindow(window_)) || !callbacks_.snapshot) {
            disconnected_ = true;
            return CO_E_OBJNOTCONNECTED;
        }
        auto snapshot = callbacks_.snapshot();
        if (!snapshot) {
            disconnected_ = true;
            return CO_E_OBJNOTCONNECTED;
        }
        result = std::move(*snapshot);
        return S_OK;
    }
    HRESULT Resolve(VARIANT child, DataAccessibleSnapshot& result) {
        const auto hr = Read(result);
        if (FAILED(hr)) {
            return hr;
        }
        return child.vt == VT_I4 && child.lVal >= 0 && static_cast<size_t>(child.lVal) <= result.rows.size()
                   ? S_OK
                   : E_INVALIDARG;
    }
    static HRESULT String(const std::wstring& text, BSTR* result) {
        *result = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        return *result ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT Empty(VARIANT child, BSTR* result) {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        DataAccessibleSnapshot s;
        const auto hr = Resolve(child, s);
        return FAILED(hr) ? hr : S_FALSE;
    }
};
} // namespace
HRESULT CreateExtensionDataAccessible(HWND window, DataAccessibleCallbacks callbacks, IAccessible** result) {
    if (!result) {
        return E_POINTER;
    }
    *result = nullptr;
    if (!callbacks.snapshot) {
        return E_INVALIDARG;
    }
    *result = new (std::nothrow) DataAccessible(window, std::move(callbacks));
    return *result ? S_OK : E_OUTOFMEMORY;
}
} // namespace pulse::ui
