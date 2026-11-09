# 原生目录与上下文（开发预览）

`pulse.directories` V1 提供显式目录的只读快照、分页、变化失效通知和正常打开路由；`pulse.context` V1 观察当前插件所属宿主已经加载的目录、选择摘要和视图状态。两者是新增版本化 C ABI，契约以 [directories.h](../../sdk/native/include/pulse/directories.h)、[context.h](../../sdk/native/include/pulse/context.h) 和 [机器清单](../../sdk/native/api-catalog.json) 为准。完整作者例子见 [directory-browser](../../examples/native/directory-browser/README.md)。

SDK 保持 developer preview。本页描述作者契约；编译、宿主功能和实际界面验收分别记录。这里不交付桌面挂载、更多挂载位置、通用文件操作、搜索/预览/标签服务，也不开放 UI 模型任意 `context.*` resolver。受控的当前选择复制/移动到另一可见窗格另见 [pulse.fileops](extensions-native-fileops.md)；原有 `pulse.files` 当前目录和重命名流程保持原语义。

## 权限与接口查询

资源权限总数上限由 6 增至 10，原有六项名称和语义保持不变：`context.read`、`files.readCurrentDirectory`、`operations.own`、`settings.own`、`audit.own`、`files.renameCurrentDirectory`。新增权限只用于相应原生接口，没有新增 JSON `ResourceService` 操作。

| 调用 | 需要声明的资源权限 |
|---|---|
| 目录 `open` / `refresh` / `list` | `files.readDirectory` |
| 目录 `watch` | `files.readDirectory` 与 `files.watchDirectory` |
| 目录 `open_entry` | `files.readDirectory` 与 `files.open` |
| 上下文 `get` | `context.read` |
| 上下文 `subscribe` | `context.read` 与 `context.events` |

例如目录浏览器的 manifest 可包含以下片段；仅列出实际需要的权限：

```json
{
  "resources": [
    "context.read",
    "files.readDirectory",
    "files.watchDirectory",
    "files.open",
    "context.events"
  ]
}
```

使用公开 SDK 查询函数表，检查返回状态后再调用：

```cpp
#include <pulse/sdk.hpp>
#include <pulse/directories.h>
#include <pulse/context.h>

PulseDirectoriesV1 directories{};
PulseContextV1 context{};
pulse::sdk::Host sdk(host);
PulseStatus directory_status = sdk.Query(
    PULSE_DIRECTORIES_INTERFACE_NAME, PULSE_DIRECTORIES_VERSION_1, directories);
PulseStatus context_status = sdk.Query(
    PULSE_CONTEXT_INTERFACE_NAME, PULSE_CONTEXT_VERSION_1, context);
```

这两组接口的所有调用和回调都在 owner UI 线程。目录枚举和 watch I/O 由宿主工作线程执行；插件不要自己在 UI 线程枚举文件或阻塞等待。需要自有后台算法时，只把已复制的数据交给工作线程，并通过 `pulse.dispatch` 回到 UI 线程发起目录/context 调用；`pulse.ui.data::publish` 仍按它自己的任意线程规则使用。权限声明不隔离可信同进程 DLL 的 OS 权限。

## 目录快照与分页

`open` 接受绝对盘符路径或 UNC 路径，不接受相对路径、设备路径或虚拟路径。`PULSE_OK` 表示异步工作已接受；目录完成回调延后发生。始终同时处理发起调用的 `PulseStatus` 和回调 `PulseDirectoryReply` 的 `status`、`system_error`、`error`，不要把请求 ID 或接受结果当作枚举成功。

| 对象/字段 | 规则 |
|---|---|
| `directory_id` | 所属 attachment 的打开目录句柄 |
| `snapshot_id` | 成功枚举得到的不可变目录快照；成功刷新替换，失败刷新保留旧快照 |
| `entry_id` | 仅在对应 owner、目录、快照内有效的条目 ID，不是稳定文件 ID |
| `PulseDirectoryRow` | 条目 ID、名称、大小、修改 FILETIME、属性和 folder/reparse 标志 |
| `list` 的 `offset` / `limit` | 显式偏移和 1–256 的页大小，始终携带对应 `snapshot_id` |
| `next_offset` | 下一次 list 的偏移，`UINT32_MAX` 表示快照已读完 |

每个 attachment 最多 4 个打开目录、32 个待处理请求、8 个 watch。单个快照最多 65536 行和 16 MiB UTF-8 名称数据；超限会失败，不静默截断成看似完整的目录。上述行数和名称字节限制约束交付的快照；现有枚举引擎在完成扫描后检查预算，扫描过程的峰值内存不具这两个硬上限。宿主进程最多并行 32 个本服务工作请求，额度耗尽时明确拒绝新工作，分页只读取已有快照，不重复扫描。目录快照上限不等于 UI 数据视图可一次显示的行数；`pulse.ui.data` 仍有单页和累计 4096 行等独立限制，插件应分页或筛选。

