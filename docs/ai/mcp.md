# 给 AI 应用：Pulse 的 MCP 接口

本文写给接入 Pulse 的 AI 应用和它们的开发者：有哪些工具、什么情况下 Pulse 会问用户、出错时该怎么办。用户怎么安装和打开，见 [../ai-pack.md](../ai-pack.md)；Pulse 开发者怎么加新动作，见 [actions.md](actions.md)。

## 连接

Pulse 通过 stdio 提供 MCP 服务：`pulse.exe --mcp-stdio`。它只负责转发，所有操作都交给正在运行的 Pulse 执行。用户在「设置 → 增强包 → AI 与 MCP」点「复制配置」，得到的就是下面这段（路径是本机 `pulse.exe` 的实际位置）：

```json
{
  "mcpServers": {
    "pulse": {
      "command": "C:\\Program Files\\Pulse\\pulse.exe",
      "args": ["--mcp-stdio"]
    }
  }
}
```

- **Claude Desktop**：合并进 `%APPDATA%\Claude\claude_desktop_config.json` 的 `mcpServers`，然后重启 Claude Desktop。
- **Claude Code**：合并进项目的 `.mcp.json`，或运行：

  ```
  claude mcp add pulse -- "C:\Program Files\Pulse\pulse.exe" --mcp-stdio
  ```

- 其他支持 stdio MCP 的应用写法相同：命令是 `pulse.exe`，参数是 `--mcp-stdio`。

应用第一次连接时，Pulse 会问用户「允许 / 只允许这次 / 拒绝」。被拒绝的应用每次调用都会收到 `client_not_approved`。Pulse 没有运行、开关没打开或增强包未安装时，`initialize` 和 `tools/list` 照常可用，但调用工具会收到 `pulse_not_running`（可以重试）。Pulse 不会因为 AI 应用的调用而自动启动或安装。

## 工具

当前开发分支共有 12 个工具。`find_actions` 和 `run_actions` 使用与菜单、快捷键、命令面板相同的动作表，执行范围仍受当前授权限制。三个只读开发工具提供增强包发现、模板与校验；它们不会把内部动作表开放成增强包权限。

| 工具 | 参数 | 作用 | 是否询问用户 |
| --- | --- | --- | --- |
| `get_context` | 无 | 用户当前看到的内容：活动标签页的各栏（按屏幕顺序，1 是左边或上面那一栏）、每栏的文件夹、视图、筛选和选择（焦点栏最多 200 个完整路径，其他栏 20 个名称加总数）、其他标签页和窗口、暂存托盘、预览、打开的对话框 | 不询问 |
| `list_folder` | `path`，`limit`（≤500，默认 100），`cursor` | 列出文件夹内容，分页；不读取文件内容 | 不询问 |
| `search` | `query`，`folder`，`type`（file / folder / image / video / audio / document / archive），`modified_after`（YYYY-MM-DD），`limit`（≤200，默认 50） | 按名称搜索 | 不询问 |
| `get_properties` | `paths`（≤50） | 大小、日期等属性 | 不询问 |
| `reveal` | `paths`（≤200） | 在 Pulse 里显示并选中 | 不询问 |
| `create_zip` | `paths`（≤1000），`dest_folder`，`name` | 打包成新的压缩包 | 每次询问，可以「始终允许这个文件夹」 |
| `find_actions` | `query`（几个词），或 `domain: "settings"` | 查找动作，返回参数、当前值，以及本应用能不能用 | 不询问 |
| `run_actions` | `steps`（1–12 步，每步 `{action, args, wait}`） | 按顺序执行动作 | 由每个动作所在的分组决定，见下文 |
| `undo_last` | 无 | 撤销 AI 应用最近一次改动 | 不询问 |
| `describe_extension_api` | 无，传 `{}` | 返回公开能力、绑定、限制及完整 manifest JSON Schema；支持 structuredCapabilityDetails 时附参数、目标及副作用详情 | 不询问 |
| `validate_extension_manifest` | `manifest`（JSON 文本字符串，≤65,536 个 UTF-8 字节） | 复用宿主解析器校验文本，返回 `valid` 和 `error`；不读取文件、不安装或执行 | 不询问 |
| `get_extension_template` | `template`（发现结果中的固定模板ID，不是路径） | 返回经当前宿主校验的内置清单文本、`validated:true` 和 `executed:false`；不安装或执行 | 不询问 |

`wait` 可以取 `listing_loaded`、`op_finished`、`preview_open`。选择类的步骤会自己等文件夹加载完，最多等 1.5 秒。

