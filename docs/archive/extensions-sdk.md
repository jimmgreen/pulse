> 文档定位：这是声明式 JSON 元数据参考，不是原生 C++ SDK 能力清单。开发增强包从 [C++ 作者快速开始](../extensions-sdk-quickstart.md) 开始；已完成与未完成状态只维护在 [主路线图](../Pulse-增强包开放能力与AI协同实施方案-2026-11-02.md)。

# 声明式 JSON 元数据参考（API v1）

本参考中的声明式包使用 UTF-8 JSON 清单；可执行增强包采用另行说明的原生 DLL 契约。Pulse 提供命令面板、原生无模式工具窗口和固定右侧面板插槽。作者可使用旧版按钮/字段面板，或通过 `panel.ui` 组合有界组件树、输入控件及本地状态，具体见[组件树指南](extensions-ui-tree.md)。可显式加载单包，也可使用显式指定的本地开发存储管理多个包；不运行第三方脚本。

本指南按创建、检查、预览、加载和诊断组织。[维护指南](extensions-maintainer.md)记录接口扩展要求、实现来源和历史验证证据；[实施方案](../Pulse-增强包开放能力与AI协同实施方案-2026-11-02.md)区分目标与当前范围。

当前开发树包含19项命令能力、10项资源操作和20份模板；固定SDK支持范围以包内发现与归档旁验收报告为准。这里列出的标签／窗格命令、宿主持久诊断和运行中本地签名信任撤销属于声明式宿主契约；分别检测 `workspaceNavigation`、`persistentHostDiagnostics`、`runtimeTrustRevocation`。接口语义见[标签与窗格](extensions-workspace.md)、[诊断](extensions-diagnostics.md)、[来源信任](extensions-signing.md)，实际验证与限制见[交付记录](extensions-platform-delivery.md)。

## 选择能实现的功能

| 需求 | 当前支持 | 边界 |
| --- | --- | --- |
| 导航与刷新 | 后退、前进、上级、刷新四种命令 | 使用宿主已有状态，不接收路径或参数 |
| 视图切换 | `view.setMode`，八种既有视图 | 保存文件夹偏好；停用不还原视图 |
| 当前目录选择与排序 | `selection.selectAll`、`selection.invert`、`selection.clear`、`view.setSort` | 只操作聚焦窗格已加载的普通目录；选择复用可见过滤集合；排序可能保存目录偏好，停用不回滚。发现 `selectionAndSorting` 后使用，详见[接口说明](extensions-focused-directory.md) |
| 工作区分栏 | `workspace.setLayout`，五种宿主分栏 | 保留已有窗格/标签，布局/会话可能保存；不等于自由工作区配置 |
| UI | 旧版分组按钮/字段面板，或 `panel.ui` 的41类节点、嵌套布局、本地状态、虚拟集合与动态数据源；视觉和交互复用宿主控件 | 两种面板形式互斥；树最多128节点/8层；不等于自由停靠或任意替换宿主界面，完整系统IME/第三方屏幕阅读器人工矩阵仍待验收 |
| 上下文 | 当前目录、选择数量、单选名称、当前视图、本包传输状态五种只读绑定 | 旧版面板最多3字段，组件树可在节点预算内绑定；视图/选择跟随活跃窗格，任务状态按包与窗口隔离 |
| 开发 | 真实宿主发现/校验、模板、静态预览、契约检查、隔离开发窗口 | test/preview 不执行命令，开发窗口的重载效果需实际验收 |
| 本地归档与诊断 | pack/verify 哈希归档、doctor 脱敏诊断 | 哈希不是签名或来源可信证明 |
| 本地安装管理 | 显式 store、版本安装/回退/卸载与持久启停，最多 32 包；可选固定发布者签名校验 | 本地信任不是生产PKI、运行中即时撤销、市场或自动更新系统 |
| 执行 | 单包加载或已启用包的启动快照、每包会话启停；双栏普通文件复制／移动 | 不开放脚本；文件操作必须逐次原生确认；存储变化需重新启动加载 |

双栏文件操作只使用下文列出的两个无参数能力。不要猜测路径参数或 `userConfirmed` 参数；安装、启用与新增能力确认均不能代替一次具体文件操作的原生确认。开发预览的实际验证结果见[维护指南](extensions-maintainer.md)。

SDK 解压版可直接使用根目录 `pulse-ext.cmd` 和随包预览器，完整操作见[解压版快速开始](../extensions-sdk-quickstart.md)。下文涉及 `build` 与仓库路径的示例用于源码开发方式；两者共用相同工具和契约。

选择与排序参数：`view.setSort` 的列为 name、modified、created、accessed、type、size、path，方向为 ascending 或 descending；三种 selection 能力禁止 args。`selection-sort-panel` 提供完整排序矩阵与顺序工作流示例。AI/MCP、助手和CLI共用发现契约，作者应检测 `selectionAndSorting`、`sortColumns`、`sortDirections`；发现、模板返回、test和preview本身不执行命令或授予权限。实际执行仍要求包声明与启用、有效caller/owner代际及宿主当前状态校验；新增能力升级仍需显式接受，内部action ID不是授权入口。

## 完成第一个包

需要 Python 3.8+、包含开发入口的 `pulse.exe`。源码方式的静态预览另需构建 `pulse_extension_preview` 目标；SDK 解压版已附带预览器。下面从仓库根目录执行；`my-view-extension` 和 `my-view-preview` 必须尚不存在。

```powershell
$pulseExe = (Resolve-Path .\build\pulse.exe).Path
$extensionCli = '.\tools\extensions\pulse_ext.py'
python $extensionCli --pulse $pulseExe --json inspect-sdk
if ($LASTEXITCODE -ne 0) { throw 'API discovery failed' }
python $extensionCli --pulse $pulseExe init .\my-view-extension --id my.view --name '我的视图面板' --template view-panel
if ($LASTEXITCODE -ne 0) { throw 'Initialization failed' }
$manifest = (Resolve-Path .\my-view-extension\manifest.json).Path
# 按需编辑 manifest.json 后，检查最终文件。
python $extensionCli --pulse $pulseExe --json validate $manifest
if ($LASTEXITCODE -ne 0) { throw 'Validation failed' }
python $extensionCli --pulse $pulseExe --json preview $manifest --output .\my-view-preview --state selected
if ($LASTEXITCODE -ne 0) { throw 'Preview failed' }
# 查看报告和六张 PNG 后，显式打开正常交互窗口。
& $pulseExe --extension-manifest $manifest
```

命令面板快捷键为 **Ctrl+Shift+K**。按 `panel.title` 搜索打开面板，或按命令标题搜索执行命令。默认以无模式工具窗口显示；明确请求侧栏并满足空间条件时放入右侧工作区。打开后仍可操作文件列表。

`examples/extensions/navigation`、`navigation-panel`、`selection-panel` 和 `view-panel` 自带 `run.cmd` / `run.ps1`，可直接体验；脚本先校验再打开窗口，不是安装程序。`workspace-panel` 与 `dock-panel` 目录只有示例清单，没有启动脚本，请按下文使用对应 `init` 模板，再通过显式清单加载、`dev` 或安装后的 `run` 流程打开。

## 加载、停用与实际验收

`--extension-manifest` 接受绝对清单路径并在独立窗口加载一个包；`--extension-store` 显式从本地存储加载启用包的启动快照。普通启动默认不扫描或加载包。命令面板提供每包会话启用/停用：停用使该包命令不可执行并关闭其工具窗口；启用后可重新打开。会话状态不写回存储注册表。

存储中的持久启用状态与窗口内状态不同：CLI `enable` 后要重新启动存储窗口才会加载该包；已加载包在窗口内停用不会改动下一次启动使用的注册表。安装、升级、回退和卸载不热改已打开窗口。

不同包可有相同命令 ID 或标题，宿主按包身份及清单代际区分执行归属。“更多”菜单按“动作 · 包名 [id]”显示，优先保留动作可读性；长身份可明确省略，完整文字保留在提示中。选择命令时核对所属包，不要把名称相同当成同一命令。同时仅打开一个包的工具面板，切换包会关闭旧面板及其回调。

视图切换沿用 Pulse 普通入口，会保存文件夹视图偏好。停用只阻止后续调用，不还原视图或已保存偏好。验证时使用隔离配置及测试文件夹。

校验通过后，还需检查实际命令效果、面板入口、停用/重新启用及使用到的绑定。静态 PNG 只能检查模拟内容的外观；无法证明导航、计时刷新、快捷键、跨标签页和生命周期正确。已有应用内测试及隔离配置方式见[维护与验证记录](extensions-maintainer.md)，固定示例断言不能直接作为任意新包的通过证据。

## 本地开发 CLI

命令及边界见下表。需要 **Python 3.8+** 与包含开发入口的 Pulse 构建；预览还需要 `pulse_extension_preview` 目标生成的 `Pulse.ExtensionPreview.exe`。工具仅使用 Python 标准库，不需要安装第三方 Python 包。入口为 `tools/extensions/pulse_ext.py`，Windows 包装器为同目录的 `pulse-ext.cmd`。

