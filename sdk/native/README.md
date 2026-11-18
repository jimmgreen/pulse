# Pulse 原生 SDK（开发预览）

本 SDK 提供可信 Windows x64 DLL 的版本化 C ABI，以及可选的 C++ 薄封装。开发者在 DLL 内实现自己的算法，使用宿主公开接口创建页面和请求文件服务。DLL 与宿主同进程，拥有相同 OS 权限；资源声明用于安装时展示，不作逐次调用授权。

当前交付是源码 SDK，不含宿主程序、私有头、内部库或安装器。包作者工具支持单 DLL 的内容寻址安装、显式启用、签名校验与升级回退；实际运行需要支持该契约的 Pulse 宿主。更完整的接口覆盖仍在扩展。可执行增强包仅使用 C++ 原生 SDK；JSON 清单和声明 UI 是宿主元数据。JavaScript runtime 已移除。

## 开始

公开头位于 `include/pulse`；头文件是字段、函数签名和结构布局的事实来源。接口摘要见 [机器清单](api-catalog.json)，上手见 [快速开始](../../docs/extensions-sdk-quickstart.md)。当前具有 UI 模型、数据、挂载、目录/上下文、文件操作、设置和线程派发服务。

原生包的 manifest、生成哈希、显式安装/启用、升级和回退流程见 [Native DLL packages](../../docs/native-packages.md)。`native-manifest` 只生成声明；`install`、`enable`、`rollback` 要求 `--allow-native` 明确确认同进程代码执行。首次安装默认禁用，升级保持启用状态；新 DLL 哈希仍需确认，运行中的实例需重启后应用升级或回退。单包可携带入口 DLL、最多 63 个私有 DLL 或资源文件；每文件校验 SHA-256，升级和回退切换整个包版本。详见打包文档的 files 与 --file。SDK/API 发现与验证不授予执行权限。

使用 `pack MANIFEST --output PACKAGE.pulse-ext` 生成可分发的 v2 原生包；其中包含 manifest、绑定哈希的 metadata、入口 DLL 及可选 `native.files` 声明的私有 DLL 和资源。`verify PACKAGE.pulse-ext` 校验而不执行，`install PACKAGE.pulse-ext --allow-native` 从同一份已验证快照写入内容寻址存储。直接 manifest 安装仍可用于开发；旧声明式执行字段会明确报错。

在 x64 Visual Studio 开发终端，从解压 SDK 根目录运行：

```powershell
cmake -S examples/native/rename-panel -B build-rename -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULSE_SDK_INCLUDE="$PWD/sdk/native/include"
cmake --build build-rename
python tools/extensions/test_native_sdk.py --sdk-root "$PWD" --work "$PWD/verification"
```

示例仅链接自身代码和系统 CRT，不链接 Pulse 内部库。`PulsePluginEntryV1` 是插件入口，`PulsePluginSdkPreview` 是必需的预览号导出，按 `size/version` 协商 `PulseHostV1` 和 `PulsePluginV1`。公开结构仅含固定宽度字段、借用字符串和函数指针，不传 STL 对象或跨 CRT 释放内存。

**SDK 预览号**：当前为 `3`，机器发现字段为 `sdkPreview`。1.0 之前接口可以在预览之间直接变化，`abi.h` 里的 `PULSE_SDK_PREVIEW` 标明当前预览。每个插件在文件作用域写一次 `PULSE_PLUGIN_SDK_PREVIEW()`（导出 `PulsePluginSdkPreview`）；没写、或用别的预览编译的插件，宿主在调用入口之前就拒绝加载并返回 `PULSE_ERROR_SDK_PREVIEW`，调试输出会写明双方的预览号。换 SDK 后重新编译即可。

```cpp
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) { /* ... */ }
```

## 文件操作

当前优先查询 `PulseFileOpsV4`（查询版本 4）；这是当前头文件符号，预览期不承诺旧表长期兼容。以下按用途说明现状，不按版本演进组织：

| 用途 | 函数 | 输入 |
|---|---|---|
| 当前选择复制/移动、移到回收站 | `submit` | 当前宿主选择与操作类型 |
| 当前选择传输到已打开目录 | `submit_to_directory` | 目标目录 handle |
| 任意已打开目录中的普通文件传输 | `submit_entries` | 源 handle、当前 snapshot、entry IDs、目标 handle |
| 当前快照中的目录树复制 | `submit_directory_tree` | 源 handle、snapshot、目录 entry、目标 handle |
| 查询/取消及逐文件结果 | `get`、`cancel`、`get_item` | 请求 ID；文件项索引和输出缓冲区 |

