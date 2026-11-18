# 原生文件操作（开发预览）

`pulse.fileops` V1 提供当前选择复制/移动到另一可见窗格，以及当前普通文件选择移到回收站的原生入口。V2 保留 V1 函数表前缀，追加当前选择到已打开显式目录 ID 的受控复制/移动。V3 再追加以已打开源目录当前快照 entry ID 为源的受控复制/移动。V4 追加基于同一 attachment 当前快照目录 entry 的递归复制和逐文件终态查询。所有版本复用宿主的原生确认、任务、进度和取消流程，不另建文件操作引擎。接口不接收插件构造的源路径。ABI 以 [fileops.h](../../sdk/native/include/pulse/fileops.h) 和 [机器清单](../../sdk/native/api-catalog.json) 为准；公开 C++ 示例见 [file-operations](../../examples/native/file-operations/README.md) 和 [directory-transfer](../../examples/native/directory-transfer/README.md)。V3 独立 DLL 真实宿主验收见 `build-extensions/native-target-v3-20261106-01/report.json`；V4 真实宿主验收（61 PASS、0 FAIL）见 `build-extensions/native-target-v4-20261106-02/report.json`，应用内截图位于同目录 `profile/capture/`。

## 权限和前置条件

V1 同时提供复制/移动和回收站动作的 Manifest 声明为：

```json
{
  "resources": [
    "files.transferCurrentSelection",
    "files.deleteCurrentSelection",
    "operations.own"
  ]
}
```

只声明实际使用的权限。V1 复制/移动使用 `files.transferCurrentSelection`，回收站使用 `files.deleteCurrentSelection`；二者均需要 `operations.own`。V2 显式目标另用 `files.transferToDirectory`、`files.readDirectory` 和 `operations.own`，不要求 `files.transferCurrentSelection`。

源窗格必须有 1–64 个已选普通文件。目录、搜索结果、加载中的列表和过期目录快照拒绝提交；回收站入口另外拒绝重解析点。V1 复制/移动要求两个可见窗格，另一窗格作为目标；回收站和 V2 显式目标不要求双窗格。V1 插件只提交操作类型；V2 另提交自己已打开的目录 ID，不能指定源路径、包/窗口身份或用户确认。

`PULSE_FILEOPS_DELETE_SELECTION_TO_RECYCLE_BIN` 在提交时捕获当前选择路径，异步查询回收策略。只要任何源项在预检时不支持回收，本次请求在提交任务前失败，不自动降级为永久删除。通过预检后始终显示 Pulse 原生回收确认，即使普通手动删除的可选确认已关闭。执行使用现有删除引擎和 Undo 记录；执行期间策略改变或用户在宿主任务界面跳过项目可能形成部分结果。宿主另行提出的永久删除确认属于独立用户操作，不由这个请求 ID 代表。

复制/移动沿用受控传输的对象身份校验；回收站入口沿用宿主删除的路径语义，未提供跨预检与执行的对象锁定。提交后切换 UI 选择不会改变已捕获的路径，但同路径对象的外部替换不在此版本的身份保证内。

## 提交、确认和任务状态

查询 `PULSE_FILEOPS_INTERFACE_NAME` / `PULSE_FILEOPS_VERSION_1` 得到 `PulseFileOpsV1`。所有接口都在所属窗口 UI 线程调用；不要在 UI 线程自行复制、移动或等待任务。`submit` 同步返回请求 ID，之后宿主异步准备对象快照并显示 Pulse 原生确认。用户确认前不会执行文件修改；拒绝、停用、取消或验证失败均不会提交任务。成功调用只表示准备请求已接受，不表示用户已确认或文件已复制。

使用 `get` 轮询该 attachment 自己的请求。每次返回当前状态和可选的任务 ID、字节/项目进度、百分比与终态。状态为 `WAITING_CONFIRMATION` 时由 Pulse 管理确认 UI；插件不得自行推断用户已确认。终态中的 `COMPLETED` 表示所有条目成功；`PARTIAL` 表示存在已提交或部分结果，不能当作回滚；`FAILED` 和 `CANCELLED` 也可能包含此前已完成的条目。

