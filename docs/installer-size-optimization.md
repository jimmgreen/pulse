# 安装包体积优化建议

测量对象：`dist\PulseSetup-1.0.58.exe`（12.7 MB），2026-11-07 Release 构建（`PULSE_WITH_TEST_HOOKS=OFF`、`PULSE_WITH_SELFTEST=OFF`、`PULSE_WITH_IPO=OFF`）。

## 结论先说

体积主要不是“防御性代码”造成的，而是三件事：

1. **同一套 UI 代码被编进了两个大文件**：`pulse.exe`（20 MB）和 `PulsePickerUiProof.dll`（8 MB）各自静态包含界面、列表、预览等代码。
2. **测试和探针代码进了正式版**：即使关闭测试钩子，`pulse.exe` 仍链接了约 40 个 `*_test.cpp` / `*_probe.cpp`。
3. **整体按速度优化（/O2）**：只有少数设置页按体积编译，其余全部 /O2，代码段偏大。

另外 `pdfium.dll`（7.4 MB）单独就占压缩后体积的两成多。

## 现状数据

安装包里实际只有一个 47 个文件、解压后 50.1 MB 的载荷（根目录启动器 + `DialogBridge/<hash>/` 下的完整程序）。下表按“单独 xz 压缩后大小”排序，近似反映每个文件对安装包的贡献（安装包整体是 solid 压缩，总数会比单独相加的 16.6 MB 小）。

| 文件 | 原始 | 压缩后 | 说明 |
|---|---:|---:|---|
| `pulse.exe`（DialogBridge 内） | 20.0 MB | 5.94 MB | `.text` 15.3 MB、`.rdata` 3.0 MB、`.rsrc` 0.75 MB |
| `pdfium.dll` | 7.4 MB | 2.77 MB | PDF 预览 |
| `PulsePickerUiProof.dll` | 8.0 MB | 2.69 MB | 进程内打开/保存对话框，`.text` 5.6 MB |
| `Pulse.Content.exe` | 2.7 MB | 0.98 MB | 内容索引 |
| `Pulse.Index.exe` | 2.1 MB | 0.69 MB | 全盘索引服务 |
| `lumatext.dll` | 1.7 MB | 0.65 MB | 文字排版（HarfBuzz/FreeType） |
| `Pulse.Preview.exe` | 1.4 MB | 0.53 MB | 预览宿主 |
| `pulse_elevated.exe` | 1.3 MB | 0.41 MB | 提权操作助手 |
| 根目录 `pulse.exe`（启动器） | 1.0 MB | 0.34 MB | |
| 对话框实验组件 4 个 | 2.5 MB | 0.89 MB | `PulseFileDialogRuntimeUiProof64R8.dll`、`PulseDialogAuto64.dll`、`PulseDialogLauncher.exe`、`PulseDialogAutoControl.exe` |
| 其余小程序 6 个 | 1.9 MB | 0.75 MB | shell、AI、hotkey broker、integration、Document、start_pin |
| 许可证文本等 | — | < 0.1 MB | |

## 建议（按“收益 ÷ 风险”排序）

估算值都是推断，落地前请按“验证方法”实测。

### 1. 把测试和探针代码移出正式版 `pulse.exe`

- **现象**：`build.ninja` 里正式版 `pulse.exe` 的链接列表包含约 40 个测试/探针源文件，例如 `window_management_test.cpp`（4.6 MB obj）、`settings_flow_test.cpp`（4.2 MB）、`apply_all_sort_probe.cpp`（3.8 MB）、`column_resize_test.cpp`、`workspace_ui_test.cpp`、`recycle_search_live_test.cpp`、`file_click_probe.cpp`、`cloud_sync_probe.cpp`、`ai_authorization_probe.cpp`、`explorer_takeover_probe.cpp` 等，obj 合计约 26 MB。它们在 `CMakeLists.txt` 的 `add_executable(pulse ...)` 主列表里，不在 `if(PULSE_TEST_HOOKS_EFFECTIVE)` 块内。
- **做法**：把这些文件移进 `PULSE_TEST_HOOKS_EFFECTIVE`（或 `PULSE_WITH_SELFTEST`）块；`app_main.cpp` 里对应的命令行分发（`--shot*`、各类 probe 开关）用同一个宏包起来，否则链接器会因为仍有引用而保留它们。
- **状态：已完成（2026-11-07）**。关闭钩子时，`CMakeLists.txt` 不再把 14 个测试/探针文件编进 `pulse.exe`；`app_main.cpp` 里对应的入口和 `RecordViewSwitchFrame` 调用改到 `PULSE_WITH_TEST_HOOKS` 下。`cloud_sync_probe`、`download_source_probe` 是产品功能，保留；`hot_entry`、`entry_command`、`explorer_takeover` 三个启动计时探针挂在正常启动流程里，体积小，暂未处理。
- **实测收益**：`pulse.exe` 从 20.03 MB 降到 19.73 MB（-302 KB，约 1.5%），`.text` 从 15.29 MB 降到 15.08 MB。比原先估算的小得多：obj 文件大小严重高估了最终进入 exe 的代码量，后面各项估算请以实测为准。
- **验证**：Release `--shot` 启动渲染正常；设置 `PULSE_TEST_VIEW_SWITCH` 后正式版照常运行、不再进入探针。开启钩子的开发构建，编译输入与之前相同（过滤只在关闭钩子时生效，`app_main.cpp` 在开启钩子时仍带 `PULSE_WITH_TEST_HOOKS` 定义），但本次没有单独构建验证。

