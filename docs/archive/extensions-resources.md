# Resource API v1：宿主持有的资源服务

本页描述宿主内部 `ResourceService::Call` 的 JSON 协议，不是 C++ 插件直接包含的接口。请求与 TypeScript 类型来自 `schemas/extensions/resource-request.schema.json`；请求中的未知字段一律拒绝。C++ 作者使用公开的 `pulse.files` / `pulse.resources` 函数表，支持范围见 [原生 SDK](../../sdk/native/README.md)。MCP 可发现契约，但不能提交 packageId、owner、principal 或 ticket 来取得增强包身份。JavaScript 执行桥已移除；声明式 resource-client 面板仅声明权限，不自行执行请求。

## 调用与生命周期

宿主以有效 CallerLease 调用，核对包启用代次、版本和窗口，再检查 manifest.resources。返回 `{ok:true,data:...}` 或 `{ok:false,error:{code:"..."}}`。除自有设置外，读取来自 UI 线程已有快照，不在 UI 线程进行文件系统访问。设置任务在 worker 执行，结果回到 UI 线程；超时30秒，同时最多64项。取消、关闭窗口、停用包或撤销授权后丢弃迟到回调。取消不撤销已经原子提交的设置。

## 操作结果

| 操作 | data |
| --- | --- |
| context.get | directory、selectionCount、selectionName、viewMode、loading |
| files.list | entries、nextCursor（无下一页为空字符串）、total |
| files.stat | name、sizeBytes、directory、reparsePoint、attributes、modifiedFileTime |
| files.prepareRename | planId、expiresInMs、rows（entry、originalName、newLeafName、status） |
| files.commitRename | taskId、status="submitted"；宿主确认后提交，不代表任务已完成 |
| operations.get | taskId、cancelRequested、terminal、completed、transferredBytes、totalBytes、items |
| operations.cancel | taskId、accepted；终态返回false，不推断操作失败 |
| settings.get / settings.set | version、values（键值对象）；写入成功返回新版本及全部当前值 |
| audit.list | 新到旧的事件数组：sequence、monotonicMs、operation、outcome |

整数计数中的文件字节数、FILETIME、任务ID、审计序号和时钟均用十进制字符串避免 JavaScript 精度丢失。FILETIME 是 Windows UTC 100ns 时间值；monotonicMs 只适合本次宿主运行内排序，不是日历时间。

files.list 默认64条，上限256；每条比 stat 多一个不透明 entry。引用有效期5分钟，绑定包身份、窗口和目录快照。刷新后的旧引用返回 stale_reference；搜索结果、虚拟目录和未完成加载不提供普通目录读取。宿主最多4096个存活引用，超额返回 reference_budget，不允许客户端改用绝对路径绕过。

operations.get 只查询本包当前启用代次和窗口的受控传输。未知任务与他包任务均返回 not_found。items 中 state 的 v1 映射为 0=notStarted、1=copied、2=moved、3=failed、4=cancelled、5=destinationCommittedSourceRetained；error/cleanupError 为 Win32 数值错误码。最后一种状态意味着目标已提交但源仍保留，不能算移动成功。terminal 只表示任务已经结束，completed 才是整体成功；取消可能晚于某项提交，不保证回滚。任务历史有界，查不到不能推断成功。

settings.set 必须先读取 version，再原样提交 expectedVersion。冲突返回 version_conflict 后重新读取，不能盲目重试覆盖。key 为1–64个 ASCII 字母、数字、下划线、横线或点；value 为最多1024 UTF-8字节字符串，null 删除。最多64键，序列化数据最多16KiB。存储使用跨进程锁、原子替换和提交前授权检查，拒绝链接逃逸，不修改 Pulse 偏好。

审计是内存会话环，不是持久合规日志：全局最多1024项、单次返回自身最多128项。只记录操作及结果码，不记录请求内容、路径或设置值；其他包的事件不可读取。

## AI 客户端错误处理

- invalid_request：按发现的 schema 修正请求，不删掉宿主校验。
- invalid_caller / wrong_owner / permission_denied：停止调用，由宿主重新建立合法会话或由用户更改授权；不得自造身份。
- stale_reference：重新读取当前目录首页并取得新引用。
- loading：等待目录完成；unsupported_context：该上下文不支持此接口。
- version_conflict：重新读取设置并重新决定修改。
- request_budget / reference_budget：减少并发、分页和重复请求，不能自动提升配额。
- timeout / io_error / corrupt_data / unsafe_path：保留错误，不把未知结果视为成功；写入是否提交须重读确认。