全局参数 `--pulse EXE` 显式选择用于发现和校验的宿主，默认使用本仓库 `build/pulse.exe`；`--json` 请求机器可读输出。把全局参数放在子命令前。不同宿主可能支持不同特性，生成和校验时应使用同一宿主。请显式选择包含所需入口的构建。兼容保护的最低前提是宿主已支持 `--extension-check`；更老的未知版本不承诺兼容，可能无法识别参数或超时，应更新构建，不要拿旧宿主试运行作为兼容性测试。

在仓库根目录的 PowerShell 中执行：

```powershell
$pulseExe = (Resolve-Path .\build\pulse.exe).Path
$extensionCli = '.\tools\extensions\pulse_ext.py'

# 发现真实宿主支持的契约。
python $extensionCli --pulse $pulseExe --json inspect-sdk

# 验证现有文件；不加载或执行增强包。
python $extensionCli --pulse $pulseExe --json validate .\examples\extensions\view-panel\manifest.json

# 目标必须尚不存在，包括不能是一个已有空目录。
python $extensionCli --pulse $pulseExe init .\my-view-extension --id my.view --name '我的视图面板' --template view-panel
```

也可使用包装器，例如 `& .\tools\extensions\pulse-ext.cmd --pulse $pulseExe --json inspect-sdk`。

| 命令 | 当前行为 |
| --- | --- |
| `inspect-sdk` | 从所选宿主的同一份 `DescribeExtensionApi` 获取契约，不维护另一份能力清单。 |
| `validate PATH` | 使用真实宿主的 `ParseManifest` 检查文件，给出机器可读诊断，不安装、不加载、不执行。 |
| `init DEST --id ID --name NAME [--template TEMPLATE]` | 可选模板为 `navigation`、`navigation-panel`、`selection-panel`、`view-panel`、`workspace-panel`、`dock-panel`、`grouped-panel`、`workspace-layout`、`transfer-panel`，默认 `view-panel`。先由真实宿主验证生成清单，再创建目标目录中的 `manifest.json` 和 `README.md`；已有目标目录一律拒绝。 |
| `preview PATH --output NEWDIR [--state selected\|empty\|loading]` | 只支持带 `panel` 的清单；真实宿主先校验，再由独立原生预览程序解析清单快照，以模拟数据生成 6 张 PNG 和 `report.json`。默认状态为 `selected`，已有输出目录即使为空也拒绝。 |
| `test PATH` | 真实宿主校验原文件及快照，并核对发现能力、模式、面板引用和绑定。不会执行命令，不是功能测试。 |
| `pack PATH --output NEWFILE` | 校验快照后生成本地 ZIP，仅含 `manifest.json` 与 `package.json`，校验 SHA-256 并拒绝覆盖。不是安装包。 |
| `doctor` | 检查 Python、宿主发现契约及预览程序是否存在；不运行预览，不输出本地路径或环境变量。 |
| `verify PATH` | 检查本地归档的成员、大小、元数据和哈希，再由真实宿主校验临时清单；不安装、不解压到用户目标目录。 |
| `dev PATH --profile NEWDIR` | 校验后检查宿主声明 `isolatedDevReload`，在全新隔离配置中打开开发窗口，前台等待该窗口进程退出。 |
| `install ARCHIVE --store DIR [--accept-capabilities]` | 验证归档及真实宿主清单后安装到显式本地存储；新包默认停用，升级新增能力需显式接受。 |
| `list --store DIR` | 列出存储注册表中的包身份、启用状态与版本。 |
| `enable ID --store DIR` / `disable ID --store DIR` | 更改持久启用状态；不热改已打开窗口。 |
| `rollback ID --store DIR` | 将活动版本与上一个版本交换，恢复旧版本名称；不热改当前会话。 |
| `uninstall ID --store DIR` | 移除注册表引用，保留不可变版本文件；不是清理整个存储目录。 |
| `run --store DIR [--profile NEWDIR]` | 验证已启用包后启动独立存储窗口，立即返回进程信息；`--profile` 可选且必须全新。 |

`init` 是本地文件生成，不是安装。生成后阅读 README，编辑清单，再运行 `validate`。通过后需显式打开单包或选择本地存储安装流程，并进行应用内功能和截图验收。`pack` 本身不安装，归档仍没有发布者签名或自动更新功能。

### 检查契约并保存本地归档

```powershell
python $extensionCli --pulse $pulseExe --json test $manifest
if ($LASTEXITCODE -ne 0) { throw 'Contract test failed' }
python $extensionCli --pulse $pulseExe --json pack $manifest --output .\my-view-extension.zip
python $extensionCli --pulse $pulseExe --json verify .\my-view-extension.zip
python $extensionCli --pulse $pulseExe --json doctor
```

`test` 明确返回 `commandsExecuted:0` 和 `functionalExecutionTested:false`。`pack` 的目标文件必须不存在，父目录须可写；归档中的 `manifestSha256` 对应原始清单字节。不能把 ZIP 路径直接传给 `--extension-manifest`；该入口加载 UTF-8 清单。`hostInstallSupported:false` 表示原生入口不直接安装 ZIP，不否定 CLI `install` 消费该归档。本地安装是单独的显式操作，`pack` / `verify` 结果不意味着已经安装。

分享或保存后使用 `verify` 重新检查。归档总大小最多 256 KiB，先限量读取为不可变快照，再解析 ZIP；必须恰好包含 `manifest.json`、`package.json` 两个成员，分别限制为 65,536 与 4,096 字节，并进行有界解压、哈希和元数据检查。随后临时清单再经真实宿主验证。成功返回 `integrityVerified:true`、`hostValidated:true`、`installed:false`。这证明完整性及兼容性，不证明来源可信或命令行为正确。损坏、超限或结构不符返回 `invalid_archive`、退出 `2`。

### 在隔离开发窗口中修改清单

```powershell
python $extensionCli --pulse $pulseExe --json dev $manifest --profile .\my-view-dev-profile
```

`--profile` 必填且必须是新目录，不能复用已有目录。CLI 先校验清单，再确认 `inspect-sdk` 返回的 `developerFeatures` 包含 `isolatedDevReload`；缺少该特性返回 `dev_unsupported`，不会猜测旧宿主支持。

开发进程使用独立实例和该目录作为配置，清除继承的 `PULSE_*` 环境项后设置隔离数据目录。CLI 前台等待；正常关闭开发窗口后输出 JSON。Ctrl+C 仅终止本次启动的子进程，必要时在 5 秒后强制结束。保留配置和诊断目录供检查，不自动清理。

进程输出写入 `extension-dev-process.log`，重载状态写入 `extension-dev-status.json`，两者均位于新配置目录。CLI 返回 `isolated:true`、`reloadVerified:false`：进程正常退出并不证明发生过成功重载，要阅读状态并实际检查窗口。非用户中断的非零退出返回 `dev_process_failed`；不要把它隐藏成成功结果。

开发模式每 250 毫秒在后台有界读取完整清单，窗口每 100 毫秒领取解析结果，仅提交同一代的最新修订。替换前关闭旧工具面板及共享会话的命令弹出菜单，旧命令 ID 随版本失效，需重新打开查看新内容。无效 JSON、协议错误或读取失败保留上一个有效版本，并写入诊断。会话停用状态不会因编辑而自动启用。

开发清单和配置目录仅接受本地驱动器绝对路径，拒绝 UNC 和网络盘。原生开发入口要求独立测试实例和存在的绝对隔离配置目录，由 CLI 准备这些条件。此处“隔离”指配置与进程会话，不是不可信代码安全沙箱；仍只运行原有声明式能力，不提供脚本模块热替换或全平台订阅系统。

重载由最初的开发窗口持有；该窗口关闭后停止观察，不转移给后来打开的窗口。其他窗口保留最后一个有效版本。需要继续编辑预览时，重新启动一个使用新配置目录的开发会话。

状态文件提供 `schemaVersion`、`manifest`、`generation`、`revision`、`appliedRevision`、`status`、`enabled`、`error`。`pending` 表示等待应用，`active` 表示有效版本已应用，`invalid` / `read_failed` 表示本次修改未应用，`stopped` 表示观察已停止。结合修订号和实际 UI 判断是否应用了目标修改，不能只看旧版本仍可操作。

`doctor` 成功执行诊断不等于所有依赖齐备：预览程序缺失时，退出码仍可为 `0`，但 `doctor.healthy` 为 `false`。宿主不可达或协议不符时返回 `doctor_failed`、退出 `3`。报告仅包含版本、支持计数、可达性等诊断，不包含路径或环境变量。

## 安装、升级和管理多个包

这些命令的 `--store` 是子命令参数，必须每次显式提供；没有自动默认安装位置。选择本地驱动器上的绝对目录，拒绝 UNC、网络盘、符号链接和重解析点。注册表与版本目录由 CLI 维护，不手改。`install` 可创建存储目录，但其父目录必须已存在；其余管理命令要求存储已存在。当前最多注册 32 个包，每个包以清单 `id` 标识，版本是清单原始字节的 SHA-256，不是发布者签名或语义版本号。

