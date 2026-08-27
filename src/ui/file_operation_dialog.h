#pragma once

#include "FluentTokens.h"
#include "fluent_components.h"
#include "ui_compositor.h"
#include "../ops/ops_manager.h"

#include <deque>
#include <functional>
#include <string>

namespace pulse::ui {

struct FileOperationCallbacks {
    std::function<void()> cancel;
    std::function<void()> pause;
    std::function<void()> resume;
    std::function<void()> dismiss;
};

class FileOperationWindow {
public:
    FileOperationWindow();
    ~FileOperationWindow();

    bool Create(HWND owner, FileOperationCallbacks callbacks);
    void Destroy();
    void SetTheme(bool dark, D2D1_COLOR_F accent);
    void Update(const ops::OpStatus& status);
    void Show(bool activate = true);
    void Hide();
    bool IsVisible() const;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void ApplyWindowTheme();
    void Render();
    void ResizeForDetails(bool preserve_center);
    int HitTestControl(float x, float y) const;
    void RequestClose();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    FileOperationCallbacks callbacks_;
    ops::OpStatus status_;
    std::deque<double> speed_history_;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool detailed_ = false;
    bool positioned_ = false;
    float scale_ = 1.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    int hover_ = 0;
    int pressed_ = 0;
};

struct ConflictDialogResult {
    ops::ConflictChoice choice = ops::ConflictChoice::Cancel;
    bool apply_to_all = false;
};

ConflictDialogResult ShowFileConflictDialog(HWND owner,
                                            const ops::ConflictItemInfo& conflict,
                                            bool dark,
                                            D2D1_COLOR_F accent);

struct ConfirmDialogSpec {
    std::wstring title;
    std::wstring message;
    std::wstring confirm_text = L"确定";
    std::wstring cancel_text = L"取消";
    bool danger = false;
};

bool ShowConfirmDialog(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent);

} // namespace pulse::ui
