#pragma once
#include <windows.h>
#include <commdlg.h>
#include <functional>
#include <string>
#include <vector>
#include <span>

namespace pulse::shell_bridge {
struct MirroredLegacyCombo {
    HWND hwnd = nullptr;
    int control_id = 0;
    std::wstring label;
    std::vector<std::wstring> items;
    int selected = -1;
    bool enabled = true;
};

// Caller-STA child dialog. No callback or template pointer crosses a process.
class LegacyHookHost {
public:
    explicit LegacyHookHost(OPENFILENAMEW* input) noexcept;
    explicit LegacyHookHost(OPENFILENAMEA* input) noexcept;
    ~LegacyHookHost();
    LegacyHookHost(const LegacyHookHost&) = delete;
    LegacyHookHost& operator=(const LegacyHookHost&) = delete;
    HRESULT Prepare(bool allow_empty_template_fallback = false);
    static bool AppendTemplateSupported(std::span<const BYTE> bytes) noexcept;
    static bool IsEmptyTemplate(std::span<const BYTE> bytes) noexcept;
    using ParentMessage = std::function<LRESULT(UINT, WPARAM, LPARAM)>;
    HRESULT Create(HWND parent, ParentMessage message = {});
    // Creates an off-screen visible popup parent + child dialog on the caller STA thread
    // so hooks that create and populate ComboBoxes only when visible (e.g. Photoshop Save for Web)
    // can be mirrored into IFileDialogCustomize without attaching a Win32 child to a DComp window.
    HRESULT CreateMirrored(ParentMessage message, const std::vector<std::wstring>& filter_labels, UINT filter_index_1based);
    std::vector<MirroredLegacyCombo> ReadMirroredCombos() const;
    bool SelectMirroredCombo(size_t index, int item_index);
    void SyncFilterIndex(UINT filter_index_1based);
    bool Notify(UINT code);
    bool FileOk();
    HWND Window() const noexcept { return child_; }
    bool IsAnsi() const noexcept { return ansi_; }
    bool IsMirrored() const noexcept { return mirrored_; }
private:
    static INT_PTR CALLBACK DialogProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ParentProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    ParentMessage parent_message_;
    std::vector<BYTE> template_;
    bool prepared_ = false;
    bool mirrored_ = false;
    void* input_ = nullptr;
    LPOFNHOOKPROC hook_ = nullptr;
    HWND child_ = nullptr;
    HWND parent_ = nullptr;
    HWND filter_combo_ = nullptr;
    bool owns_parent_ = false;
    bool attached_ = false;
    DWORD thread_ = 0;
    bool ansi_ = false;
    UINT_PTR notification_return_ = 0;
    bool notifying_ = false;
};
}
