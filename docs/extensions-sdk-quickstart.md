# C++ 增强包作者快速开始

目标是开发者编写自己的 DLL，使用 Pulse 的真实 UI 和可复用服务，然后安装挂载到 Pulse。当前发布为 Windows x64/MSVC/C++20 开发预览。能力边界见 [原生 SDK](../sdk/native/README.md)，规划不等于已实现接口。

## 1. 选择示例并编译

从匹配版本的源码 SDK 或开发预览包开始，在 x64 Visual Studio 开发终端运行：

```powershell
cmake -S examples/native/rename-panel -B build-rename -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULSE_SDK_INCLUDE="$PWD/sdk/native/include"
cmake --build build-rename
```

`rename-panel` 由插件计算名称，Pulse 负责原生确认与实际文件任务；`inspector` 自行聚合目录数据；`reactive-panel` 演示后台算法和 UI 回调。业务源码只依赖公开头，不包含 Pulse 的 AppState/私有控件类，也不链接宿主内部静态库。

## 2. 生成清单并打包

源码 SDK 不包含宿主程序。先准备与 SDK 预览号匹配的 Pulse 宿主；下面的 C:/PulsePreview/pulse.exe 应替换为其绝对路径。命令在 SDK 解压根目录执行。输出清单和包必须尚不存在；清单必须与声明 DLL 同目录。

```powershell
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json native-manifest examples/native/rename-panel/manifest.json --library build-rename/pulse_rename_lower.dll --output build-rename/manifest.json
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json pack build-rename/manifest.json --output rename.pulse-ext
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json verify rename.pulse-ext
```

清单生成和验证不执行 DLL。`native.library`、架构和二进制哈希由工具与宿主校验；v2 包包含清单、metadata、入口 DLL，以及可选 `native.files` 声明的相邻私有 DLL 和资源；每个文件分别校验哈希。

## 3. 安装、启用并挂载

```powershell
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json install rename.pulse-ext --store C:/PulseAuthor/extensions --allow-native
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json enable sample.native-rename --store C:/PulseAuthor/extensions --allow-native
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json run --store C:/PulseAuthor/extensions --profile C:/PulseAuthor/run-1
```

默认安装目录为 `%LOCALAPPDATA%/Pulse/extensions`。初次安装禁用；启用后重启 Pulse，在命令面板打开该包，DLL 才按需激活。日常使用不需要 `--extension-native` 开发参数。上述命令使用独立测试库，run 的 --profile 隔离用户配置；profile 必须为新目录，重跑时使用 run-2 等路径。返回 PID 只证明进程启动，仍须打开插件并验证交互和停用。

当前原生挂载能力是插件页面和正常打开入口，**不是所有侧栏、工作区或桌面贡献点都已开放**。面板里的启停只改变当前进程；作者 CLI 修改持久状态，下一次启动生效。完整升级/回退与卸载语义见 [原生包契约](native-packages.md)。

## 4. 实现业务与验证

- 按公开接口查询 UI、文件、资源与派发函数表；不把宿主内部动作 ID 当作 C++ 服务。
- 耗时任务放插件后台线程；按线程/回调寿命契约回到 UI 更新；停用时取消并收回工作。
- 验证真实文件结果、错误/取消、停用/重启和资源释放；UI 使用应用内截图。
- SDK 编译和实际宿主验证入口见 [开发预览说明](../sdk/native/README.md)。

原生 DLL 与 Pulse 同进程，应明确审阅后启用。公开 API 的参数与主体检查不构成原生代码沙箱。

## 给 AI 助手的任务模板

```text
使用 Pulse 原生 SDK 开发一个 Windows x64 C++ 增强包。
先读取 sdk/native/README.md、api-catalog.json 和公开头，只调用已发布接口。
插件负责自己的业务算法；界面复用 Pulse 真实组件；不引用宿主私有头。
给出接口与未支持需求的对照，不杜撰尚未存在的 surface/桌面 API。
完成独立编译、manifest 生成、打包校验，以及隔离测试目录中的真实宿主验证。
使用应用内截图验证 UI，记录实际结果和资源释放；不得把编译成功写成功能通过。
输出 DLL/包/报告路径、安装挂载步骤和剩余缺口。
```

