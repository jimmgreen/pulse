> 历史记录：JavaScript 实验实现已退役；以下命令和接口不适用于当前宿主。当前作者入口见 [C++ 快速开始](../extensions-sdk-quickstart.md)。

# 内嵌 JavaScript 契约（开发阶段）

此页仅描述可选 JavaScript 开发适配器，不定义 SDK 的唯一开发语言。平台边界以 [UI 与可复用 API 契约](extensions-platform-contract.md) 为准。当前逐命令创建隔离进程尚未满足最终运行层的会话复用与性能验收。

此页描述 v1 清单契约，不代表任意不可信第三方脚本的生产隔离已验收。
作者应检查 `isolatedJavaScriptRuntime`，执行时仍以宿主门禁和返回结果为准；引擎或隔离不可用必须拒绝，不能回退到 UI 进程。

清单增加可选 `runtime`，包含且仅包含：

- `kind: "javascript"`、`version: 1`。
- `source`：内嵌 UTF-8 源码，1–32768 字节，允许换行、禁止 NUL；整个清单仍限制 65536 字节。
- `entrypoints`：1–16 个唯一入口名，长度 1–64，满足 `[A-Za-z_$][A-Za-z0-9_$]*`。

源码直接包含在已签名、已哈希的 manifest 中，不支持外部脚本资产、模块路径或远程导入。
清单解析验证结构与预算，不执行或编译 JavaScript；源码语法、入口函数存在性由实际引擎加载时验证。

命令使用 `capability: "runtime.invoke"`，参数为 `{ "entrypoint": "run", "inputState": ["query"] }`。
`entrypoint` 必须在该包 runtime 中声明；`inputState` 可省略，最多 16 个唯一键，每个键必须已在 `panel.ui.state` 声明。
命令不得携带临时代码。声明 runtime 却没有任何 runtime.invoke 命令将被拒绝，避免无入口的死配置。
第一批不允许 runtime.invoke 成为声明式 workflow 步骤；直接按钮、菜单或命令入口由宿主分派。

开发者逻辑收到 白名单字符串 state 字典（例如 `{prefix: "demo-"}`，入口读取 `input.prefix`），字典只含命令显式列出的本地值。
返回 `{updates: [{key, value}]}`，由宿主检查声明键、值约束及 UI 修订，原子提交；过期、关闭、重载或撤权结果不应用。
目录、文件和选中项上下文不会因 inputState 自动授予；需要资源时必须通过宿主 broker，并独立声明相应资源权限。
宿主保存调用者身份，不接受脚本自报 package、owner 或授权凭据。

纯 JS 环境不提供 std/os、import、直接网络、FFI 或原生窗口对象。runtime.invoke 不隐含执行其他命令能力。
资源操作的确认、取消和已完成效果仍遵守各自契约；终止脚本不代表回滚已经提交的操作。
运行时消息预算、时间限制和实际隔离证据由宿主实现及专门验收说明，不能根据清单解析成功推断已执行或安全通过。

旧声明式清单行为不变。旧宿主严格拒绝未知 runtime 字段；作者须先发现能力，不能只凭 apiVersion 为 1 假定所有 v1 宿主支持本扩展。