`cancel` 取消准备/确认阶段或向已提交任务发出尽力取消请求。取消不撤销已经完成的文件效果。查询和取消仅接受当前附件成功提交的请求 ID，并要求 `operations.own`。每个 attachment 最多 32 个尚未回收的请求；读取终态会释放本地请求槽。回收站任务的终态快照保留到读取或停用，避免后续其他任务挤掉结果。该入口目前只提供状态与项目计数，`system_error` 在没有结构化底层错误码时为 0；应以状态判断失败，不能仅检查错误码。

```cpp
#include <pulse/sdk.hpp>
#include <pulse/fileops.h>

PulseFileOpsV1 fileops{};
auto status = pulse::sdk::Host(host).Query(
    PULSE_FILEOPS_INTERFACE_NAME, PULSE_FILEOPS_VERSION_1, fileops);
if (status != PULSE_OK) return status;

PulseFileOperationRequestId request = 0;
status = fileops.submit(fileops.context,
    PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE, &request);
if (status != PULSE_OK) return status;

PulseFileOperation snapshot{};
snapshot.size = sizeof(snapshot);
status = fileops.get(fileops.context, request, &snapshot);
// Call get again on a later UI turn while snapshot.terminal == 0.
```

插件在 `deactivate` 中取消未完成请求；`destroy` 前停止自有轮询器/线程。宿主停用也会向该附件已提交的请求发送取消。任务继续由宿主工作线程运行，UI 线程不等待文件系统。

## V2：当前选择到显式目标目录

查询 `PULSE_FILEOPS_INTERFACE_NAME` / `PULSE_FILEOPS_VERSION_2` 得到 `PulseFileOpsV2`。V1 的布局、版本和行为不变；V2 具有相同的 `size` / `version` / `context` / `submit` / `get` / `cancel` 前缀，在末尾追加 `submit_to_directory`。原 `submit` 继续只接收 V1 三种操作；新操作 `PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY`（4）和 `PULSE_FILEOPS_MOVE_SELECTION_TO_DIRECTORY`（5）通过新函数提交。查询 V2 失败时应展示接口不可用，而不是自行进行路径复制。

Manifest 的最小资源声明为：

```json
{
  "resources": ["files.transferToDirectory", "files.readDirectory", "operations.own"]
}
```

先通过 `pulse.directories` V1 的 `open` 异步打开用户输入的绝对目录路径，在成功回调中保存 `PulseDirectoryId`。`PULSE_OK` 只接受打开请求；回调里的路径和错误文本按 [目录契约](extensions-native-directories.md) 借用，需复制后才能保存。目标 ID 必须仍打开且属于同一 attachment；外包、外窗口、已关闭或伪造 ID 均不接受。目录读取不隐含写权限，也不自动授予传输资源。

```cpp
#include <pulse/sdk.hpp>
#include <pulse/directories.h>
#include <pulse/fileops.h>

PulseFileOpsV2 fileops{};
auto status = pulse::sdk::Host(host).Query(
    PULSE_FILEOPS_INTERFACE_NAME, PULSE_FILEOPS_VERSION_2, fileops);
if (status != PULSE_OK) return status;

// destination is the successful open callback's still-open, owner-bound ID.
PulseFileOperationRequestId request = 0;
status = fileops.submit_to_directory(fileops.context,
    PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY, destination, &request);
// On PULSE_OK, poll get and retain request until its terminal snapshot is read.
```

新函数在所属 UI 线程同步捕获当前普通文件选择和目标路径，再在 worker 准备对象。源选择仍限 1–64 项；插件不能自行传入源路径。目标无需出现在另一窗格。宿主始终显示原生确认，确认前不执行写入；接受请求不代表准备完成、用户确认或传输成功。状态、进度、终态读取和取消与 V1 相同。

