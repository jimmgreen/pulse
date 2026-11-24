#include "../ui/batch_rename_dialog.h"
#include "../ui/typography.h"
#include "../common/localization.h"
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Upload {
    unsigned count = 0;
    bool ink = false;
    int width = 0, height = 0, ink_top = 0, ink_bottom = 0;
    std::wstring text;
};
std::map<HWND, Upload> uploads;
int failures = 0;
int shown = 0;
int initial_missing = 0;
float test_scale = 1.75f;
namespace rn = pulse::app::rename;
using PresetControl = pulse::ui::BatchRenameControlForTest;
using PresetPoint = pulse::ui::BatchRenameControlPointForTest;
enum class PresetCase { SelectAndEdit, Explicit, Review, Reopen, Builtin, Last, Delete, Missing, Save, ReopenSaved };
std::optional<PresetCase> preset_case;
std::optional<bool> ai_visibility;
std::optional<POINT> compact_add, compact_preview;
struct ExecutionCase {
    HWND dialog = nullptr;
    ULONGLONG clicked_at = 0;
    ULONGLONG submitted_at = 0;
    ULONGLONG finished_at = 0;
    unsigned poll_count = 0;
    bool saw_partial = false;
    bool timed_out = false;
};
ExecutionCase* execution_case = nullptr;
constexpr UINT_PTR kExecutionWatchdog = 0xCA11;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
BOOL CALLBACK Collect(HWND hwnd, LPARAM param) {
    wchar_t name[32]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (_wcsicmp(name, L"EDIT") == 0)
        reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
    return TRUE;
}
std::vector<HWND> Editors(HWND dialog) {
    std::vector<HWND> out;
    EnumChildWindows(dialog, Collect, reinterpret_cast<LPARAM>(&out));
    return out;
}

HWND FindVisibleEditor(HWND dialog, std::wstring_view expected) {
    for (HWND edit : Editors(dialog)) {
        wchar_t text[256]{};
        GetWindowTextW(edit, text, ARRAYSIZE(text));
        if (IsWindowVisible(edit) && expected == text) return edit;
    }
    return nullptr;
}