沿用前面的 `$pulseExe` 和 `$extensionCli`，先用 `pack` 生成归档，再安装：

```powershell
$store = Join-Path (Get-Location) 'my-extension-store'
python $extensionCli --pulse $pulseExe --json install .\my-view-extension.zip --store $store
if ($LASTEXITCODE -ne 0) { throw 'Install failed' }
python $extensionCli --pulse $pulseExe --json list --store $store
python $extensionCli --pulse $pulseExe --json enable my.view --store $store
if ($LASTEXITCODE -ne 0) { throw 'Enable failed' }
# 新包默认停用；启用后才会在新窗口的启动快照中加载。
python $extensionCli --pulse $pulseExe --json run --store $store --profile .\my-store-profile
```

`run` 先确认宿主发现 `developerFeatures` 中的 `installedDeclarativePackages`，然后真实校验每个启用包，再启动独立进程。它立即返回 `launched:true`、`processId`、`enabledPackages`、`isolated`、`processLog` 和 `loadVerified:false`；成功启动不证明所有 UI 已完成加载或功能验收。指定 `--profile` 使用全新隔离配置目录；省略时使用普通用户偏好，应按正常交互窗口看待。启动会清除继承的 `PULSE_*` 开发环境项。

主程序入口为 `pulse.exe --extension-store ABSROOT`。它只在启动时读取快照，不监视存储变化。先严格验证整个注册表，再读取启用包；任一启用版本的哈希、清单身份、名称或语义不匹配，整次启动失败，不保留“加载了一半”的会话。停用包不读取版本内容，也不进入当前运行时。

原生加载失败退出 `2`，标准错误以 `Pulse extension:` 开头说明原因，不弹出对话框。CLI `run` 将输出保存在返回的 `processLog`：指定 `--profile` 时为该目录中的 `extension-store-process.log`，否则为系统临时目录下唯一的 `pulse-extension-run-*.log`。启动失败也尽可能保留日志路径；未出现窗口时先读取日志。需要绕过包装器诊断时，可直接捕获一次原生输出：

```powershell
$store = (Resolve-Path $store).Path
$process = Start-Process -FilePath $pulseExe -ArgumentList @('--extension-store', ('"' + $store + '"')) -RedirectStandardError .\store-start-error.txt -PassThru -Wait
$process.ExitCode
Get-Content .\store-start-error.txt
```

若启动成功，该命令会等待窗口关闭；它使用普通用户偏好。测试需隔离时先选择 `run --profile`，不要将正常启动与静态校验混为一谈。

### 升级前确认新增能力

同一个包 ID 再次安装不同内容就是升级；相同内容重复安装不改变版本。升级保留该包原有持久启用状态，并保留上一个活动版本供回退。

工具比较新旧清单的 capability 字符串集合。若新增了能力，普通 `install` 会拒绝并列出新增项；先阅读清单和诊断，再决定是否显式接受。例如：

```powershell
# 仅在已查看并决定接受新增能力后执行。
python $extensionCli --pulse $pulseExe --json install .\my-view-extension-v2.zip --store $store --accept-capabilities
```

该确认不是发布者信任认证或操作系统权限沙箱。它只检查能力集合新增，不代表参数、标题、按钮映射等变化都经过人工审核；即使集合相同，也应审查实际改动。首次安装默认停用，不会因安装自动运行。

### 回退、停用、卸载和坏包恢复

```powershell
python $extensionCli --pulse $pulseExe --json disable my.view --store $store
python $extensionCli --pulse $pulseExe --json rollback my.view --store $store
python $extensionCli --pulse $pulseExe --json uninstall my.view --store $store
```

以上是三种独立操作示例，按需要选择，不必依次全部运行。回退交换活动/上一版本并恢复其名称，不是任意历史版本选择器。卸载只移除注册表引用，保留版本文件；它不会删除整个目录，也不会立即从已打开窗口移除已加载包。关闭并重新 `run` 后才使用新的存储状态。

若启用包损坏导致启动失败，可先 `disable` 或 `uninstall` 该包再启动其他包；这两个操作不读取损坏版本。存在有效上一版本时可 `rollback`，它验证上一版本而不要求损坏活动版本可读。`enable` 检查活动版本的哈希与身份；`enable` 和 `rollback` 都在真实宿主校验目标清单后才写注册表。若注册表本身结构损坏，则管理操作会拒绝；保留原始文件和诊断，从可信备份恢复或在新的显式存储中重新安装，不让 AI 猜测修改注册表来绕过检查。

存储管理使用操作系统非阻塞文件锁，句柄或进程退出会释放锁。`.registry.lock` 文件常驻不代表仍被锁定，不要为处理 `store_busy` 手工删除锁文件。路径检查和锁并不构成防恶意文件系统竞争的沙箱。

多包命令与面板仍受所属包的会话启用状态约束。检查 `list` 的包 ID 与命令面板中的归属，特别是多个包存在同名标题时；本地安装不会使内部动作 inventory 变成新的公开能力。

### 用模拟数据生成静态面板预览

开发环境先构建预览目标：

```powershell
cmake --build build --target pulse_extension_preview
```

全局 `--preview-host EXE` 可显式指定预览程序；默认在 `--pulse` 指定的程序所在目录寻找 `Pulse.ExtensionPreview.exe`。它与用于清单校验的主程序是不同程序。沿用前面的 PowerShell 变量：

```powershell
$previewExe = (Resolve-Path .\build\Pulse.ExtensionPreview.exe).Path
python $extensionCli --pulse $pulseExe --preview-host $previewExe --json preview .\examples\extensions\view-panel\manifest.json --output .\view-preview-selected --state selected
python $extensionCli --pulse $pulseExe --preview-host $previewExe --json preview .\examples\extensions\selection-panel\manifest.json --output .\selection-preview-empty --state empty
python $extensionCli --pulse $pulseExe --preview-host $previewExe --json preview .\examples\extensions\selection-panel\manifest.json --output .\selection-preview-loading --state loading
```

每次指定一个尚不存在的输出目录，重跑时选择新目录，不覆盖已有产物。每次输出固定六张原生 PNG，覆盖亮色/暗色与 100%/150%/200% 缩放，以及一份 `report.json`。工具不自动打开图片；应读取报告并逐张检查相关文字、空值、间距、截断和缩放效果。

图片命名为 `panel-light-100.png`、`panel-light-150.png`、`panel-light-200.png` 和对应的三个 `panel-dark-*.png`。原生结果包含 `ok`、`mockData:true`、`commandsExecuted:0`、`state`、`output`、`images`、`report`；`report.json` 还含 `mockValues`（目录、数量、名称、视图、加载状态、字段值与模拟来源说明）。`images` 先列三张亮色，再列三张暗色，路径均为绝对路径。

模拟目录固定为 `C:\PulsePreview\Sample`；`selected` 提供选中数量 `1` 和名称 `example.txt`，`empty` 与 `loading` 提供数量 `0`、空名称，名称按字段的 `empty` 或默认破折号显示。视图模式为本地化的“详细信息”。这些数据完全是模拟值，不读取实际目录或用户偏好。

预览按钮全部禁用，从不执行命令、不安装增强包、不加载真实用户上下文。它是离线静态渲染，不是可交互窗口或热加载，也不能证明导航、视图切换、停用和绑定刷新等实际功能正确。真实功能仍须另行在隔离测试配置或用户明确打开的 Pulse 窗口中验收。

### Python CLI 的 JSON 结果与退出码

`--json` 成功结果采用以下外壳，退出码为 `0`：

```text
inspect-sdk: {"ok":true,"command":"inspect-sdk","api":<宿主 API 描述>}
validate:    {"ok":true,"command":"validate","validation":<宿主校验结果>}
init:        {"ok":true,"command":"init","path":<目标绝对目录>,"manifest":<清单绝对路径>,"template":<模板名>}
preview:     {"ok":true,"command":"preview","preview":<原生预览完整结果>}
test:        {"ok":true,"command":"test","test":<契约检查结果>}
pack:        {"ok":true,"command":"pack","archive":<路径、包元数据与未安装声明>}
doctor:      {"ok":true,"command":"doctor","doctor":<健康状态与脱敏检查项>}
verify:      {"ok":true,"command":"verify","verification":<归档完整性与宿主验证结果>}
dev:         {"ok":true,"command":"dev","dev":<隔离路径、诊断路径、退出码和停止原因>}
install:     {"ok":true,"command":"install","installation":{"changed":<布尔>,"package":<注册包条目>}}
list:        {"ok":true,"command":"list","registry":{"formatVersion":1,"packages":[<注册包条目>]}}
enable/disable/rollback/uninstall: {"ok":true,"command":<操作>,"update":{"changed":<布尔>,"package":<条目>,"immutableVersionsRetained":<布尔>}}
run:         {"ok":true,"command":"run","run":{"launched":true,"processId":<进程ID>,"store":<目录>,"enabledPackages":<数量>,"isolated":<布尔>,"loadVerified":false,"processLog":<日志路径>}}
```