同路径目标目录在 worker 准备前被替换时沿用路径解析，未承诺 `open` 与准备之间锁定同一对象；准备后由受控引擎验证目标对象身份。所有复制/移动沿用既有受控引擎的冲突策略：执行前先检查全部目标名称，已有任何同名项时整请求失败，不覆盖。多文件请求并非原子事务；执行期间的外部变化仍可能产生部分结果。不提供递归或冲突选项。目录枚举支持 UNC 不代表传输支持 UNC。

准备或等待确认期间关闭目标目录会取消尚未提交的请求。任务已经提交后，关闭目录只释放目录服务句柄，不取消已经接受的传输；插件仍应保留请求 ID，使用 `get` / `cancel` 跟踪该请求。已接受的原生传输请求会保留自己的终态，不受宿主普通受控任务最近 16 项历史淘汰影响；插件消费终态 `get` 或 attachment 关闭时释放结果。每个 attachment 最多保留 32 个未消费结果，达到上限时应先消费或关闭请求；不要把有界保留解释成永久任务历史。停用时取消自己的文件请求、取消未完成的目录打开请求、关闭目标目录并撤销 UI 订阅。不要让目录关闭把未结束请求替换成“已完成”，也不要把取消解释为回滚。

`directory-transfer` 示例使用 `destination-input`（状态键 `destination`）、`open-target`、`close-target`、`copy-selection`、`move-selection`、`refresh-status`、`cancel-request` 和四个状态文字节点。目标打开后显示 `target-ready`，失败显示 `target-error`；提交显示 `preparing` 或 `rejected`。手动刷新展示与 V1 示例一致的请求状态词，终态清除请求 ID。关闭没有活动请求的目标显示 `target-closed`；有活动请求时保留请求状态，刷新和取消仍可用。本节描述实现契约，不代替应用内验收报告。

## V3：当前目录快照到显式目标目录

V3 仅在 V1/V2 之后追加 `submit_entries`，V1/V2 的函数表大小与字段偏移保持不变。Manifest 必须同时声明 `files.transferFromDirectory`、`files.transferToDirectory`、`files.readDirectory` 和 `operations.own`。先用同一个 attachment 的 `pulse.directories` 打开源目录与目标目录，并用 `list` 取得源目录当前 `snapshot_id` 和行的 `entry_id`；只把普通文件行的 ID 传给 `submit_entries`。宿主要求源/目标句柄和快照都属于该 attachment，快照仍是源目录当前快照，ID 唯一且数量为 1–64。目录、重解析点、未知 ID、重复 ID、过期快照一律拒绝。

```cpp
PulseFileOpsV3 fileops{};
auto status = host.Query(PULSE_FILEOPS_INTERFACE_NAME, PULSE_FILEOPS_VERSION_3, fileops);
if (status != PULSE_OK) return status;
// source_snapshot and entry_ids come from the source handle's current list reply.
PulseFileOperationRequestId request = 0;
status = fileops.submit_entries(fileops.context, PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY,
    source_directory, source_snapshot, entry_ids, entry_count, destination_directory, &request);
```

源快照记录的文件大小与最后修改时间会在 worker 准备阶段重新核对；不匹配、对象不再是普通文件或句柄/快照在确认前失效时，请求失败/取消，不展示确认且不修改文件。宿主仍执行原生确认和受控引擎的对象身份检查、目标冲突拒绝及取消流程。关闭句柄或刷新源快照在任务提交前会使准备/确认请求取消；任务提交后通过请求 ID 查询或取消。V3 不是目录同步 API：不递归、不跟踪后续变更、不是多文件原子事务。

## V4：当前快照目录树到显式目标目录