普通文件批次最多 64 项；目录树最多 128 个目录、512 个文件与 1 MiB 路径计划。源来自宿主捕获的选择或当前目录快照。树复制创建同名新根目录，包含空目录；已有目标拒绝，暂不支持合并、覆盖、目录树移动、永久删除和 UNC 传输。reparse、EFS、稀疏、离线及命名流等不支持项会拒绝。

耗时准备与 I/O 在 worker，提交后须等待原生确认。请求被接受不表示操作已完成；用请求 ID 查询并区分等待确认、执行中、成功、失败、部分完成和取消。确认前关闭目录或刷新快照会使准备失效；任务接受后按请求 ID 查询/取消。取消可能留下已提交文件和空目录，不是事务回滚。

逐文件结果用 `get.total_items` 遍历；`get_item` 先传空缓冲区读取 `path_bytes`，再分配含 NUL 的 UTF-8 路径缓冲区读取。全部终态文件结果读完后释放保留结果；空文件树在终态查询时消费。完整结构见 [fileops.h](include/pulse/fileops.h)。manifest 的 resources 用于安装时展示声明能力，不作为逐次调用授权；宿主仍验证句柄、快照、启用状态与实例生命周期。同进程 DLL 拥有整个进程的权限。

## 已提供的接口

`pulse.ui.model` V1 是唯一 UI 接口，提供 create/update/subscribe_command/set_states/get_state/unsubscribe/close，公开 27 种通用控件。旧 pulse.ui 查询与宿主外壳模型已移除。选择列表使用 Select，大数据列表使用 VirtualList。公开编号保留空洞，不能用 COUNT 遍历有效控件；机器清单的 nodeTypes 是有效集合。

结构提交同步复制，通过临时内存 DOM 验证，不经 JSON 文本传输。最多128节点、深度8、32个状态键、每值1024 UTF-8字节。更新保留已有状态和仍声明的命令订阅。data_source、context.* 和 action.data_operation 仍返回 UNAVAILABLE。输入事件、挂载与分页数据分别使用 events、mount、data。预览2插件须用预览3重新编译。

`pulse.ui.mount` V1 将现有页面/组件树移动到独立窗口、宿主右侧栏或主内容工作区，沿用同一个 HWND、状态、订阅与数据绑定。窗口位置可多实例；每个 owner 的 sidebar/workspace 槽位各限一个，争用返回错误而不顶替；窄布局报告实际窗口回退。`get`/`subscribe` 提供 placement、DIP 尺寸、DPI、主题、焦点及合并 revision。该接口不贡献原生标签页或工具栏，也不接管 Windows 桌面。契约见 挂载指南（本页）。真实目录工作台及应用内验收见 [样例](../../examples/native/directory-workbench/README.md)。

`pulse.ui.data` V1 注册插件数据提供者，绑定上述模型中已有的空 `virtualList` / `tree` / `dataTable`，复用共享渲染器和数据控制器。支持显式刷新、游标分页、稳定 ID 增量及取消；工作线程通过 `publish` 同步复制并排队数据，`PULSE_OK` 不代表已显示。每实例最多 16 个提供者和 16 个绑定，单页 1–256 个顶层行，累计显示最多 4096 行，控制器队列最多 64 页。绑定不会自动请求，模型成功更新后需重新绑定。完整线程、版本、状态和关闭规则见 原生数据视图（本页） 与 [公开头](include/pulse/ui_data.h)。此独立接口不开放模型的 `data_source`、`context.*` 或 `data_operation`，也不自行声明宿主功能/UI 验收通过。

`pulse.files` V1 的六项业务操作为当前目录、分页列表、准备重命名、确认提交重命名、查询本包任务、取消本包任务。另有请求取消函数。列表单页至多 256 项，重命名至多 64 项，计划 60 秒有效且一次使用，至多四个活动计划。宿主仍验证实例生命周期、当前目录与快照，提交必须经过原生确认；返回接受请求或任务 ID 不代表文件操作成功。可信 DLL 本身仍能调用 OS，这些约束只适用于宿主服务。

