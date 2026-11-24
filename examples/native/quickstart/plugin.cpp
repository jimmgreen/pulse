// Quickstart: the smallest complete Pulse extension built on pulse/app.hpp and
// pulse/kit.hpp. It shows the four things most extensions need:
//   1. a right-click command that opens a Pulse-styled dialog,
//   2. background work with the standard task progress UI,
//   3. a settings page whose controls save themselves,
//   4. text in Simplified Chinese / Traditional Chinese / English that follows
//      the Pulse language setting, including the menu titles.
// Rename the class, edit OnStart, build with `pulse_ext build`, done.
#include <pulse/app.hpp>
#include <pulse/kit.hpp>
#include <cstdio>
#include <filesystem>
#include <string>
#include <memory>
#include <system_error>

using namespace pulse::sdk;

class Quickstart : public App {
public:
    using App::App;

    LocalText Name() const override { return Tr("快速上手示例", "快速上手範例", "Quickstart example"); }

    PulseStatus OnStart() override {
        // 1. Right-click a selection -> summary dialog.
        Command("summary", Tr("查看所选摘要", "檢視所選摘要", "Show selection summary"))
            .Icon("info")
            .Anything(1, 4096)
            .Group(Name())
            .Run([this](const Selection& selection) { ShowSummary(selection); });

        // 2. Measure the selection on a worker thread.
        Command("measure", Tr("计算总大小", "計算總大小", "Measure total size"))
            .Icon("hash")
            .Anything(1, 4096)
            .Group(Name())
            .Run([this](const Selection& selection) { Measure(selection); });

        // 3. Settings > Extensions > Quickstart. Keys load and save automatically.
        return SettingsPage({
            Name(),
            [this] { return SettingsTree(); },
            {{"greet", "true"}, {"unit", "1"}, {"export", ""}},  // key -> default
            {{"browse", "export"}},                              // Browse button -> folder picker -> key
        });
    }

private:
    ModelTree SettingsTree() {
        using namespace kit;
        return ModelTree(Page("page", {
            Section("general", Text(Tr("常规", "一般", "General")), {
                SwitchRow("greet", Text(Tr("显示欢迎提示", "顯示歡迎提示", "Show a welcome note")),
                          Text(Tr("在摘要对话框顶部显示一行说明", "在摘要對話方塊頂端顯示一行說明", "Adds a short note above the summary")),
                          "greet", "icon:sparkle"),
                ChoiceRow("unit", Text(Tr("大小单位", "大小單位", "Size unit")), {}, "unit", {"KB", "MB", "GB"}, "icon:hash"),
            }),
            Section("output", Text(Tr("导出", "匯出", "Export")), {
                FolderField("export", Text(Tr("导出文件夹", "匯出資料夾", "Export folder")), "export", "browse",
                            Text(Tr("浏览...", "瀏覽...", "Browse...")),
                            Text(Tr("留空表示所选项所在的文件夹", "留空表示所選項目所在的資料夾", "Empty: next to the selection"))),
            }, Text(Tr("示例设置，演示自动保存。", "範例設定，示範自動儲存。", "Sample settings that save as you change them."))),
        }));
    }

    void ShowSummary(const Selection& selection) {
        using namespace kit;
        std::vector<ModelNode> body;
        if (Settings().GetBool("greet", true))
            body.push_back(Notice("hello", "informational", Text(Tr("这是用 kit.hpp 搭出来的对话框。", "這是用 kit.hpp 組出來的對話方塊。",
                                                                    "This dialog is built with kit.hpp."))));
        body.push_back(Facts("facts", {
            {Text(Tr("项目", "項目", "Items")), std::to_string(selection.Count())},
            {Text(Tr("文件", "檔案", "Files")), std::to_string(selection.Files())},
            {Text(Tr("文件夹", "資料夾", "Folders")), std::to_string(selection.Folders())},
            {Text(Tr("位置", "位置", "Location")), selection.directory},
        }));
        ModelTree form(Column("summary", std::move(body), 12));

        DialogOptions options;
        options.heading = Tr("所选内容", "所選內容", "Selection");
        options.icon = PULSE_DIALOG_ICON_INFO;
        options.subject_kind = selection.Count() == 1 && selection.First().directory ? PULSE_DIALOG_SUBJECT_FOLDER : PULSE_DIALOG_SUBJECT_FILE;
        options.subject = selection.Count() == 1 ? selection.First().name : std::to_string(selection.Count()) + Text(Tr(" 项", " 項", " items"));
        options.ok = Tr("复制路径", "複製路徑", "Copy paths");
        options.cancel = Tr("关闭", "關閉", "Close");
        const auto paths = selection.Paths();
        (void)Ask(options, &form, [this, paths](const DialogValues&) {
            std::string joined;
            for (const auto& p : paths) { if (!joined.empty()) joined += "\r\n"; joined += p; }
            if (Copy(joined) == PULSE_OK) Toast(Text(Tr("已复制路径", "已複製路徑", "Paths copied")), Tone::Success);
        });
    }

    void Measure(const Selection& selection) {
        auto total = std::make_shared<uint64_t>(0);
        const auto paths = selection.Paths();
        (void)RunTask(Tr("计算大小", "計算大小", "Measuring size"),
            [paths, total](TaskProgress& progress) -> PulseTaskState {
                namespace fs = std::filesystem;
                uint64_t done = 0;
                for (const auto& p : paths) {
                    if (progress.Cancelled()) return PULSE_TASK_CANCELLED;
                    progress.Items(done, paths.size(), p);
                    std::error_code ec;
                    const fs::path root(std::u8string(p.begin(), p.end()));
                    if (fs::is_directory(root, ec)) {
                        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                            if (progress.Cancelled()) return PULSE_TASK_CANCELLED;
                            std::error_code fe;
                            if (it->is_regular_file(fe)) *total += it->file_size(fe);
                        }
                    } else {
                        const auto size = fs::file_size(root, ec);
                        if (!ec) *total += size;
                    }
                    ++done;
                }
                progress.Items(done, paths.size());
                return PULSE_TASK_COMPLETED;
            },
            [this, total](PulseTaskState state) {
                if (state != PULSE_TASK_COMPLETED) return;
                static const char* const kUnits[] = {"KB", "MB", "GB"};
                const auto unit = Settings().GetInt("unit", 1);
                const auto index = unit < 0 || unit > 2 ? 1 : static_cast<size_t>(unit);
                double value = static_cast<double>(*total);
                for (size_t i = 0; i <= index; ++i) value /= 1024.0;
                char number[64];
                std::snprintf(number, sizeof(number), "%.2f %s", value, kUnits[index]);
                Toast(Format(Text(Tr("总大小 {0}", "總大小 {0}", "Total size {0}")), {number}), Tone::Success);
            });
    }
};

PULSE_APP(Quickstart)