失败结果为 `{"ok":false,"command":<子命令>,"error":{"code":<代码>,"message":<说明>,...}}`，扩展诊断字段按错误类型提供。宿主拒绝清单时退出 `2`，`error.code` 保留宿主错误代码，`error.host` 保留完整的宿主诊断；不要只检查输出是否是合法 JSON。

| 退出码 | CLI 诊断 |
| --- | --- |
| `2` | 清单被宿主拒绝，或 `usage`、`destination_exists`、`write_failed`、`contract_mismatch`、`invalid_archive`。按具体诊断修正参数、内容或目标。 |
| `3` | `host_not_found`、`host_timeout`、`host_launch_failed`、`host_protocol_error`、`template_failed`、`io_error`、`pack_write_failed`、`doctor_failed`、`dev_unsupported`、`profile_failed`、`dev_launch_failed`、`dev_process_failed`。检查宿主构建、可访问路径或运行条件；这类失败不证明清单有效或无效。 |

预览先执行主宿主校验，校验失败沿用其诊断。预览程序不存在、超时、启动失败或响应协议不符时，分别返回 `preview_not_found`、`preview_timeout`、`preview_launch_failed`、`preview_protocol_error`，退出码为 `3`；预览调用超时为 60 秒。原生预览报告正常错误时，CLI 保留其错误代码、说明和退出码（`2` 或 `3`），并在 `error.preview` 中保留原诊断。

存储管理的 `invalid_store`、`store_not_found`、`store_busy`、`store_full`、`package_not_found`、`no_previous_revision`、`capability_confirmation_required` 退出 `2`；底层 `io_error`、`store_unsupported`、`store_launch_failed` 退出 `3`。`run` 立即返回，子进程输出写入返回的 `processLog`；必须查看日志及窗口是否成功加载，不能把 PID 当成验收结果。

CLI 的外壳和退出码属于 Python 工具；下面的原生接口直接输出宿主结果，二者不要混淆。

### 按诊断处理失败

| 现象 | 处理方式 |
| --- | --- |
| `usage` 或 AI 工具 `invalid_args` | 检查参数类型、必填参数、全局参数位置与 64 KiB 限额；不要把调用错误当成内容校验结果 |
| 清单无效 / `valid:false` | 按实际 `code`、`error` 修正清单；诊断不保证有行号 |
| `destination_exists` | 保留已有目录或文件，选择新的输出；空目录也不能用于 init/preview |
| `host_*` / `preview_*` | 确认选中了包含入口的构建及正确预览程序；旧未知宿主不保证兼容，不靠反复启动猜测支持 |
| `contract_mismatch` | 校验与发现不一致，保留诊断并检查宿主/SDK版本；不能绕过检查交付 |
| `pack_write_failed` | 检查父目录可写及文件系统是否支持原子硬链接发布；不覆盖已有归档 |
| `invalid_archive` | 检查是否为该工具生成的两成员归档，保留原文件并重新取得或重新打包；不跳过哈希或大小检查 |
| `dev_unsupported` / `profile_failed` / `dev_launch_failed` | 使用声明开发特性的构建及全新可写配置目录；不要把普通加载模式冒充隔离开发模式 |
| `dev_process_failed` | 查看返回的隔离状态文件和进程日志；CLI 不自动断言重载成功 |
| `capability_confirmation_required` | 阅读新增能力与清单改动，明确决定后才重新 install 并加 --accept-capabilities |
| `store_not_found` / `store_full` / `store_busy` | 核对显式目录、32 包上限和其他正在写入的管理操作；不要绕过锁或直接编辑注册表 |
| `invalid_store` / `package_not_found` / `no_previous_revision` | 核对存储是否已初始化、注册表完整性、包 ID 和可回退版本；run拒绝未初始化存储，保留原数据与诊断，不能猜测修补 |
| `store_unsupported` / `store_launch_failed` | 使用发现 installedDeclarativePackages 的宿主，核对可执行路径；未启动不等于包无效 |
| doctor 退出 0 但 `healthy:false` | 读取具体 checks；缺少预览程序不等于主宿主不可用 |
| 没看到命令或工具窗口 | 确认显式加载的是最终绝对路径、包已启用，再用 Ctrl+Shift+K 搜索标题；静态预览不会打开交互面板 |
| 请求 sidebar 却出现浮窗 | 确认发现 sidebarPanels，检查在保留现有面板后是否足以容纳 320 DIP + 8 DIP 间距和至少360 DIP文件区；窄窗回退正常，放宽后需关闭并重新打开尝试 |

`test`、静态 PNG、归档哈希和真实窗口交互证明不同事情。交付说明应分别报告，不将它们合并为“全部功能通过”。

### 原生机器接口

CLI 使用的原生入口也可用于开发工具集成：

- `pulse.exe --extension-api` 输出原始 API 描述 JSON。
- `pulse.exe --extension-validate-json <绝对清单路径>` 输出 `{ "valid": true/false, "error": "诊断文字", "code": "诊断代码", "path": "传入的清单文件路径" }`。成功时 `error`、`code` 都为空字符串；`path` 保留原传入路径，不是 JSON 成员位置，路径无效时也不伪造或规范化它。退出码 `0` 表示有效，`2` 表示失败，`3` 表示标准输出写入故障。

路径、参数和 JSON 错误只报告实际可确定的诊断，不虚构行号。协议校验通过仍不等于 UI 验收通过。

维护说明：发现和校验调用在末尾附加无路径的 `--extension-check` 保护；已支持该开关但没有新 API 的上一阶段宿主会因缺少路径退出，不打开 UI。新宿主接受精确的 `--extension-api --extension-check` 或 `--extension-validate-json ABSFILE --extension-check` 保护形式，同时保留不带保护的原始调用。交互 `dev` 不使用此保护串，而是在启动前检查 `isolatedDevReload` 特性。

### 给本地编码 AI 的可复制指令

```text
请使用 Pulse 的最小开发 CLI 创建一个视图面板增强包。
先确认 Python 3.8+ 和包含开发入口的新 Pulse 构建可用，显式使用 --pulse 指定该构建。
调用 python tools/extensions/pulse_ext.py --pulse <EXE> --json inspect-sdk，按真实返回的 Schema、能力、绑定和 viewModes 开发。
选择一个不存在的目标目录，调用 init <DEST> --id my.view --name "我的视图面板" --template view-panel。
不要预先创建目标目录；已有目录即使为空也不能覆盖，失败时保留其内容并选择新的目标。
按需求编辑生成的 manifest.json，再调用 --json validate <PATH>，按实际诊断修复并重新校验，不猜测错误行号。
若清单有 panel，确认 pulse_extension_preview 目标已构建，使用 --preview-host 指定独立预览程序。
调用 preview <PATH> --output <尚不存在的新目录> --state selected；需要空值或加载状态时分别用新的输出目录运行 empty、loading。
读取 report.json，检查六张明暗主题与不同缩放的PNG；这些仅使用模拟数据，按钮禁用，不执行任何包命令。
如果宿主不支持入口或超时，报告需要更新构建，不尝试用旧宿主反复验证。
需要交互开发时明确使用 dev <PATH> --profile <全新目录>，并先确认 isolatedDevReload；检查状态文件，不能凭退出码声称重载成功。
交付前可运行 test、pack、verify；说明test不执行命令、ZIP不是自动安装程序，doctor要同时检查healthy。
需要本地安装时，先确认用户选择的 --store，再安装归档；新包默认停用，不能擅自启用或启动。
升级新增capability被拒绝时，展示新增集合并让用户决定是否使用--accept-capabilities，不自动加此参数绕过确认。
存储运行优先用全新--profile验证；run的loadVerified:false不能描述为功能通过。区分持久enable/disable与窗口内会话开关。
坏包先保留诊断，按disable/rollback/uninstall恢复；不要直接编辑registry或把内容哈希称为发布者签名。
交付最终文件、各项真实结果和用户显式启动命令；不要自动安装，未授权时不要打开交互开发窗口。
启动步骤使用 --extension-manifest 的绝对清单路径。验收需在实际应用中检查按钮、绑定、停用/启用及相关主题和缩放截图。
区分协议通过、静态预览检查与实际功能通过；未运行的检查明确写为未验证，不引用历史示例的通过数字证明新包可用。
```

## 清单格式

完整示例位于 [`examples/extensions/navigation/manifest.json`](../../examples/extensions/navigation/manifest.json)，TypeScript 声明位于 [`sdk/typescript/index.d.ts`](../../sdk/typescript/index.d.ts)，编辑器用 Schema 位于 [`schemas/extensions/manifest.schema.json`](../../schemas/extensions/manifest.schema.json)。