`pulse.fileops` V2 保留 V1 的 `submit` / `get` / `cancel` 前缀并追加 `submit_to_directory`。显式目标先由 `pulse.directories` V1 异步打开；传输使用同一 attachment 的 `PulseDirectoryId`，需要 `files.transferToDirectory`、`files.readDirectory`、`operations.own`。只接受当前 1–64 个普通文件，复用原生确认和受控引擎；目标已有同名项时整请求失败、不覆盖。原生受理请求的任务终态保留到终态 `get` 消费或 attachment 关闭，单 attachment 上限 32 项；不受普通任务最近 16 项历史淘汰影响。准备/确认期间关闭目标会取消请求，任务提交后关闭目标不取消任务；仍可凭请求 ID 查询和取消。示例见 [directory-transfer](../../examples/native/directory-transfer/README.md)，目录枚举的 UNC 支持不扩展为传输保证。

`pulse.directories` V1 提供绝对盘符/UNC 目录的只读不可变快照、显式偏移分页、合并失效 watch 和普通打开路由。每 attachment 最多 4 个目录、32 个请求、8 个 watch；快照最多 65536 行/16 MiB UTF-8 名称，超限失败而不截断。对应的安装能力声明为 `files.readDirectory`、`files.watchDirectory`、`files.open`；成功打开条目只表示路由接受。watch READY 后须刷新，后续失效/overflow 重刷，不将通知当成精确 journal。

`pulse.context` V1 的 `get` / `subscribe` 观察所属宿主已加载摘要，不枚举文件或完整选择。`get` 同步回调，`subscribe` 初始延后并按 30 ms 合并，最多 16 个订阅；读取和订阅对应安装能力声明 `context.read`、`context.events`。目录/快照/条目 ID 均限定 owner，所有目录/context 调用及回调在 UI 线程。资源声明最多 15 项，不作为逐次调用权限门槛。完整借用、版本、取消与使用示意见 原生目录与上下文（本页）；这不开放任意 UI 模型 `context.*` resolver、桌面挂载或通用文件写入。

`pulse.resources` V1 提供四项业务操作：对不透明目录条目的 stat、本包 settings_get / settings_set、当前 owner/principal 的 audit_list。设置写入必须提交 expected_version，冲突返回 version_conflict 且不覆盖；取消未完成请求不回滚已提交设置。与 `pulse.files` 合计十项资源业务操作；受控选择复制/移动另见 `pulse.fileops`，不表示搜索、预览、标签等其他宿主模块已经开放。

目录分析示例已经由主代理进行真实宿主验收：18 PASS、三张应用内截图、三个原文件保持不变，验证 CAS 版本竞争与重新读取。仓库证据为 `build-extensions/native-inspector-ui-01/report.json`；资源模型测试 36 PASS，日志 `build-extensions/native-resources-test-01.log`。这些测试日志和用户隔离配置不随源码 SDK 分发。此证据不扩大为全模型交互验收。

## 原生命令与选择

通过 [commands.h](include/pulse/commands.h) 查询 `pulse.commands` V1，在 `PulsePluginEntryV1` 中注册，每 attachment 最多32项；入口应及时返回，菜单发现不要求先打开插件面板。图标支持 default、archive、extract。条件支持选择数量范围、文件/目录类型以及小写带点扩展名的包含或排除列表，每个选中条目均须符合条件。扩展名最多32项，每项32字节。

命令出现在宿主菜单中，可按稳定标识 `native:package:command` 保存到现有 `toolbar_tools` 工具栏配置。菜单 token 仅当前实例有效，停用或注销后失效。快捷键仅在所属宿主主界面派发，内置快捷键优先，文本编辑器不派发插件快捷键；同 owner 的重复命令标识或快捷键拒绝注册。宿主在调用时重新检查条件，并把最新已加载选择传给命令回调；停用等待在栈回调结束后卸载。

`PulseContextV1.get_selection` 同步回调提供当前目录、条目的绝对路径、名称、目录标志和 `selection_revision`，只读取已加载快照，不访问磁盘。普通目录未选中时成功返回零条，仍含目录。最多4096条、合计4 MiB UTF-8；超限返回 UNAVAILABLE，不截断。加载期间返回 STATE，不支持的虚拟/内容视图返回 STATE 或 UNAVAILABLE。回调字符串和数组仅在回调期间借用，后台任务须先复制；不允许递归读取。停止会抑制后续回调，当前回调可完成。