### 开发增强包

先调用 `describe_extension_api({})`，按照返回的 `manifestSchema`、能力与限制生成清单；有 `declarativeTemplates` 时，可从 `templates` 中选择ID调用 `get_extension_template({"template":"transfer-panel"})`。返回的 `manifest` 是JSON文本数据，可编辑后交给 `validate_extension_manifest({"manifest":"完整 JSON 文本"})`。错误参数返回 `invalid_args`；清单内容不合法返回正常结果 `{"valid":false,"error":"诊断"}`；通过时返回 `{"valid":true,"error":""}`。三工具仍经过既有连接身份与客户端授权，不改变 AI 权限设置。模板中的示例身份应由作者修改，模板校验通过也不代表实际功能已验收。

发现结果中的可选 `developerFeatures` 用于识别开发工具支持；`isolatedDevReload` 表示所选宿主支持显式隔离开发会话，不是新增增强包权限或 MCP 执行工具。旧 v1 宿主可能没有该字段，不能只按 `apiVersion` 猜测支持。完整创建、测试、预览、开发重载、归档及诊断流程见[增强包作者指南](../../sdk/native/README.md)。

`structuredCapabilityDetails` 表示可读取新增的 `capabilityDetails` 和 `bindingDetails`，所有旧字段保留。能力详情包含 `id`、`summary`、`effect`、`target`、`modifiesFiles`、`persistsPreferences`、`supportsCancellation`、`argsRequired` 和 `argsSchema`；绑定详情包含 `id`、`summary`、`displayOnly:true`。客户端先做特性探测，不把旧宿主缺失的详情解释成“无副作用”。

`controlledFileTransfers` 描述两个增强包声明式命令：`files.copySelectionToOtherPane` 与 `files.moveSelectionToOtherPane`。它们不接收参数，要求两个可见本地文件夹窗格、最多64个普通文件及逐次原生确认。这里的发现不增加MCP执行工具，也不能把AI工具的身份、确认或撤销资格转交给增强包。`supportsCancellation:true` 指宿主任务可取消，不表示清单可以填写任务ID或确认布尔。边界与示例见[增强包作者指南](../../sdk/native/README.md)。

当前导航和刷新不修改用户文件内容，`view.setMode` 也不改用户文件内容，但会保存文件夹视图偏好；workspace.setLayout可能保存窗口布局/会话，停用不会还原。`persistsPreferences:false` 仅表示不直接修改可配置偏好；导航仍可记录普通历史或最近位置，不能推断宿主状态/缓存零写入。四种无参能力的 `argsSchema` 为 `null`，view.setMode与workspace.setLayout分别提供完整参数Schema并要求args.mode或args.layout；后者先发现workspaceLayouts，作用调用窗口，设置页拒绝多栏。当前不提供取消能力。AI 生成清单前应核对目标、参数及副作用，再调用校验工具；发现不授予执行权限，新增详情也不新增 MCP 执行工具，不开放内部动作 inventory 或完整 CapabilityService。

校验通过仅代表协议合法，仍需保存清单、本地加载和 UI 验收。完整返回字段、可复制 AI 指令、示例及应用内截图流程见 [增强包 SDK](../../sdk/native/README.md)。部署时应配套更新 Pulse 主程序与 AI 增强包；旧版宿主不具备这两个工具。

### `find_actions` 返回什么

每一行包括：

| 字段 | 含义 |
| --- | --- |
| `id` | 动作 id，`run_actions` 里的 `action` 就填它 |
| `title` | 动作的名称 |
| `args` | 参数和取值说明 |
| `now` | 当前值（有的话） |
| `enabled` / `why` | 现在能不能用；不能用时给出原因 |
| `without_args` | 只出现在文件步骤上：不传参数时现在做不了（例如没有选中项目、只有一栏），但传了 `items` / `dest` 仍然可以执行 |
| `risk` | `read`、`view`、`organize`、`create`、`move`、`system`、`external`、`delete`、`permanent_delete`、`user_only` 之一 |
| `asks_user` / `shows_plan` / `locked` | Pulse 会先确认 / 会先显示计划卡 / 只能用户自己做 |
| `shortcut` | 用户自己的快捷键 |
| `group` | 所在的权限分组（见下文） |
| `allowed` / `why_not_allowed` | 本应用能不能用；不能用时给出原因，例如「整理 Pulse」没打开，用户可以在哪里打开 |