| 字段 | 约束 |
| --- | --- |
| `apiVersion` | 必填，数值 `1` |
| `id` | 必填，增强包标识，1–64 个 ASCII 字母、数字、`.`、`_` 或 `-` |
| `name` | 必填，非空显示名称，解码后最多 128 个 UTF-8 字节 |
| `commands` | 必填，0–32 个命令；允许空数组 |
| `commands[].id` | 必填，包内唯一，字符与长度规则同包标识 |
| `commands[].title` | 必填，非空命令标题，解码后最多 128 个 UTF-8 字节 |
| `commands[].capability` | 必填，只能使用下表中的能力标识 |
| `commands[].args` | `view.setMode` 必填且仅含mode；`workspace.setLayout` 必填且仅含layout。其余四种导航/刷新能力禁止此字段，包括空对象 |
| `commands[].args.mode` | 必须为下述八种视图模式之一，不接受数字或其他字符串 |
| `commands[].args.layout` | 仅用于workspace.setLayout，必须是single、side_by_side、stacked、three、four之一 |
| `toolbar` | 可选对象，只允许 `commands`；贡献到工具栏“更多”菜单 |
| `toolbar.commands` | 1–8 个互不重复的本包命令 ID，按引用顺序显示；每个都必须已在根 `commands` 定义 |
| `panel` | 可选对象，只允许 `title`、`description`、`commands`、`fields`、`layout`、`density`、`placement`、`groups`、`primaryCommand`、`titleTone` |
| `panel.title` | 提供 `panel` 时必填，非空，解码后最多 128 个 UTF-8 字节；也用于命令面板的打开入口 |
| `panel.description` | 可选说明，解码后最多 512 个 UTF-8 字节 |
| `panel.commands` | 提供 `panel` 时必填，1–8 个互不重复的命令 ID，每个都必须引用根 `commands` 中已定义的命令 |
| `panel.fields` | 可选数组，0–3 个只读字段，`binding` 不得重复 |
| `panel.fields[].label` | 必填，1–128 个 UTF-8 字节的显示标签 |
| `panel.fields[].binding` | 必填，从 `context.directory`、`context.selectionCount`、`context.selectionName`、`context.viewMode`、`context.transferStatus` 五种中选择；一个面板仍最多 3 个字段 |
| `panel.fields[].empty` | 可选，0–128 个 UTF-8 字节的空值文字，不允许控制字符；未提供时显示破折号 |
| `panel.layout` | 可选，`auto`（默认）、`stack`、`grid` |
| `panel.density` | 可选，`comfortable`（默认）、`compact` |
| `panel.placement` | 可选，`window`（默认）或 `sidebar`；只接受这两个字符串，不接受 null、数字或未知值 |
| `panel.groups` | 可选；提供时为1–4组，必须把已有 `panel.commands` 精确分组：不遗漏、不重复、不引用面板外命令 |
| `panel.groups[].title` / `description` | title必填、非空、最多128个UTF-8字节；description可选、最多512个UTF-8字节；文字限制同面板标题/说明 |
| `panel.groups[].commands` | 必填非空命令ID数组，引用已有面板按钮；所有组加起来仍最多8个按钮 |
| `panel.primaryCommand` | 可选，必须引用已有 `panel.commands` 中的命令，为该按钮使用宿主Primary视觉样式 |
| `panel.titleTone` | 可选，`default`（默认）或 `accent`；由宿主解析颜色，高对比或强调色对比度不足时回退普通主题文字色 |

`panel` 没有自己的能力定义。例如，在已有 `back`、`forward`、`up`、`refresh` 命令的清单中可加入：

```json
"panel": {
  "title": "导航工具",
  "description": "使用按钮操作当前窗口的位置和视图。",
  "commands": ["back", "forward", "up", "refresh"]
}
```

该片段需要作为根对象成员合并到完整清单中。按钮按引用顺序排列，标题来自对应命令的 `title`。没有 `panel` 的旧包继续按原方式提供命令，无需修改；`apiVersion` 仍为 `1`。空的根 `commands` 数组仍合法，但不能配合要求有效命令引用的 `panel` 使用。

清单使用 UTF-8 JSON，整个文件不得超过 **64 KiB（65,536 字节）**。只允许上述字段；不要加入注释、尾逗号、`$schema`、版本描述或自定义元数据。若编辑器需要 Schema，请在编辑器配置中关联，而不是向清单加入 `$schema` 字段。不要重复 JSON 属性名。

JSON Schema 的字符串长度按字符计算，无法精确限制 UTF-8 字节数；也不能用本 Schema 精确判断根命令 ID 是否唯一或面板引用是否存在。宿主验证是最终依据。中文通常占多个 UTF-8 字节，不能把 128 字节理解为 128 个汉字。

| 能力标识 | 行为 |
| --- | --- |
| `navigation.back` | 使用当前窗口已有历史后退 |
| `navigation.forward` | 使用当前窗口已有历史前进 |
| `navigation.up` | 导航到上级位置 |
| `view.refresh` | 刷新当前视图 |
| `view.setMode` | 通过必填的 `args.mode` 切换当前文件夹视图，并保存文件夹视图偏好 |
| `workspace.setLayout` | 通过必填的 `args.layout` 切换调用窗口的宿主分栏，布局/会话可能保存，停用不还原 |

增强包不能自行传入路径或额外权限。两个有参能力分别只接受 `view.setMode` 的 `args.mode` 与 `workspace.setLayout` 的 `args.layout`，不能混用；其他六种能力不接受参数。能力映射采用宿主已有行为及其可用条件。

`args.mode` 允许值依次为 `extra_large_icons`、`large_icons`、`medium_icons`、`small_icons`、`list`、`details`、`tiles`、`content`。例如：

```json
{ "id": "details", "title": "详细信息", "capability": "view.setMode", "args": { "mode": "details" } }
```

API 版本仍为 `1`，通过功能发现判断增量支持：生成新包前检查 `describe_extension_api` 的 `capabilities` 是否包含 `view.setMode`、`bindings` 是否包含 `context.viewMode`，以及 `viewModes` 是否包含所需模式。新宿主继续接受旧包；旧宿主会拒绝未知能力或绑定，不能仅凭 `apiVersion: 1` 推断支持所有新增功能。

### 切换宿主工作区分栏

先发现 `capabilities` 中的 `workspace.setLayout`，再核对 `workspaceLayouts` 和对应详情的 `argsSchema`。可选值为 `single`（单栏）、`side_by_side`（左右双栏）、`stacked`（上下双栏）、`three`（三栏）、`four`（四栏），例如：

```json
{ "id": "split", "title": "左右双栏", "capability": "workspace.setLayout", "args": { "layout": "side_by_side" } }
```

这是调用窗口的工作区操作，不是面板内部的 `panel.layout`。它复用宿主已有窗格并保留其标签页和目录；收起的窗格仍保留，新增窗格从当前焦点目录初始化。设置页不允许切换为多栏，不能通过增强包绕过该宿主限制。

布局/会话可能随宿主正常机制保存，停用包不会自动恢复之前的布局。它不修改用户文件内容，也不授权任意布局或文件操作。可用 `workspace-layout` 模板创建五按钮示例；编辑后要实际检查已有窗格/标签/目录保留、焦点及设置页拒绝状态，静态面板预览无法证明工作区切换正确。

## 给命令增加入口和布局预设

`toolbar` 把已有命令加入工具栏的“更多”菜单，不会把按钮直接固定到主工具栏，也不创建侧栏。它与 `panel.commands` 分别选择本包命令，可以使用不同顺序；引用不存在的 ID、重复引用或超过 8 个都会被拒绝。

例如，在已有 `details` 和 `extra-large` 命令的清单根对象中加入：

```json
"toolbar": {
  "commands": ["details", "extra-large"]
}
```

按钮布局由 `panel.layout` 选择：`stack` 始终单列；`grid` 在宽度至少 420 个设备无关像素时使用双列，窄窗口回退一列；`auto` 仅在按钮超过 4 个且达到上述宽度时使用双列。`panel.density` 可选舒适的 `comfortable` 或更紧凑的 `compact`，后者缩小按钮和行间距而不改变字体。省略两个字段时保留原有布局；这不是自定义像素坐标或任意布局语言。静态预览与真实工具窗口使用同一套布局逻辑。

体验完整示例：

```powershell
python $extensionCli --pulse $pulseExe init .\my-workspace-extension --id my.workspace --name '我的工作区工具' --template workspace-panel
```

该模板包含八种视图命令、按“详细信息→超大图标”顺序贡献的“更多”菜单入口，以及 `stack` / `compact` 工具面板和当前视图字段。`workspace-panel` 是示例名，不意味着可替换整个 Pulse 工作区。

生成之前先检查 `describe_extension_api` 的 `developerFeatures` 包含 `toolbarContributions` / `panelLayoutPresets`，并核对 `panelLayouts`、`panelDensities` 与 `limits.toolbarCommands`。早期 API v1 宿主没有这些字段或特性，不能仅按版本号推断支持；新宿主继续接受未声明这些可选项的旧包。

### 把面板放入右侧工作区