## 验证证据

`bench_data/platform-resource-host-20261104-01`：实际宿主28项通过、6张应用内截图，涵盖分页/stat、旧引用、伪造及跨窗口主体、自有设置CAS/Unicode/删除、会话审计和撤权迟到回调。原生设置测试通过；文件符号链接创建因系统权限不足跳过，不能计为通过。补充验收 `bench_data/platform-resource-host-20261104-02` 已通过48项及6张应用内截图，覆盖真实受控文件任务运行态/取消/终态、跨窗口拒绝、旧窗口主体不可查询前任任务；校验源文件不变、已完成目标哈希一致、取消无暂存残留。该结果替代首轮计数，不与28项重复累加。

历史补充：`bench_data/platform-resource-host-20261104-03` 为49项通过、6张应用内截图，加入真实Details模式返回稳定 `details` 的检查。28/48/49均保留为各自阶段证据，不与后续复验计数累加。

本轮实际结果：`bench_data/platform-resource-lifecycle-20261104-01/checks.json` 的主阶段为 **57 PASS、0 FAIL、退出0、6 PNG**；独立lifecycle阶段为 **16 PASS、0 FAIL、退出0**，不额外截图。两阶段均记录 `noSearchHelpers:true` 和fixture不变；主阶段另确认资源源文件不变、复制目标哈希一致、取消目标及暂存已清理。详细断言分别在 `resource-client/capture/report.txt` 与 `resource-client/lifecycle/capture/report.txt`，不能把主阶段和lifecycle计数当作互不重叠的功能数量。

纯模型日志 `build-extensions/platform-next-resource-reference-01.log` 为 **21 PASS、0 FAIL**，使用生产引用存储与真实授权Registry，覆盖跨包/owner、重载换代、snapshot/path/引用种类、精确到期、容量恢复与快照释放。TTL测试注入内部单调时钟，检查299999 ms仍有效、300000 ms失效及读取不续期；**没有真实等待300秒**，没有更改系统时钟或生产300000 ms TTL，也没有向第三方开放时钟控制。

## context.get 的机器视图模式

资源结果 `data.viewMode` 使用与 manifest `view.setMode.args.mode` 相同的稳定值：`extra_large_icons`、`large_icons`、`medium_icons`、`small_icons`、`list`、`details`、`tiles`、`content`，不随界面语言变化。没有活动 tab（或宿主模式尚无公开映射）时为 JSON `null`，不是空字符串。Schema 和 `ResourceResponse<"context.get">` 同步约束这些值。

UI 的只读绑定 `context.viewMode` 仍是本地化展示标签；不要用该展示文字进行机器分支。资源使用者应处理 null，并用稳定枚举判断模式。

## 引用生命周期的定向验证

生产 `ResourceReferenceStore` 共用于 entry 与 cursor：固定 300000 ms 有效期、不因读取续期、全局最多4096条；引用同时绑定真实授权主体（包revision/generation和owner incarnation）、不可变snapshot、目录与引用种类。过期、刷新、同包重载、跨包或跨owner后返回 stale_reference；旧 CallerLease 本身失效时先返回 invalid_caller。关闭owner时清除其引用，不影响其他owner。

内部纯模型允许测试代码注入单调时钟，生产仍使用 GetTickCount64；该接口不进入清单、SDK、MCP或任何第三方请求。`pulse_extension_resource_reference_test` 使用同一生产实现与真实授权Registry，在299999/300000 ms边界验证entry/cursor到期、重载/owner换代、snapshot替换、类型混用、容量及快照释放，无须等待五分钟，也不缩短生产TTL。

`live_reference_check.py --page resource-client` 的真实验收分两阶段：双包store在同一owner验证双向引用、任务和设置隔离，保留原六张主题/DPI截图；独立 `--extension-dev` 阶段由worker改动隔离清单并等待真实开发watcher提交新generation，验证旧lease/ref拒绝、新引用可用、同包持久设置保留，以及关闭/撤权后的异步回调抑制。第二阶段不额外截图，结果单列在checks.json的lifecycle与lifecycle/capture/report.txt。

上述指定报告已完成本轮实际运行，结论限于记录中的检查。模型时钟测试不冒称真实等待五分钟；resource owner释放与撤权回调验证观察实际worker闭包释放，不冒称覆盖所有真实窗口销毁竞争。此次文档更新只核验已有报告，未重新构建或运行UI。
