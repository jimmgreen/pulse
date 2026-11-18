# 声明式命令与资源协议维护参考

本文件约束公开接口的维护方式。当前公开集合是 **19 个 command capability、10 个 resource operation、6 个独立 resource permission**；数量与名称以生成的[能力台账](extensions-capability-inventory.md)及机器发现结果为准。维护者维护宿主接口、权限和验证，作者与 AI 组合这些接口；不能由内部函数存在推导出新的公开能力。

本文根据当前源码建立模块与验证入口映射，不是测试执行报告。“已实现”表示存在严格解析与真实宿主接线；不代表每个平台、每种交互都已验收。这些 JSON 契约不能直接当作原生 C++ 接口清单；原生作者以 sdk/native 的公开头为准。当前阶段与证据见[主路线图](../Pulse-增强包开放能力与AI协同实施方案-2026-11-02.md)。

## 作者和 AI 应读取什么

1. 先调用只读开发工具 `describe_extension_api`，或运行 `pulse.exe --extension-api`，读取当前宿主能力、参数、预算、资源契约、模板和 feature 标记。版本号相同也不能假定旧宿主含全部后来新增能力。
2. 按当前 Schema 生成清单，再调用 `validate_extension_manifest` 或 `pulse.exe --extension-validate-json <绝对路径>`。TypeScript 正确不代替原生验证：UTF-8 字节预算、跨节点/命令引用、主体归属和运行时可用性由宿主检查。
3. `get_extension_template` 只返回作者模板；它与另外两个开发工具均不执行增强包命令。MCP 发现资源操作不等于存在资源执行工具；不得伪造 CallerLease、packageId、owner 或路径参数。
4. 只声明实际使用的 commands/resources。`resources` 是独立权限集合，不从文件命令、只读 UI 绑定或旧清单隐式获得。工作流只能引用本包已声明命令，不增加权限。
5. 不支持的需求应报告缺少哪个公开接口。不能改用内部 action ID、任意 Shell、DLL、文件路径或脚本字段绕过边界。

## 权威来源与版本

| 维护对象 | 权威输入 / 实现 | 同步输出或使用者 |
| --- | --- | --- |
| command、workflow、resource permission、UI 清单结构 | `schemas/extensions/manifest.schema.json`；`src/app/extensions/manifest.h/.cpp`、`ui_tree.h/.cpp` | 原生解析、SDK 清单类型、嵌入 Schema |
| 8 个资源请求结构 | `schemas/extensions/resource-request.schema.json`；`resource_contract.h/.cpp` | `ResourceRequest` 类型、开发工具描述、`ResourceService::Call` |
| 8 个资源结果结构 | `schemas/extensions/resource-response.schema.json`；`api_metadata.json` 中引用该文件 | 逐 op SDK 类型与发现 responseSchema；字段必须对应 resource_service/audit_log/resource_settings 实际输出 |
| 公开说明、效果、能力发现、资源权限映射及预算 | `schemas/extensions/api_metadata.json` | `api_description.generated.h`、`sdk/typescript/api.generated.d.ts` |
| 清单类型与模板嵌入 | `tools/extensions/generate_sdk.py`、`template_catalog.py` 及已登记模板 | `manifest_schema.generated.h`、`manifest.generated.d.ts`、`templates.generated.h` |
| CLI / MCP 三个开发工具 | `developer_contract.cpp`、`developer_tools.cpp`、`developer_tool_catalog.h`、`developer_cli.cpp` | 共享契约及 `src/ai_host/mcp_tools.cpp` 注册；禁止独立手写第二套公开枚举 |
| 调用者身份与撤销 | `authorization.h/.cpp`、`runtime.h/.cpp` | command、resource、workflow、受控任务的统一主体绑定 |
| 台账 | `tools/extensions/generate_capability_inventory.py` | 本目录 `extensions-capability-inventory.md`，不得单独改生成表 |

当前 manifest `apiVersion=1`，UI 树另有 `version=1`；资源发现 `resourceApi.version=1`，资源请求对象本身**没有** `version` 参数。metadata 的 `formatVersion` 是生成输入格式，包内容 revision 是内容身份，二者都不是公开 API 大版本。当前没有已经实现的 v2 协议协商、弃用字段或自动迁移框架。