完成回调中的路径、错误、行数组和行名称均借用到回调返回；保留或异步处理前必须复制。分页始终针对同一不可变快照，不要把新旧 `snapshot_id` 的行混在一起；刷新成功后，丢弃旧条目 ID、旧分页位置和基于旧快照的待执行操作。插件自身也应使用代次检查，忽略已经切换目录的迟到业务结果。

## 监听与刷新顺序

watch 是合并后的失效通知，不是精确文件变化日志。它不提供完整逐文件增删改 journal，也不保证每次磁盘变化各有一个回调。`PulseDirectoryChange` 的 `sequence` 可用于通知顺序，不应被解释为完整文件系统变更序列。

1. 打开目录成功后注册 watch，检查即时返回状态并保存 watch ID。
2. 收到 `PULSE_DIRECTORY_WATCH_READY` 后刷新一次，再读取该快照的分页。不要只依赖注册前那次枚举，以免漏掉监听建立期间的变化。
3. 收到 `PULSE_DIRECTORY_INVALIDATED` 或 `overflow` 时再次刷新；如果刷新途中又有失效，标记待刷新并在当前完成后重刷。
4. 收到 `PULSE_DIRECTORY_WATCH_ERROR` 时展示错误并依据业务选择重建监听或手动刷新；不要将失效监听当成内容未变化。

同一轮变化可以合并成一次刷新，避免高频 I/O 和重复重建 UI。刷新失败时保留旧快照并明确显示错误；旧数据显示不代表它仍与磁盘最新状态一致。

## 打开条目

`open_entry` 接收 `directory_id`、`snapshot_id` 和 `entry_id`，先验证条目属于现有快照，再委托 Pulse 普通导航/打开路由。过期或跨 owner 的标识被拒绝。插件不要把不透明 ID 当作路径，也不要用它跨实例查找用户文件。

打开完成状态成功表示路由已接受，不表示外部应用已经启动、文档执行完成或最终业务操作成功。需要验收普通文件打开时，分别记录宿主路由接受和可观察的外部结果，不能将前者替代后者。

## 上下文观察

`context.get` 同步调用回调并返回当前已加载状态；它不保留回调或 `user`。`context.subscribe` 的初始通知延后发送，后续变化按宿主 30 ms session tick 合并，因此它不是每次变化都送达的事件日志。每个 attachment 最多 16 个订阅。

`PulseContextSnapshot` 提供 `directory`、`selection_name`、`selection_count`、`view_mode`、`loading`，以及总 `revision`、`navigation_revision`、`selection_revision`。`changes` 是 INITIAL / NAVIGATION / SELECTION / VIEW 位标志。加载期间选择摘要清空；上下文观察只读取 owner 已加载状态，不枚举文件或完整选择列表。

revision 在每个 attachment 内单调递增。多选内容变化即使选择数和名称摘要相同，也会通过选择 revision 表达；不要只比较 `selection_count` 或 `selection_name` 决定是否更新。回调字符串只在回调中借用，需要复制后再存储。插件可将这些值显式写入自己声明的 UI 状态，但不能把该观察接口当成 UI 模型任意 `context.*` 绑定已经接通。

## 取消与关闭

目录 `cancel`、`close` 和 `unwatch` 会立即抑制相应未来回调；当前栈上的回调可以正常返回。上下文 `unsubscribe` 和宿主停止同样抑制未来通知。它们不是强行终止当前回调的机制，回调中引用的插件对象需要保持有效。

停用时撤销监听和订阅，取消待处理请求并关闭目录。停止流程不会在 UI 线程等待文件系统 I/O；插件自己的工作线程仍需在 `destroy` 返回前结束并 join。宿主 context 在插件 destroy 期间有效，之后到期；所有 ABI 回调都不能抛出 C++ 异常。

## 可复制的 AI 开发提示

```text
为 Pulse developer-preview 原生 SDK 编写一个 C++20 目录浏览插件。
先读取 sdk/native/api-catalog.json 与 sdk/native/include/pulse 中需要的公开头；
以 size/version 和 query_interface 协商 pulse.directories、pulse.context、
pulse.ui.model、pulse.ui.data、pulse.ui、pulse.ui.events、pulse.dispatch 中实际需要的接口。
只使用公开 API 和机器清单，不包含 Pulse 私有头、不链接内部库、不自建控件。
UI 使用已有原生模型和共享数据视图；声明最小资源权限，保留原六项权限语义。
复制借用回调数据；目录 I/O 使用宿主异步服务；所有目录/context调用在 UI 线程。
按目录/快照/条目 owner 绑定和代次处理分页、切换、过期结果与取消。
watch READY 后刷新，INVALIDATED/overflow 之后合并重刷，不当作精确 journal。
context.get 是同步回调，subscribe 初始延后并按30ms合并；检查 selection_revision。
open_entry 成功只表示路由接受；不宣称外部应用成功。
模型的 data_source、data_operation、任意 context.* resolver 仍未开放。
提供隔离假宿主回归测试、源码 SDK 独立编译命令和真实宿主 UI 验收步骤；
分别汇报编译、功能、截图结果，不把未执行检查写成通过，不宣称桌面挂载或通用文件写入。
```
