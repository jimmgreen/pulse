# 对话框运行时拆分计划（2026-11-05）

## 本轮交付边界

本轮只实现安装升级时的旧版本清理及现代占用提示 UI。尚未完成 `Runtime/<digest>` 目录迁移，也尚未实现固定轻量 COM 代理与完整跨进程对话框架构。不得以目录清理或打包完成推断第三方程序兼容性。

本文件基于代码只读检查形成；未构建、未运行功能测试。行号是检查时定位，后续修改应以符号为准。

## 现有架构及证据

| 文件 / 符号 | 已存在的行为及边界 |
| --- | --- |
| `tools/pack_dialog_experiment/pack.py::package_installed`（约 236 行） | 根目录已仅放稳定启动器，完整运行文件位于一个版本闭包；目录名仍为 `DialogBridge/<digest>`。 |
| 同文件 `package_addressed`、`verify_installed`；`src/common/dialog_bundle.cpp::ResolveInstalledBundle`（约 270 行） | 发布和解析均严格匹配 `DialogBridge/<digest>`，目录迁移必须同步发布器、解析器、安装器及测试。 |
| `src/shell_bridge/file_dialog_auto_server.cpp::Services`（约 218–225 行）与 `Provider`（约 148–159 行） | 从 COM 模块自身目录定位 `pulse.exe`，加载并缓存 in-process UI provider。模块自身还在约 105 行 PIN。 |
| `src/shell_bridge/file_dialog_inprocess_session.cpp::Prepare`（约 179–183 行）；`src/picker_inprocess/picker_ui_bootstrap.cpp`（约 60 行） | provider 实现模块被 PIN；释放引用不等于解除版本目录占用。 |
| `src/ipc/picker_protocol.h` | v1 只有 `Request / Response / Cancel`，只携带基础文件选择参数和结果。 |
| `src/app/picker_host_mode.cpp`（约 79、93 行） | 活动期间只接收取消命令，独立显示 `ShowFilePickerDetailed(nullptr, ...)`。 |
| `src/shell_bridge/file_dialog_wrapper.cpp::Show`（约 89、102–125、163–168 行） | events、自定义控件、扩展 FOS、LocalSave、labels、places、properties 等依赖 `inprocess_provider`。 |
| `src/shell_bridge/file_dialog_inprocess_session.cpp::Observe`（约 266 行） | 要求事件 HWND 属于当前 PID、当前 STA。远程 HWND 不符合现有会话契约。 |
| `src/picker_inprocess/picker_ui_api_v2.h`（约 290–338 行） | 契约包含同步 STA 回调、Query/GetWindow/Close 重入、generation、veto、Close 优先及候选失效语义。 |

现有 out-of-process 路径可复用启动、管道、取消、调用者消息泵和基础结果构造。它不包含完整 V2 参数及回调语义，不能仅切换 `run_bridge` 后宣称完成迁移。

## 目标架构

- QQ、微信、PS 等调用进程仅驻留固定轻量 COM 代理；native backing object、COM sinks 及 wrapper 身份仍由调用 STA 管理。
- UI 与相关依赖仅加载到 Pulse 自管 host。代理每次 Show 解析当前完整运行包，不缓存版本 UI DLL。
- 应用与对话框共用 `Runtime/<digest>` 完整运行包，通常保留 current + previous；正在使用的包作为暂时保留项显式报告，释放后再清理。
- 固定代理独立于版本运行包；代理自身升级时仍可能被第三方进程占用，需要单独的更新与协议兼容策略。

## 首次迁移

1. 同步打包、指针解析、稳定启动器、COM 注册、安装器和测试，避免新指针引用旧解析器不认识的路径。
2. 保留可恢复的上一版本指针；新包须完整验证后才发布 current。
3. 新激活使用固定代理；已运行进程中的旧 COM/UI 模块仍会存活至相关进程退出，首次迁移不能承诺立即消除旧目录占用。
4. 检查并清理旧 `DialogBridge/<digest>`；明确区分已删除、仍占用及清理失败，不能把目录改名当作卸载已加载 DLL。
5. 新旧组件的协议版本与能力必须显式协商。不兼容应在 UI 显示前作出确定选择；显示之后的失败不得再次弹出原生对话框。