void TestPresetReopen(HWND dialog, const PresetPoint& point) {
    const auto click = [&](PresetControl control, int index = 0) {
        const auto pt = point(control, index);
        if (!pt) {
            std::printf("[DIAG] no preset hit for control=%d index=%d\n", static_cast<int>(control), index);
            return false;
        }
        const LPARAM pos = MAKELPARAM(pt->x, pt->y);
        SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, pos);
        SendMessageW(dialog, WM_LBUTTONUP, 0, pos);
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return true;
    };
    const auto choose = [&](PresetControl control, int index = 0) {
        if (!click(PresetControl::Picker) || !click(control, index)) return false;
        // The real edit hosts stay hidden until the staggered cards settle.
        Sleep(430);
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return true;
    };
    const auto field = [&](std::wstring_view text) { return FindVisibleEditor(dialog, text); };
    switch (*preset_case) {
    case PresetCase::SelectAndEdit:
        Check(field(L"_") && !field(L"executed-rule"), "legacy settings open the default, not the executed-history rule");
        Check(choose(PresetControl::User, 1), "select second saved user preset via preset menu Win32 clicks");
        if (HWND editor = field(L"recurring-project")) {
            Check(field(L"archived"), "selected preset paints both saved rule values");
            SendMessageW(editor, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"unsaved-draft"));
            Check(field(L"unsaved-draft"), "type an unsaved rule edit before closing without applying");
        } else Check(false, "selected saved preset paints its rule editor");
        break;
    case PresetCase::Explicit:
        Check(field(L"provided-rule") && !field(L"recurring-project"), "explicit caller preset overrides remembered user selection");
        break;
    case PresetCase::Review:
        Check(Editors(dialog).empty() && !point(PresetControl::Picker, 0), "proposed rename review opens without editable rules or preset picker");
        break;
    case PresetCase::Reopen:
        Check(field(L"recurring-project") && field(L"archived") && !field(L"unsaved-draft") && !field(L"executed-rule"),
            "plain reopen restores selected saved rule; not draft or executed history");
        Check(choose(PresetControl::Builtin, 2) && field(L"-"), "explicit nondefault built-in selection supersedes user choice");
        break;
    case PresetCase::Builtin:
        Check(field(L"-") && !field(L"recurring-project"), "plain reopen restores explicitly selected built-in");
        Check(choose(PresetControl::Last) && field(L"executed-rule"), "explicit Last Used remains independently selectable");
        break;
    case PresetCase::Last:
        Check(field(L"executed-rule") && !field(L"recurring-project"), "plain reopen restores explicitly selected Last Used");
        break;
    case PresetCase::Delete:
        Check(choose(PresetControl::User, 1) && field(L"recurring-project"), "reselect saved user preset before deleting");
        Check(choose(PresetControl::DeleteUser, 1) && !point(PresetControl::User, 1), "delete selected preset from the real menu");
        break;
    case PresetCase::Missing:
        Check(field(L"_") && !field(L"recurring-project"), "deleted remembered preset falls back to default");
        break;
    case PresetCase::Save: {
        if (HWND editor = field(L"_"))
            SendMessageW(editor, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"new-saved-rule"));
        Check(field(L"new-saved-rule"), "modify a rule to save under a new name");
        const auto before = Editors(dialog);
        Check(click(PresetControl::Save), "open real Save as Preset editor");
        HWND name = nullptr;
        for (HWND edit : Editors(dialog))
            if (std::find(before.begin(), before.end(), edit) == before.end() && IsWindowVisible(edit)) name = edit;
        Check(name != nullptr, "Save as Preset shows a new name editor");
        if (name) {
            SendMessageW(name, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Saved during test"));
            Check(click(PresetControl::ConfirmSave), "commit new user preset via Win32 click");
        }
        break;
    }
    case PresetCase::ReopenSaved:
        Check(field(L"new-saved-rule"), "plain reopen restores the newly saved active preset");
        break;
    }
}

rn::Preset MakePreset(const wchar_t* name, const wchar_t* find) {
    rn::Step replace;
    replace.type = rn::StepType::Replace;
    replace.find = find;
    replace.with = L"archived";
    return {name, {replace}, {}};
}

void RunPresetReopen() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    rn::PresetStore legacy;
    legacy.presets = {MakePreset(L"First saved", L"first-saved-rule"),
                      MakePreset(L"Recurring project", L"recurring-project")};
    legacy.has_last = true;
    legacy.last = MakePreset(L"Last used", L"executed-rule");
    const bool initialized = rn::SavePresets(legacy);
    Check(initialized, "write isolated legacy store with two saved presets and unrelated executed history");
    if (!initialized) return;
    for (const auto scenario : {PresetCase::SelectAndEdit, PresetCase::Explicit, PresetCase::Review, PresetCase::Reopen,
                                PresetCase::Builtin, PresetCase::Last, PresetCase::Delete, PresetCase::Missing,
                                PresetCase::Save, PresetCase::ReopenSaved}) {
        preset_case = scenario;
        pulse::ui::BatchRenameDialogOptions options;
        if (scenario == PresetCase::Explicit) options.preset = MakePreset(L"Caller supplied", L"provided-rule");
        if (scenario == PresetCase::Review) {
            options.proposed_names.emplace();
            options.proposed_plan.emplace();
        }
        if (scenario == PresetCase::Builtin) pulse::l10n::SetLanguage(L"zh-CN");
        const auto result = pulse::ui::ShowBatchRenameDialog(nullptr, {}, false, D2D1::ColorF(0, 0.5f, 1), options);
        Check(!result.accepted, "close real window without any rename operation");
    }
    Check(shown == 10, "all ten real window opens reached the first rendered frame");
    const auto reloaded = rn::LoadPresets();
    Check(reloaded.presets.size() == 2 && reloaded.presets.front().name == L"First saved" &&
          reloaded.presets.back().name == L"Saved during test" && reloaded.has_last &&
          reloaded.last.steps.front().find == L"executed-rule",
        "real file store retains unrelated saved preset and executed history; deleted/saved rows updated");
}