### 2. 默认按体积编译，只对热路径用 /O2

- **现象**：现在只有 `PULSE_SIZE_SOURCES` 里的 18 个设置/对话框文件用 `/Os /Oi-`，其余全部 `/O2`，IPO 关闭。
- **做法**：反过来：`pulse.exe` 及辅助程序默认 `/O1`（或 `/Os /Oi-`）加 `/Gw`，链接加 `/OPT:REF /OPT:ICF`；只给文件列表绘制、排序、枚举、索引查询、缩略图解码等热点文件保留 `/O2`（建一个 `PULSE_SPEED_SOURCES` 白名单）。之后再试 `PULSE_WITH_IPO=ON` 配合 /O1：LTCG 能删掉更多未用代码，但构建会更慢。
- **状态：已完成 `pulse.exe` 和 `PulsePickerUiProof.dll`（2026-11-07）**。新增 CMake 选项 `PULSE_SIZE_DEFAULT`（默认 ON），Release 下这两个目标整体用 `/Os /Oi- /Ob1 /Gw`；为避免 D9025，从 `CMAKE_CXX_FLAGS_RELEASE` 里去掉了显式的 `/Ob2`（`/O2` 本身已包含 `/Ob2`，其他目标的内联不变）。想退回速度优先，配置时加 `-DPULSE_SIZE_DEFAULT=OFF`。
- **热路径白名单没有采用**：`pulse.exe` 使用预编译头，单个源文件的优化开关只要和 PCH 不一致就会被编译器忽略（C4653），白名单实际不生效，还会产生 D9025 警告。这也说明原来的 `PULSE_SIZE_SOURCES`（设置页 `/Os`）在使用 PCH 的文件上一直没有生效。真要给热点保留 `/O2`，需要把这些文件放进单独的 OBJECT 库，或者对它们关掉 PCH。
- **实测收益**：
  - `pulse.exe`：19.73 → 15.90 MB（-3.8 MB，-19%），`.text` 15.08 → 12.20 MB，`.rdata` 3.00 → 2.20 MB；
  - `PulsePickerUiProof.dll`：8.04 → 7.19 MB（-0.85 MB）；
  - 安装包（含建议 1）：12.67 → 11.91 MB（-0.77 MB，-6%）。体积优化后的代码本身更难压缩，所以安装包缩小的比例明显低于 exe。
- **性能**：在 System32（约 5000 项）上交替各跑 8 次 `--shot`：枚举+排序中位数 19.9 → 19.2 ms，首帧中位数 985 → 825 ms，没有可测出的退步。测量时机器上另有编译在跑，数据噪声大。滚动、缩略图解码、大目录搜索还没有专门测过，建议用 `pulse_d2d_list`、`pulse_index_perf` 补测。
- **未做**：`Pulse.Content`、`Pulse.Index`、`Pulse.Preview`、`pulse_elevated` 仍是 `/O2`。前两个是索引服务，对速度更敏感，需要实测后再决定。

### 3. 消除 `PulsePickerUiProof.dll` 与 `pulse.exe` 的重复代码

- **现象**：`cmake/PulsePickerInProcess.cmake` 用 `PULSE_PICKER_INPROCESS_SOURCES` 把选择器 UI 及其依赖（`pulse_text`、`pulse_sqlite_core`、`pulse_fs_watch`、`pulse_virtual_disk`、lumatext 等）完整再编译一份进 DLL。两份代码在磁盘和安装包里各占一份。
- **做法（二选一）**：
  - a. 把共用的 UI/模型/存储层提成一个共享 DLL（如 `pulse_core.dll`），`pulse.exe` 和选择器 DLL 都动态链接它；
  - b. 选择器 DLL 只保留一个薄的宿主层，界面由已运行的 Pulse 进程提供（已有 shell bridge / IPC 基础）。
- **预计收益**：压缩后 2–2.7 MB，是单项最大的可回收量之一。
- **风险**：高，工作量大。进程内注入的 DLL 加载更多依赖会影响第三方程序的稳定性，方案 a 要注意 DLL 搜索路径和版本一致性。

### 4. `pdfium.dll` 改为按需下载，或改用系统 PDF 渲染