## 下一阶段最小双向协议及实现步骤

1. **会话与能力协商**：请求/session/show 标识、协议版本、已实现且验证的能力、host 身份及存活绑定。保持有界序列化与严格字段校验，不跨进程传原始指针。
2. **完整配置**：native options、Open/Save 策略、过滤器、标题/按钮/文件名标签、默认扩展名、places、properties、自定义控件及 items、初始状态。
3. **事件与回复**：Ready、FolderChanging/Changed、Selection/TypeChanged、FileOk、导航完成、自定义控件/属性事件，以及 Overwrite/ShareViolation 的 HRESULT 与 response。
4. **快照与命令**：带 generation 的 Query、GetWindow、Close、UpdateControl、Navigate；终态包含 accepted/cancelled/closed/failed、精确 HRESULT、结果 paths/filter index 和是否已经显示 UI。
5. **双向等待与重入**：调用端在原 STA 分发 sink；host 等待同步回调时仍处理回调触发的 Query/Close/UpdateControl/Navigate。不得持锁调用用户代码；不得把同步等待变成互相阻塞。
6. **会话抽象**：拆分当前 in-process 假设与通用 wrapper 状态。远程 HWND 需绑定已认证 host/session，不能简单删掉同 PID/同线程验证。保留 generation、veto、Close 优先、候选失效和显示后禁止重开原生 UI。
7. **切换与清理**：完成新 host/provider 路径后才取消调用进程对版本 UI 的加载；验证跨版本更新后再启用对应生产能力。旧 v1 路径不是完整 V2 替代品。

## 兼容验收矩阵

以下都是待验证项，不代表现有测试已通过。优先按新增协议、会话及包装层分层定向验证，再在明确版本的真实程序中验证关键路径。

| 范围 | 必须验证的行为 | 失败判据 |
| --- | --- | --- |
| 基础选择与 FOS | Open/Save/Folder、多选、过滤器、默认扩展名、受支持 FOS 组合与已有验证策略 | 静默丢字段、接受原本应拒绝的组合或改变结果语义 |
| 自定义内容 | labels、places、properties、各支持控件及 items/state；回调内更新控件 | 状态不同步、控件消失、更新失败后两端状态不一致 |
| 事件身份与 STA | 回调在调用 STA，wrapper COM 身份一致，订阅生命周期与顺序符合已验证契约 | 错线程、释放后回调、身份变化或未经验证的顺序假设 |
| 同步回调与 veto | FileOk/FolderChanging veto 保留 UI；Overwrite/ShareViolation 返回值透传 | 提前关闭、丢失 veto、重复提交或错误映射 HRESULT |
| 回调重入 | 回调内 Query/GetWindow/Close/Navigate/UpdateControl；回调泵消息期间状态改变 | 死锁、嵌套事件乱序、旧 generation 候选被接受 |
| 终止与异常 | Close 精确 HRESULT、用户取消、owner 销毁、host/管道中断 | 返回虚假成功、已显示后重开原生窗口、进程或句柄遗留 |
| 窗口与体验 | owner 模态恢复、焦点、DPI/主题、窗口存活；调用方确实依赖的窗口操作 | 错误启用原本禁用 owner、失焦、窗口跨进程假设失效 |
| 运行时隔离 | 调用进程模块清单无版本 UI DLL；host 加载完整同版本依赖 | 第三方仍加载 UI 或混用不同版本资源/模块 |
| 升级与回滚 | 老会话使用原包，新 Show 使用新 current；previous 可恢复 | 活跃包被删除、更新后读取旧缓存或回滚指针不可用 |
| 首次迁移 | 旧进程仍占用时准确报告；退出后旧目录可清理 | 报称已清理但文件仍存在，或将占用推断为数据损坏 |
| 真实应用 | 明确记录 QQ/微信/PS 的版本、位数和 Open/Save/自定义回调场景 | 仅凭编译或通用样例宣称所有应用完全兼容 |

验收记录应分别列实际命令/操作、观察结果与未覆盖场景；构建成功不等于行为兼容。
