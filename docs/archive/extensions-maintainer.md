# 原生增强包平台维护指南

[主路线图](../Pulse-增强包开放能力与AI协同实施方案-2026-11-02.md) 是唯一阶段/状态入口；[作者快速开始](../extensions-sdk-quickstart.md) 面向插件开发者。本页只保留实现责任和更新流程，历次声明式阶段记录已移到 `docs/archive`。

## 维护哪些契约

| 范围 | 来源 | 责任 |
|---|---|---|
| C ABI / C++ 封装 | `sdk/native/include/pulse` | 接口版本、结构大小、线程、寿命、失败/取消，禁止私有对象跨边界 |
| 原生 UI / 事件 | `src/app/extensions/native`、`src/ui/extension_*` | 复用真实控件；结构、更新、事件、代际和回调寿命一致 |
| 公共文件/资源服务 | `native/files_adapter.*`、`native/resources_adapter.*` 及原服务模块 | 类型化接线与真实执行；内部 JSON 操作不自动计作公开 C++ API |
| 模块和实例 | `native/module.*`、`native/session.*` | 哈希绑定、按包/窗口归属、停用/排空、销毁及系统依赖搜索 |
| 包发现与身份 | `runtime.*`、`package_store.*`、`authorization.*` | 固定启动快照、默认/显式存储、主体和故障诊断 |
| 清单与发现 | `manifest.*`、`schemas/extensions`、`developer_contract.*` | 解析器、schema、生成描述和实际支持一致 |
| 作者工具 | `tools/extensions/pulse_ext.py`、`package_store.py`、`native_archive.py` | 同一有界快照校验、原子发布、启停/升级/回退及结构化错误 |
| AI / MCP | `developer_tools.*` 与现有 MCP 适配 | 使用相同发现和校验；执行工作流另行实现，不把发现当授权 |
| UI 文案 | `tools/l10n/catalog.tsv` | 只改 catalog，再运行生成器；不手改生成资源 |

## 新增公共能力的完整流程

1. 写明开发者场景、宿主复用点、输入/输出、所有者、线程、寿命、取消、错误和兼容策略。
2. 先决定属于 UI 组件、挂载点还是内部服务，不把三者混为“UI 已开放”。内部实现仍留在原拥有模块。
3. 实现公开头/封装与宿主适配，同时更新机器清单和文档；不提供没有实现的成功路径。
4. 更新 schema/元数据后运行 `generate_sdk.py`；原生目录与约束运行 `generate_native_catalog.py`。生成器只同步声明，不实现行为。
5. 用独立示例证明开发者代码可改变业务。覆盖与修改相关的成功、错误、取消/过期及释放边界；只构建和验证受影响目标。
6. 可见 UI 使用应用内截图；文件行为检查实际结果；性能结论附环境与口径。不要把旧报告或编译成功记成新功能验收。
7. 生成匹配的原生 SDK/宿主归档，验证解压后的作者工具与宿主发现一致；在主路线图更新状态和证据链接。

## 清理和兼容

JavaScript 实验执行层已经删除。`runtime`、`runtime.invoke`、脚本命令行开关不再是能力；应有拒绝用例。历史审计记录的操作名称可只读兼容，但没有对应执行分支。

JSON 清单、声明式 UI 以及 `sdk/typescript` 只描述宿主元数据，不是另一条脚本路线。相关详细参考见 [声明式元数据](extensions-sdk.md)、[资源协议](extensions-resources.md) 和 [API 维护参考](extensions-api-maintenance.md)。

删除能力时一起清理实现、构建依赖、工具入口、机器清单、示例和专属测试；保留明确拒绝和其他仍使用的通用基础。不要删除无关的 JavaScript 文件预览/语法高亮等文件管理器功能。

## 定向验证入口

- `pulse_extension_manifest_test --legacy-runtime-rejection`：旧清单拒绝。
- `pulse_extension_developer_contract_test`：公开契约/发现与解析一致。
- `pulse_extension_native_contract_test`、`pulse_native_package_runtime_test`、`pulse_native_loader_test`：原生包、启动与加载生命周期。
- `tools/extensions/test_native_packages_ui.py`：独立配置中用真实 DLL 包验证正常入口、并行插件与算法升级回退，保存应用内截图。
- 对受影响作者工具运行对应 Python 测试；对生成结果运行 `--check`。不默认执行全部历史套件。

桌面磁贴场景要额外检查承载/退出恢复、多屏/DPI、拖放、增量文件变化与持续更新性能；这些不是当前面板验收已经覆盖的项目。