宿主发现现在提供可选 `contractIdentity`，MCP/助手/CLI同源，SDK可用 `compare-sdk` 比较旧基线与目标宿主；它不会把同为v1判为兼容。指纹算法、返回字段和迁移规则见[兼容检查](extensions-compatibility.md)。全部内部动作的逐项开放决策由 `schemas/extensions/action-exposure-policy.json` 维护并生成台账；计划公开项不授予任何权限。

以下命令表每项版本均为 manifest v1，状态均为“已实现的声明式宿主命令”。统一入口是 `CapabilityService::Invoke`：检查宿主持有的 CallerLease、窗口 owner、包身份和已声明 command index，再分派真实宿主行为；`Available` 不能替代调用时校验。`Dispatched` 只表示已分派，不保证页面发生变化或异步任务完成。

## 19 个命令的维护归属

| capability / v1 参数 | 实现归属与真实宿主调用 | 权限、确认与效果 | 直接验证入口 |
| --- | --- | --- | --- |
| `navigation.back`；无 args | `capability_service.cpp` → `app_navigation.cpp::GoBack` | 本包声明；调用者窗口的历史后退，无独立确认；可记录正常导航历史 | `pulse_extension_manifest_test` 的合法/非法 args；`pulse_extension_developer_contract_test`；有后退历史的真实窗口定向检查，不能以解析通过代替导航结果 |
| `navigation.forward`；无 args | 同上 → `GoForward` | 本包声明；前进历史，无独立确认 | 同上；另检查历史为空时行为与宿主一致 |
| `navigation.up`；无 args | 同上 → `GoUp` | 本包声明；父目录导航，无任意调用者路径 | manifest 测试；`live_workflow_check.py --page workflow-navigation` |
| `view.refresh`；无 args | 同上 → `RefreshActiveTab` | 本包声明；刷新当前 tab，不是任意路径枚举权限 | manifest 测试；`live_reference_check.py --page resource-client` 的真实 snapshot 刷新/引用失效；workflow-navigation |
| `view.setMode`；`args:{mode}` | `view_mode_mapping.h` → `app_commands.cpp::SetViewMode` | 本包声明；8 个固定模式枚举，状态可按宿主偏好持久化；无独立确认 | manifest 枚举/错误参数测试；workflow-navigation 的实际 details 切换 |
| `workspace.setLayout`；`args:{layout}` | `workspace_layout_mapping.h` → `app_navigation.cpp::ApplyLayoutPreset`，随后 `SyncExtensionDock` | 本包声明；single/side_by_side/stacked/three/four；可能持久化布局，禁用包不自动恢复 | manifest 映射/枚举测试、开发契约测试；真实布局切换需按改动范围验证 pane/tab 保留及窄布局 |
| `files.copySelectionToOtherPane`；无 args | `transfer_ui.cpp` → `transfer_preparation` / `transfer_plan` → `ops_manager` / `controlled_transfer` | 本包声明；捕获当前选中普通文件与另一 pane，最多 64 项；每次真实原生确认；worker 重验身份，不接收调用者路径，不覆盖冲突目标 | `pulse_extension_transfer_preparation_test`、`pulse_controlled_transfer_test`；`live_transfer_check.py`；resource-client 的真实受控任务读/取消 |
| `files.moveSelectionToOtherPane`；无 args | 与 copy 共用链路，operation=Move | 同上；移动可能部分完成，不能把拒绝/取消描述为已提交动作回滚 | 同上，`live_transfer_check.py` 含真实 move；保留失败/源保留结果 |
| `workflow.run`；`args:{workflow}` | `workflow_runtime.cpp`、`workflow_controller.cpp` → 同一 CapabilityService | 本包 workflow ID；最多 8 个 workflow，每个 1–16 个命令引用；步骤禁 run/cancel、未知引用和递归；传输步骤各自确认；已完成步骤不回滚 | `pulse_extension_manifest_test`、`pulse_extension_workflow_test`；`live_workflow_check.py` 的 navigation / transfer 两分支 |
| `workflow.cancel`；无 args | `capability_service.cpp` → `workflow_runtime.cpp::CancelWorkflow` | 本包声明；仅本主体运行中的 workflow；停止后续步骤，并请求取消它持有的当前传输；不取消其他主体任务，不撤销已完成步骤 | workflow 控制器测试、原生参数负例；`live_workflow_check.py --page transfer-workflow` 的真实取消/部分完成 |
| `tabs.new`；无 args | `focused_workspace_commands.cpp` → `OpenNewTab` | 当前owner窗口顶栏layout tab；按宿主新标签偏好打开，64标签上限 | focused_workspace模型；live_workspace_check真实新建及caller拒绝 |
| `tabs.duplicate`；无 args | 同上 → 现有 `TabController` 类型化复制入口 | 复制当前聚焦目录到新单pane标签，不深拷贝多pane布局；64标签上限 | 真实双pane聚焦位置复制、目录与单pane结果 |
| `tabs.closeActive`；无 args | 同上 → `CloseActiveTab` | 拒最后一个或固定标签，不能关闭owner窗口；不隐式回滚已完成文件任务 | 真实关闭、最后/pinned保护及撤销 |
| `tabs.activate`；`args:{index}` | 同上 → `SwitchTab` | index整数0..63，执行时再次检查实际顶栏顺序与范围；加载由宿主处理 | manifest/模型范围；真实激活及工作流加载替换 |
| `panes.focus`；`args:{index}` | 同上 → `FocusPane` | index整数0..3，按活动layout的VisiblePanes顺序；不访问隐藏pane/其他窗口 | 真实聚焦、越界、owner与撤销 |
| `selection.selectAll`；无 args | `focused_directory_commands.cpp` → 宿主CollectFilterMatches/SelectIndices | 本主体聚焦普通目录ready状态；当前可见过滤集合，不接收路径，不修改文件 | manifest/映射测试；live_focused_directory_check.py真实按钮与过滤结果 |
| `selection.invert`；无 args | 同上 → InvertIndices | 同一范围内反选，其他窗格保持原状，不增加文件操作权限 | 同上 |
| `selection.clear`；无 args | 同上 → ClearSelection | 清空当前目录选择；不影响其他窗格，不修改文件 | 同上，空目录/无选择状态 |
| `view.setSort`；`args:{column,direction}` | 同上 → SetSort及工作流精确完成观察 | 七列与双方向枚举；仅聚焦ready普通目录，可能保存正常目录视图偏好；不改变全局排序默认值，停用不回滚 | 参数与枚举负例；真实14组合顺序/选择保留/其他窗格隔离/工作流等待 |