- **做法（二选一）**：
  - a. 像 ffmpeg 一样做成预览扩展包（仓库里已有 `content_pack_ui.cpp` 和 `preview-pack-ffmpeg` 工作流），首次预览 PDF 时提示下载；
  - b. Windows 10 及以上用系统自带的 `Windows.Data.Pdf` 渲染预览，彻底去掉 pdfium；Win8.1 渠道保留 pdfium 或者不提供 PDF 预览。
- **预计收益**：压缩后约 2.8 MB（约占安装包的 22%）。
- **风险**：低到中。方案 a 只改打包和首次使用流程；方案 b 要核对渲染质量和缩略图速度。

### 5. 对话框接管组件改为可选

- **现象**：4 个对话框实验组件（文件名里还带着 `Proof`、`R8`、`Auto` 等实验命名）压缩后约 0.9 MB，再加上建议 3 的选择器 DLL 2.7 MB，接管系统打开/保存对话框这个功能总计约 3.6 MB。
- **做法**：如果这是可选功能，安装时做成可勾选组件，或在设置里开启时再下载。
- **预计收益**：不勾选的用户可省压缩后 0.9–3.6 MB。
- **风险**：低（打包层面）。

### 6. 合并小型辅助程序

- **现象**：`pulse_hotkey_broker`、`pulse_integration`、`Pulse.AI`、`Pulse.Document`、`PulseDialogLauncher`、`PulseDialogAutoControl` 各自是独立 exe，每个都静态链接一份 CRT 和公共库；`pulse_elevated.exe` 作为提权助手有 1.3 MB，说明链接了大量不需要的模块。
- **做法**：合并成一个多角色的 `pulse_helper.exe --role=...`（需要独立 manifest 的，如提权、服务，可单独保留）；先用链接器 `/MAP` 查 `pulse_elevated` 为什么这么大，裁掉不需要的依赖。
- **预计收益**：压缩后 0.4–1 MB。
- **风险**：中。服务、提权、计划任务等注册路径要同步修改；安装和卸载需要回归测试。

### 7. 关于“防御性代码”

- **判断**：上面的数据说明大头不在防御性检查本身。可疑的是常见的模板和抽象膨胀：项目里约 830 处 `std::function`、67 个文件引入 iostream/sstream/fstream（流库会带进大量代码），每个 lambda 包装都会实例化一份代码。
- **做法**：
  - 用 [SizeBench](https://www.microsoft.com/store/productId/9NDF4N1WG7D6)，或链接器的 `/MAP` 加 `/VERBOSE:REF`，列出 `pulse.exe` 里最大的 100 个函数和编译单元，按事实决定删什么；
  - 热点之外的代码把 `std::stringstream` 换成 `std::format` / `swprintf`；
  - 只有一个调用者的 `std::function` 回调可以改成模板参数或函数指针；
  - 同样的校验或重试逻辑出现在多处时，收拢成一个公用函数。
- **预计收益**：先测量再说，通常每项是几百 KB 级别。

### 8. 不建议做的事

- **改用动态 CRT**：安装包就得带上 VC 运行库，总体积反而增加；现在 solid LZMA 已经能合并各 exe 里重复的静态 CRT（`pack_installer.py` 的排列方式实测已省约 1.1 MB）。
- **更换压缩算法**：已经是 xz 的 extreme 档加 x86 BCJ 过滤，换 zstd 会更大；BCJ2 最多再省 1–3%。
- **删许可证文本**：合计不到 0.1 MB，而且是法律义务。

## 建议的执行顺序

| 步骤 | 内容 | 压缩后预计节省 | 工作量 |
|---|---|---:|---|
| 1 | 测试/探针代码移出正式版（建议 1，已完成） | 实测原始 -0.3 MB，压缩后约 -0.1 MB | 小 |
| 2 | pdfium 改为按需下载或系统渲染（建议 4） | 约 2.8 MB | 中 |
| 3 | 默认按体积编译（建议 2，已完成 pulse.exe 和选择器 DLL） | 实测安装包 -0.7 MB（与第 1 步合计 -0.77 MB） | 中 |
| 4 | 对话框接管改为可选组件（建议 5） | 0.9–3.6 MB（视用户选择） | 小到中 |
| 5 | 合并辅助程序（建议 6） | 0.4–1 MB | 中 |
| 6 | 选择器 DLL 去重（建议 3） | 2–2.7 MB | 大 |

前四步做完，安装包有望从 12.7 MB 降到约 7–9 MB；第 6 步做完可进一步降到 6 MB 左右。真正的大头是第 2、4、6 步（pdfium 与对话框/选择器组件），第 3 步需要实测确认。

## 复测方法

```powershell
# 1. 正式构建并打包
.\build_installer.bat
# 2. 各文件压缩前后大小（参考本文“现状数据”表的测量方式：
#    暂存 pack_dialog_experiment 生成的 installed 目录，逐个 xz 压缩）
# 3. 看代码段大小
dumpbin /headers build\pulse.exe | findstr /C:"virtual size" /C:" name"
```