先确认当前宿主的 `developerFeatures` 包含 `sidebarPanels`，且 `panelPlacements` 包含 `sidebar`。然后在 `panel` 中添加 `"placement": "sidebar"`，或创建示例：

```powershell
python $extensionCli --pulse $pulseExe init .\my-dock-extension --id my.dock --name '我的侧栏工具' --template dock-panel
```

打开示例窗口后，用 **Ctrl+Shift+K** 搜索清单的 `panel.title` 并打开；安装或加载清单本身不会强行展开侧栏。省略 `placement` 或使用 `window` 时继续显示原有浮窗，旧包无需修改。

侧栏是所属窗口右侧的真实子面板，为自己保留 **320 DIP 宽度与 8 DIP 间距**，不覆盖文件内容；宿主至少保留 **360 DIP 文件区**。原有左侧导航及详情面板偏好保持不变。只有满足空间条件才停靠；空间不足时使用独立工具窗口，这是预期回退而不是解析错误。侧栏宽度不由清单配置，320 DIP 下的网格预设按窄窗规则使用单列，内容需要时可滚动。

窗口缩窄导致回退后，即使重新放宽也不会自动重新停靠。若希望再次尝试侧栏，关闭该面板并从命令面板重新打开。多包仍同时只开一个包面板，切换包关闭旧面板。关闭、会话停用、有效热重载替换或所属窗口退出时，应释放侧栏占用区域，恢复文件工作区；这些是实际宿主验收事项，静态预览 PNG 无法证明它们正确。

`preview` 仍用于检查模拟面板内容，并不展示或验证完整工作区停靠。作者应在真实窗口检查较宽/较窄尺寸、详情面板共存、文件列表可操作、关闭后空间恢复和相关 DPI。固定右侧插槽不是任意停靠系统，不开放拖放停靠、左侧位置选择、任意分区或完整工作区替换。

### 按任务分组并强调主要操作

先发现 `groupedPanels`，再使用 `groups`、`primaryCommand` 或 `titleTone`；同时检查 `panelTitleTones` 和 `limits.panelGroups`。旧宿主缺少该特性时不要发送这三个字段；省略它们的旧包保持原有显示方式。

以下片段假定面板已有 `back`、`up`、`refresh` 三个按钮：

```json
"groups": [
  { "title": "浏览位置", "commands": ["back", "up"] },
  { "title": "更新内容", "description": "重新读取当前视图。", "commands": ["refresh"] }
],
"primaryCommand": "refresh",
"titleTone": "accent"
```

这些字段应合并到完整 `panel` 对象中。分组只组织已有面板按钮，不新增命令或权限。每个 `panel.commands` 必须恰好出现一次；分组可以改变视觉排列，Tab顺序跟随组和组内命令顺序，执行仍按对应命令身份路由，不会因重排执行另一个按钮。每组分别应用layout预设，整个面板按需滚动。

主按钮只复用Pulse的Primary视觉样式，不改变Enter执行当前焦点按钮的行为，也不自动执行。标题强调复用宿主主题语义；清单不能指定任意颜色、字体或CSS。高对比模式或强调色对比度不足时，标题回到普通文字色；titleTone是语义强调请求，不保证把任意用户强调色原样显示，不能把颜色差异作为理解操作的唯一线索。旧版分组组合仍最多4组、8按钮，不支持嵌套组；需要嵌套布局时改用独立的 `panel.ui` 契约，不把树节点写入 `groups`。

可从 `grouped-panel` 模板开始，目标目录必须尚不存在：

```powershell
python $extensionCli --pulse $pulseExe init .\my-grouped-extension --id my.grouped --name '我的分组工具' --template grouped-panel
```

示例把八种模式分为两组并改变显示顺序，`details`为主按钮，标题使用accent。编辑后分别检查明暗主题、高对比、窄浮窗/侧栏、长组说明、滚动以及Tab顺序与实际命令效果。静态预览仍禁用按钮，不代替键盘和路由验收。

## 只读上下文字段

`panel.fields` 中的每个对象只允许 `label`、`binding`、`empty`。例如，可向已有 `panel` 对象加入以下成员：

```json
"fields": [
  { "label": "当前目录", "binding": "context.directory" },
  { "label": "已选数量", "binding": "context.selectionCount" },
  { "label": "单选名称", "binding": "context.selectionName", "empty": "未单选普通项目" }
]
```

| 绑定 | 数据含义与边界 |
| --- | --- |
| `context.directory` | 打开工具窗口的宿主窗口中，活跃标签页、活跃窗格的当前路径。显示最多 2,048 个 UTF-16 代码单元，超长时省略。 |
| `context.selectionCount` | 当前已加载完成模型中的选中数量。加载中显示 `0`。 |
| `context.selectionName` | 仅在已加载完成的普通内存条目中恰好单选时显示名称，文件或文件夹均可。内容搜索、没有单选或加载中为空，显示 `empty` 或默认破折号。名称最多显示 256 个 UTF-16 代码单元，超长时省略。 |
| `context.viewMode` | 打开工具窗口的宿主窗口中，当前活跃标签页和窗格的视图模式名称，使用当前 UI 语言（不是 `details` 等机器枚举）。加载中仍保留模式名称，通过普通 Pulse 入口切换后也会更新。 |

`context.transferStatus` 显示本包在所属窗口的最近受控传输状态；首次使用需发现该绑定。具体归属、空状态和部分完成语义见文末说明。

路径和名称的显示上限按 UTF-16 代码单元计算，与清单文字的 UTF-8 字节限制不同。字段由宿主读取已有内存状态并只读展示，不读取文件内容，不把路径或名称发送给外部服务，也不新增命令能力。`empty` 仅替换空值，不会把选中数量 `0` 替换为空值文字。

已打开的工具窗口每 250 毫秒刷新只读信息，仅在信息变化时重绘；它不是逐事件即时通知 API。窗口关闭后释放定时器和回调。截图渲染不调用上下文刷新回调。未提供 `fields` 的面板与原有纯命令包继续按原方式工作；旧宿主可能拒绝新增字段，需使用包含该功能的新构建。

## 让 AI 编写增强包

### 先发现接口，再生成和校验

当前 Pulse MCP 服务和内置助手提供三个开发工具。它们描述接口、返回内置模板或校验传入文本，不安装、加载或执行增强包，不读取调用者提供的清单路径，也不扩大增强包权限。

| 工具 | 参数 | 用途 |
| --- | --- | --- |
| `describe_extension_api` | `{}`，不接受其他参数 | 保留能力/绑定列表、viewModes/workspaceLayouts、面板选项、限制、manifestSchema与说明，支持structuredCapabilityDetails时提供capabilityDetails/bindingDetails；支持declarativeTemplates时返回templates目录。当前为8种能力、4种绑定、8种文件视图和5种工作区分栏；以连接宿主为准。 |
| `validate_extension_manifest` | `{ "manifest": "完整 JSON 清单文本" }` | 在内存中校验清单，返回 `{ "valid": true/false, "error": "诊断文字" }`。`manifest` 是字符串，不是文件路径，也不是已经解析的对象。 |
| `get_extension_template` | `{ "template": "transfer-panel" }` | 先发现declarativeTemplates并从templates目录选ID。返回template、manifest文本、validated:true、executed:false；固定内置数据，不读取路径或执行。修改作者身份/功能后重新校验。 |

调用 `validate_extension_manifest` 时，用调用端的 JSON 序列化功能编码字符串，不要手工多层转义。工具参数错误与清单无效是两种结果：

- 参数不是对象、缺少 `manifest`、其值不是字符串、出现未知参数或输入超过 65,536 个 UTF-8 字节，属于工具的 `invalid_args` 错误。先修正调用参数或缩小清单，再重试。
- 参数正确，但清单内容不是合法 JSON 或不符合增强包协议，属于正常工具结果 `valid: false`，由 `error` 说明原因。修复清单后重新校验；不能把“调用成功”理解为“清单通过”。
- `valid: true` 时 `error` 为空字符串，只说明这份文本通过协议校验，不代表按钮、当前目录或 UI 已通过验收，也不会自动创建文件或打开窗口。

TypeScript 入口重新导出生成的 `ExtensionApiDescription`、`CapabilityDetail`、`BindingDetail`、`JsonValue` 和 `JsonSchema`，并提供 `ValidateExtensionManifestArgs`、`ExtensionManifestValidationResult`。新详情字段在类型中可选，兼容早期宿主；不能只因类型存在就认为连接宿主支持。它们只是类型定义，不提供 JavaScript 运行时或安装入口。

### 让 AI 判断参数与副作用

先检查 `developerFeatures` 是否包含 `structuredCapabilityDetails`，再读取新增详情。旧宿主仍可使用原有 `capabilities`、`bindings` 和 `manifestSchema`，缺少详情时不能把未知副作用默认为无副作用。