具体模式列表由 Schema 的 `viewModes` 对应定义与发现结果提供；不能根据宿主 enum 数值编写清单。命令未知字段及不允许的 args（包括空对象）均应被拒绝。

## 10 个资源操作的维护归属

全部版本为 `resourceApi.version=1`，状态均为“已实现的宿主资源服务；原生适配按公开 C ABI 提供”。入口为 `ResourceService::Call(AppState&, CallerLease, request, completion)`，来源是宿主持有主体；资源请求不能传 AppState/CallerLease/路径。统一响应为 `{ok:true,data:...}` 或 `{ok:false,error:{code:...}}`。逐操作结果定义位于 `schemas/extensions/resource-response.schema.json`；按 `x-operationResponses[request.op]` 选择响应定义验证。响应自身没有 op 字段。SDK 提供 `ResourceResponseMap`、`ResourceResponse<"files.stat">` 和 `ResourceResponseFor<Request>`，成功分支按请求操作关联结果，失败分支统一。uint64 字节数/任务 ID/FILETIME/审计计数保持十进制字符串；不先转可能损失精度的浮点数。Schema 限制 uint64 上界、settings version 安全整数及 item state 0–5；TypeScript 的 string/number 不会检查数值范围或 UTF-8 字节数，仍需 Schema/宿主验证。默认 `ResourceResponse` 现在是已知结果并集，不再是任意 JsonValue：这是 SDK 类型收紧，原来构造缺字段对象的调用方须修正，不改变运行时包络。

