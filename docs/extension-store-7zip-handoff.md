# 交接：增强包商店 + 7-Zip 扩展（2026-11-09）

目标（用户原话）：按 `extension_store_mockup.html` 做好插件商店界面；把 7-Zip 作为第一个用 SDK 开发的插件，参考 NanaZip 完善解压和压缩体验；建公共仓库方便别人发布扩展。

用户已确认的决定：
- 商店只列真实条目（官方增强包 + 目录里的扩展 + 本机已装/开发中的扩展），社区区为空时显示空状态，不放假条目。
- 分发走公共仓库 `jimmgreen/pulse-extensions`：作者 PR + CI + 维护者审核后合并。
- 宿主新增对话框 API（`pulse.dialogs`），由宿主绘制 Fluent 模态框。
- 只用官方 7-Zip 26.00 的 7z.dll，不加 zstd 等 NanaZip 专有编解码。
- 官方签名密钥本机生成，不入库。

全部改动都**未提交**（Pulse 主仓库）。公共仓库已推送首个提交。

---

## 1. 已完成

### 1.1 公共仓库 `C:\Users\SS\Desktop\pulse-extensions` → https://github.com/jimmgreen/pulse-extensions （public）
- `tools/catalog.py`：校验 `publishers/*.json`、`extensions/<id>/extension.json`；`validate --download` 下载安装包核对大小/SHA-256/清单/x64 DLL/签名；`build` 生成 `index.json`；`publish --private-key` 用官方密钥签名并上传到 `catalog` Release（`gh release upload --clobber`）。
- `tools/pulse_sign.py`：CNG RSA 密钥生成、清单签名（前缀与 Pulse `package_signature.cpp` 一致，已与 `tools/extensions/package_signing.py` 互验通过）、目录签名（前缀 `Pulse.Extension.Catalog.Signature\0v1\0`）。
- `README.md`、`CONTRIBUTING.md`、`.github/CODEOWNERS`、PR 模板、`LICENSE`、`.gitignore`（忽略 `*.private.blob`）。
- 官方发布者：`publishers/86f9a10e981d16ac677eb11b9c45e82bf38a7e00c0d3d271e84ef9e0d7a4762b.json`（`official: true`）。
- 目录地址（Pulse 要读的）：`https://github.com/jimmgreen/pulse-extensions/releases/download/catalog/index.json` 与 `index.sig.json`。只有 GitHub Release 资源地址能走镜像（见 `src/app/update_transport.cpp` `AcceleratedUpdateUrl`）。
- **未推送**：`.github/workflows/validate.yml` 在本地。`gh` token 缺 `workflow` scope 被拒。用户需运行 `gh auth refresh -s workflow`，然后 `git add .github/workflows && git commit && git push`。
- 官方私钥：`%LOCALAPPDATA%\Pulse\signing\official\publisher.private.blob`（公钥同目录）。提醒用户备份。

### 1.2 宿主 `pulse.dialogs` API（Pulse 主仓库，已编译通过，未做运行时验证）
- 公共头：`sdk/native/include/pulse/dialogs.h`（show / set_states / close；1–3 个按钮；可选 UI-model 表单；每会话同时一个）。
- 窗口：`src/ui/form_dialog.h/.cpp`（仿 `connect_server_dialog`，异步：Show 立即返回，窗口收到 `kRunMessage` 后跑自己的模态循环；表单是 `ExtensionTreeView` 子窗口）。
- 适配器：`src/app/extensions/native/dialogs_adapter.h/.cpp`；已接入 `session.cpp`（成员、Stop、Drain、Query）。
- 文本框 `kind: "password"`：`ui_tree.h` 加 `input_kind`，`ui_tree.cpp` 允许并校验，`extension_tree.cpp` 建编辑框时加 `ES_PASSWORD`（LumaText 已支持 `EM_GETPASSWORDCHAR` 掩码）。
- 已重新生成：`sdk/native/api-catalog.json`、`src/app/extensions/api_description.generated.h`（`python tools/extensions/generate_native_catalog.py`、`generate_sdk.py`，不要加 `-I`），ABI 快照 `sdk/native/abi/windows-x64-msvc-preview3.json`（只增不减，`check_native_abi.py check` 通过）。`bundle_native_sdk.py` 已加入 dialogs.h。

### 1.3 暂存提交上限（宿主）
- `src/app/extensions/native/staging_adapter.cpp` `Inspect()`：8192 项 / 256 顶层 / 4 MiB → 2,000,000 项 / 4096 顶层 / 1 GiB（仅计数，不占内存）；`staging.h` 注释与 `generate_native_catalog.py` limits 同步。

### 1.4 7-Zip 扩展 `extensions/7zip/`（新目录，已编译，引擎测试全过）
- `engine.h/.cpp` + `sevenzip_com.h`：任意 7z.dll 格式（先按扩展名，再常见格式）；`.001` 分卷与 `.tar.gz/.tar.xz/.tar.bz2` 嵌套直接打开到内层（内层先解到临时目录，`PULSE_7ZIP_TEMP` 可覆盖）；密码（7z 头加密、zip AES/ZipCrypto），错误密码识别；安全路径校验、跳过链接、CREATE_NEW 不覆盖；创建 7z/zip/tar/tar.gz/tar.xz/tar.bz2，等级、固实、密码、加密文件名、zip AES-256、分卷；CRC32/SHA-1/SHA-256。
- `engine_test.cpp`：24 项全过（`build-extensions\7zip\pulse_7zip_engine_test.exe`）；`--fixture DIR` 生成手测用压缩包（加密 7z 密码 `pulse`、多顶层 zip、单文件夹 tar.gz）。
- `text.*`：简/繁/英字符串，按 Windows 显示语言选（SDK 不提供 Pulse 语言，已知限制）。
- `dialogs.*`：对话框队列；密码框、压缩选项框（名称/格式/等级/分卷/固实/密码/确认/加密文件名/zip 加密方式，格式切换时联动扩展名与可见性；校验失败带错误重新打开）、校验值结果框（复制到剪贴板）。
- `jobs.*`：任务 + 暂存（同盘）+ 提交；工作线程经 `pulse.dispatch` 到 UI 线程弹密码框并等待（可取消），密码错误最多重试 5 次；智能解压（单个顶层项原地，多个则包进同名文件夹）、解压到同名文件夹、解压到…、测试、压缩、校验值。
- `settings.*`：扩展设置页（默认格式/等级/“解压到…”默认位置），键 `7zip.preferences`。
- `plugin.cpp`：8 个右键命令（智能解压、解压到同名文件夹、解压到…、测试压缩包、添加到压缩包…、压缩为 7z、压缩为 zip、计算校验值），压缩包预览提供者（.7z .rar .tar .gz …）。
- `manifest.json`（id `pulse.7zip`）、`NOTICE.txt`、`CMakeLists.txt`、`7z.def`。