| 详情字段 | 含义 |
| --- | --- |
| `capabilityDetails[].id` / `summary` | 对应公开能力标识及摘要，不是新的执行工具 |
| `effect` / `target` | 描述作用与目标范围；AI 在生成前应检查是否符合用户意图 |
| `modifiesFiles` / `persistsPreferences` | 前者指用户文件内容是否改变，后者指是否直接修改可配置偏好；view.setMode保存文件夹视图偏好，workspace.setLayout的布局/会话可能保存。两者都不表示宿主历史、状态或缓存零写入 |
| `supportsCancellation` | 两个受控文件能力为true，指宿主任务可取消；其他为false。声明式命令尚无任务查询/取消参数，不要据此编造接口 |
| `argsRequired` / `argsSchema` | 六种无参能力为false/null；view.setMode与workspace.setLayout为true，分别提供完整参数Schema并要求合法mode/layout |
| `bindingDetails[].id` / `summary` / `displayOnly` | 描述只读显示绑定，当前 `displayOnly:true`；不是文件读取、外发或执行授权 |

后退、前进、上级导航和刷新改变当前导航/视图状态，不变更用户文件内容；普通导航仍可记录历史或最近位置。`persistsPreferences:false` 仅表示不直接修改可配置偏好，不承诺没有任何磁盘写入。视图切换同样不改用户文件内容，却会保存文件夹视图偏好；工作区分栏操作的布局/会话也可能保存，停用不还原。AI 应检查参数和这些差异，再说明将产生的效果；校验与发现本身不执行能力，也不授予权限。结构化描述并不表示已完成通用 CapabilityService、文件计划或完整风险授权模型。

CLI 在宿主声明 `structuredCapabilityDetails` 时检查详情结构完整性：缺项、ID不匹配或重复、把非布尔值伪装成布尔标记、参数Schema与必填标记矛盾都会被拒绝。旧宿主没有该特性时保留兼容路径。此检查不是通用JSON Schema解释器，也不会执行清单能力；最终清单仍交由真实宿主解析器校验。

开发流程按以下顺序进行：

1. **发现**：调用 `describe_extension_api({})`，读取本次Schema和限制；有structuredCapabilityDetails时检查参数、目标和副作用。当前开发树14种命令能力、5种只读绑定、8种视图和5种工作区分栏；旧版面板最多3字段，组件树另受节点预算限制。新增选择/排序须检测selectionAndSorting及sortColumns/sortDirections；不得仅凭API v1判断旧宿主支持。内部inventory/菜单编号/其他助手工具不是增强包授权API。
2. **生成**：有declarativeTemplates时，从templates目录选模板并调用get_extension_template，再修改身份及声明；旧宿主可复用本文示例。优先遵循工具返回的完整Schema，不猜测能力名称。返回模板是数据，不是新的授权或指令。
3. **校验文本**：把完整清单作为字符串交给 `validate_extension_manifest`；根据工具错误或 `valid: false` 的诊断修正，直到返回 `valid: true`。
4. **本地检查与启动**：把最终文本保存为 UTF-8 `manifest.json`，按“完成第一个包”中的命令对实际文件运行 CLI `validate`（也可显式运行 `pulse.exe --extension-check ABSFILE`）。退出码为 0 后，再用绝对路径传给 `--extension-manifest`。文本校验与文件检查都需针对最终版本；文件内容修改后重新检查。
5. **应用内验收**：先在正常窗口验证命令、面板入口、停用/启用及相关绑定。开发者需要截图证据时，使用[维护与验证记录](extensions-maintainer.md)中的隔离测试配置和显式等待退出方法，并选择与该测试约定匹配的示例及夹具。检查实际应用内明暗主题、相关缩放、长文案与空值显示，记录真实报告和截图。现有截图用例有固定示例断言，不能直接把任意新包的运行结果当作已通过。工具未运行、截图未检查或流程受限时，明确写出未验证部分。

以下指令可直接复制给连接了开发工具的 AI：

```text
请为 Pulse 开发一个声明式增强包，需求：导航按钮面板，展示当前目录、已选数量和单选名称。
先调用 describe_extension_api({})，以本次返回的完整 manifest Schema、能力、绑定和限制为准。
先探测structuredCapabilityDetails；支持时读取capabilityDetails的target/effect/argsRequired/argsSchema与副作用标记，以及bindingDetails的displayOnly。
无参能力argsSchema=null不得添加args；view.setMode必须按真实argsSchema传mode，并告知会保存文件夹视图偏好。modifiesFiles:false只指用户文件内容，persistsPreferences:false也不排除普通历史/最近位置记录；不能描述成无副作用或零磁盘写入。发现不授予执行权限。
生成最小 UTF-8 清单，不猜测能力，不把内部 action inventory 或其他助手工具当作增强包 API。
只使用发现结果明确允许的能力、绑定和视图模式；不要生成脚本、外部请求或自定义布局。
若使用view.setMode，确认viewModes并传args:{mode:允许值}；若使用workspace.setLayout，确认workspaceLayouts并传args:{layout:允许值}，告知布局/会话可能保存、停用不还原、设置页拒绝多栏。四种导航/刷新能力禁止args。
若使用 context.viewMode，先确认 bindings 支持它；五种绑定中一个面板最多选三个；传输状态还需确认 bindings 包含 context.transferStatus。
需要更多菜单入口或面板预设时，分别确认toolbarContributions与panelLayoutPresets，按panelLayouts/panelDensities生成；不得把toolbar描述为直接固定按钮或侧栏。
需要右侧面板时，先确认sidebarPanels及panelPlacements支持sidebar，再设置panel.placement；说明320DIP侧栏、8DIP间距和至少360DIP文件区，不够时回退浮窗。不能承诺任意停靠或自动重新停靠。
需要旧版分组时先发现groupedPanels；groups为1至4组，精确覆盖panel.commands一次且总按钮不超过8。primaryCommand仅引用已有面板按钮，titleTone只能选发现的default/accent。Tab按组顺序，语义色由宿主控制，高对比或低对比度时允许回退，不保证任意用户色原样显示。需要组件树时先发现 `declarativeUiTree`，从 `uiComponents` 取得合法节点和属性，使用 `ui-tree` 模板；不能猜测不存在的组件或混用旧面板字段。
调用 validate_extension_manifest，把完整清单序列化为 manifest 字符串参数。
工具 invalid_args 时先修正参数；普通结果 valid:false 时按 error 修正清单，然后重试。
valid:true 后交付最终 manifest.json、用户本地 --extension-check 和 --extension-manifest 命令，以及应用内验收步骤。
不要声称校验会安装、加载或执行清单；未经实际运行和截图检查，不要声称功能或外观验证通过。
```

如果当前连接未提供发现与校验工具，可把本说明、Schema 和示例交给 AI，并使用本地 `--extension-check` 完成协议检查。离线编写可使用以下提示：

> 为 Pulse API v1 生成一个 UTF-8 manifest.json。根对象只能包含 apiVersion、id、name、commands；命令包含 id、title、capability，view.setMode必须额外包含args:{mode:允许值}，workspace.setLayout必须包含args:{layout:允许值}，四种导航/刷新能力禁止args。apiVersion 必须是 1。包和命令 ID 只能用 ASCII 字母数字及 ._-，长度 1–64，命令 ID 包内唯一。name 和 title 非空且最多 128 个 UTF-8 字节。最多 32 个命令，整个文件最多 65,536 字节。能力只能选navigation.back、navigation.forward、navigation.up、view.refresh、view.setMode、workspace.setLayout；layout只能选single、side_by_side、stacked、three、four。mode 只能选 extra_large_icons、large_icons、medium_icons、small_icons、list、details、tiles、content。使用中文显示名称。不要输出脚本、未支持字段或虚构能力。输出后说明如何用 --extension-check 检查，再用 --extension-manifest 加载；旧宿主不一定支持新增能力。

对 AI 生成的文件仍需运行宿主检查，再实际验证命令效果、停用后不可执行以及重新启用。仅修改名称或映射不能实现新的底层能力。

如果需要按钮面板，可在上述提示后追加：

> 本次允许根对象额外包含可选 panel。生成一个宿主面板：panel 可包含 title、description、commands、fields、layout、density、placement、groups、primaryCommand、titleTone；title 必填、非空、最多128个UTF-8字节，description可选且最多512字节，commands必须包含1–8个互不重复且已在根commands中定义的ID。layout只能是auto/stack/grid，density只能是comfortable/compact，生成前先发现panelLayoutPresets。placement默认window，仅发现sidebarPanels时才选sidebar。仅发现groupedPanels时才添加groups/primaryCommand/titleTone；组必须精确分区已有面板命令。不要生成HTML、CSS、任意布局/拖放停靠、脚本或新能力。参考navigation-panel、dock-panel或grouped-panel示例，检查实际键盘顺序/执行、窄窗回退、关闭/停用恢复及重新启用。

需要“更多”菜单入口时追加：

> 先发现 toolbarContributions，根对象增加 toolbar:{commands:[本包命令ID]}，仅引用已有命令，1–8项且唯一，按所需菜单顺序排列。它只贡献到工具栏“更多”菜单，不是直接固定工具栏按钮。参考 workspace-panel 模板，并用真实窗口检查归属、顺序和停用拒绝执行；静态面板预览不等于菜单验收。

