# quickstart：五分钟写出一个完整增强包

这是 `pulse_ext new` 的默认模板。它只用两个头文件：

- `pulse/app.hpp`：应用层，替你写好入口、接口查询、生命周期、多语言、命令注册、对话框、后台任务和设置读写。
- `pulse/kit.hpp`：Pulse 风格组件（页面、分区卡片、设置行、提示条、空状态、键值列表），拼出来的界面和 Pulse 自带页面一样。

一共约 130 行，演示了大多数增强包都会用到的四件事：

| 功能 | 写法 |
|---|---|
| 右键菜单命令，弹出 Pulse 风格对话框 | `Command(...).Anything(1, 4096).Run(...)` + `Ask(options, &form, ok)` |
| 后台任务，带标准进度与取消 | `RunTask(title, work, finished)`，`TaskProgress::Items/Cancelled` |
| 设置页，控件改动自动保存 | `SettingsPage({title, build, {{key, default}}, {{button, key}}})` |
| 简体 / 繁体 / 英文，跟随 Pulse 语言 | `Tr("简", "繁", "English")`、`Text(...)`；菜单标题会自动切换 |

## 构建与试运行

```powershell
python tools/extensions/pulse_ext.py --json new C:/PulseAuthor/hello --id com.example.hello --name "Hello"
python tools/extensions/pulse_ext.py --json build C:/PulseAuthor/hello --config Release
python tools/extensions/pulse_ext.py --json test C:/PulseAuthor/hello --smoke-commands --strict-resources
```

`test` 用无界面宿主加载 DLL，注册命令、设置页并逐个执行命令，报告接口、任务结果和卸载是否干净。

## 最小骨架

```cpp
#include <pulse/app.hpp>
using namespace pulse::sdk;

class Hello : public App {
public:
    using App::App;
    PulseStatus OnStart() override {
        Command("hello", Tr("打个招呼", "打個招呼", "Say hello"))
            .Icon("sparkle").Anything(1, 64)
            .Run([this](const Selection& s) { Toast(std::to_string(s.Count()) + " selected", Tone::Success); });
        return PULSE_OK;
    }
};
PULSE_APP(Hello)
```

## App 速查

| 分类 | 成员 |
|---|---|
| 生命周期 | `OnStart()`、`OnStop()`、`OnLanguageChanged()`、`Name()`、`Active()` |
| 命令 | `Command(id, title)` → `.Icon/.Detail/.Group/.Files/.Folders/.Anything/.Background/.Extensions/.Shortcut/.Primary/.Checked/.Hidden/.Run`；`FindCommand(id)->SetHidden/SetChecked` |
| 反馈 | `Toast(text, tone, detail)`、`Copy(text)`、`Reveal(path)` |
| 对话框 | `Alert`、`Confirm`、`Ask(DialogOptions, &form, ok, cancel, changed)`；回调在对话框关闭后执行，可以接着打开下一个 |
| 后台 | `RunTask(title, work, finished)`、`Post(fn)`（工作线程安全） |
| 列与角标 | `AddColumn(spec, fn)`、`AddBadge(spec, fn)`、`RefreshColumns(id, path)`、`SetColumnTitle(id, title)`（`pulse.columns`，见 [file-columns](../file-columns/README.md)） |
| 设置 | `Settings().Get/GetBool/GetInt/Set/SetBool/SetInt/Remove/OnReady/OnChanged`；`SettingsPage(spec)`、`OnSettingsCommand`、`SetSettingsState` |
| 多语言 | `Tr(zh, tw, en)`、`Text(LocalText)`、`CurrentLanguage()`、`Format("{0} 项", {n})` |
| 底层服务 | `HostApi()`、`Workbench()`、`Pickers()`、`Icons()`、`Dialog()`：需要时直接使用原有 C++ 封装 |

## kit 速查

`Page`、`Section`、`Card`、`Row`、`Column`、`Columns`、`When`（按状态显隐）、`Title`、`Heading`、`Body`、`Caption`、`Live`（显示状态值）、`SwitchRow`、`ChoiceRow`（≤4 项分段，否则下拉）、`StepsRow`、`TextField`、`FolderField`、`Tile`、`PrimaryButton`、`Button`、`DangerButton`、`IconButton`、`Notice`、`Empty`、`Busy`、`ProgressBar`、`Pill`、`Facts`。

所有组件都只由公开控件组成，宿主照常校验；节点上限 128、深度 8 不变（`Page` + `Section` 占 3 层）。

## 注意

- 除 `Post` 和 `TaskProgress` 外，所有成员只能在 UI 线程调用。
- `RunTask` 的 work 运行在宿主工作线程：不要在里面调用对话框、提示或设置接口，结果交给 `finished`（已回到 UI 线程）。
- `Settings().Set` 会排队、带版本检查；若外部先改了，会自动刷新版本再试一次。值最长 1024 字节。
- 设置读写需要在 manifest 声明 `settings.own`，后台任务需要 `operations.own`。