构建：
```
cmake -S extensions/7zip -B build-extensions/7zip -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULSE_7ZIP_DIR=C:/Users/SS/Desktop/pulse/build-extensions/p1-archive
cmake --build build-extensions/7zip
```
（需 x64 VS 开发环境：`"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"`；沙箱里 `%TEMP%` 不可写，测试数据放在 exe 旁。）

已打包（未签名、未发布）：`build-extensions/7zip-package/pulse.7zip-1.0.0.pulse-ext`，manifestSha256 `8124bf7c6209329b9724fa8660a30a6d36f15dcf1a550c1f778177cf882cbee9`。7z.dll（26.00）SHA-256 `bbd705e3b58ca7677c1e9e67473f166a6712da034dcb567d571fbb67507a443f`，**发布前要与官网 7-Zip 26.00 x64 里的 7z.dll 核对**。

已装进隔离商店并启用：`build-extensions/7zip-live/store`；手测压缩包在 `build-extensions/7zip-live/fixture`。

### 1.5 一步到位改版（2026-11-09 晚）
- 宿主写入确认框加“始终允许，不再询问”（`ExtensionPublishAlways`，id 6432）；选过的扩展记在 `AppPrefs::extension_publish_trusted`，之后直接发布（仍可撤销、仍走冲突处理）。改动：`staging_adapter_host.cpp`、`app_prefs.h/.cpp`、`tools/l10n/catalog.tsv`。
- 7-Zip 设置页重做（`settings.*`）：分“解压 / 压缩 / 密码”三组；解压方式（智能 / 总是同名文件夹 / 直接解压）、解压到（压缩包所在文件夹 / 固定文件夹 + 浏览）、快速压缩格式与等级、记住本次密码；**改动即保存**（订阅 `pulse.ui.events`，无保存按钮），值格式 `3\n…`。
- 右键命令收为 6 个：按设置命名的“解压”（智能解压 / 解压到同名文件夹 / 解压到此处 / 解压到固定文件夹）、解压到…、测试压缩包、“快速压缩为 7z”（按默认格式，无弹窗）、添加到压缩包…（唯一带选项的弹窗）、计算校验值。设置变化时重新注册改标题（`plugin.cpp` `Refresh`）。
- 本次会话输入成功的密码会记住（最多 20 个，仅内存），解压时先静默尝试，都不对才弹密码框（`jobs.cpp` `PasswordSession`）。
- 压缩选项框改成一列对齐的标签/控件网格，按格式隐藏不适用项；密码框只剩一个输入框。
- 已编译（扩展与 pulse 均无警告），引擎测试 24/24；**界面未在真机里看过**。`pulse.7zip-1.0.0.pulse-ext` 已过期，需重新打包、重签并更新 extension.json 的哈希。

### 1.6 已正式发布（2026-11-09）
- Release `pulse.7zip-1.0.0`：https://github.com/jimmgreen/pulse-extensions/releases/tag/pulse.7zip-1.0.0 （包 1069159 B，sha256 `584df97f…3be46d`，manifestSha256 `45a9fbdc…8462`）。
- `extensions/pulse.7zip/extension.json` 已提交推送；`catalog.py validate --download` 通过；签名目录已发布到 `catalog` Release。
- 本地 `pulse-extensions` 文件夹曾丢失，已从 GitHub 重新克隆；`.github/workflows/validate.yml` 随之丢失，需重写并在 `gh auth refresh -s workflow` 后推送。
- 端到端探针 `src/bench/extension_store_live_probe.cpp`（目标 `pulse_extension_store_live_probe`，联网，手动运行）：用 Pulse 自己的 CatalogService/InstallService 拉取并验签目录 → 下载并校验包 → 信任发布者 → 安装到隔离商店，已通过。
- 版本：按用户决定把目录里的 minPulse 降为 1.0.59 并重新发布签名目录；探针以 1.0.59 身份再跑一遍通过。注意：已发布的正式版 1.0.59 没有 `pulse.dialogs`，但它也没有商店，装不上；只有含本工作区改动的 1.0.59 构建能用。

---

## 2. 下一步（按顺序）

1. **真机验证 7-Zip + 对话框**（已完成部分定向验证，见第 5 节）：
   ```
   build\pulse.exe --test-instance --extension-store C:\Users\SS\Desktop\pulse\build-extensions\7zip-live\store-fixed C:\Users\SS\Desktop\pulse\build-extensions\7zip-live\fixture
   ```
   注意：`pulse_ext.py run` 会清除外部 `PULSE_*` 环境变量；若需隔离配置应传 `--profile`。此外 Release（`PULSE_WITH_SELFTEST=OFF`）的 `GetPulseDataDir()` 不支持 `PULSE_TEST_DATA_DIR` 重定向，不能把仅设置环境变量等同于配置已隔离。GUI 验证始终使用 `--test-instance`，仅操作仓库内夹具，不停止其他 Pulse 进程。
   打开 fixture 文件夹，右键测试：加密 7z 智能解压（密码框、错误密码重试、取消）、多顶层 zip 智能解压（包进文件夹）、tar.gz 直接出文件、添加到压缩包…（对话框布局/联动/深浅色/DPI）、压缩为 7z、计算校验值、测试压缩包、扩展设置页。重点看 `form_dialog` 的模态、Enter/Esc、焦点、表单背景与对话框是否一致、窗口高度是否够（压缩框 height=470）。修问题后再打包。
2. **签名并发布 7-Zip 1.0.0**（已完成本地部分，2026-11-09）：
   - 已签名：`build-extensions/7zip-package/pulse.7zip-1.0.0.sig.json`（936 B，sha256 `ae412ca0…0bd98`），已用公钥验证；包 1058283 B，sha256 `9a9915f9…fd70b`。
   - 已写目录条目：`C:\Users\SS\Desktop\pulse-extensions\extensions\pulse.7zip\extension.json`（未提交），`catalog.py validate` 通过。
   - **第 1 步真机验证后如重新打包，必须重签并更新 extension.json 里的 size/sha256/manifestSha256。**
   - 剩下：上传 Release、`validate --download`、`publish`（公开操作，先问用户）。原步骤：
   - `python C:\Users\SS\Desktop\pulse-extensions\tools\pulse_sign.py sign-manifest build-extensions/7zip-package/manifest.json --private-key %LOCALAPPDATA%\Pulse\signing\official\publisher.private.blob --output build-extensions/7zip-package/pulse.7zip-1.0.0.sig.json`（重新打包后必须重签）。
   - 在 pulse-extensions 建 Release `pulse.7zip-1.0.0`（`--latest=false`），上传 `.pulse-ext` 和 `.sig.json`。
   - 写 `extensions/pulse.7zip/extension.json`（字段见 CONTRIBUTING.md；category `fileops`，icon `{"glyph":"archive","color":"#A16207"}`，license `LGPL-2.1-or-later (7-Zip) + unRAR restriction`，minPulse `1.0.60`，sdkPreview 3，resources `["settings.own"]`）。
   - `python tools/catalog.py validate --download` → `python tools/catalog.py publish --private-key …`。发布是公开操作，先向用户确认。