| op / 参数 | 必须声明的 permission | 实现模块与语义 | 确认 / 直接验证入口 |
| --- | --- | --- | --- |
| `context.get`；无其他字段 | `context.read` | `resource_contract.cpp` → `resource_service.cpp` / `CapturePanelContext`；返回当前 pane 目录、选中数量/名称、viewMode、loading | 无额外确认；`pulse_extension_resource_contract_test`、resource-client 实际读取和 owner 拒绝 |
| `files.list`；可选 cursor、limit | `files.readCurrentDirectory` | 同服务读取当前普通目录不可变 snapshot；limit 1–256，默认 64；返回 entries、nextCursor、total，不执行任意路径 FS 访问 | 无额外确认；契约测试、resource-client 分页/刷新过期/任意 path 拒绝 |
| `files.stat`；entry | `files.readCurrentDirectory` | 同服务用不透明 entry 取同一 snapshot 元数据；引用绑定主体、owner、snapshot，5 分钟过期；不接受文件路径 | 无额外确认；契约测试、resource-client 同快照成功/刷新后 stale_reference |
| `operations.get`；taskId 字符串 | `operations.own` | `resource_service.cpp` → `ops_manager.cpp::QueryControlled`；正数规范十进制 uint64；返回取消请求、终态、完成标记、字节计数与逐项结果；未知/不归属均 not_found | 无额外确认；契约测试、resource-client 真实运行/Cancelled/Copied、跨 owner/主体 incarnation 拒绝 |
| `operations.cancel`；taskId 字符串 | `operations.own` | 同服务 → `CancelControlled`；只向本主体真实任务发取消请求，返回 accepted；之后用 get 读取最终结果 | 不再次显示传输确认；原任务创建已有原生确认；resource-client first-write checkpoint 的真实取消与 partial staging 清理，不用 mock executor |
| `settings.get`；无其他字段 | `settings.own` | `resource_service.cpp` 将任务交 worker → `resource_settings.cpp::ReadOwnSettings`；按包隔离持久化，返回 version/values | 无额外确认；契约/设置测试、resource-client 实际持久化读取与撤销抑制回调 |
| `settings.set`；key、value、expectedVersion | `settings.own` | 同服务 → `WriteOwnSetting`；value 字符串或 null 删除；CAS 版本；最多 64 key、key ASCII 1–64、值 UTF-8 ≤1024 bytes、整份 ≤16 KiB；版本 ≤2^53−1；拒损坏、路径重解析、过期授权 | 无额外确认；`pulse_extension_resource_settings_test` 的跨包/CAS/锁/损坏/取消/路径测试；resource-client Unicode、冲突、删除与源文件不变 |
| `audit.list`；可选 limit | `audit.own` | `audit_log.cpp` → 同服务；本主体会话记录，limit 1–128，默认 64；全局有界 1024；只记录操作与结果，不记录请求 payload 或路径 | 无额外确认；契约测试、resource-client 范围和敏感载荷不泄露 |

另有 `files.prepareRename` / `files.commitRename`，权限为 `files.renameCurrentDirectory`：插件提交自己的命名结果，宿主准备有界、到期且一次使用的计划，确认后提交真实操作任务。参数以 resource-request.schema.json 为准；实现与定向验证见 `rename_service.*`、`pulse_extension_rename_plan_test`、`pulse_extension_rename_ops_test` 及原生重命名示例。

资源调用撤销闭包取消等待/订阅，不承诺撤销已提交持久化写入。任务取消不等于事务回滚；get 的 terminal/completed、逐项 state/error/cleanupError 才是操作结果。绑定失效后，异步结果不再回调旧 owner。不要把扩展设置映射为宿主全局偏好编辑器。

## 不兼容变更与弃用流程

以下是后续维护规则，不是当前自动化版本协商已经实现的声明。

- 修改现有参数类型、必填性、默认值、枚举含义、字节预算、权限范围、确认要求、归属/撤销语义、错误含义、结果字段含义或取消/终态语义，都应做兼容评审。删除响应字段、把异步成功解释为完成、把 opaque ID 改成路径，均属破坏性变更。
- 新增可选能力不意味着旧宿主会接受：原生解析严格拒绝未知字段/枚举。作者必须 feature-detect；新增能力、模板和 SDK 要同时标出最低支持宿主/发布版本，不能只写 apiVersion=1。
- 优先新增能力/版本而不是静默改变已有接口。确需不兼容：先记录受影响作者输入和结果、替代契约、迁移例子与回退方案；实现明确的版本选择/拒绝路径后再发布。不能先写 apiVersion=2 却让现有 v1 parser 接受新含义。
- 弃用必须先在维护文档、机器发现 metadata 和 SDK 发布说明中公告替代项及明确移除版本/条件；当前没有 deprecation 字段，若需要机器可读标记，须作为独立兼容变更同步设计。旧版本支持期间保留原生和 SDK 回归 fixture，禁止为取得通过而删除旧输入测试。
- 安全收紧可拒绝原本危险的输入，但须记录触发条件、错误与迁移方式；不得绕过确认、暗中扩权或为兼容退回不安全实现。撤销旧 revision 的授权与资源主体，不能让新包继承过期调用票据。