`allowed:false` 的动作不要调用，把 `why_not_allowed` 转告用户即可。`domain: "settings"` 会列出所有设置项，包括 key、可选值和当前值；改设置用 `settings.set {key, value}`。

### `run_actions` 的结果

```json
{
  "ok": true,
  "done": 2,
  "steps": [
    {"action": "view.mode", "ok": true, "result": "…"},
    {"action": "tab.pin", "ok": true, "already": true}
  ],
  "undo": "undo_last undoes this whole call",
  "summary": "…"
}
```

- 每一步有 `action`、`ok`；失败时带 `error`，有结果时带 `result`；`already: true` 表示本来就是这样。
- 改设置的步骤还会带 `from` / `to`；改动 Windows 的设置带 `affects_windows: true`。
- 某一步失败后，后面的步骤不再执行，前面已经完成的保留，结果里会带 `stopped`。
- 碰到只能用户自己做的事，结果里会带 `only_the_user {title, why, where}`。

## 权限分组

用户在「设置 → 增强包 → AI 与 MCP → AI 应用可以做的事」里按分组开关，所有 AI 应用共用这套设置。分组由动作的风险类决定，规则和 Pulse 助手相同。

| 分组（`group`） | 包括 | 默认 | 打开后 |
| --- | --- | --- | --- |
| 查看与界面（`view`） | 只读和改变显示：栏、标签页、视图、排序、分组、面板、选择 | 开 | 直接执行 |
| 整理 Pulse（`organize`） | Pulse 自己的数据：标签、收藏、托盘、别名、快捷键、设置 | 关 | 直接执行，可以撤销 |
| 新建（`create`） | 新建文件夹 / 文件、打包、导出别名 | 开 | 每次询问；用户可以选「始终允许这个文件夹」（含子文件夹） |
| 移动、重命名和删除（`files`） | 复制到、移动到、粘贴、重命名、归入文件夹、解散、移到回收站、永久删除、从回收站还原、清空回收站 | 关 | 每次显示计划并询问；不能「始终允许」 |
| Pulse 之外的效果（`external`） | 打开程序或脚本、第三方右键菜单项、Git 推送 / 拉取、云文件保留 / 释放、弹出驱动器、映射网络驱动器等 | 关 | 每次询问；不能「始终允许」 |
| 只能你自己做（`locked`） | 退出 Pulse、解锁 BitLocker 驱动器、助手的权限 | — | 始终拒绝 |

几条细则：

- **`nav.open` 看打开的是什么**：打开程序、脚本或快捷方式（`.exe .bat .cmd .ps1 .vbs .msi .lnk` 等）归入 `external`；打开文档和媒体文件属于 `view`。
- **有些设置 AI 不能改**：关闭删除确认、助手权限、始终允许规则、连接和 API 密钥只能用户自己改，即使 `organize` 已经打开。
- **询问的方式**：Pulse 在自己的窗口里弹出对话框，每个需要询问的步骤一个对话框，标题写明是哪个应用在请求。等待期间，客户端会收到进度通知 “Waiting for the user to confirm in Pulse”。

## 一次调用里的规则

Pulse 在执行之前会先检查整串步骤。下面任何一条不满足，就整次拒绝，**什么都不做**，错误信息以 “nothing of this call was done” 结尾：

| 情况 | 错误码 |
| --- | --- |
| 有不存在的动作 | `invalid_args`（用 `find_actions` 查 id） |
| 有只能用户自己做的动作 | `locked` |
| 有动作所在的分组没打开 | `not_allowed` |
| 一次调用里有两个以上的文件步骤（移动、复制、重命名、删除、还原） | `invalid_args`（拆成几次调用） |
| 新建文件夹 / 文件时，前面的步骤会换掉当前文件夹（例如 `nav.go`、`window.layout`） | `invalid_args`（先用一次调用打开文件夹，下一次再新建） |
| 新建时当前标签页显示的不是真实文件夹（例如「设置」页） | `invalid_args`（先用一次调用打开文件夹，下一次再新建） |
| `steps` 为空 | `invalid_args` |

检查通过后：

1. **先问「新建」和「Pulse 之外」**：这两类的问题在任何步骤执行之前问完，所以用户拒绝时什么都没发生。文件夹已有「始终允许」规则的，不再问。
2. **文件步骤轮到它时显示计划**：计划写明移到哪里、回收站空间够不够；永久删除要用户点两次。用户拒绝时，前面已经完成的步骤保留，可以用 `undo_last` 撤销，后面的步骤不再执行。
3. **其余步骤直接执行**：`view` 和打开了的 `organize` 动作。