LRESULT CALLBACK WatchExecution(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                UINT_PTR id, DWORD_PTR context) {
    if (message == WM_TIMER && wparam == kExecutionWatchdog) {
        auto& state = *reinterpret_cast<ExecutionCase*>(context);
        state.timed_out = true;
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, WatchExecution, id);
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void ClickApply(HWND dialog) {
    auto& state = *execution_case;
    state.dialog = dialog;
    const bool guarded = SetWindowSubclass(dialog, WatchExecution, 1, reinterpret_cast<DWORD_PTR>(&state)) &&
        SetTimer(dialog, kExecutionWatchdog, 6500, nullptr) != 0;
    Check(guarded, "real execution window has a bounded watchdog");
    if (!guarded) {
        PostMessageW(dialog, WM_CLOSE, 0, 0);
        return;
    }
    RECT client{};
    GetClientRect(dialog, &client);
    // The right-aligned Apply button ends 16 DIPs before the client edge.
    const LPARAM point = MAKELPARAM(client.right - static_cast<int>(45 * test_scale),
                                   client.bottom - static_cast<int>(30 * test_scale));
    state.clicked_at = GetTickCount64();
    SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(dialog, WM_LBUTTONUP, 0, point);
    if (!state.submitted_at) PostMessageW(dialog, WM_CLOSE, 0, 0);
}

void TestAiVisibility(HWND dialog, const PresetPoint& point) {
    const bool show = *ai_visibility;
    const bool has_edit = GetDlgItem(dialog, 100) != nullptr;
    const bool has_example = point(PresetControl::AiExample, 0).has_value();
    const bool has_content = point(PresetControl::ContentMode, 0).has_value();
    const auto add = point(PresetControl::AddStep, 0);
    const auto preview = point(PresetControl::PreviewFilter, 0);
    std::printf("[GEOMETRY] enabled=%d scale=%.2f edit=%d example=%d content=%d add_y=%ld preview_y=%ld\n",
                show, test_scale, has_edit, has_example, has_content, add ? add->y : -1L, preview ? preview->y : -1L);
    Check(has_edit == show, "AI edit is only created with the assistant enabled");
    Check(has_example == show && has_content == show, "AI examples and AI content naming expose no hit targets when disabled");
    Check(add && preview && point(PresetControl::Picker, 0), "ordinary rules, preview and preset controls remain usable");
    if (!show) {
        compact_add = add;
        compact_preview = preview;
    } else {
        Check(add && preview && compact_add && compact_preview &&
              add->y - compact_add->y >= 85 * test_scale && preview->y - compact_preview->y >= 85 * test_scale,
              "ordinary rules and preview both reclaim the space under the title");
    }
    PostMessageW(dialog, WM_CLOSE, 0, 0);
}

void RunAiVisibility() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int requests = 0;
    for (const float scale : {1.0f, 1.75f}) {
        test_scale = scale;
        for (const bool show : {false, true}) {
            pulse::ui::BatchRenameDialogOptions options;
            options.ai_enabled = show;
            if (!show) {
                options.ai_model = L"fixture-model";
                options.ai_start = [&](const std::string&, std::wstring&) -> std::shared_ptr<pulse::ui::BatchRenameAiTask> {
                    ++requests;
                    return {};
                };
            }
            ai_visibility = show;
            const auto result = pulse::ui::ShowBatchRenameDialog(nullptr, {}, scale > 1.0f,
                D2D1::ColorF(0, 0.5f, 1), options);
            Check(!result.accepted, "the test closes the ordinary batch rename window");
        }
    }
    ai_visibility.reset();
    Check(requests == 0, "no model requests were started");
}