如果需要选择信息，可继续追加：

> 为 panel 添加 fields 数组，最多 3 项，每项只能有 label、binding 和可选 empty。label 为 1–128 个 UTF-8 字节；empty 为 0–128 个 UTF-8 字节且不含控制字符。binding 只能选 context.directory、context.selectionCount、context.selectionName、context.viewMode，且不能重复。这些字段仅展示宿主内存中的上下文，不读取文件内容，不发送外部数据。单选名称在内容搜索、加载中或没有单选时为空。参考 examples/extensions/selection-panel/manifest.json 或 examples/extensions/view-panel/manifest.json，说明如何观察打开该面板的窗口切换活跃标签页、选择变化和空值状态；不要虚构跨窗口跟随或实时事件接口。使用视图切换时须告知会保存文件夹视图偏好，停用不会还原视图。

## 双栏文件复制与移动

先发现 `controlledFileTransfers`、`limits.transferSources == 64` 和以下两个能力；旧宿主缺少任意一项时，不生成该功能。`transfer-panel` 模板提供两个按钮：

```json
{"id":"copy","title":"复制到另一栏","capability":"files.copySelectionToOtherPane"}
```

移动使用 `files.moveSelectionToOtherPane`。两者都必须省略 `args`，不能传绝对路径、目标窗格编号或 `userConfirmed`。

使用时恰好打开两个可见文件夹窗格，在当前栏选中 1–64 个普通文件。目标是唯一的另一栏；三栏、四栏、搜索结果、加载中或文件夹选择会被拒绝。宿主在点击时固定选择及目标，后台记录实际对象身份，再展示原生确认窗口。之后改变选择或导航不会改变待确认计划。初始 Enter 选择取消，安装或启用包也不会自动确认操作。

面板里的复制、移动按钮根据当前窗口状态启停：无选择、选择超过 64 项、包含文件夹、窗格正在加载、并非恰好双栏或已有准备／确认流程时禁用。状态每 250 毫秒刷新，点击与键盘触发前再次检查。禁用状态由宿主计算，清单不提供绕过开关；文件身份、访问权限和目标冲突仍在后台准备及执行时检查，按钮可用不表示操作必定成功。其他六项命令保持现有宿主行为，尚无通用 `when` 表达式。

首版只处理现有本地目录与普通文件，不递归、不覆盖。拒绝 UNC、重解析点、离线、加密、稀疏、压缩文件及命名数据流（包括 Zone.Identifier），避免无声丢失元数据。文件数据、普通属性和时间戳按支持范围复制；目标继承所在目录的权限，不保证复制源 ACL 或所有文件系统特性。

确认后任务进入 Pulse 现有文件操作队列。宿主任务界面支持取消；当前声明式命令没有任务查询／取消参数，也没有可转交的确认凭据。停用、重载或关闭所属窗口使原主体失效，排队或执行中的操作在检查点停止。已完成的项目不会回滚；移动可能已提交目标而保留源文件，结果会显示这种双份状态。该子集不提供自动重试或撤销，重试需要重新选择和确认。

确认长行和长清单会省略显示，Ctrl+C 复制全部安全显示文本；控制字符被显式转义，复制文本不是原始可执行路径。运行期间的暂存文件只按已持有的句柄清理；进程崩溃可能遗留暂存文件或移动双份，不承诺断电事务或自动恢复。

维护者可运行 `python tools/extensions/live_transfer_check.py --output NEW_DIRECTORY`，在新建隔离夹具中验证真实原生确认、取消、复制、移动、冲突与停用，并保存应用内截图。它会修改自己新建的测试文件；`pulse-ext test` 和静态 `preview` 仍不执行文件操作。

## 尚未开放的范围

任意布局、自由拖放停靠、左右分区组合、脚本、工作流、通用文件计划 API、稳定第三方权限、签名与可信分发仍属于后续设计。固定右侧插槽、本地安装、双栏普通文件传输和多包快照不等于这些平台能力已经开放。不要依赖内部 C++ 类型、窗口消息或菜单编号作为公开 API。


### 本包传输状态绑定

生成前检查 `bindings` 是否包含 `context.transferStatus`。它提供本包在调用窗口中最近一次受控传输的本地化显示文字，可作为面板第三个字段；`transfer-panel` 模板已提供示例：

```json
{"label":"本包传输状态","binding":"context.transferStatus","empty":"暂无可用状态"}
```

准备、等待确认、排队、传输、取消中、全部完成、部分完成、取消和失败分别显示。准备或等待确认不表示已执行；移动目标已提交但源仍保留、部分项目完成后取消都不是全部成功。该字段只供展示，不能将本地化文字当作状态 ID、工作流条件或成功判断 API。

归属按包版本和窗口核对，不显示其他包或旧启用代次的任务。没有任务、结果已被宿主有限历史淘汰或归属失效时使用字段的 `empty`；状态丢失不能推断成功或失败。窗口关闭后不保留该绑定历史，不提供任务 ID、路径、错误详情或取消权限。详细结果仍从宿主任务界面查看，准备失败按宿主提示检查文件。静态预览只有空状态，不模拟执行成功。

## 声明式工作流与资源权限

workflow.run 的 args 只接受 `{workflow: "已声明ID"}`。顶层 workflows 最多 8 项，每项步骤为 1–16 个本包命令 ID；步骤不能引用 workflow.run 或 workflow.cancel，不执行脚本。每一步仍经宿主能力服务，文件传输保留逐次原生确认；失败停止后续步骤，不隐式回滚已完成步骤。参考 workflow-navigation 与 transfer-workflow 模板。

resources 是独立权限声明，最多 5 个唯一值：context.read、files.readCurrentDirectory、operations.own、settings.own、audit.own。它不是内部 action ID 的白名单，也不授权任意路径、他包任务或外部脚本。CLI 升级同时比较 command capabilities 与 resources，新增权限须显式使用 --accept-capabilities；原生接线与请求 payload 应按当前宿主发现契约验证。

完整平台能力边界见 [源码能力台账](extensions-capability-inventory.md)：扩展 capabilities、内部 kActions 与 MCP 工具分开计数，内部动作登记不能冒充扩展公开能力。

## Host-held Resource API

`resourceApi.requestSchema` 和 `ResourceRequest` 描述严格 op 对象；`resourceApi.responseSchema` 按请求操作映射精确返回结构。`ResourceResponse<Op>` / `ResourceResponseFor<Request>` 是类型化的 `{ok:true,data:...}` 或统一 `{ok:false,error:{code:string}}`，响应本身不添加 op 字段。入口是宿主持有 CallerLease 的 ResourceService::Call，开发工具仅发现，当前没有让 MCP 自报 packageId/caller 借权执行的工具。resource-client 模板供宿主 broker 服务测试阶段使用，面板本身不会执行资源请求。

| op | 参数与范围 | 权限 |
| --- | --- | --- |
| context.get | 无额外参数 | context.read |
| files.list | cursor 可选≤64；limit 1–256，默认64 | files.readCurrentDirectory |
| files.stat | 非空 opaque entry≤64 | files.readCurrentDirectory |
| operations.get/cancel | taskId 为正 uint64 规范十进制字符串 | operations.own |
| settings.get | 无额外参数 | settings.own |
| settings.set | ASCII key 1–64；value UTF-8≤1024或null删除；expectedVersion 0–2^53−1 | settings.own |
| audit.list | limit 1–128，默认64 | audit.own |

文件请求只使用已加载的普通目录 snapshot；opaque token 绑定 principal、owner、snapshot，5分钟过期，不接受任意路径。设置按包持久隔离，CAS 防止陈旧覆盖，最多64键/16KiB。审计只返回自身session最近最多128条，宿主全局1024条有界，不记录payload/path。类型不能证明字节长度、uint64上限或调用者所有权，必须由宿主检查。

工作流模板同时提供 workflow.cancel 按钮及 context.workflow 的单行监控源，status/error/progress 绑定本地显示状态。取消仅针对调用包和 owner 的当前工作流，不回滚已完成步骤。该数据源 pageSize 为1，不接受筛选/排序；源监控不增加脚本执行能力。

资源结果字段、生命周期和错误处理详见 [Resource API v1](extensions-resources.md)。

当前选择/排序SDK已完成独立验收（145作者流程、25安装窗口、17启动；19模板/73类型负例），固定归档与摘要见[平台交付记录](extensions-platform-delivery.md#2026-11-04-选择与排序固定交付)。本条在验收后补录，不回写已测试ZIP。

当前窗口业务能力新增 `tabs.new`、`tabs.duplicate`、`tabs.closeActive`、`tabs.activate`、`panes.focus`；由 `workspaceNavigation` 发现，严格参数与真实窗口模型见[标签与窗格契约](extensions-workspace.md)。当前阶段验收以新归档旁报告为准，不用历史14能力包证明这五项执行行为。