[sdk/native/samples/archive](samples/archive/plugin.cpp) 演示“解压到此处”“解压到…”和“添加到压缩包…”命令的注册与选择传递。两个提取命令已接入公开同卷暂存提交（P1.5已验收），压缩命令已接入真实7z/ZIP创建与宿主发布； `verify-archive` 已在源码中接入后台真实解压到私有临时目录并清理，P1.4已通过真实宿主验收。归档预览读取器见 `archive_reader.cpp`；P1.5提取提交已通过真实宿主验收，压缩创建已通过P1.10本地路径范围验收。

## 预览提供者（P1.3 已验收）

通过 [preview.h](include/pulse/preview.h) 查询 `pulse.preview` V1，在入口的 owner UI 线程调用 `register_provider` / `unregister_provider`。每实例最多16个提供者，扩展名为2–32字节的小写带点 ASCII，如 `.7z`、`.rar`。这是新增接口，当前 `PULSE_SDK_PREVIEW` 仍为2。

宿主在后台线程执行提供者，每实例最多同时运行2次；共享 userdata 必须同步，建议每次回调创建独立读取器。此回调不在 UI 线程，禁止直接调用 UI 接口。循环读取时检查 `request->cancelled(request->context)` 并及时返回。宿主在选择变化或停用后丢弃旧结果，等待全部提供者返回后才卸载 DLL；插件若不合作返回，卸载也需继续等待。

`request->publish` 只在当前回调工作线程调用，并在返回前复制借用的数据；一次回调最多成功发布一次，返回 `PULSE_OK` 却没有结果视为失败。不得保存 request、publish/cancel context 到回调结束以后，不得跨 ABI 抛异常。下面是没有条目的有效归档响应，用于说明内存寿命；真实读取示例见 `samples/archive/archive_reader.cpp`：

```cpp
#include <pulse/preview.h>
static PulseStatus PULSE_CALL Preview(void*, const PulsePreviewRequest* request) {
    if (request->cancelled(request->context)) return PULSE_ERROR_STATE;
    PulsePreviewResult result{};
    result.size = sizeof(result);
    result.kind = PULSE_PREVIEW_ARCHIVE;
    result.format = {"7Z", 2};
    // Populate entries from this invocation's reader, checking cancellation.
    // Entry strings/arrays need only remain alive until publish returns.
    return request->publish(request->context, &result);
}
```

归档结果最多4096项，路径合计最多4 MiB UTF-8，每路径最多32768字节，格式名最多128字节；`modified_ticks` 使用 Windows FILETIME。位图为预乘 BGRA，宽高均为1–4096，`stride >= width*4`，`byte_count == stride*height` 且最多16 MiB。提供者只读源文件，不把结果发布当作文件修改。

`PULSE_PREVIEW_UI_MODEL` 结果通过 `model` 借用 `PulseUiModelTree`。宿主在提供者工作线程转换并复制模型，在快速预览与详情窗格使用现有停靠控件显示；支持模型本地状态和动作。最多128节点、深度8、64 KiB，沿用模型对 data_source、context.* 绑定、action.data_operation 的限制。预览没有原生命令回调绑定，`command_count > 0` 返回 `PULSE_ERROR_UNAVAILABLE`。P1.3 已通过真实7z/RAR读取、两处预览、浅色/深色、快速切换、模型本地动作和位图替换验收；证据见实施方案 P1.3。

## 线程和生命周期

生命周期、接口查询、UI/资源调用与回调均在拥有页面的宿主 UI 线程。例外是 `pulse.dispatch` 的 post/cancel 和 `pulse.ui.data` 的 publish，可从任意线程调用；`pulse.preview` 提供者及其 publish 在宿主工作线程运行；`pulse.tasks` 的work/report及 `pulse.staging` 除choose_destination外的调用遵守下方各自的worker契约。DLL 不应在该线程执行长时间计算或阻塞 I/O；自行管理的后台线程不能直接调用其他 UI 接口。异步文件回调保证晚于发起函数返回，回复内存仅在回调期间借用。DLL 需自行复制数据；数据提供者请求视图也仅在请求回调期间有效，publish 则在返回前同步复制整页借用数据。

停用时取消未完成请求、撤销订阅并关闭页面。`cancel_request` 抑制尚未派发的回调，不等于取消已经提交的任务；任务应显式调用 `task_cancel`。宿主等活动回调返回后才可卸载 DLL。`destroy` 返回前插件必须结束所有自有线程和回调，不能跨 ABI 抛异常。支持受控关闭，不承诺任意热卸载或对插件崩溃的隔离。