void RunExecutionTiming() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse batch execution test owner", WS_OVERLAPPED,
                                     0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "created an isolated owner window");
    if (!owner) return;
    constexpr uint64_t task = 42;
    constexpr ULONGLONG max_close_ms = 250;
    struct Scenario { size_t rows; ULONGLONG delay_ms; size_t finished_items; };
    for (const auto [rows, delay, finished_items] : {Scenario{12, 0, 12}, Scenario{128, 0, 128},
                                                   Scenario{128, 1850, 128}, Scenario{32, 0, 7}}) {
        ExecutionCase state;
        pulse::ui::BatchRenameDialogOptions options;
        options.proposed_names.emplace();
        std::vector<std::wstring> paths;
        for (size_t i = 0; i < rows; ++i) {
            const std::wstring old_name = L"old-" + std::to_wstring(i) + L".txt";
            paths.push_back(L"fixture\\" + old_name);
            options.proposed_sources.push_back({paths.back(), old_name, L"fixture"});
            options.proposed_names->push_back(L"new-" + std::to_wstring(i) + L".txt");
        }
        options.proposed_plan = rn::RunNames(options.proposed_sources, *options.proposed_names, {}, {});
        options.execute = [&](const std::vector<pulse::app::BatchRenameItem>& items) -> uint64_t {
            Check(items.size() == rows && !IsWindowEnabled(owner), "Apply click submits every row once with the owner disabled");
            Check(!state.submitted_at, "the execute callback was not called twice");
            state.submitted_at = GetTickCount64();
            return task;
        };
        options.poll = [&](uint64_t id) {
            ++state.poll_count;
            if (state.poll_count == 1) Check(id == task, "progress polls the actual submitted task ID");
            const auto now = GetTickCount64();
            const auto elapsed = now - state.submitted_at;
            if (elapsed >= delay) {
                if (!state.finished_at) state.finished_at = now;
                return pulse::ui::BatchRenameProgress{true, finished_items};
            }
            if (elapsed >= 150) {
                state.saw_partial = IsWindowVisible(state.dialog) && !IsWindowEnabled(owner);
                return pulse::ui::BatchRenameProgress{false, rows / 2};
            }
            return pulse::ui::BatchRenameProgress{};
        };
        execution_case = &state;
        const auto result = pulse::ui::ShowBatchRenameDialog(owner, paths, false, D2D1::ColorF(0, 0.5f, 1), options);
        const auto returned_at = GetTickCount64();
        execution_case = nullptr;
        const auto total_ms = state.clicked_at ? returned_at - state.clicked_at : 0;
        const auto finish_ms = state.finished_at ? returned_at - state.finished_at : 0;
        std::printf("[TIMING] rows=%zu backend_delay_ms=%llu finished_items=%zu click_to_return_ms=%llu finish_to_return_ms=%llu polls=%u\n",
                    rows, delay, finished_items, total_ms, finish_ms, state.poll_count);
        Check(result.accepted && result.executed && result.task == task && result.items.size() == rows,
              "execution returns the accepted task and original rename items");
        Check(!state.timed_out && state.finished_at && !IsWindow(state.dialog) && IsWindowEnabled(owner),
              "completion destroys the window and restores its owner");
        Check(state.finished_at && finish_ms <= max_close_ms, "finished task closes without a row animation or minimum hold");
        if (delay) Check(state.saw_partial && total_ms >= delay, "long-running partial progress stays modal until completion");
        else Check(state.clicked_at && total_ms <= max_close_ms, "a fast task is not held open by its row count");
    }
    DestroyWindow(owner);
}
}
namespace pulse::ui {
float BatchRenameScaleForTest() { return test_scale; }
bool BatchRenameUsePresetStoreForTest() { return preset_case.has_value(); }
void ObserveEditUploadForTest(HWND hwnd, const void* bits, int width, int height) {
    auto& upload = uploads[hwnd];
    ++upload.count;
    wchar_t text[128]{};
    GetWindowTextW(hwnd, text, ARRAYSIZE(text));
    upload.text = text;
    const auto* pixels = static_cast<const std::uint32_t*>(bits);
    upload.width = width;
    upload.height = height;
    upload.ink_top = height;
    upload.ink_bottom = -1;
    int changed = 0;
    for (int i = 1; i < width * height; ++i) {
        if ((pixels[i] & 0xffffff) != (pixels[0] & 0xffffff)) {
            ++changed;
            const int row = i / width;
            if (row < upload.ink_top) upload.ink_top = row;
            if (row > upload.ink_bottom) upload.ink_bottom = row;
        }
    }
    upload.ink = changed > 20;
}
void BatchRenameShownForTest(HWND dialog, const BatchRenameControlPointForTest& preset_point) {
    ++shown;
    if (execution_case) {
        ClickApply(dialog);
        return;
    }
    if (ai_visibility.has_value()) {
        TestAiVisibility(dialog, preset_point);
        return;
    }
    if (preset_case) {
        TestPresetReopen(dialog, preset_point);
        PostMessageW(dialog, WM_CLOSE, 0, 0);
        return;
    }
    std::vector<HWND> edits;
    EnumChildWindows(dialog, Collect, reinterpret_cast<LPARAM>(&edits));
    Check(edits.size() >= 3, "real batch dialog creates the AI field and the default step fields");
    // Identify by geometry: the hidden numeric field retains the initial '1'.
    HWND number = nullptr, pattern = nullptr;
    std::vector<HWND> visible;
    for (HWND edit : edits) {
        DWORD flags = 0;
        BYTE alpha = 0;
        Check(GetLayeredWindowAttributes(edit, nullptr, &alpha, &flags) &&
            (flags & LWA_ALPHA) && alpha == 255,
            "batch field uses the same redirected surface as address/search editors");
        if (!IsWindowVisible(edit)) number = edit;
        else visible.push_back(edit);
    }
    Check(visible.size() == 2 && number, "replace step fields visible; AI and custom fields wait hidden");
    LONG bottom = LONG_MIN;
    for (HWND edit : visible) {
        RECT rect{}; GetWindowRect(edit, &rect);
        if (rect.top > bottom) { bottom = rect.top; pattern = edit; }
        if (uploads[edit].count == 0) ++initial_missing;
        std::printf("[DIAG initial-empty] uploads=%u (separate from entered-text result)\n", uploads[edit].count);
        const unsigned before = uploads[edit].count;
        SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Typed 中文"));
        Check(uploads[edit].count > before && uploads[edit].ink && uploads[edit].text == L"Typed 中文",
            "real host text change uploads visible glyph pixels");
        const auto& upload = uploads[edit];
        RECT client{}; GetClientRect(edit, &client);
        std::printf("[DIAG glyphs] surface=%dx%d client=%ldx%ld ink_rows=%d..%d scale=%.2f\n",
            upload.width, upload.height, client.right, client.bottom,
            upload.ink_top, upload.ink_bottom, test_scale);
        Check(upload.width == client.right && upload.height == client.bottom &&
            upload.ink_top > 0 && upload.ink_bottom < upload.height - 1,
            "uploaded text fits actual editor height without touching top or bottom edge");
        const int length = GetWindowTextLengthW(edit);
        SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
        DWORD selection_start = 0, selection_end = 0;
        SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start),
            reinterpret_cast<LPARAM>(&selection_end));
        Check(selection_start == static_cast<DWORD>(length) && selection_end == static_cast<DWORD>(length),
            "WM_CHAR fixture places collapsed selection at actual text end");
        const unsigned before_char = uploads[edit].count;
        SendMessageW(edit, WM_CHAR, L'Z', 1);
        const bool appended = uploads[edit].count > before_char && uploads[edit].ink &&
            uploads[edit].text == L"Typed 中文Z";
        if (!appended) {
            wchar_t native_text[128]{};
            GetWindowTextW(edit, native_text, ARRAYSIZE(native_text));
            std::printf("[DIAG WM_CHAR] selection=%lu..%lu length=%d uploads=%u->%u ink=%d native='%ls' uploaded='%ls'\n",
                selection_start, selection_end, length, before_char, uploads[edit].count,
                uploads[edit].ink ? 1 : 0, native_text, uploads[edit].text.c_str());
        }
        Check(appended,
            "real host WM_CHAR appends text and uploads glyph pixels");
    }
    // The AI sentence field shows a drawn placeholder while empty; typing
    // into it must bring the real editor up and upload its glyphs.
    if (HWND ai = GetDlgItem(dialog, 100)) {
        Check(!IsWindowVisible(ai), "empty AI field shows the drawn placeholder instead of the editor");
        SendMessageW(ai, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"去掉 (1)"));
        UpdateWindow(dialog);
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        Check(IsWindowVisible(ai) && uploads[ai].count > 0 && uploads[ai].ink,
            "typed AI sentence brings the editor up and uploads glyph pixels");
    } else {
        Check(false, "AI sentence field exists");
    }
    (void)pattern;
    (void)number;
    PostMessageW(dialog, WM_CLOSE, 0, 0);
}
}
int wmain(int argc, wchar_t** argv) {
    // Never switch the interactive desktop or write user files.
    const bool preset_reopen = argc == 3 && std::wstring_view(argv[1]) == L"--preset-reopen";
    const bool execution_timing = argc == 2 && std::wstring_view(argv[1]) == L"--execution-timing";
    const bool ai_visibility_test = argc == 2 && std::wstring_view(argv[1]) == L"--ai-visibility";
    if (argc != 1 && !preset_reopen && !execution_timing && !ai_visibility_test) return 2;
    if (execution_timing || ai_visibility_test) std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (preset_reopen) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        const std::filesystem::path fixture(argv[2]);
        std::error_code error;
        if (!fixture.is_absolute() || !std::filesystem::is_directory(fixture, error) ||
            !std::filesystem::is_empty(fixture, error) ||
            !SetEnvironmentVariableW(L"LOCALAPPDATA", fixture.c_str())) {
            std::printf("[FAIL] --preset-reopen requires an empty isolated absolute fixture directory\n");
            return 2;
        }
    }
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    const std::wstring name = L"PulseBatchPresent_" + std::to_wstring(GetCurrentProcessId());
    HDESK isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!isolated || !SetThreadDesktop(isolated)) {
        std::printf("[FAIL] isolated desktop unavailable: %lu\n", GetLastError());
        if (isolated) CloseDesktop(isolated);
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    if (preset_reopen) RunPresetReopen();
    else if (execution_timing) RunExecutionTiming();
    else if (ai_visibility_test) RunAiVisibility();
    else {
        for (const auto mode : {pulse::ui::typography::TextRenderMode::Auto,
                pulse::ui::typography::TextRenderMode::Sharp, pulse::ui::typography::TextRenderMode::Smooth}) {
            pulse::ui::typography::SetTextRenderMode(mode);
            for (const bool dark : {false, true}) {
                std::printf("[CASE] simulated_scale=1.75 dark=%d mode=%d; Windows DPI is unchanged\n",
                    dark ? 1 : 0, static_cast<int>(mode));
                uploads.clear();
                const auto result = pulse::ui::ShowBatchRenameDialog(nullptr, {}, dark, D2D1::ColorF(0, 0.5f, 1));
                Check(!result.accepted, "test closes real dialog without applying rename");
            }
        }
        Check(shown == 6, "all six mode/theme Show paths reached presentation checkpoint");
        std::printf("[DIAG initial-empty] %d fields had no pre-input upload; not counted as entered-text failure\n",
            initial_missing);
    }
    CoUninitialize();
    SetThreadDesktop(original);
    CloseDesktop(isolated);
    return failures ? 1 : 0;
}
