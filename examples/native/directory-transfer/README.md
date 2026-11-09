# C++ 原生目录传输示例

示例只使用公开 SDK 头，组合 `pulse.ui` V1、`pulse.directories` V1 和 `pulse.fileops` V4。DLL 可打开并枚举显式源目录快照，按 entry ID 提交普通文件复制/移动或目录树复制；同时保留 V2 当前 Pulse 选择传输按钮。宿主负责目录 I/O、快照身份/元数据复核、异步准备、原生确认、任务执行与取消。

在 x64 MSVC 开发环境中运行：

```powershell
cmake -S examples/native/directory-transfer -B build-native-directory-transfer -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-directory-transfer
```

输出为 `pulse_directory_transfer.dll`。使用作者 CLI 的 `native-manifest`、`pack`、`verify`、`install --allow-native` 和 `enable --allow-native` 安装，再通过正常增强包菜单打开。详情见 [原生包指南](../../../docs/native-packages.md)。

输入源目录绝对路径并选择“Open and list source”，然后输入目标目录路径并选择“Open target”。插件保留属于同一 attachment 的目录 ID；源打开后通过不可变快照读取至多 64 行。选择快照文件名后可执行 V3 复制/移动，选择普通目录名后可执行 V4 递归复制，也可用独立 V2 按钮传输 Pulse 当前选择。V4 完成后逐项查询并显示相对路径及结果。插件不自行枚举或写入路径。

Pulse 当前选择路径限制为 1–64 个普通文件。V3 快照路径按当前快照的唯一 entry ID 提交，要求快照未刷新、源/目标句柄同属本 attachment；宿主重验文件大小与修改时间。V4 只复制，不移动；最多 128 个目录、512 个文件和 1 MiB 计划路径。每次传输都由宿主显示原生确认。执行前检查目标根，已有同名项时请求失败且不覆盖；执行期间的外部变化仍可能产生部分结果。目录枚举支持 UNC 不代表受控传输支持 UNC；其他边界见 [文件操作接口](../../../sdk/native/README.md)。

“Refresh status”按请求 ID 手动查询；终态读取后清除本地请求 ID。“Cancel request”发出尽力取消，不能撤销已完成效果。状态词与 `file-operations` 示例一致，包括 `waiting`、`running`、`completed`、`partial`、`cancelled` 和 `failed`；只有 `completed` 表示所有项目成功。原生受理请求的终态会保留到终态读取消费或 attachment 关闭，每 attachment 最多 32 个未消费结果；普通任务最近 16 项历史淘汰不会删除这些传输结果，详见[文件操作接口](../../../sdk/native/README.md)。

请求未结束时仍可关闭目标。准备/确认期间关闭会使宿主取消请求；任务已经提交后关闭目标不取消执行。关闭保留请求 ID 和当前状态，因此仍可刷新或取消；没有活动请求时状态变为 `target-closed`。停用时先撤销按钮订阅，再取消自己的文件操作与目录打开请求，关闭目录和面板；回调捕获异常，不跨 ABI 抛出。

同路径目录在 worker 准备前被替换时，沿用路径解析；准备后由受控引擎验证目标对象身份。定向验收入口为 `tools/extensions/test_native_target_ui.py`，仅使用该脚本创建的隔离样本并保存应用内截图。

```text
AI 作者提示：编写显式目录传输的 C++20 Pulse 插件，先读 fileops.h、
directories.h、api-catalog.json 和文件操作指南。若从源目录快照选文件，
声明 files.transferFromDirectory、files.transferToDirectory、
files.readDirectory、operations.own；用 pulse.directories V1 打开源/目标，
枚举当前快照，再查询 pulse.fileops V3 并按 entry_id 调用 submit_entries。
若复制目录树，改用 V4 submit_directory_tree 并额外声明 files.transferDirectoryTree；
终态逐项调用 get_item，先查询 path_bytes，之后解析 UTF-8 相对路径。
所有 SDK 调用在 UI 线程执行，I/O 留给宿主；保留请求 ID，读取终态后释放。
刷新源快照后重新列举，不复用旧 entry ID。关闭任一目录会取消尚未提交的
准备/确认请求；任务受理后使用请求 ID 查询或取消。始终展示原生确认、部分
结果、失败和取消，不声称原子事务、UNC 或取消回滚。
```