固定错误码和函数的具体借用规则以公开头及机器清单为准。MCP `describe_extension_api` 的 `nativeSdk` 字段及源码包的 `bundle_native_sdk.py describe` 只读提供此原生清单，不接入原生执行或授予权限。另有单独的 原生作者 MCP（本页），可在作者工作目录创建/构建/打包/验证/安装工程并诊断宿主；显式 `allowNative=true` 才能安装/启动原生代码。其 `native_host_run` 只确认宿主启动，不宣称插件加载或 UI 验收成功。`developerFeatures` 描述声明/作者工具契约；当前可查询的原生服务以 `nativeSdk.interfaces` 和公开函数表为准。JavaScript 插件运行层已移除。

## 事件、工作线程与 C++20 便利层

`pulse.ui.events` 订阅状态变化，按键合并为一轮最终值，区分 USER / PROGRAMMATIC，附带单调 revision，不发送初始快照或 IME 预编辑。成功更新树会丢弃旧批次，保留仍存在键和 all-key 订阅，仅删除键的订阅退休；回调内更新树会终止旧批次剩余监听者。回调内写状态在下一轮通知，不递归。

`pulse.dispatch` 提供有界 128 项 worker→UI 投递。回调可能早于 post 返回；cancel 返回 OK 才保证回调不会再开始，STATE 不能作为释放正在运行 userdata 的许可。普通 UI 接口仍不允许后台线程调用。

`client.hpp` 是可选 C++20 层，提供 Panel、Subscription、DispatchQueue 和字符串状态读写。functor 与共享生存期留在 DLL，ABI 仍是 C 函数指针和 userdata。RAII 对象在 UI 线程销毁；销毁队列前必须 join 所有生产线程。队列 Close 不替调用者 join，也不承诺恶意或不可取消任务可安全卸载。

[reactive-panel](../../examples/native/reactive-panel/README.md) 使用 std::jthread 合作取消、最新代次与当前输入双重检查。真实宿主验收 9 PASS、两张应用内图、exit 0，仓库证据 `build-extensions/native-reactive-ui-01/report.json`；封装现有 28 项定向生命周期及模型所有权测试。此证据不表示所有宿主能力或全部 UI 模型交互已开放/验收。

## 生命周期与崩溃恢复

插件代码在 Pulse 进程执行。激活、UI 与普通服务调用遵守 owner UI 线程；后台算法由插件管理，通过 dispatch 回投。停用时停止发布，撤销订阅，取消后台工作并结束线程，再销毁实例；跨 ABI 不传 STL 对象、不释放对方内存、不传播异常。线程例外以各公开头为准，例如 ui.data.publish 的排队语义。

激活前记录插件 id 与 DLL 哈希；崩溃日志按故障地址归因到已登记 DLL。下次正常启动停用相同构建并提示一次，重新启用会清除崩溃记录。开发重载模式不应用此停用策略。原生崩溃仍会结束整个宿主进程；归因只覆盖已登记插件映像内的故障地址。

## 预览期现状

UI 已统一为 pulse.ui.model。原生命令贡献与多文件包已提供；插件预览提供者的 archive/bitmap/UI-model 接口及两处宿主预览已验收；宿主可见插件任务已实现接口与源码、真实宿主验收已通过；同卷暂存提交已加入公开接口与样例，P1.5真实宿主验收已通过；插件设置页已接入，P1.6真实宿主验收已通过。样例位于 examples/native 与 sdk/native/samples/archive，压缩包样例包含命令入口、选择上下文及独立归档预览读取器；`verify-archive` 已接入真实解压到私有临时目录并清理的工作流，真实宿主验收已通过；extract-here/extract-to已接入P1.5同卷暂存提交，真实宿主验收已通过；压缩创建已通过P1.10本地路径范围验收。

## Manifest 当前字段

只接受 apiVersion、id、name、resources 和 native；commands、toolbar、palette、workflows、panel、runtime 均已移除并明确报错。UI 由 DLL 通过公开 UI 接口创建。模板可暂不含 native，打包前由 native-manifest 注入 DLL 哈希和入口信息。原生包正常打开与启停由宿主管理，插件自定义命令通过 `pulse.commands` 在入口注册。可选 `native.files` 声明相邻私有 DLL 和资源的文件名与哈希，使用 `native-manifest --file PATH` 生成；整包升级和回退见 [打包文档](../../docs/native-packages.md)。