3. **C++ 目录客户端 + 应用内安装器**（新模块，建议 `src/app/extensions/store/`）：
   - 拉取 `index.json` + `index.sig.json`：复用 `ReadUpdateWithFallback`/`UpdateSourceUrls`（镜像 1、镜像 2、GitHub）；用内置官方公钥（上面的指纹，公钥十六进制在 publishers 文件里）按目录前缀验签；缓存到 `%LOCALAPPDATA%\Pulse\catalog\` 供离线显示。
   - 安装：下载包与签名，核对大小/SHA-256；读 ZIP 用系统 libarchive `archiveint.dll`（`src/app/zip_compress.cpp` 已加载其写接口，读接口同库，Win10 1803+；Win8.1 显示不支持）；按 `tools/extensions/package_store.py` 的布局与规则移植写入：`versions/<sha>.json` + `.sig.json`、`packages/<sha>/<files>`、`registry.json`（事务、`.registry.lock`）、`signature-policy.json` 加入该发布者公钥；启用/停用/卸载同 `update()`。宿主只在启动时读商店（`LoadPackageStore`），安装/启用后提示“重启 Pulse 生效”。
   - 已装版本 = 用 registry 的 `activeRevision`（清单 SHA-256）对照目录里各版本的 `manifestSha256`。
   - 加单元测试（ops/独立测试目标），覆盖篡改哈希、错误签名、未知发布者、成员多/少、回滚。
4. **商店界面**（设置页 8“增强包”，按 mockup）：
   - 现状：`settings_layout_sections.h` `LayoutSettingsPacks`（page 8 分支）、`ui_settings_packs.cpp` 绘制、`ui_hit_test.cpp` 822 行附近、`src/app/settings_ui.cpp` 处理 `PackAction`，状态在 `ui_view_model.h`（`settings_pack_*`）。
   - 要做：标题区 + 三个标签（发现 / 已安装 N / 更新 N，导航“增强包”带更新角标）、分类芯片（全部/预览/搜索/文件操作/面板/工具）、“Pulse 官方”与“社区扩展”两段卡片网格（图标块颜色+字形、名称、发布者+认证标记、两行说明、大小+权限摘要、主按钮或下载进度）、点卡片开右侧详情抽屉（截图占位、“它能做什么”权限逐条、信息、更新内容、来源）、页顶横幅（连不上下载源 / 签名或版本不匹配 / 崩溃停用 `ExtensionCrashDisabled*` / 需要重启）。
   - 现有功能不能丢：FFmpeg 自定义路径、图像/RAW/内容搜索/AI 增强包卡片及其操作、自动更新、下载源下拉与测速、卸载时删除、打开文件夹、扩展设置入口（page 15）。可放进对应详情抽屉或“已安装”页底部“下载源与更新”区。
   - 文字一律加到 `tools/l10n/catalog.tsv` 后跑 `python tools/l10n/gen_strings.py`，不要手改生成文件、不要用 `l10n::Pick`。
   - 按 mockup 的状态逐个截图核对（加载、离线、不匹配、崩溃、窄宽度 680、深浅色）。
5. **收尾**：`cmake --build build --target pulse`，只跑相关测试（folder 以外：extension tree/ui_model 相关测试、`pulse_ops_test` 若动到操作），更新 `docs/native-packages.md`（7-Zip 与 dialogs、暂存上限），再按用户要求提交。

---

## 3. 本会话其他未提交改动（与本任务无关，别丢）
- 文件夹缩略图 E 方案：`src/preview_host/folder_thumbnail.cpp`（立卡 + 透视前片，`style` 6），`pulse_folder_thumbnail_test` 29/29 通过。
- 空状态插画重绘：`assets/*-state.svg` 五个；去掉浅色下 0.68 透明度（`ui_pane_list.cpp`、`ui_details_panel.cpp`、`ui_settings_view.cpp`、`global_search_window.cpp`）。
- 吸附窗口尺寸保存：`src/app/window_placement.*` `SavedNormalRect()`、`app_main.cpp` 三处、`src/bench/window_placement_test.cpp`（26/26）。
- 效果页：`folder_thumb_mockup.html`（仓库根目录，未跟踪）。

## 4. 已知风险
- `form_dialog` 嵌套模态循环在宿主消息分发中运行；会话停止时 `Destroy()` 不回调，7-Zip 工作线程靠任务取消退出等待（`jobs.cpp` `PasswordPrompt`）。
- 7-Zip 插件在 Pulse 进程内运行（原生扩展不是沙箱）。
- 宿主暂存上限放大后，超大压缩包提交时宿主文件操作的耗时/内存未实测。

## 5. 续做记录（2026-11-09，修复“点了没反应”）

### 已修复
- `ContextMenuController::ExecuteShellCommand()` 原先把所有 `command >= CmdShellComBase` 都当成 Explorer COM 菜单命令吞掉，原生扩展的 `0x20000000` 命令根本到不了扩展回调。新增 `CmdShellComLast = CmdShellComBase + 0x7FFF`，仅处理 Shell 实际分配的编号范围。
- `src/bench/ctx_menu_controller_test.cpp` 新增 6 条边界检查：Shell 最大编号仍被处理；上界之外、原生命令/固定按钮/包管理命令继续分发，且不等待 Explorer、不触发 Shell 调用或刷新。
- 实际弹框后发现密码框的 form height 72 DIP 不够，增到 96 DIP，150% DPI 截图确认输入框完整、无滚动条，并验证密码掩码。
- 长时间等密码时，文件操作进度窗原先会在 2 秒后盖住密码框。`app_ops_ui.cpp` 自动弹进度窗前检查宿主是否启用，模态期间延后自动弹出；等待 4 秒的真机截图确认密码框不再被遮挡。
- 临时诊断已从 commands/tasks/plugin/jobs 清除；引擎保留了有价值的 I/O 错误详情（临时目录路径与 Windows 错误），不再只报一句无法创建临时目录。

### 已验证（不是完整验收）
- x64 Release `pulse` 构建通过；`pulse_ctx_menu_controller_test` 全部通过（包含本次新增边界）。测试文件原有 C4456 隐藏局部变量警告位于本次新增代码之前，未扩大范围修改。
- 7-Zip DLL 重新构建通过；`pulse_7zip_engine_test.exe` 24 项通过。
- 真正通过右键菜单启动：加密 7z 密码框、错误密码重试、Esc 取消后宿主恢复启用；正确密码 `pulse` 后经宿主确认发布文件，核对两份输出字节与夹具相同、无暂存残留。
- 多顶层 ZIP 智能解压：经宿主确认后包入同名文件夹，输出字节已核对。
- “添加到压缩包…”框实际打开/取消，150% DPI 浅色布局截图已检查；尚未验收格式切换、真正压缩、深色及其他 DPI。
- tar.gz：默认临时目录路径在 GUI 工作线程创建目录时返回 `Access is denied`，尚未定位权限差异，**不能声称默认环境已通过**。显式设置 `PULSE_7ZIP_TEMP` 到仓库内独立可写目录后，真机解出单个顶层文件夹、不多套同名目录，文件字节及嵌套临时目录清理通过。引擎测试本身也使用显式可写临时目录。
- 验证脚本与截图在 `build-extensions/7zip-live/dispatch-check/`；脚本 `ui_drive.ps1` 只操作自己创建的 `--test-instance` 进程，失去前台即拒绝发送键盘输入，结束关闭该测试进程，不杀其他 Pulse 实例。

### 最新本地候选包（旧包不要发布）
- 最新包目录：`build-extensions/7zip-package-r2/`；安装并启用的签名测试商店：`build-extensions/7zip-live/store-r2/`。
- manifest SHA-256：`4138605c28d99c469807750192545ea7968b303b2bca8692267353e2e40d8361`。
- `.pulse-ext`：1,058,405 B，SHA-256 `d2f433ed3ee817b08e3d24e17dece372d2941c89ffecec9596d7a9eb0462ac41`。
- `.sig.json`：934 B，SHA-256 `7bfa8b9ef425efb7c4b3d352817d29632e93ccffbca812bcaac00b15f6d124d1`。
- 官方签名、包完整性、宿主清单验证通过；使用正式 CLI 安装到 requireSigned 商店并启用成功（不是手拼目录）。公共仓库本地 `extension.json` 已同步上述散列，`catalog.py validate` 通过。未上传、未提交、未推送。
- 第 2 节发布仍需用户确认；仍要完成剩余真机验收及官网 DLL 校验，后续有重打包则再更新本节和公共目录元数据。目录客户端进度见第 6 节；应用内安装器和商店 UI 尚未接入。

## 6. 目录客户端与后续验收（2026-11-09）

### 目录底层已实现并集成构建
- `src/app/extensions/store/catalog.h/.cpp`：纯离线 `LoadVerifiedCatalog`，先按公共 tools 的 `Pulse.Extension.Catalog.Signature\0v1\0` 域验原始 index 字节，再解析 v1 schema；生产固定官方 pin/公钥。禁止把目录内发布者当作目录签名信任锚。强制边界、UTF-8、严格整数、无重复/未知键、安全 URL、发布者归属、保留 pulse.* 命名空间、版本排序等。输入上限 index 4 MiB、signature 16 KiB；发布者 512、扩展 2048、累计版本 8192，每扩展最多20个版本。
- `catalog_client.h/.cpp`：同步 worker-only `FetchCatalog(cache_file, cancelled, snapshot, error)`，结果 Online/Offline/Failed/Cancelled。开始时捕获下载源，复用 `UpdateSourceUrls` 和 `ReadUpdateResponse`，每个镜像成对拉取 index/signature、验签后才采用，不混流；失败换下一个来源。
- 离线缓存是一个原始字节 envelope，读时必须重验官方签名；同目录 CREATE_NEW 临时文件、写入/flush 后按句柄原子替换。失败/取消保护旧缓存，取消不回退离线。缓存持久化失败仍返回有效在线数据并附警告。缓存路径须绝对且父目录由调用方预先创建。多窗口共享同一缓存时调用方须串行化；不是安装器的跨进程事务锁。
- 对比已验缓存的 generated 拒绝在线旧目录；这不等于可信时钟新鲜度，缓存删除/初次启动时没有持久防回滚锚点。调用方必须显示 offline 状态，不能把目录签名当作包签名/包 hash 已验证。
- 主 `CMakeLists.txt` 已加入独立测试目标（测试信任注入宏只在测试目标上），生产源码由现有递归源列表收录。x64 Release 主程序集成构建通过，无新增警告。
- 主工程 `pulse_extension_catalog_test` **91 passed, 0 failed**；`pulse_extension_catalog_client_test` **34 passed, 0 failed**。覆盖合成 CNG 签名、畸形/篡改目录、镜像失败/半流/尺寸边界、缓存重验/原子覆盖/回滚/取消。符号链接两个分支因 Windows 创建 symlink 报1314缺少权限而明确跳过，目录 leaf 拒绝已过，**不要把 symlink 分支写成通过**。
- 公共 catalog release 两文件尚不可下载：读取固定公开 URL 返回 HTTP404，没有写伪造正式目录，没有发布任何内容。真实已发布官方 index 端到端验签仍待发布获准后验证；现有成功证据是合成测试签名，不是官方发布目录。

### 7-Zip 新增实机结果/限制
- `temp_probe.exe`（仅 `build-extensions/7zip-live/` 下临时探针源码）：独立链接引擎、不加载 Pulse，在 `C:\Users\SS\AppData\Local\Temp\` 对两种目录名调用原生 `CreateDirectoryW` 都返回5/Access denied。由此排除只在 Pulse/7-Zip 回调内出现的故障，但尚未证明具体权限来源。没有运行权限修复或修改系统目录权限；该默认路径限制仍未解决。
- 浅色真实表单创建 `hello.7z`，经宿主确认发布，再用引擎解压，文件字节与输入一致。
- 150% DPI 深色 7z/ZIP 表单截图已检查；改 ZIP 后名称更新为 `.zip`、隐藏固实/加密文件名、显示 ZIP 加密方式。真实 ZIP 发布后用 Python zipfile 核对只有 `hello.txt` 且字节完全相同。证据 `dispatch-check/shots-create-dark/`、`shots-format2-dark/`。
- 两次最初键盘序列没有真正完成预期的 TAR 切换；部分截图名含 tar 但内容仍是 ZIP，不得算 TAR 联动通过。后续验证使用确定的控件点击并检查状态，不仅靠脚本退出码。驱动已修正 IntPtr::Zero 布尔判断，未找到菜单会明确失败，不再静默向0句柄发消息。
- 早先中断确认框的测试夹具内发现一处旧 `.pulse-stage-*` 残留（`compression-theme` 内，PID5896），保留现场不删除。旧驱动向禁用的宿主直接发 WM_CLOSE 后最多等5秒就强杀，不能当作正常退出清理证据。驱动现先关闭自己PID的表单/确认框，再等正常退出，日志明确记录 NORMAL/FORCED；未找到菜单明确失败。
- 新 `confirmation-cancel` 用例：真实压缩完成后停在宿主确认框，关闭确认框后正常退出，日志 `CLEANUP NORMAL`；目录只剩原始 `hello.txt`，没有发布归档和 `.pulse-stage-*`。因此该定向正常取消/退出流程通过；崩溃/强杀不保证清理，旧残留不是已证明的正常退出缺陷。

- 单文件“计算校验值”实机通过：弹框显示 CRC32 `f47dd391`、SHA-1 `e5df6c4f0d781ccd860c9747e763d100a152989c`、SHA-256 `fc36d7cf36552c0180014463880cf4bc182919ce234058f2be3fb4f687d480b8`，逐项与 Python zlib/hashlib 计算相同；Esc 后宿主恢复启用，正常退出。未点击复制，不改用户剪贴板。截图 `dispatch-check/shots-hash/checksums.png`。多文件长消息/UTF-8 截断边界未验证。

## 7. 应用内安装器安全核心（2026-11-09，目标第2轮）

### 已实现并构建验证
- `src/app/extensions/store/package_archive.h/.cpp`：`VerifyCatalogPackage` 验证下载资产实际长度/SHA-256、清单原始摘要、ID/resources、发布者公钥及现有包签名协议。调用方必须提供同一已验目录内关联的 extension/release/publisher；ZIP 内元数据不建立信任。成功结果 `VerifiedPackage` 是 private shared_ptr 的不可变内存快照，失败/取消不覆盖旧输出。
- 在调用系统 `archiveint.dll` 内存解码前预扫 ZIP EOCD/central/local，一致性、CRC、实际解压字节数和预算均检查；精确成员及 package.json 与 manifest 匹配。只支持 flat regular stored/DEFLATE，拒绝 ZIP64、多盘、链接、加密、路径/ADS/DOS名称、重复大小写名称和多余/缺失成员。2–66成员、16 KiB central directory、单payload64 MiB、payload总256 MiB、metadata及manifest单独限额。不落盘、不执行包DLL、不调用Python生产安装器。
- `native_image_verify.h/.cpp` 仅公开 `ValidateNativeImageBytes`，复用原生产 PE结构校验；原宿主加载路径未改变。这样避免只满足Python最小magic检查的畸形DLL被“安装成功”后启动拒载。共享合成fixture不执行DLL，不读取官方私钥。
- `src/app/extensions/store/install_transaction.h/.cpp`：worker-only ReviewInstall/CommitInstall，要求本地既存根目录；volume root向下pin祖先、拒绝reparse；兼容Python的 `.registry.lock` byte-range锁，hardlink锁在初始化写入前拒绝。复验本地publisher policy、已装清单、发布者身份；不同已信任发布者也不得直接接管同ID。
- 审阅快照绑定候选摘要、发布者ID、已装revision/enabled/native/新增resources；提交锁内重读，过时审阅拒绝。即使两位已信任发布者签同一份清单，也不能复用另一发布者的用户审阅授权。安装、额外权限、native执行分别显式同意；新装默认disabled，升级保留enabled/previousRevision。**此核心绝不自动信任发布者**：本地policy必须已通过显式流程配置，不存在则返回publisher_trust_required。
- 文件使用独占CREATE_NEW、flush、按句柄rename；多文件payload先完整写临时目录再整体发布；现有不可变内容只校验、不覆盖/修复，额外成员拒绝。已验policy/manifest/signature/payload读句柄保持到registry提交，防校验后写入。registry原子替换为提交点，成功返回restart_required，不热载native代码。
- 取消/失败不改registry；已发布但未引用的正确不可变版本允许留存重试。临时文件是尽力清理，若外部锁住文件，不能承诺清理完成；崩溃/掉电恢复和垃圾回收仍未实现。没有自动创建store根、发布者授权初始化、enable/uninstall/rollback完整服务/UI。

### 实际测试与一次集成失败的处置
- 独立归档测试最终 **71 passed,0 failed**（70合成边界+只读真实r2包兼容），包括公开policy/signature/目录资产长度hash、4payload、DEFLATE与相同生产PE检查。主工程新增 `pulse_extension_package_archive_test` **70 passed,0 failed**（不带真实本地候选参数）。真实r2兼容不是“已下载公开目录”的证明。
- 主工程 `pulse_extension_install_transaction_test` 最终 **47 passed,0 failed**，/W4 /WX；生产 `pulse` 构建通过，无新增warning。包括实际生产LoadPackageStore读取已新装启用的singleDLL/multi-file，升级/幂等/过时授权/缺少信任或同意/取消/失败registry替换/锁争用/锁hardlink外写防护/已验payload写锁/篡改和额外文件拒绝/暂存清理。
- 第一次主集成multi目录rename曾报Win32 5，保留 `build-extensions/7zip-live/integrate-installer.log`；同一exe无改动二跑39/0，因此不能凭第一次结果宣称持久权限损坏或产品已验证通过。窄探针及成功/失败fixture只读比较见 `build-extensions/rename-diagnosis/`：对应三层SDDL完全相同；原生目录中持有打开的child/nested child可确切复现rename=5，关闭后同handle成功。**未确定最初是哪个进程/句柄，不能归因AV**，未改任何权限。
- 多文件目录发布因此增加可取消、至多900ms等待的有限重试；永久拒绝仍失败，不绕权限。新增确定性用例真实打开stage child触发Win32失败，释放后重试成功；在重试中取消则registry不变且清理stage。最终集成记录 `integrate-installer-retry.log` 为43/0。这个改进不修复默认Windows临时目录问题。
- 最终窄审查补两个安全回归：元数据名字仅大小写变化时原先会从map.at抛异常，已用6个大小写/缺失用例先证3个异常，再严格要求两个精确小写成员并防御find访问；以及同清单被不同已信任发布者签名时，审阅必须绑定publisher ID，新增4个授权/已装身份用例。最终 `integrate-installer-final.log` 记录生产build成功、归档70/0、事务47/0。
- 未在Win8.1实机验证缺archiveint分支；仅预取消+发布重试中取消有确定性证据，未做解码过程中取消注入；没有实际分配满256 MiB做峰值内存负载，只覆盖声明预算/成员边界。包下载、用户授权UI和端到端应用内安装仍未接线。

### 当轮下一步（最新进展见第8节）
1. 接包/签名下载器（复用网络传输/镜像，严格预算、资产hash、取消），接已有 VerifiedPackage 和事务核心。实现显式publisher信任/根目录初始化流程，不要静默写信任policy；审阅/native授权与重启提示接宿主UI。服务须保留本会话pending-restart，不能用后续幂等安装返回false把尚未重启的状态清掉。
2. 把目录客户端放到应用服务 worker/generation-cancellation 状态，接入真实商店 UI（包括空/加载/离线/失败/不兼容/崩溃状态）并保留原增强包所有功能。目前仍**没有商店 UI 入口**。
3. 继续7-Zip格式/加密联动、测试压缩包/设置入口与必要 DPI 验收；单文件校验和正常确认取消/退出已通过，无需机械重跑。默认临时路径限制仍未修复。
4. 公开发布前必须另获用户确认；本轮未发布、未push、未读取私钥、未修改用户偏好。

## 8. 下载、发布者授信和目录 worker（2026-11-09，目标第3轮）

### 已完成的可用组件
- `store/package_download.h/.cpp`：`FetchCatalogPackage(catalog,id,version,cancelled,out,error,reader)` 从同一已验Catalog选择关联的extension/release/publisher，捕获一次下载源。按资产预算/合法GitHub release URL/实际size/SHA-256逐源重试，每次新缓冲，不拼接半流；即使注入reader忽略consume=false也保留overflow拒绝。两资产各自可来自不同镜像，但必须逐个匹配固定目录hash。最后复用 `VerifyCatalogPackage`。取消不fallback，失败不改旧out，不落盘/执行DLL/改policy。
- `store/publisher_trust.h` 的 `ReviewPublisherTrust` / `CommitPublisherTrust` 实现在现有 `install_transaction.cpp` 中，复用同一Session/根目录pin/注册表锁/flush+rename后端，而非复制第三套路径校验。预览不创建缺失root或授信；已有root可能初始化锁文件。提交必须显式同意并绑定root、包revision、publisher、policy原始摘要、root是否已存在和already_trusted。过时状态拒绝重审，不把目录验签当作用户授信。
- 同意后才可逐级创建缺失root、原子添加指定发布者。保留已有pins与requireSigned；新创建store默认requireSigned=true，既有无policy开发商店保持原隐式unsigned模式false，避免仅“信任一个作者”就悄悄改变开发包策略。坏/空/满policy不重置，不移除已有发布者。授信不安装、不启用、不代替native权限同意；原生代码仍需下一次独立安装审阅。
- 新统一路径检查拒绝volume root、相对/dot及Win32尾点/尾空格别名、reparse。InstallReview新增绑定store root，防相同包/相同已装状态的授权被重放到另一个商店。信任policy与Python trust/untrust均使用 `.registry.lock`；不承诺抵御绕过锁的同用户policy编辑者在最终read/rename间抢写（不是CAS或系统权限隔离）。取消/失败保留旧policy/out；获明确同意后创建的空目录/锁允许留下重试。
- `store/catalog_service.h/.cpp`：单worker串行 `Refresh(cache_file)->generation`、`Cancel`、`GetSnapshot`、`Stop`，状态Idle/Loading/Online/Offline/Failed/Cancelled，结果shared_ptr<const CatalogSnapshot>。刷新立即新Loading/清旧结果，旧request原子cancel，pending合并只留最新。晚到success/异常都按generation丢弃；std/unknown异常转Failed后worker还能继续。fetch/网络不持UI状态锁，不注册裸窗口owner回调。
- Stop只发取消不join，析构Stop+join**必须在离开活跃UI线程的生命周期中执行**；网络取消仍可能等已有WinHTTP超时，未承诺即时断网。每实例串行不等于多个实例共享cache锁，UI应共享一个AppCore级服务。它不创建cache父目录，也尚未由AppCore持有或接到设置窗口。

### 实际验证
- 主工程新增 `pulse_extension_package_download_test` **65 passed,0 failed**；fake网络覆盖镜像顺序、半流重置、尺寸/hash拒绝重试、固定来源快照、恶意reader、取消/out保持及最终签名/清单不符，不是公开网络端到端验证。
- 主工程新增 `pulse_extension_catalog_service_test` **39 passed,0 failed**；确定性condition_variable gate覆盖快切/旧结果丢弃、合并、取消、Stop/离线程销毁、异常及恢复，不sleep轮询，不是真实WinHTTP关闭延迟测试。生产构建无fetch注入构造器，仅测试宏可注入。
- 主工程新增 `pulse_extension_publisher_trust_test` 最终 **32 passed,0 failed**：缺失root预览/拒绝/预取消不创建、明确授信不安装、签名可验证、幂等、native同意仍必需、旧policy/pins/隐式开发模式保持、陈旧policy/root/包/publisher/root重放拒绝、畸形policy不覆盖、替换失败旧文件保留及临时清理、路径别名。
- 受影响 `pulse_extension_install_transaction_test` **48 passed,0 failed**（新增跨store重放拒绝）；生产 `pulse` 集成构建成功，无新增warning。记录 `build-extensions/7zip-live/integrate-store-r3.log`。只运行受影响目标，未全量回归、未做UI截图，因为没有界面接线。
- 下载/目录状态测试用合成已验模型/测试操作，没有新的真实公开目录验证；没有读取官方私钥、公开发布、push、用户偏好写入或权限修复。默认Windows临时目录限制仍未解决。

### 下一步（不要再重复实现这些底层）
1. 应用级安装协调器：把已验目录快照、下载、可选发布者授信审阅、InstallReview/InstallConsent、提交串成worker状态机，按钮命令绑定当前generation/包/发布者/root。补host版本/SDK兼容性门控；保留会话pending-restart，不让幂等结果清掉待重启状态。cache父目录应放store外，不为取目录偷偷初始化安装根。
2. 接 `extension_store_mockup.html` 的真实商店UI，保留原增强包的预览/AI/高级下载源/本机扩展功能；社区无真实条目就空状态，加载/离线/失败/不兼容/崩溃、深浅色/DPI/窄宽均需实际验证。当前仍无商店入口、无授权对话框、无端到端应用内安装，不能把以上组件测试当成已交付UI。
3. UI绑定root需要遵循启动的 `--extension-store`，不能写另一个默认store；目前runtime内部 `trust_root` 保存已启动路径但无只读getter，`runtime.h` 仅有PackageStoreMode等。正常无store启动也应能显式初始化新的默认store；开发模式/本机disabled记录与运行时启用列表需区分。
4. 保留剩余7-Zip联动/设置/DPI验收、官方26.00 DLL来源核验；公开发布前必须另获用户确认。

## 9. 应用安装协调器与商店入口（2026-11-09，目标第4轮）

### 本轮实现
- 新 `store/install_service.h/.cpp` 单worker安装状态机：Prepare立即新代，持有同一已验CatalogSnapshot，先检查当前host版本/SDK预览号兼容，再下载、审阅发布者、等待独立授信命令、重新审阅安装、等待InstallConsent、提交。双击/旧代/错阶段确认不会生效。UI拿的是不可变审阅副本，worker用自己的原件提交。Config由真实PULSE_VERSION_STRING_A与PULSE_SDK_PREVIEW传入；公开纯 `IsReleaseCompatible` 供UI禁用不兼容卡片，复用唯一版本parser。
- pending_restart会话内只OR，不因后续幂等、Cancel、Stop或旧代结果丢弃清掉已经真实commit的重启要求。授信成功后取消安装不撤销已同意的信任。所有磁盘/下载在worker且不持状态mutex；Stop不join，析构join只能在活跃窗口退出之后。
- 新 `extension_store_ui.h/.cpp` 由AppCore共享一个controller（非每窗口一套worker/cache），打开page8时lazy创建；runtime新增只读 `PackageStoreRoot()`，优先遵循已加载的--extension-store。无store/manifest且普通偏好持久化启动才选LOCALAPPDATA/Pulse/extensions；隔离偏好启动不悄悄fallback。**persist=false不是文件系统sandbox**，显式--extension-store仍然是可写目标，测试必须指定自己的fixture。
- 缓存在store同级extension-catalog.cache，网络metadata读取不初始化安装root；cache父目录不存在时还没有专门创建逻辑。last-window teardown发Stop，AppCore离开活跃窗口循环后才析构join。UI timer只拉取状态/触发重画，无裸owner worker回调。
- page8已接“扩展商店”入口，浏览/管理增强包两个tab；浏览用真实已验目录行，不制造社区扩展；loading/failed/offline、原生信任说明（明确原生不在沙箱中运行，不能暗示没有宿主授权就无法写文件/联网）、兼容性禁用、notice、pending-restart和刷新/取消。加载目录时也显示取消；取消安装不丢弃已经成功取得的目录。管理保留原本预览/AI/原生设置/高级下载源，page5快速预览不变，其“管理增强包”直接进管理tab。尚不是mockup的完整官方/社区分组、搜索/分类、已安装原生记录列表。
- 已接两个独立FormDialog：发布者指纹+root；安装root+新增resources+native/resources/新包enable的未勾选checkbox。无默认批准按钮；必选未勾时保持AwaitingInstall，重新显示校验提示并保留已勾草稿。不把信任、安装、启用和native执行合并同意。UI响应(含拒绝/关闭)用纯 `extension_store_review.h` 检查generation/phase后再调用服务。
- 窄审发现并修补：旧安装框取消曾裸Cancel影响新代；发起窗口关闭而其它窗口存活时曾无法继续审阅。现在旧代/错阶段回调忽略，WM_DESTROY用AppState身份+HWND清模态、取消该owner的活跃请求，避免悬空等待。已提交结果仍可能真实需要重启。Show失败亦比较generation才取消。

### 已有证据与当前限制
- 独立及主工程首轮安装协调器测试 **73 passed,0 failed**：CV gate而非sleep，正常/已信任仍native同意、旧代/双击/合并/晚异常、取消/Stop晚commit sticky、host/SDK先于网络及公开helper边界。包括隔离目录真实CNG/VerifyPackage/ReviewTrust/CommitTrust/ReviewInstall/CommitInstall链，只把下载替换为已验内存包，确有registry commit；不是公开网络GUI端到端。
- 首轮主工程 `pulse` 构建成功，无新增warning；第一版布局/命中 **398 cases,0 failures**，覆盖三语言、100/150%DPI、三宽度、125字体模型，官方管理入口/禁用点击/后项索引/page5隔离。它使用null Painter，不能代替实际文字测量和截图。
- 增量主构建成功：加载/失败不伪称“已验空目录”的布局回归后 **488 cases,0 failures**；旧拒绝/独立checkbox纯UI响应 **16 passed,0 failed**。记录分别 `integrate-store-r4.log` / `integrate-store-r4-final.log`。截图另发现重复失败提示和notice背景与管理卡片相接，已去重并增加表面间距回归；该增量主构建成功、布局 **578 cases,0 failures**，见 `integrate-store-r4-spacing.log`。随后本地化原生非沙箱提示增量见 `integrate-store-r4-warning.log`。
- GUI仅用--test-instance与workspace store-r2/hash fixture。最终浅色 `r4-shots-light-final/` 实际看到Loading→取消回Idle→刷新→Failed、正确的原生非沙箱说明、1600×960/1000×900浏览与管理布局；深色 `r4-shots-dark-targeted/` 看到Loading/Failed、宽窄浏览/管理与旧快速预览可达（此深色图早于最后非沙箱文案更正，最终文案已在浅色图复核）。当前新商店这些截图无文字裁切/重叠。旧page5窄窗长说明有压边现象，不能把“可达”说成其完整视觉通过，本轮没顺手改旧分支。
- 初次脚本的 `r4-shots-light/store-narrow.png` 实际还是管理tab，不当作窄浏览证明；两个dark-final尝试因前台保护在发送输入前中止（Pulse本身正常code=0），不计通过。最终改为PID核对后的定向窗口消息、临时置顶仅自身测试窗口、无全局键鼠输入的driver模式，捕获后只关闭所启动PID；两次targeted均正常退出code=0，不据此宣称真实前台/键盘/模态焦点通过。
- 尚未验正向签名目录下载/授信/安装对话框完整GUI链。早先PowerShell Start-Process在返回PID前卡住，停止的仅是两个driver；marker定位到启动API，改为UseShellExecute=false直接Process.Start后启动正常，未证明具体系统原因，未修ACL。最后 `build-store-r4-smoke.log` 确认只增量编译最终wrapper并链接pulse，所有截图driver均无用户偏好写入。
- 官方26.00 x64来源比对**已完成**：从官网链接的ip7z/7zip官方26.00 release下载x64 installer（1655627 B，sha256 `6fe18d5b3080e39678cabfa6cef12cfb25086377389b803a36a3c43236a8a82c`，与release API digest一致），未执行安装器；只读切出0xB400开始的内嵌7z，经系统tar提取单一7z.dll。成员1908736 B/AMD64/26.00，SHA256为既有 `bbd705e3…a443f`，并且与7zip、p1-archive、7zip-package-r2三份fc /b逐字节完全一致。`build-extensions/7zip-origin-check/evidence.md` / `verification.log`留证。官网当前标题是26.06，但用户此前明确选26.00，本轮没有偷换版本；没有独立Authenticode或最初获取历史的证明。
- 本轮未公开发布、push或修改用户偏好；外部公共贡献repo先前路径当前glob返回不存在，未重建/替换外部目录。发布前仍必须另获用户确认。

### 后续优先级
1. 浏览/取消/失败/管理入口的本轮定向GUI状态已留证，不再无因重跑。下一步是正向签名目录->授权表单->安装->重启验收（含真实键盘/模态焦点）；公共目录不可用时用明确隔离的合法签名测试输入，不能发布或伪造生产信任来让测试通过。
2. 补已安装原生registry记录（含disabled）的worker快照、启用/卸载/回滚与持久化状态，避免只看当前运行时enabled包；再补mockup官方/社区分组、搜索/分类与状态/DPI/窄宽视觉差异。当前新包默认disabled，需要显式勾启用，不能把native设置页的运行时列表当完整安装记录。
3. 完成7-Zip剩余交互/设置验收；官方26.00 DLL来源比对已经完成。当前生成host版本1.0.59，而原准备条目minPulse=1.0.60，正向目录安装/正式版本需先对齐，不降低minPulse绕过兼容性。默认Temp目录限制仍需实际诊断，未作ACL修复。

## 10. 真机 UI 验收（2026-11-09，side session）

驱动方式：本机没有 computer-use 工具，改用自写 Win32 驱动（`SendInput`/`mouse_event` 真实输入 + 屏幕截图，只操作自己启动的 `--test-instance` PID，失焦即拒绝输入）。截图在 `build-extensions/7zip-live/ui-test/shots/`。测试实例必须带 `PULSE_TEST_DATA_DIR`（否则扩展设置读写一律失败，界面全灰）。只验了浅色 150% DPI 的大部分界面；深色只做了效果页，没有逐项在真机深色里截图。

### 已验通过（真机）
- 右键菜单：压缩包显示 6 项，普通文件只显示 快速压缩/添加到压缩包…/计算校验值。
- 加密 7z：密码框 → 错误密码重试提示 → 正确密码 → 写入确认 → 文件落地；Esc 取消后宿主恢复。会话密码记忆：第二次解压不再询问。
- 多顶层 zip 智能解压包进 `多个顶层项\`；tar.gz 直接出文件（修复后，见下）。
- 快速压缩无弹窗；添加到压缩包… 7z→zip→tar 联动（扩展名、固实/加密文件名隐藏、ZIP 加密方式出现、tar 禁用等级）；真实 tar 输出用 `tar -tvf` 核对。
- 校验值：CRC32/SHA-1/SHA-256 与 Python/Get-FileHash 一致；“复制并关闭”写入 `sha256 *hello.txt`（测试前后已保存/恢复剪贴板）。测试压缩包完成后只在状态栏显示“已完成”。
- 写入确认“始终允许，不再询问”：选后同一实例内后续解压不再询问。测试实例不持久化偏好，重启后会再问（预期）。
- 商店页：在线拉取公开目录、列出 7-Zip。扩展设置页：改动即保存（`extension-settings/*.json` 已核对），右键标题随设置变化。

### 本轮修复
- 文本框外多画了一圈键盘焦点环（与 Fluent 底部强调线重复）：`extension_tree.cpp` 对 TextInput/CommandSearch 不再设 keyboard_focus。
- 表单对话框：表单区比消息/按钮缩进 8px、下方留空。`form_dialog.cpp` 让树视图外扩其内边距对齐，并按内容高度自适应（请求高度变为上限，状态变化后重新适配）；`ExtensionTreeView::ContentHeight()`、`kExtensionTreeContentInset`。
- 左对齐文本在 32 DIP 控件高度框里改为垂直居中，网格里的标签与控件对齐（`extension_tree_controls.cpp`）。
- 扩展下拉框每项前多一个圆点（选项被画成 radio）：`extension_tree.cpp` OpenSelect 去掉 radio。
- 进度窗口在两个模态框（密码框→写入确认）之间的空档弹出并压在模态框下：`app_ops_ui.cpp` 新增 `operationAutoShowFrom`，模态期间重置自动弹出计时。
- 写入确认里的文件夹显示为文件图标：`staging_adapter_host.cpp` 按属性给 item_glyphs。
- 商店：7-Zip 被归到“社区增强包”，且已安装仍显示“下载并安装”。现在官方发布者归“Pulse 官方”，社区区为空时显示“还没有社区扩展”；已装且清单哈希等于目录最新版时按钮为禁用的“已安装”，不同则“更新”。顺带修了目录里无版本条目会让卡片索引错位的问题。新文案 6433–6435。
- 7-Zip 扩展（需重新打包，**尚未打包/签名/发布**）：
  - tar.gz/嵌套解压的临时目录被拒（本机火绒 HIPS 禁止桌面目录下的 exe 写 %TEMP%/%LOCALAPPDATA%，PowerShell 不受限）时回退到压缩包旁的隐藏 `.PulseZip-*` 目录，用完删除；引擎测试新增 2 项（26/26 通过）。
  - 密码框按 Esc 不再当作失败弹红色“操作失败”窗。
  - 设置改变后只重注册改名的命令会让它们跑到菜单末尾；改为整组按序重注册。
  - 添加到压缩包：空的错误说明不再占行（新增 `haserror`），请求高度改为上限 480，密码框请求 40。

### 未解决 / 限制
- 启动后 10–40 秒内扩展设置才读到（之前菜单标题和快速压缩格式按默认值），根因未定位（疑似宿主 I/O worker 启动期排队）。
- 商店没有详情抽屉、没有官方/社区分组以外的 mockup 功能；“获取→下载→验证→安装→重启提示”的空商店全流程本轮未跑。
- 写入确认标题被用作撤销项名称（“撤销确认增强包生成的文件 · …”），状态栏撤销显示“复制”。
- 校验值框数值未对齐；测试压缩包成功只有状态栏提示。
- `pulse_extension_tree_test` 在 `src/bench/extension_tree_test.cpp:888` 有重定义编译错误（与本轮无关，未修）；`pulse_extension_tree_catalog_test` 通过。
- 用户要求重做压缩/解压界面和扩展设置页：效果页 `archive_ui_mockup.html`（待确认后实现）。RAR 只能解压不能创建（7-Zip 不含 RAR 编码器）。

### 10.1 压缩/解压界面重设计（按 `archive_ui_mockup.html` 实现，2026-11-09）
- 宿主新增（扩展 UI 模型，schema 与校验同步）：`slider.options`（每档下方文字，数量须等于 stops）、`group.bordered`（卡片分组，内边距 16/12 DIP）、`stack.alignment: "center"`（横向行垂直居中）、`textInput.badgeText` 支持 `state.<key>`（尾部标签随状态变化）；文本节点不再被撑到 32 DIP 控件高度；无动作的信息条不进 Tab 顺序（否则重试密码框的初始焦点落在信息条上）。`schemas/extensions/ui-model.schema.json` 补上早先缺失的 `kind`。
- 原生对话框适配器原先在启动时冻结主题，从不更新：深色下对话框是浅色框 + 看不见的表单。现在每次 Show 读取窗口当前深浅色和强调色（`dialogs_adapter.cpp`）。
- 7-Zip：添加到压缩包（来源卡片、扩展名尾标、分段格式 + 一句话说明、6 档等级滑块、密码开关卡片、更多选项卡片）、输入密码（文件名 + 错误条 + 会话记忆说明）、校验值（按文件分卡、行内值、粘贴核对自动比对）、扩展设置（概览卡 + 解压/压缩/密码三组设置卡，改动即保存）。单个扩展设置页时隐藏宿主的“选择增强包”下拉。
- 真机已验（浅色 150% DPI；添加框和设置页另验深色）：格式/密码联动与不一致提示、错误密码重试后输入、校验值核对一致、设置页布局。未验：繁体/英文文案、窄宽度、其他 DPI、显示密码按钮（效果页有，宿主未做）、校验值多文件超过 8 个只列前 8 个。
- 未重新打包/签名/发布；`pulse_extension_tree_test` 仍是既有编译错误，`pulse_extension_tree_catalog_test` 通过。

### 10.2 7-Zip 1.0.1 发布（2026-11-09）
- 已在 `jimmgreen/pulse-extensions` 创建 Release `pulse.7zip-1.0.1`（非 latest；包 1,088,780 B sha256 `7e372137…d822`，签名 934 B，manifestSha256 `dc2c2e6e…e426`），`extensions/pulse.7zip/extension.json` 已加 1.0.1（未提交），`catalog.py validate --download --only pulse.7zip` 通过。
- **目录签名并上传被权限拦截，尚未执行**：`python tools/catalog.py publish --private-key %LOCALAPPDATA%\Pulse\signing\official\publisher.private.blob`。执行前公共目录仍只有 1.0.0；之后提交并推送 extension.json。