不要在调用之前先问用户「可以吗？」，因为 Pulse 自己会问。

## 错误码

工具出错时返回 MCP 的 `isError: true`，内容是 `code: message`；可以重试的，末尾带 “(retryable)”。

| 错误码 | 含义 | 应该怎么做 | 可重试 |
| --- | --- | --- | --- |
| `pulse_not_running` | Pulse 没有运行，或 MCP 开关没打开 | 请用户打开 Pulse 和开关 | 是 |
| `client_not_approved` | 用户拒绝了这个应用 | 告诉用户可以在「AI 与 MCP → 应用」里改成允许 | 否 |
| `not_allowed` | 动作所在的分组没打开 | 把消息里的分组名和位置转告用户；不要换个动作绕过去 | 否 |
| `locked` | 只能用户自己做 | 告诉用户去哪里做 | 否 |
| `invalid_args` | 参数不对、动作不存在，或违反了上面的规则 | 按消息修正后再调用 | 否 |
| `user_denied` | 用户在 Pulse 里点了「拒绝」 | **不要用同样的方式再问**，改问用户想怎么做 | 否 |
| `user_timeout` | 2 分钟内没人回答 | 等用户回来再试 | 是 |
| `not_ai_operation` | `undo_last` 没有可撤销的 AI 改动，或最新的改动是用户自己的 | 告诉用户；不要改用别的动作来撤销 | 否 |
| `busy` | 同时调用太多，或上一个改动还在进行 | 稍后重试 | 是 |
| `unsupported` | 没有这个工具、Pulse 当前没有打开窗口，或这个动作只能通过 Pulse 自己的流程完成 | 按消息处理 | 否 |
| `timeout` / `internal` | 工具超时或内部错误 | 稍后重试 | 视消息而定 |
| `cancelled` | 调用被取消 | — | — |

## 撤销（`undo_last`）

- 撤销 AI 应用**最近一次**改动：`create_zip` 生成的压缩包（移到回收站），或整次 `run_actions` 调用。返回 `{"undone": …, "what": "the last run_actions call"}`。
- **只撤销 AI 自己的改动**：用户之后自己又做了操作，最新的就不再是 AI 的改动，这时返回 `not_ai_operation`，不会去撤销用户的操作。
- 状态已经被改过、或者窗口已经关闭，也会拒绝，并且什么都不撤销。
- 文件操作进入 Pulse 自己的撤销列表，用户也可以在 Pulse 里按 Ctrl+Z 撤销；视图类的改动不在 Ctrl+Z 里，只能用 `undo_last` 撤销。
- 永久删除和清空回收站不能撤销，所以 Pulse 会让用户点两次确认。
- 一次调用超时后，`undo_last` 撤销的是**上一个已经完成的**调用。客户端自己的超时比 Pulse 的 2 分钟短时尤其要注意。

## 活动记录

每次调用都会写进本机的活动记录：时间、应用、工具、结果，不记录文件内容。记录在 `%LOCALAPPDATA%\Pulse\ai\activity\`，用户可以在「AI 与 MCP」页查看。

## 测试（开发者）

普通的 `--test-instance` 不提供 AI 服务，以免测试实例接管用户的 MCP 连接。做端到端测试时：

1. 用带测试钩子的构建（`PULSE_WITH_TEST_HOOKS`）。
2. 把 `LOCALAPPDATA` 指向一个临时目录，再以 `--test-instance --test-ai-broker` 启动 Pulse。只有 `LOCALAPPDATA` 不是用户自己的目录时，这个开关才会生效；管道名和 `ai.json` 都跟着它走，所以测试永远不会用到用户的管道和设置。
3. 在临时目录的 `Pulse\ai\ai.json` 里写好 `mcp_enabled`、`clients` 和 `mcp_allow`，然后用同样的 `LOCALAPPDATA` 启动 `pulse.exe --mcp-stdio`，作为客户端连上去。

自动化测试：

- `src/bench/ai_mcp_test.cpp`：协议、参数校验、`pulse_not_running`、`client_not_approved` 等。
- `src/bench/ai_broker_test.cpp`：管道和授权。
- `selftest_1b2.cpp` 的 `TestMcpRunActions`：分组和预检。


增强包开发工具的模型说明与输入 Schema 由 `src/app/extensions/developer_tool_catalog.h` 统一提供给内置助手和 MCP；模板枚举来自生成目录。此项归并限于三个只读开发工具，其他执行工具及其权限检查保持独立。修改返回的模板后仍须重新调用清单校验，获取模板不会安装或执行它。