`pulse.preview.unregister_provider` 分两阶段完成：调用即撤下提供者并取消工作，之后不再开始新请求；仍有回调运行时返回 `PULSE_ERROR_STATE`。此时必须保留 userdata，在 UI 线程重试至 `PULSE_OK` 后释放，或一直保留到 `destroy`。返回 STATE 不表示回调已停止，不得提前释放读取器或回调共享状态。

## 插件后台任务（P1.4 已验收）

[tasks.h](include/pulse/tasks.h) 的 `pulse.tasks` V1 提供 UI 线程 `create/cancel/get/release`。描述符含 `size/title/type/work/user/on_cancel`，title/type 各1–128 UTF-8字节；宿主接纳任务后启动自己的 worker，最多两个并发、16个保留任务，其他任务排队。创建达到上限时可淘汰最早终态记录，任务ID永不复用。

worker收到 `PulseTaskContext` 的 `id/report/cancelled`。`report` 可在任意线程调用，同步复制 `percent`、完成/总项目数、完成/总字节数、`current_item` 与 `error`；百分比为0–100或 `UINT32_MAX`（不确定），完成计数不超过总数，完成计数与确定百分比不得倒退。当前项目最多32768 UTF-8字节，错误最多4096字节。report仅在work返回前有效，自建报告线程必须先结束。

work返回 Completed、Partial、Failed 或 Cancelled；Running/Cancelling由宿主管理，终态不可改写。取消只按准确任务ID发出合作信号，`on_cancel` 在UI线程最多调用一次；清理期间仍可report错误，返回Failed不会被取消标志改成Cancelled。取消不承诺回滚。

`get` 回调只在调用期间借用文本。插件必须保留userdata直到查询观察到终态、release成功或destroy，因为work返回后仍可能待派发取消回调。停用取消队列和在途工作，宿主保持DLL直到worker、取消回调和UI终态处理全部结束。任务桥接及压缩包后台任务已通过真实宿主验收；向用户目标目录发布输出使用下节P1.5暂存接口，当前新增流程的真实宿主验收已通过。

`verify-archive` 的源码已接入 `archive_tasks.h/.cpp`：使用真实7-Zip读取器解压到进程/任务独占临时目录，完成或取消后清理，不向用户目标目录发布输出。提取失败或临时文件清理失败返回Failed并保留错误；取消仍需等待清理结束。真实宿主已验证任务窗口、进度胶囊、成功、失败、中途取消及停用清理；P1.5同卷暂存提交已加入样例，已验收。

P1.4定向验证：`pulse_native_tasks_test` 45项、`external_tasks_test` 49项通过；读取器46项覆盖真实7z/RAR及256 MiB提取，进度按成功WriteFile字节累计，取消后实际文件大小与已报告字节一致。真实宿主 `p14-task-large-accepted` 使用2 GiB提取，验证成功、失败、18%时取消、停用清理及既有操作窗口/进度胶囊；`native_host_diagnose` 返回matched。这些P1.4结果不替代新增P1.5目标输出提交验收。

## 同卷暂存与提交（P1.5 已验收）

公开 [staging.h](include/pulse/staging.h) 的 `pulse.staging` V1 将私有生成目录与用户目标分开。UI线程的 `choose_destination` 显示宿主目录选择器，在同步回调中借用UTF-8路径；取消选择不创建任务。复制路径后才能交给worker。其余 `create/commit/get/cancel/release` 可从worker调用；不要在UI线程自行扫描或写文件。

`create(destination, callback, userdata)` 返回接纳结果，之后在宿主worker回调同卷私有目录及stage ID。回调路径仅在回调期间有效；接纳后即使取消也必须保留userdata并等待回调结束。写入只限该目录，禁止自行把输出移动到目标。写入线程及所有句柄结束后调用 `commit`；接纳不表示用户已确认或文件已交付。宿主检查普通树、展示标准确认并通过普通文件操作处理冲突、取消及撤销记录。`get` 在调用线程同步回调借用状态，必须复制错误文本；区分Completed、Partial、Failed、Cancelled，取消不承诺回滚已经发布的文件。

`cancel` 只发合作取消；已提交后继续每10ms检查取消和 `get`，观察终态后才能 `release`。未提交stage也可release。release返回OK只表示异步清理被接纳，不代表磁盘目录已经删除；清理诊断及卸载等待由宿主生命周期管理。停用后禁止create/commit，但get/cancel/release保留到destroy，宿主等待插件writer退出后才清理剩余目录。Stage ID属于当前实例，最多16个；路径及树预算以公开头为准。