当前已有动态数据和目录变化订阅。桌面磁贴仍需拖放与桌面承载等接口；现阶段以压缩包参考插件推动公共服务。

## AI 作者 MCP

在 x64 Visual Studio 开发环境中启动 stdio 服务：

```powershell
python tools/extensions/native_author_mcp.py --workspace C:/PulseAuthor --sdk-root C:/PulseSdk --pulse C:/PulsePreview/pulse.exe
```

需要 Python 3、MSVC x64、CMake 和 Ninja。工作目录和 SDK 根目录使用绝对路径。

| 工具 | 用途 |
|---|---|
| native_sdk_describe | 发现接口，按 interface 读取完整公开头 |
| native_project_create | 从公开样例创建独立工程 |
| native_project_build | 编译并返回 DLL 与真实诊断 |
| native_package_stage | 计算哈希、生成 manifest、打包和校验 |
| native_package_install | 安装到作者工作目录的独立测试库 |
| native_host_diagnose | 比较宿主与 SDK 契约 |
| native_host_run | 启动独立配置的宿主，返回 PID 和日志 |

安装与运行参数需带 allowNative=true；安装可用 enable=true。构建与 CLI 串行执行，取消通知停止在途命令进程树，已返回的宿主 PID 由用户正常关闭。每次结果含命令、退出码与诊断；启动成功仍需实际打开插件验证行为。只读发现无需加载插件。