## 一次接口变更的同源检查

1. 在上述表定位负责模块，定义请求、结果、权限、状态和失败边界，先补能暴露变化的正负 fixture。保持 UI/内部布局实现不泄露为公开指针或系统句柄。
2. 同步 Schema、metadata 与原生 parser/实现；新增资源时更新独立 resources 权限映射，新增命令时同步 capability dispatch、审计名称和包升级权限差异。原生 UTF-8 字节限制不被 JSON Schema 字符数自动替代。
3. 运行 `python tools/extensions/generate_sdk.py`；生成文件不手改。若生成器不支持新 Schema 结构，先扩充生成器和负例，不能悄悄丢弃约束。台账入口由 `python tools/extensions/generate_capability_inventory.py` 更新。
4. 检查无漂移并运行与本次变更相关的 Python/TS 契约检查。按需选择，而非每次全部重跑：

```powershell
python tools/extensions/generate_sdk.py --check
python tools/extensions/generate_capability_inventory.py --check
python -m unittest discover -s tools/extensions -p test_generate_sdk.py
python -m unittest discover -s tools/extensions -p test_template_catalog.py
python tools/extensions/typecheck_sdk.py --tsc tools/extensions/typecheck/node_modules/typescript/lib/tsc.js --output bench_data/<新的类型检查目录>
```

TypeScript 命令使用工程锁定且已准备好的编译器；路径不存在应先遵循 SDK 依赖准备流程，不以跳过类型检查冒充通过。输出目录替换为本次独立路径。

5. 仅构建/运行受影响原生目标：`pulse_extension_manifest_test`、`pulse_extension_resource_contract_test`、`pulse_extension_developer_contract_test`、`pulse_extension_developer_tools_test`、`pulse_extension_developer_cli_test` 中相关项；状态机、设置或受控传输变化再选各自目标。开发契约测试保留旧 API fixture，并检查 CLI/MCP 与共享发现契约的一致性。
6. 对行为变化执行表内真实宿主入口，使用隔离 profile 与新的输出目录。常用命令如下，**不表示本次文档任务执行过它们**：

```powershell
python tools/extensions/live_reference_check.py --pulse build-extensions/pulse.exe --output bench_data/<新的资源验收目录> --page resource-client
python tools/extensions/live_workflow_check.py --pulse build-extensions/pulse.exe --output bench_data/<新的工作流验收目录> --page transfer-workflow
python tools/extensions/live_transfer_check.py --pulse build-extensions/pulse.exe --output bench_data/<新的传输验收目录>
```

resource-client runner 仅在它的隔离 fixture 清单中追加 copy 命令以创建真实待查询任务；生产资源模板不因此获得文件命令。验证源/目标哈希、取消残留、实际主体拒绝和终态，不只检查截图存在或编译成功。

7. 交接记录修改的公开接口、实际运行的检查与日志、未执行/环境失败项及兼容影响。更新功能支持状态只依据实现与验收证据，不能靠类型已生成或模板已添加宣称上线。

## 明确不属于公开增强包接口的内容

`src/app/actions/action_inventory.h` 的 138 个内部 action、菜单动态项、MCP 自有 `run_actions`、私有 AppState 和 UI/FS helper 都不是额外 command capability。内部 action 的风险/策略由其所属模块维护，公开一个新包装仍须完成本契约的 Schema、权限、确认、归属和测试链路。MCP 的 9 个 AI 工具与 3 个增强包开发工具各有原有授权，不互相授予权限。

公开 UI 组件树、local actions、dataSource 是另一组有界声明契约，不等于 resource transport 或通用脚本。原生 helper 可以内部扩展，只要公开行为不变，不需要作者跟着内部重构；公开行为变化则不能只改内部实现而不更新此维护链路。