当前发布目标仅支持本地绝对路径（含长路径）；UNC及网络目标尚不支持。宿主通过同卷Move发布stage输出，Replace先保留目标旧版本。生成输出的undo/redo仅限当前会话，不写入持久化历史；redo目标被重新占用时保守失败并保留恢复项，待冲突解除后可重试，不另存新名后继续沿用旧路径历史。

正常stage的release清理与旧版本备份是两条不同生命周期：`.pulse-generated-original-*` 保存Replace前的用户内容，不随stage释放而删除。历史被淘汰或宿主退出后，这些备份可能保留在目标旁供手工恢复；当前不承诺自动垃圾回收或崩溃后恢复历史。不要把保留旧版本误报成stage清理失败，也不要把release接纳当作备份已删除。

归档样例的 `extract-here` 使用归档父目录，`extract-to` 使用宿主目录选择结果，只支持实际读取器支持的 `.7z` / `.rar`。任务先将真实输出写到宿主stage；提取进度映射到0–90%，提交阶段保持90%并说明等待宿主确认和文件操作，只有提交Completed才显示100%。它不把提取成功误报成发布成功；Partial及提交错误保留为终态。`verify-archive` 仍只验证私有临时输出并清理；`compress` 使用已保存格式和级别生成7z/ZIP，再通过同一暂存提交接口发布。P1.5真实宿主已验证选择/确认取消、真实发布、冲突Replace、定位、撤销重做及暂存清理，安装/升级/回退三轮均通过。关闭writer后清理与卸载等待另由暂存生命周期测试覆盖。

P1.5定向证据：`pulse_native_staging_test` 40项、`pulse_generated_output_test` 28项通过；公开头在调用方pack(1)下的ABI布局检查通过。`build-extensions/p15-staging-host-final/profile/{installed,upgraded,rolled-back}/report.txt` 记录真实宿主结果，`staging-confirm.png` 和 `staging-conflict.png` 为应用内帧复制截图。生成文件重做绑定本次撤销的精确回收项，支持同路径多代版本；回执不确定时阻止重复撤销，保留用户重建文件。MCP `native_host_diagnose` 返回 `catalogMatches=true`。

## 插件设置页（P1.6 已验收）

`pulse.settings.pages` V1 的create(title,tree,page)同步复制既有UI-model树，每个包会话最多一页，挂在宿主增强包设置区；所有调用和回调在owner UI线程。page也是普通panel ID；query仅提供该页专用的 `pulse.ui.model`、`pulse.ui.events` 表，独立于插件主面板，不提供mount/data。remove或停用使ID与订阅失效，专用context保留到会话destroy；回调借用文本须立即复制，userdata保留到当前回调返回。

持久化复用 `pulse.resources.settings_get/set`，接纳请求不表示保存成功，只有完成回调可显示Saved。样例以单键 `archive.preferences` 保存版本化值（`1\n格式索引\n级别索引\n目录`，总计最多1024 UTF-8字节），使用expected_version做CAS避免多字段部分保存。version_conflict不覆盖外部修改：重新get版本，保留草稿，明确要求用户再次Save；请求失败提供Reload/Save重试。停用/销毁取消未完成resource请求，不回滚已提交设置。

归档页提供默认解压目录、Browse、压缩格式7z/zip和级别Store/Fast/Normal/Maximum。空目录表示归档所在目录；extract-to使用已保存目录作为宿主选择器初始位置，extract-here仍固定归档父目录。格式和级别用于下一次压缩创建；未保存草稿不影响任务。引擎面板与设置页可独立存在。P1.6已通过真实页面、跨进程持久化、冲突及停用重载验收。