私有 DLL 和资源文件随包发布时，使用 `native-manifest --file PATH`（可重复）；文件须与入口 DLL 同目录。AI 作者 MCP 的 `native_package_stage.files` 接收工作目录内文件路径数组。完整规则与压缩包参考样例见 [打包文档](native-packages.md#archive-reference-sample)。

## 原生命令与当前选择

当前 `PULSE_SDK_PREVIEW` 为2；机器发现中的 `sdkPreview` 必须与头文件一致，升级 SDK 后重新编译 DLL。在插件入口查询 `pulse.commands` 并注册命令，每实例最多32项。命令可按选择数量、文件/目录类型和扩展名包含或排除条件启用，图标为 default/archive/extract；调用时宿主重检最新选择。快捷键仅主界面生效，内置快捷键优先。固定工具栏标识 `native:package:command` 保存到现有 `toolbar_tools`，停用使瞬时菜单 token 失效。

命令回调直接收到借用选择快照，也可调用 `pulse.context.get_selection` 同步读取当前目录与选择路径/名称/目录标志。普通空选择返回零条和目录；最多4096条、4 MiB UTF-8，超限失败而不截断，不访问磁盘。复制数据后再启动后台工作；不要保留回调中的指针或递归读取。

`sdk/native/samples/archive` 的提取命令已接入同卷暂存提交（P1.5已验收），压缩命令仍为明确标识的演示入口； `verify-archive` 已在源码中实现后台真实解压到私有临时目录并清理，P1.4已通过真实宿主验收。向用户目标目录发布输出的P1.5流程已独立验收；归档预览读取器见下节。

## 预览提供者与 AI 编写约束

新增 `pulse.preview` V1，当前预览号为3。在入口 UI 线程注册 `.7z` / `.rar` 等小写扩展名；宿主将读取交给工作线程，每实例最多并发2次。每次回调使用独立读取状态，周期检查取消，完成后在该回调中调用一次 `publish`。发布同步复制借用内存，回调返回后 request 和 context 全部失效；不要另开线程后保存 request，也不要从提供者调用 UI API。停用后宿主等所有回调返回才卸载 DLL。

AI 工作包可直接使用以下约束：

```text
仅使用 pulse/preview.h 的 register_provider/unregister_provider。
在 owner UI 线程注册；在宿主提供者工作线程读文件，不调用 UI API。
每次回调独立 reader，最多两次并发；循环检查 cancelled 并及时退出。
publish 同步复制，最多成功一次；检查返回值，所有异常在 ABI 内转成错误。
archive 最多4096项、路径总计4 MiB，每路径32768字节、format 128字节。
bitmap 使用预乘 BGRA，尺寸<=4096、stride>=width*4、总量<=16 MiB。
UI-model 使用 model 指向 PulseUiModelTree，publish 在工作线程转换并复制。
支持本地状态/动作，最多128节点、深度8、64 KiB；不支持data_source/context.*。
不设置原生命令：command_count>0 返回 UNAVAILABLE；action.data_operation 也不支持。
P1.3 已完成宿主验收；修改提供者后仍须定向验证取消和选择快速切换，不把既有验收当作新改动的测试结果。
```

压缩包样例的源代码包应包含 `archive_reader.h/.cpp`、`archive_reader_test.cpp`。使用 `-DPULSE_ARCHIVE_READER_TEST=ON` 构建独立读取器测试；该测试不替代真实宿主详情窗格与快速预览验收。完整字段与内存契约见 [SDK 参考](../sdk/native/README.md) 和 [公开头](../sdk/native/include/pulse/preview.h)。

`pulse.preview.unregister_provider` 分两阶段完成：调用即撤下提供者并取消工作，之后不再开始新请求；仍有回调运行时返回 `PULSE_ERROR_STATE`。此时必须保留 userdata，在 UI 线程重试至 `PULSE_OK` 后释放，或一直保留到 `destroy`。返回 STATE 不表示回调已停止，不得提前释放读取器或回调共享状态。

## 后台任务接口

`pulse.tasks` 已通过P1.4真实宿主验收。在UI线程create，worker仅做业务与report，通过cancelled轮询取消；不要在worker调用UI服务。任务最多两个并发、16个保留记录。进度复制百分比/字节/项目计数及当前文件、错误文本；work显式返回Completed/Partial/Failed/Cancelled。取消清理失败时继续report诊断并返回Failed，不能伪报已取消或成功。查询到终态前保留userdata，随后release；关闭仍需等待在途worker返回。完整字段与限制见SDK README和tasks.h。

验证任务使用 `verify-archive`，观察项目/字节进度与当前文件；测试完成、取消及清理失败的终态。它只解压到私有临时目录，结束后删除临时输出，不会交付目标目录文件。P1.4真实宿主验收已通过，P1.5暂存提交已加入接口与样例，真实宿主验收已通过。

### 交给 AI 的任务开发约束

```text
使用 pulse.tasks 实现后台任务，先读取 tasks.h 与 SDK README。
UI线程create/cancel/get/release；worker仅执行算法、report和cancelled检查。
进度与错误文本由report复制；取消清理失败须报告原因并返回Failed。
userdata保留到get观察终态、release成功或destroy，先结束自建报告线程再返回work。
复用verify-archive验证真实解压到私有临时目录及清理，不向用户目录发布输出。
验证成功、取消、失败、停用及任务ID隔离；P1.4真实宿主验收已通过，修改后仍需定向验证，P1.5提交流程已另做真实宿主验收。
```

## 发布生成文件：同卷暂存

先查询 `pulse.staging`，读取公开 `staging.h`。在UI线程用 `choose_destination` 选择目录，回调中复制借用路径；选择取消不创建任务。worker调用create并等待异步回调（取消也必须等回调结束），在宿主给出的私有同卷目录写入。所有writer及句柄关闭后commit，由宿主确认、冲突处理和普通文件操作交付。不要把commit返回OK当作完成。

每10ms检查任务取消，取消时调用stage.cancel并继续get直到终态；随后release。release接纳异步清理，不代表清理已完成；由宿主生命周期等待收尾。Partial表示部分交付，Cancelled不保证回滚。停止后get/cancel/release仍可用于收尾，userdata保留到回调及worker结束。

当前发布目标仅支持本地绝对路径（含长路径）；UNC及网络目标尚不支持。宿主通过同卷Move发布stage输出，Replace先保留目标旧版本。生成输出的undo/redo仅限当前会话，不写入持久化历史；redo目标被重新占用时保守失败并保留恢复项，待冲突解除后可重试，不另存新名后继续沿用旧路径历史。

正常stage的release清理与旧版本备份是两条不同生命周期：`.pulse-generated-original-*` 保存Replace前的用户内容，不随stage释放而删除。历史被淘汰或宿主退出后，这些备份可能保留在目标旁供手工恢复；当前不承诺自动垃圾回收或崩溃后恢复历史。不要把保留旧版本误报成stage清理失败，也不要把release接纳当作备份已删除。

```text
仅使用pulse/tasks.h和pulse/staging.h；不要引入宿主私有头。
目录选择及其同步回调在UI线程；样例把生成文件和create/commit/get/cancel/release放在worker。
get回调同步运行在调用线程；create/commit/release磁盘工作由宿主worker执行。
接受异步create后一直保留userdata到回调结束，复制借用UTF-8路径。
只写宿主stage，先结束writers再commit，等待确认和文件操作终态。
取消提交后继续等待terminal再release，不自行删除仍被文件操作使用的目录。
分开呈现提取与发布阶段，百分比和完成计数不倒退。
extract-here/extract-to仅支持7z/rar；verify不发布文件，compress仍是演示。
P1.5确认、冲突、定位、撤销重做、暂存清理已有真实宿主证据；修改后按影响范围复验。
```

## 插件设置页（P1.6 已验收）

`pulse.settings.pages` V1 的create(title,tree,page)同步复制既有UI-model树，每个包会话最多一页，挂在宿主增强包设置区；所有调用和回调在owner UI线程。page也是普通panel ID；query仅提供该页专用的 `pulse.ui.model`、`pulse.ui.events` 表，独立于插件主面板，不提供mount/data。remove或停用使ID与订阅失效，专用context保留到会话destroy；回调借用文本须立即复制，userdata保留到当前回调返回。

持久化复用 `pulse.resources.settings_get/set`，接纳请求不表示保存成功，只有完成回调可显示Saved。样例以单键 `archive.preferences` 保存版本化值（`1\n格式索引\n级别索引\n目录`，总计最多1024 UTF-8字节），使用expected_version做CAS避免多字段部分保存。version_conflict不覆盖外部修改：重新get版本，保留草稿，明确要求用户再次Save；请求失败提供Reload/Save重试。停用/销毁取消未完成resource请求，不回滚已提交设置。

归档页提供默认解压目录、Browse、未来压缩格式7z/zip和级别Store/Fast/Normal/Maximum。空目录表示归档所在目录；extract-to使用已保存目录作为宿主选择器初始位置，extract-here仍固定归档父目录。格式和级别仅为未来偏好，压缩命令仍是演示，不生成压缩包。引擎面板与设置页可独立存在。P1.6已通过真实页面、跨进程持久化、冲突及停用重载验收。

```text
用pulse/settings_pages.h注册每实例一页原生UI-model设置，不另造窗口或HTML控件。
在UI线程create/query/remove，通过专用query取得ui/model/events，保留主面板独立context。
回调借用值及时复制；resource请求userdata保留到完成或取消，销毁前取消请求。
用settings_get/set版本CAS持久化；仅完成回调报告Saved；冲突保留草稿、刷新版本并明确重试。
默认提取目录用于extract-to初始位置，extract-here语义不变；压缩格式/级别明确是未来偏好。
验证加载、失败、冲突、停用、重新打开与重启保留，不把独立构建成功当作宿主UI验收。
```

P1.6交付证据：`pulse_native_settings_pages_test` 23项、公开头ABI布局检查通过；`test_native_multifile_ui.py --settings-fixtures` 的 `build-extensions/p16-settings-host-verified` 安装/升级/回退三轮通过。应用内截图覆盖设置入口、浅色页、深色窄页及滚动后Save可达性。目录允许空值或本地绝对路径，拒绝相对/UNC/换行并保留草稿。模型辅助函数只设置适用于该控件的present字段，Stack/ScrollView不能携带text字段，即使空串也会验证失败；参照归档样例的显式Text标签。


## P1.7 C++20 开发便利层（2026-11-08）

包含 `pulse/client.hpp` 即可使用拥有数据的 `ModelNode`、`ModelRow`、`ModelColumn`、`ModelTree`，由构建器填写 present 和数组视图。控件仍是 Pulse 原生模型，宿主保留属性、可访问性标签及状态绑定验证。输入框、下拉框必须提供 text 标签；布局中另有文字标签不能替代它。

```cpp
using namespace pulse::sdk;
using N = PulseUiModelNode;
auto tree = ModelTree(ModelNode(PULSE_UI_MODEL_STACK, "root").Children({
    ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "query")
        .String(&N::text, "Search").String(&N::state, "query")}))
    .State("query", "");
// model 为 Host::Query 得到的 PulseUiModelV1。
uint64_t panel = 0;
auto status = model.create(model.context, View("Example"), &tree.Build(), &panel);
```

构建器拥有字符串及子节点；Build 返回借用视图，只可在所属对象下次修改、Build、移动或销毁之前使用，宿主 create/update 同步复制。复制或移动后重新 Build，禁止缓存旧指针。显式空字符串、false、0、空数组与未设置属性不同。

`Subscription` 管理 UI 订阅，`CommandRegistration::Register` 接受命令描述符与 lambda，移动句柄转移注销责任；回调可自行 Reset，异常不会跨 C ABI。`DispatchQueue` 支持工作线程投递 UI lambda；Close 不负责 join，销毁前必须结束生产线程。

`TaskGroup::Start(title, type, work, on_cancel, &id)` 把 lambda 交给宿主工作线程，out 必须非空；UI 线程调用 Collect 回收已成功 release 的终态任务。CancelAll 只请求取消，不等待完成。未知任务 ID、释放失败仍保留捕获对象；必须等 Collect 清空，或宿主排空工作线程和取消回调后的插件 destroy 才销毁 TaskGroup，禁止回调内销毁组。所有 STL 对象和捕获分配留在 DLL，C ABI 未改变。

压缩包样例的设置树、引擎状态树、UI 订阅、命令和后台任务已迁移；目录工作台的模型与订阅也已迁移。定向 wrapper_test 共 28 项通过。真实宿主证据：`build-extensions/p17-settings-host-final`（安装/升级/回退与设置持久化）、`build-extensions/p17-archive-host`（真实 2 GiB 解压取消、停用排空、发布/冲突/撤销重做）、`build-extensions/p17-workbench-host`（18 项，通过应用内截图核对）。

P1.8 UI API 收敛已完成（SDK 预览3）；P1.9 旧压缩下载包已移除；压缩创建仍未实现，格式和级别是未来功能的持久化偏好。本阶段没有引入 JavaScript 运行时。

## 外部作者走查与发布

P3.1 已由未读取 Pulse 私有源码的独立作者验证：只用解压 SDK，从空工程实现 Number Lens 数列统计插件，经交付 MCP 完成编译、打包、隔离安装启用和宿主契约核对。维护者随后通过真实宿主完成 15 项交互与停用验证，并检查浅色、深色 150% 和错误态应用内截图。此结果验证该插件的作者流程，不表示全部控件和任意插件都已验收。

MCP 当前没有点击、截图或停用自动化接口；native_host_run 明确返回 loadVerified=false。作者应在实际宿主手动打开页面并验收，自动化截图验收使用维护者测试宿主探针。不要把工具启动结果当作功能通过。宿主测试探针和历史证据目录不随源码 SDK 交付。

发布者密钥格式、签名、可信指纹配置、分发文件及升级说明见 [签名与发布](native-packages.md#signing-and-distributing-a-preview-release)。源码 SDK 不含宿主程序，也不代替发布者的密钥托管和外部发布渠道。