V4 在 V3 后追加 `submit_directory_tree` 与 `get_item`，不改变 V1/V2/V3 表的字段和偏移。Manifest 必须声明 `files.transferDirectoryTree`、`files.transferToDirectory`、`files.readDirectory` 和 `operations.own`。调用方传源目录 handle、当前 snapshot ID、目录 entry ID、目标目录 handle；源 entry 必须是普通目录且不能是 reparse 项。宿主异步枚举并冻结树，限制 128 个目录、512 个普通文件和 1 MiB 计划路径数据；EFS、稀疏、离线、压缩、命名流与 reparse 项拒绝。确认一次创建以源目录叶名称命名的新目标目录，目标根已存在则请求失败，不合并、不覆盖、不递归移动、不永久删除。

`get_item(request_id, index, path_utf8, path_capacity, out)` 返回文件相对路径、字节和该项状态。先以 `path_utf8=nullptr, path_capacity=0` 查询 `path_bytes`，再分配包含终止 NUL 的缓冲区读取。目录创建成功后即使后续文件失败，仍可能保留空目录或已提交文件；终态逐文件结果为准，不承诺事务回滚。关闭源/目标句柄、刷新快照或停用会在确认前使请求取消；进入已提交任务后通过请求 ID 取消。执行端对文件沿用句柄身份复核、暂存、不覆盖相对提交和 Pulse 原生确认。

V4 头文件、适配器、准备器与执行器已接入，定向测试覆盖嵌套文件、空目录、既有目标哨兵和相对路径逃逸。公开独立 DLL 的端到端隔离宿主验收已通过：61 PASS、0 FAIL，验证嵌套字节、空目录、逐文件终态、目标冲突不覆盖、确认及停用取消；同时采集并检查了真实应用内完成态、确认态和冲突态截图。报告见 `build-extensions/native-target-v4-20261106-02/report.json`。尺寸上限、执行中取消及 reparse 替换竞态仍属于后续强化覆盖。

## 尚未开放

该阶段并未完成完整文件服务：永久删除、任意源目录间复制/移动、冲突选项、图标/缩略图、搜索、预览、标签和收藏仍未由这组接口开放。目录树复制的基础路径已完成真实宿主验证，边界/竞态强化测试仍在后续计划中。当前复制/移动引擎不支持网络共享、重解析点、EFS、稀疏/离线文件及命名流；停止或进程崩溃可能留下暂存文件或已提交部分结果。目录引擎对 UNC 的枚举支持不代表此传输引擎支持 UNC。

```text
AI 作者提示：编写 C++20 Pulse 原生增强包时，若只需要把用户当前选择
复制/移动到另一可见窗格，使用 pulse.fileops V1；先读取 api-catalog.json、
fileops.h 和本指南。按需声明 files.transferCurrentSelection 与 operations.own，
在 Pulse UI 线程提交，按请求 ID 轮询状态，明确展示等待确认、部分完成、
失败和取消，不接受自行构造的路径，也不声称取消会回滚已完成效果。
回收站动作使用 files.deleteCurrentSelection 与 operations.own。
当前选择传输到显式目标使用 pulse.fileops V2 和 submit_to_directory，声明
files.transferToDirectory、files.readDirectory 与 operations.own。若需从任意
已打开目录快照选择文件，使用 V3 submit_entries，并声明
files.transferFromDirectory、files.transferToDirectory、files.readDirectory、
operations.own。刷新源快照后重新列举，不能复用旧 entry ID。保留目录打开请求
和文件操作请求各自的生命周期，句柄关闭后按契约继续查询/取消已受理任务。
V4 目录树复制只接受当前快照目录 entry，声明 files.transferDirectoryTree、
files.transferToDirectory、files.readDirectory 与 operations.own。用
submit_directory_tree 提交并轮询 get；对每个文件读取 total_items 项的
get_item，先查询 path_bytes，再读取 UTF-8 相对路径与该项状态。已有目标根
不合并不覆盖，逐项失败不能当成回滚。当前 API 不支持任意字符串源路径、
永久删除或目录树移动。
```