```text
用pulse/settings_pages.h注册每实例一页原生UI-model设置，不另造窗口或HTML控件。
在UI线程create/query/remove，通过专用query取得ui/model/events，保留主面板独立context。
回调借用值及时复制；resource请求userdata保留到完成或取消，销毁前取消请求。
用settings_get/set版本CAS持久化；仅完成回调报告Saved；冲突保留草稿、刷新版本并明确重试。
默认提取目录用于extract-to初始位置，extract-here语义不变；压缩格式/级别用于P1.10真实压缩创建。
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

P1.8 UI API 收敛已完成（SDK 预览3）；P1.9 旧压缩下载包已移除；P1.10真实压缩创建已验收，格式和级别为实际任务偏好。本阶段没有引入 JavaScript 运行时。

## ABI 1.0 冻结与弃用规则（预览期规则草案）

当前仍为 SDK 预览3；ABI version 1 是入口协议版本，不表示 SDK 1.0 已发布或 ABI 已冻结。`abi/windows-x64-msvc-preview3.json` 是当前预览快照，预览之间仍可变更，宿主继续拒绝不同预览号的插件。正式1.0冻结必须另行发布明确版本、支持平台及经审查的基线，不能通过重新生成本文件宣告完成。

未来1.0在明确支持的 Windows x64/MSVC ABI 范围内保持已发布接口名、版本、错误码和枚举数值，以及类型宽度、对齐、字段顺序/偏移、函数签名、调用约定和所有权/线程/取消语义。既有字段不可删除、重排或改变类型/偏移；仅当接口实现明确支持按调用方 size 读取和写入、忽略未知尾部且验证旧宿主降级时，才允许在尾部追加字段。当前头中存在要求准确 sizeof 的接口，不能仅因有 size 字段就假定支持协商；未验证协商的扩展使用新接口和新结构。破坏性变更必须使用新的接口名，不能仅提高同名接口查询版本来替换旧契约；旧接口继续提供。调用方遵守所查询接口的 size/version 契约，缺少新接口时明确降级或返回 unavailable。未知枚举值必须按各接口契约处理，不能当成功。

弃用须在发布说明和公开文档中标出首次弃用版本、替代接口及迁移步骤；1.x保留旧符号与原有语义，不复用其数值或改变其布局。删除或不兼容变更只能进入另行公布的新主版本，并提供迁移说明和兼容策略。安全修复若迫使限制旧行为，应明确记录限制和影响，不把 ABI 布局通过等同于行为兼容。

在 x64 Visual Studio developer prompt 中运行（仓库可先 call scripts\vcvars.bat x64；源码 SDK 使用已安装的 VS 开发者命令行）：

```text
python tools/extensions/check_native_abi.py check
python tools/extensions/test_native_abi.py
# 仅经人工审查、明确更新预览快照时运行；check 永不覆盖基线
python tools/extensions/check_native_abi.py generate
```

检查器自动遍历 include/pulse/*.h，记录全部当前支持的 C 结构、字段、typedef、函数指针、枚举与公开宏。MSVC 编译并实际运行探针测量 sizeof/alignof/offsetof、字段类型签名及枚举值；在调用方 pack(1) 下包含头文件并检查 pack 恢复。新头、类型、字段（包括尾部）或声明变化必须审查，未知声明语法直接失败，不能静默跳过。日志和完整差异默认写入 build-extensions/p32-abi；--include、--baseline 和 --build-dir 可指定隔离副本。

覆盖边界：这是当前简单 C 声明语法的严格检查，不是通用 C/C++ AST 或完整二进制兼容性证明；不覆盖 .hpp 便利层、DLL 实际导出、运行时能力和行为。数组/位域/union 等新声明须先扩展解析器与回归测试，否则拒绝。函数指针使用编译器规范化签名，同时保留声明 token 和 PULSE_CALL 宏，因此 x64 编译器归一的 __cdecl/__stdcall 源码变更仍会触发审查。参数名、等价 typedef 或宏写法变化也可能触发保守差异；工具链升级若改变 __FUNCSIG__ 文本需人工评估，不能盲目刷新基线。预处理指令本身不构成通用条件编译分析；当前平台的实际布局由探针保证，其他架构、编译器和条件分支不在本快照承诺内。

### P1.10 创建归档

`archive_writer.cpp` 只在插件worker中调用私有7z.dll。输入为本地绝对路径（含宿主提供的扩展盘符路径），stage必须为空；不支持UNC、重解析点和硬链接。限制：256个来源、4096个成员、64层、输入合计4 GiB。创建成功后由tasks/staging完成确认、冲突、撤销重做；writer不自行发布文件。

构建样例时传 `-DPULSE_ARCHIVE_WRITER_TEST=ON`，运行 `archive_writer_test.exe` 验证10个关键用例。真实宿主验收使用 `tools/extensions/test_native_compression.py`（宿主须开启PULSE_WITH_TEST_HOOKS），证据见主路线图P1.10。ZIP内容由Python zipfile独立回读。
