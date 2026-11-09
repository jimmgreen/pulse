# Pulse 能力台账

由 tools/extensions/generate_capability_inventory.py 从权威清单生成。此表区分扩展公开能力、内部动作与 MCP 注册入口；源码注册证据不等于当前调用者有权限或运行时可用。

公开接口的模块归属、权限、验证入口及兼容流程见[增强包 API 维护契约](extensions-api-maintenance.md)。

分母：action_inventory.h 的 kActions 有 138 个不同动作；MCP 明确注册 9 项，加共享开发工具 3 项；扩展 capability enum 有 19 项。设置的每个 key、动态菜单项和工作流步骤不是额外 capability，不能混为一个覆盖率。

## 扩展公开能力

| capability | 权威来源 | 边界 |
| --- | --- | --- |
| navigation.back | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| navigation.forward | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| navigation.up | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| view.refresh | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| view.setMode | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| workspace.setLayout | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| files.copySelectionToOtherPane | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| files.moveSelectionToOtherPane | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| workflow.run | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| workflow.cancel | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| selection.selectAll | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| selection.invert | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| selection.clear | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| view.setSort | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| tabs.new | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| tabs.duplicate | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| tabs.closeActive | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| tabs.activate | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |
| panes.focus | `schemas/extensions/manifest.schema.json:commands`；`src/app/extensions/capability_service.cpp` | 仅声明且通过调用者校验后执行；具体副作用由 describe_extension_api.capabilityDetails 提供 |

## 独立资源权限与请求契约

分母为 manifest.resources 的权限枚举与 resourceApi.operations 的请求列表，二者不是 command capability。宿主持有 CallerLease；开发工具仅发现，没有自报身份的 MCP 执行入口。

| op | 所需独立权限 | 声明/接线边界 |
| --- | --- | --- |
| context.get | context.read | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| files.list | files.readCurrentDirectory | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| files.stat | files.readCurrentDirectory | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| operations.get | operations.own | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| operations.cancel | operations.own | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| settings.get | settings.own | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| settings.set | settings.own | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| audit.list | audit.own | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| files.prepareRename | files.renameCurrentDirectory | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |
| files.commitRename | files.renameCurrentDirectory | `schemas/extensions/resource-request.schema.json`；宿主 ResourceService::Call 调用者校验及实际接线另验 |

## MCP 注册入口

| 工具 | 注册源码 | 状态与权限边界 |
| --- | --- | --- |
| get_context | `src/ai_host/mcp_tools.cpp:91` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| list_folder | `src/ai_host/mcp_tools.cpp:97` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| search | `src/ai_host/mcp_tools.cpp:104` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| get_properties | `src/ai_host/mcp_tools.cpp:114` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| reveal | `src/ai_host/mcp_tools.cpp:119` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| create_zip | `src/ai_host/mcp_tools.cpp:124` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| find_actions | `src/ai_host/mcp_tools.cpp:133` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| run_actions | `src/ai_host/mcp_tools.cpp:143` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| undo_last | `src/ai_host/mcp_tools.cpp:147` | 已注册的 AI/MCP 接口；受其参数、用户 AI 权限和宿主确认策略限制，不是增强包 capability |
| describe_extension_api | `src/app/extensions/developer_tool_catalog.h:23` | 已注册只读作者工具；描述、验证或返回模板不执行扩展 |
| validate_extension_manifest | `src/app/extensions/developer_tool_catalog.h:27` | 已注册只读作者工具；描述、验证或返回模板不执行扩展 |
| get_extension_template | `src/app/extensions/developer_tool_catalog.h:31` | 已注册只读作者工具；描述、验证或返回模板不执行扩展 |

## 全部内部动作

风险等级来自权威 action_inventory；by_target 表示目标可能提高风险。kDefs 行代表菜单/命令面板适配；kNoRow 的例外原因单列。AI 是否允许由 action_policy、参数适配和用户开关决定；这里不把内部 ID 提升成扩展权限。

| 动作 ID | 风险 | 目标敏感 | 接线证据 | 状态 |
| --- | --- | --- | --- | --- |
| nav.open | View | 是 | `src/app/actions/action_registry.cpp:657` | 内部已登记；受当前状态/策略限制 |
| nav.go | View | 否 | `src/app/actions/action_registry.cpp:655` | 内部已登记；受当前状态/策略限制 |
| nav.back | View | 否 | `src/app/actions/action_registry.cpp:646` | 内部已登记；受当前状态/策略限制 |
| nav.forward | View | 否 | `src/app/actions/action_registry.cpp:648` | 内部已登记；受当前状态/策略限制 |
| nav.up | View | 否 | `src/app/actions/action_registry.cpp:650` | 内部已登记；受当前状态/策略限制 |
| nav.refresh | View | 否 | `src/app/actions/action_registry.cpp:652` | 内部已登记；受当前状态/策略限制 |
| nav.open_in_new_tab | View | 否 | `src/app/actions/action_registry.cpp:822` | 内部已登记；受当前状态/策略限制 |
| nav.reveal | View | 否 | `src/app/actions/action_registry.cpp:824` | 内部已登记；受当前状态/策略限制 |
| nav.recent_changes | View | 否 | `src/app/actions/action_registry.cpp:653` | 内部已登记；受当前状态/策略限制 |
| nav.recycle_bin | View | 否 | `src/app/actions/action_registry.cpp:659` | 内部已登记；受当前状态/策略限制 |
| tab.new | View | 否 | `src/app/actions/action_registry.cpp:662` | 内部已登记；受当前状态/策略限制 |
| tab.close | View | 否 | `src/app/actions/action_registry.cpp:664` | 内部已登记；受当前状态/策略限制 |
| tab.close_others | View | 否 | `src/app/actions/action_registry.cpp:814` | 内部已登记；受当前状态/策略限制 |
| tab.duplicate | View | 否 | `src/app/actions/action_registry.cpp:812` | 内部已登记；受当前状态/策略限制 |
| tab.pin | View | 否 | `src/app/actions/action_registry.cpp:806` | 内部已登记；受当前状态/策略限制 |
| tab.rename | View | 否 | `src/app/actions/action_registry.cpp:809` | 内部已登记；受当前状态/策略限制 |
| tab.color | View | 否 | `src/app/actions/action_registry.cpp:808` | 内部已登记；受当前状态/策略限制 |
| tab.group | View | 否 | `src/app/actions/action_registry.cpp:811` | 内部已登记；受当前状态/策略限制 |
| tab.activate | View | 否 | `src/app/actions/action_registry.cpp:816` | 内部已登记；受当前状态/策略限制 |
| tab.to_window | View | 否 | `src/app/actions/action_registry.cpp:668` | 内部已登记；受当前状态/策略限制 |
| window.layout | View | 否 | `src/app/actions/action_registry.cpp:671` | 内部已登记；受当前状态/策略限制 |
| pane.focus | View | 否 | `src/app/actions/action_registry.cpp:817` | 内部已登记；受当前状态/策略限制 |
| pane.mark_target | View | 否 | `src/app/actions/action_registry.cpp:673` | 内部已登记；受当前状态/策略限制 |
| pane.to_window | View | 否 | `src/app/actions/action_registry.cpp:675` | 内部已登记；受当前状态/策略限制 |
| window.new | View | 否 | `src/app/actions/action_registry.cpp:677` | 内部已登记；受当前状态/策略限制 |
| window.close | View | 否 | `src/app/actions/action_registry.cpp:679` | 内部已登记；受当前状态/策略限制 |
| window.reopen | View | 否 | `src/app/actions/action_registry.cpp:666` | 内部已登记；受当前状态/策略限制 |
| window.activate | View | 否 | `src/app/actions/action_registry.cpp:818` | 内部已登记；受当前状态/策略限制 |
| window.arrange | View | 否 | `src/app/actions/action_registry.cpp:683` | 内部已登记；受当前状态/策略限制 |
| window.merge | View | 否 | `src/app/actions/action_registry.cpp:695` | 内部已登记；受当前状态/策略限制 |
| window.overview | View | 否 | `src/app/actions/action_registry.cpp:681` | 内部已登记；受当前状态/策略限制 |
| window.maximize | View | 否 | `src/app/actions/action_registry.cpp:697` | 内部已登记；受当前状态/策略限制 |
| sidebar.toggle | View | 否 | `src/app/actions/action_registry.cpp:699` | 内部已登记；受当前状态/策略限制 |
| sidebar.section | View | 否 | `src/app/actions/action_registry.cpp:820` | 内部已登记；受当前状态/策略限制 |
| favbar.toggle | View | 否 | `src/app/actions/action_registry.cpp:701` | 内部已登记；受当前状态/策略限制 |
| details.toggle | View | 否 | `src/app/actions/action_registry.cpp:703` | 内部已登记；受当前状态/策略限制 |
| app.settings_page | View | 否 | `src/app/actions/action_registry.cpp:913` | 内部已登记；受当前状态/策略限制 |
| app.shortcut_help | View | 否 | `src/app/actions/action_registry.cpp:915` | 内部已登记；受当前状态/策略限制 |
| app.exit | UserOnly | 否 | `src/app/actions/action_registry.cpp:917` | 内部已登记；UserOnly 锁定 |
| view.mode | View | 否 | `src/app/actions/action_registry.cpp:706` | 内部已登记；受当前状态/策略限制 |
| view.icon_size | View | 否 | `src/app/actions/action_registry.cpp:826` | 内部已登记；受当前状态/策略限制 |
| view.wall | View | 否 | `src/app/actions/action_registry.cpp:827` | 内部已登记；受当前状态/策略限制 |
| view.sort | View | 否 | `src/app/actions/action_registry.cpp:707` | 内部已登记；受当前状态/策略限制 |
| view.group | View | 否 | `src/app/actions/action_registry.cpp:710` | 内部已登记；受当前状态/策略限制 |
| view.folders_first | View | 否 | `src/app/actions/action_registry.cpp:828` | 内部已登记；受当前状态/策略限制 |
| view.folder_sizes | View | 否 | `src/app/actions/action_registry.cpp:711` | 内部已登记；受当前状态/策略限制 |
| view.columns | View | 否 | `src/app/actions/action_registry.cpp:713` | 内部已登记；受当前状态/策略限制 |
| view.apply_all | Organize | 否 | `src/app/actions/action_registry.cpp:715` | 内部已登记；受当前状态/策略限制 |
| view.filter | View | 否 | `src/app/actions/action_registry.cpp:717` | 内部已登记；受当前状态/策略限制 |
| view.compare | View | 否 | `src/app/actions/action_registry.cpp:719` | 内部已登记；受当前状态/策略限制 |
| select.items | View | 否 | `src/app/actions/action_registry.cpp:729` | 内部已登记；受当前状态/策略限制 |
| select.all | View | 否 | `src/app/actions/action_registry.cpp:722` | 内部已登记；受当前状态/策略限制 |
| select.invert | View | 否 | `src/app/actions/action_registry.cpp:723` | 内部已登记；受当前状态/策略限制 |
| select.pattern | View | 否 | `src/app/actions/action_registry.cpp:725` | 内部已登记；受当前状态/策略限制 |
| select.clear | View | 否 | `src/app/actions/action_registry.cpp:727` | 内部已登记；受当前状态/策略限制 |
| edit.undo | View | 是 | `src/app/actions/action_registry.cpp:734` | 内部已登记；受当前状态/策略限制 |
| file.copy | Organize | 否 | `src/app/actions/action_registry.cpp:735` | 内部已登记；受当前状态/策略限制 |
| file.cut | Organize | 否 | `src/app/actions/action_registry.cpp:736` | 内部已登记；受当前状态/策略限制 |
| file.copy_path | Organize | 否 | `src/app/actions/action_registry.cpp:738` | 内部已登记；受当前状态/策略限制 |
| file.paste | Move | 否 | `src/app/actions/action_registry.cpp:737` | 内部已登记；受当前状态/策略限制 |
| file.copy_to | Move | 否 | `src/app/actions/action_registry.cpp:740` | 内部已登记；受当前状态/策略限制 |
| file.move_to | Move | 否 | `src/app/actions/action_registry.cpp:742` | 内部已登记；受当前状态/策略限制 |
| file.new_folder | Create | 否 | `src/app/actions/action_registry.cpp:744` | 内部已登记；受当前状态/策略限制 |
| file.new_file | Create | 否 | `src/app/actions/action_registry.cpp:746` | 内部已登记；受当前状态/策略限制 |
| file.rename | Move | 否 | `src/app/actions/action_registry.cpp:748` | 内部已登记；受当前状态/策略限制 |
| file.batch_rename | View | 否 | `src/app/actions/action_registry.cpp:749` | 内部已登记；受当前状态/策略限制 |
| file.group_into_folder | Move | 否 | `src/app/actions/action_registry.cpp:751` | 内部已登记；受当前状态/策略限制 |
| file.dissolve | Move | 否 | `src/app/actions/action_registry.cpp:753` | 内部已登记；受当前状态/策略限制 |
| file.organize | Move | 否 | `src/app/actions/action_registry.cpp:634` | 无面板行的明确例外：AI apps only: it needs moves[{item, dest}]; the assistant uses organize_files |
| file.find_duplicates | Read | 否 | `src/app/actions/action_registry.cpp:755` | 内部已登记；受当前状态/策略限制 |
| file.zip | Create | 否 | `src/app/actions/action_registry.cpp:757` | 内部已登记；受当前状态/策略限制 |
| file.recycle | Delete | 否 | `src/app/actions/action_registry.cpp:759` | 内部已登记；受当前状态/策略限制 |
| file.delete_permanent | PermanentDelete | 否 | `src/app/actions/action_registry.cpp:762` | 内部已登记；受当前状态/策略限制 |
| recycle.restore | Move | 否 | `src/app/actions/action_registry.cpp:764` | 内部已登记；受当前状态/策略限制 |
| recycle.empty | PermanentDelete | 否 | `src/app/actions/action_registry.cpp:766` | 内部已登记；受当前状态/策略限制 |
| file.properties | View | 否 | `src/app/actions/action_registry.cpp:768` | 内部已登记；受当前状态/策略限制 |
| file.context_menu | View | 否 | `src/app/actions/action_registry.cpp:877` | 内部已登记；受当前状态/策略限制 |
| file.terminal | View | 否 | `src/app/actions/action_registry.cpp:770` | 内部已登记；受当前状态/策略限制 |
| file.compute_size | Read | 否 | `src/app/actions/action_registry.cpp:862` | 内部已登记；受当前状态/策略限制 |
| shell.verbs | Read | 否 | `src/app/actions/action_registry.cpp:879` | 内部已登记；受当前状态/策略限制 |
| shell.run_verb | External | 否 | `src/app/actions/action_registry.cpp:880` | 内部已登记；受当前状态/策略限制 |
| cloud.keep | System | 否 | `src/app/actions/action_registry.cpp:898` | 内部已登记；受当前状态/策略限制 |
| cloud.free | System | 否 | `src/app/actions/action_registry.cpp:900` | 内部已登记；受当前状态/策略限制 |
| tag.toggle | Organize | 否 | `src/app/actions/action_registry.cpp:778` | 内部已登记；受当前状态/策略限制 |
| tag.clear | Organize | 否 | `src/app/actions/action_registry.cpp:775` | 内部已登记；受当前状态/策略限制 |
| tag.panel | View | 否 | `src/app/actions/action_registry.cpp:773` | 内部已登记；受当前状态/策略限制 |
| tag.create | Organize | 否 | `src/app/actions/action_registry.cpp:830` | 内部已登记；受当前状态/策略限制 |
| tag.edit | Organize | 否 | `src/app/actions/action_registry.cpp:832` | 内部已登记；受当前状态/策略限制 |
| tag.delete | Organize | 否 | `src/app/actions/action_registry.cpp:833` | 内部已登记；受当前状态/策略限制 |
| tag.group | Organize | 否 | `src/app/actions/action_registry.cpp:834` | 内部已登记；受当前状态/策略限制 |
| smartview.save | Organize | 否 | `src/app/actions/action_registry.cpp:835` | 内部已登记；受当前状态/策略限制 |
| fav.add | Organize | 否 | `src/app/actions/action_registry.cpp:779` | 内部已登记；受当前状态/策略限制 |
| quick_access.pin | Organize | 否 | `src/app/actions/action_registry.cpp:783` | 内部已登记；受当前状态/策略限制 |
| recent.remove | Organize | 否 | `src/app/actions/action_registry.cpp:837` | 内部已登记；受当前状态/策略限制 |
| workspace.pin | Organize | 否 | `src/app/actions/action_registry.cpp:781` | 内部已登记；受当前状态/策略限制 |
| alias.set | Organize | 否 | `src/app/actions/action_registry.cpp:839` | 内部已登记；受当前状态/策略限制 |
| alias.display | View | 否 | `src/app/actions/action_registry.cpp:840` | 内部已登记；受当前状态/策略限制 |
| alias.manage | View | 否 | `src/app/actions/action_registry.cpp:785` | 内部已登记；受当前状态/策略限制 |
| alias.export | Create | 否 | `src/app/actions/action_registry.cpp:841` | 内部已登记；受当前状态/策略限制 |
| tray.add | Organize | 否 | `src/app/actions/action_registry.cpp:788` | 内部已登记；受当前状态/策略限制 |
| tray.remove | Organize | 否 | `src/app/actions/action_registry.cpp:844` | 内部已登记；受当前状态/策略限制 |
| tray.find_stale | View | 否 | `src/app/actions/action_registry.cpp:845` | 内部已登记；受当前状态/策略限制 |
| tray.send_to | Move | 否 | `src/app/actions/action_registry.cpp:853` | 内部已登记；受当前状态/策略限制 |
| tray.zip | Create | 否 | `src/app/actions/action_registry.cpp:851` | 内部已登记；受当前状态/策略限制 |
| tray.compare | Read | 否 | `src/app/actions/action_registry.cpp:847` | 内部已登记；受当前状态/策略限制 |
| tray.text_diff | View | 否 | `src/app/actions/action_registry.cpp:849` | 内部已登记；受当前状态/策略限制 |
| search.files | Read | 否 | `src/app/actions/action_registry.cpp:632` | 无面板行的明确例外：the search_files tool runs it |
| search.open | View | 否 | `src/app/actions/action_registry.cpp:856` | 内部已登记；受当前状态/策略限制 |
| search.advanced | View | 否 | `src/app/actions/action_registry.cpp:791` | 内部已登记；受当前状态/策略限制 |
| search.project | View | 否 | `src/app/actions/action_registry.cpp:793` | 内部已登记；受当前状态/策略限制 |
| search.global | View | 否 | `src/app/actions/action_registry.cpp:858` | 内部已登记；受当前状态/策略限制 |
| index.status | Read | 否 | `src/app/actions/action_registry.cpp:860` | 内部已登记；受当前状态/策略限制 |
| index.install_full | System | 否 | `src/app/actions/action_registry.cpp:795` | 内部已登记；受当前状态/策略限制 |
| preview.toggle | View | 否 | `src/app/actions/action_registry.cpp:798` | 内部已登记；受当前状态/策略限制 |
| preview.step | View | 否 | `src/app/actions/action_registry.cpp:864` | 内部已登记；受当前状态/策略限制 |
| preview.cloud_download | System | 否 | `src/app/actions/action_registry.cpp:902` | 内部已登记；受当前状态/策略限制 |
| drive.eject | System | 否 | `src/app/actions/action_registry.cpp:904` | 内部已登记；受当前状态/策略限制 |
| drive.disconnect | System | 否 | `src/app/actions/action_registry.cpp:905` | 内部已登记；受当前状态/策略限制 |
| drive.unlock | UserOnly | 否 | `src/app/actions/action_registry.cpp:907` | 内部已登记；UserOnly 锁定 |
| net.map_drive | System | 否 | `src/app/actions/action_registry.cpp:803` | 内部已登记；受当前状态/策略限制 |
| net.connect_server | View | 否 | `src/app/actions/action_registry.cpp:801` | 内部已登记；受当前状态/策略限制 |
| net.place | Organize | 否 | `src/app/actions/action_registry.cpp:867` | 内部已登记；受当前状态/策略限制 |
| net.recent | Organize | 否 | `src/app/actions/action_registry.cpp:868` | 内部已登记；受当前状态/策略限制 |
| net.rescan | View | 否 | `src/app/actions/action_registry.cpp:870` | 内部已登记；受当前状态/策略限制 |
| net.system_settings | View | 否 | `src/app/actions/action_registry.cpp:871` | 内部已登记；受当前状态/策略限制 |
| git.refresh | Read | 否 | `src/app/actions/action_registry.cpp:891` | 内部已登记；受当前状态/策略限制 |
| git.pull | External | 否 | `src/app/actions/action_registry.cpp:893` | 内部已登记；受当前状态/策略限制 |
| git.push | External | 否 | `src/app/actions/action_registry.cpp:895` | 内部已登记；受当前状态/策略限制 |
| toolbar.open_with | External | 否 | `src/app/actions/action_registry.cpp:882` | 内部已登记；受当前状态/策略限制 |
| toolbar.pin_app | Organize | 否 | `src/app/actions/action_registry.cpp:886` | 内部已登记；受当前状态/策略限制 |
| toolbar.customize | View | 否 | `src/app/actions/action_registry.cpp:884` | 内部已登记；受当前状态/策略限制 |
| toolbar.tools | Organize | 否 | `src/app/actions/action_registry.cpp:888` | 内部已登记；受当前状态/策略限制 |
| settings.read | Read | 否 | `src/app/actions/action_registry.cpp:731` | 内部已登记；受当前状态/策略限制 |
| settings.set | View | 是 | `src/app/actions/action_registry.cpp:633` | 无面板行的明确例外：one row per setting, from action_settings (SettingView) |
| keymap.read | Read | 否 | `src/app/actions/action_registry.cpp:874` | 内部已登记；受当前状态/策略限制 |
| keymap.set | Organize | 否 | `src/app/actions/action_registry.cpp:875` | 内部已登记；受当前状态/策略限制 |
| assistant.prefs | UserOnly | 否 | `src/app/actions/action_registry.cpp:910` | 内部已登记；UserOnly 锁定 |
| assistant.organize_here | UserOnly | 否 | `src/app/actions/action_registry.cpp:908` | 内部已登记；UserOnly 锁定 |

## 逐项开放决策（只读政策，不授予权限）

机器源：[action-exposure-policy.json](../../schemas/extensions/action-exposure-policy.json)。每个 action 恰有一条显式决策；新增动作必须先审阅政策，不能按前缀自动获得权限。计划公开表示候选接口与验收条件，没有交付期限或现有执行保证；限制项只有满足所列重新审查条件后才能变更政策。

决策分母：已公开映射（仅所列子集） 17；计划公开（未实现） 85；限制开放 36。workflow.run/cancel 是公开编排能力，没有对应的内部 action 别名；包级 settings 资源也不等于全局 settings.set。

| 动作 ID | 决策 | 模块/源码 | 理由 | 已公开 capability | 拟公开接口/保留边界 | 进入条件 |
| --- | --- | --- | --- | --- | --- | --- |
| nav.open | 限制开放 | 导航 / `src/app/app_navigation.h` | 导航目标可能跨出当前目录或启动外部文件 | 无 | 当前不开放；拟新增受限导航接口，以当前目录不透明条目和 owner 绑定导航目标 | 分离目录导航与外部启动；验证失效条目、目录切换和 owner 销毁 |
| nav.go | 计划公开（未实现） | 导航 / `src/app/app_navigation.h` | 导航目标可能跨出当前目录或启动外部文件 | 无 | 拟新增受限导航接口，以当前目录不透明条目和 owner 绑定导航目标 | 分离目录导航与外部启动；验证失效条目、目录切换和 owner 销毁 |
| nav.back | 已公开映射（仅所列子集） | 导航 / `src/app/app_navigation.h` | 已由CapabilityService提供显式参数适配与调用者/owner校验；内部action本身不授予包 | navigation.back | 仅通过 navigation.back；不接受内部action ID | 包声明能力并启用；当前调用者代际有效；参数严格校验 |
| nav.forward | 已公开映射（仅所列子集） | 导航 / `src/app/app_navigation.h` | 已由CapabilityService提供显式参数适配与调用者/owner校验；内部action本身不授予包 | navigation.forward | 仅通过 navigation.forward；不接受内部action ID | 包声明能力并启用；当前调用者代际有效；参数严格校验 |
| nav.up | 已公开映射（仅所列子集） | 导航 / `src/app/app_navigation.h` | 已由CapabilityService提供显式参数适配与调用者/owner校验；内部action本身不授予包 | navigation.up | 仅通过 navigation.up；不接受内部action ID | 包声明能力并启用；当前调用者代际有效；参数严格校验 |
| nav.refresh | 已公开映射（仅所列子集） | 导航 / `src/app/app_navigation.h` | 已由CapabilityService提供显式参数适配与调用者/owner校验；内部action本身不授予包 | view.refresh | 仅通过 view.refresh；不接受内部action ID | 包声明能力并启用；当前调用者代际有效；参数严格校验 |
| nav.open_in_new_tab | 计划公开（未实现） | 导航 / `src/app/app_navigation.h` | 导航目标可能跨出当前目录或启动外部文件 | 无 | 拟新增受限导航接口，以当前目录不透明条目和 owner 绑定导航目标 | 分离目录导航与外部启动；验证失效条目、目录切换和 owner 销毁 |
| nav.reveal | 计划公开（未实现） | 导航 / `src/app/app_navigation.h` | 导航目标可能跨出当前目录或启动外部文件 | 无 | 拟新增受限导航接口，以当前目录不透明条目和 owner 绑定导航目标 | 分离目录导航与外部启动；验证失效条目、目录切换和 owner 销毁 |
| nav.recent_changes | 计划公开（未实现） | 导航 / `src/app/app_navigation.h` | 导航目标可能跨出当前目录或启动外部文件 | 无 | 拟新增受限导航接口，以当前目录不透明条目和 owner 绑定导航目标 | 分离目录导航与外部启动；验证失效条目、目录切换和 owner 销毁 |
| nav.recycle_bin | 计划公开（未实现） | 导航 / `src/app/app_navigation.h` | 导航目标可能跨出当前目录或启动外部文件 | 无 | 拟新增受限导航接口，以当前目录不透明条目和 owner 绑定导航目标 | 分离目录导航与外部启动；验证失效条目、目录切换和 owner 销毁 |
| tab.new | 已公开映射（仅所列子集） | 标签页 / `src/app/tab_controller.h` | 通过显式宿主适配与caller/owner校验；不接受内部action ID | tabs.new | 仅通过 tabs.new；作用于当前owner窗口的顶栏layout tabs或可见pane | 包声明并启用；caller代际有效；索引按执行时可见顺序校验；最后或固定标签不关闭；不允许关闭owner窗口 |
| tab.close | 已公开映射（仅所列子集） | 标签页 / `src/app/tab_controller.h` | 通过显式宿主适配与caller/owner校验；不接受内部action ID | tabs.closeActive | 仅通过 tabs.closeActive；作用于当前owner窗口的顶栏layout tabs或可见pane | 包声明并启用；caller代际有效；索引按执行时可见顺序校验；最后或固定标签不关闭；不允许关闭owner窗口 |
| tab.close_others | 计划公开（未实现） | 标签页 / `src/app/tab_controller.h` | 操作用户标签及持久显示状态，需要明确目标归属 | 无 | 拟新增当前 owner 标签句柄接口，不接受全局索引 | 定义稳定标签句柄、已关闭目标拒绝与关闭前保护；测试多窗口隔离 |
| tab.duplicate | 已公开映射（仅所列子集） | 标签页 / `src/app/tab_controller.h` | 通过显式宿主适配与caller/owner校验；不接受内部action ID | tabs.duplicate | 仅通过 tabs.duplicate；作用于当前owner窗口的顶栏layout tabs或可见pane | 包声明并启用；caller代际有效；索引按执行时可见顺序校验；最后或固定标签不关闭；不允许关闭owner窗口 |
| tab.pin | 计划公开（未实现） | 标签页 / `src/app/tab_controller.h` | 操作用户标签及持久显示状态，需要明确目标归属 | 无 | 拟新增当前 owner 标签句柄接口，不接受全局索引 | 定义稳定标签句柄、已关闭目标拒绝与关闭前保护；测试多窗口隔离 |
| tab.rename | 计划公开（未实现） | 标签页 / `src/app/tab_controller.h` | 操作用户标签及持久显示状态，需要明确目标归属 | 无 | 拟新增当前 owner 标签句柄接口，不接受全局索引 | 定义稳定标签句柄、已关闭目标拒绝与关闭前保护；测试多窗口隔离 |
| tab.color | 计划公开（未实现） | 标签页 / `src/app/tab_controller.h` | 操作用户标签及持久显示状态，需要明确目标归属 | 无 | 拟新增当前 owner 标签句柄接口，不接受全局索引 | 定义稳定标签句柄、已关闭目标拒绝与关闭前保护；测试多窗口隔离 |
| tab.group | 计划公开（未实现） | 标签页 / `src/app/tab_controller.h` | 操作用户标签及持久显示状态，需要明确目标归属 | 无 | 拟新增当前 owner 标签句柄接口，不接受全局索引 | 定义稳定标签句柄、已关闭目标拒绝与关闭前保护；测试多窗口隔离 |
| tab.activate | 已公开映射（仅所列子集） | 标签页 / `src/app/tab_controller.h` | 通过显式宿主适配与caller/owner校验；不接受内部action ID | tabs.activate | 仅通过 tabs.activate；作用于当前owner窗口的顶栏layout tabs或可见pane | 包声明并启用；caller代际有效；索引按执行时可见顺序校验；最后或固定标签不关闭；不允许关闭owner窗口 |
| tab.to_window | 计划公开（未实现） | 标签页 / `src/app/tab_controller.h` | 操作用户标签及持久显示状态，需要明确目标归属 | 无 | 拟新增当前 owner 标签句柄接口，不接受全局索引 | 定义稳定标签句柄、已关闭目标拒绝与关闭前保护；测试多窗口隔离 |
| window.layout | 已公开映射（仅所列子集） | 窗口布局 / `src/app/window_registry.h` | 已由CapabilityService提供显式参数适配与调用者/owner校验；内部action本身不授予包 | workspace.setLayout | 仅通过 workspace.setLayout；不接受内部action ID | 包声明能力并启用；当前调用者代际有效；参数严格校验；仅schema布局预设，不授权创建/关闭其他窗口 |
| pane.focus | 已公开映射（仅所列子集） | 分栏 / `src/app/tab_transfer.h` | 通过显式宿主适配与caller/owner校验；不接受内部action ID | panes.focus | 仅通过 panes.focus；作用于当前owner窗口的顶栏layout tabs或可见pane | 包声明并启用；caller代际有效；索引按执行时可见顺序校验；最后或固定标签不关闭；不允许关闭owner窗口 |
| pane.mark_target | 计划公开（未实现） | 分栏 / `src/app/tab_transfer.h` | 分栏目标会影响后续导航与传输接收方 | 无 | 拟新增 owner 内分栏句柄与显式焦点/目标接口 | 验证分栏合并后句柄失效及传输目标不随焦点漂移 |
| pane.to_window | 计划公开（未实现） | 分栏 / `src/app/tab_transfer.h` | 分栏目标会影响后续导航与传输接收方 | 无 | 拟新增 owner 内分栏句柄与显式焦点/目标接口 | 验证分栏合并后句柄失效及传输目标不随焦点漂移 |
| window.new | 计划公开（未实现） | 窗口布局 / `src/app/window_registry.h` | 窗口生命周期或跨窗口操作可能影响其他工作区 | 无 | 拟新增当前 owner 窗口操作接口；跨窗口目标单独授权 | 先确定窗口生命周期/会话撤销和跨窗口确认模型，验证关闭竞态 |
| window.close | 计划公开（未实现） | 窗口布局 / `src/app/window_registry.h` | 窗口生命周期或跨窗口操作可能影响其他工作区 | 无 | 拟新增当前 owner 窗口操作接口；跨窗口目标单独授权 | 先确定窗口生命周期/会话撤销和跨窗口确认模型，验证关闭竞态 |
| window.reopen | 计划公开（未实现） | 窗口布局 / `src/app/window_registry.h` | 窗口生命周期或跨窗口操作可能影响其他工作区 | 无 | 拟新增当前 owner 窗口操作接口；跨窗口目标单独授权 | 先确定窗口生命周期/会话撤销和跨窗口确认模型，验证关闭竞态 |
| window.activate | 计划公开（未实现） | 窗口布局 / `src/app/window_registry.h` | 窗口生命周期或跨窗口操作可能影响其他工作区 | 无 | 拟新增当前 owner 窗口操作接口；跨窗口目标单独授权 | 先确定窗口生命周期/会话撤销和跨窗口确认模型，验证关闭竞态 |
| window.arrange | 计划公开（未实现） | 窗口布局 / `src/app/window_registry.h` | 窗口生命周期或跨窗口操作可能影响其他工作区 | 无 | 拟新增当前 owner 窗口操作接口；跨窗口目标单独授权 | 先确定窗口生命周期/会话撤销和跨窗口确认模型，验证关闭竞态 |
| window.merge | 计划公开（未实现） | 窗口布局 / `src/app/window_registry.h` | 窗口生命周期或跨窗口操作可能影响其他工作区 | 无 | 拟新增当前 owner 窗口操作接口；跨窗口目标单独授权 | 先确定窗口生命周期/会话撤销和跨窗口确认模型，验证关闭竞态 |
| window.overview | 计划公开（未实现） | 窗口布局 / `src/app/window_registry.h` | 窗口生命周期或跨窗口操作可能影响其他工作区 | 无 | 拟新增当前 owner 窗口操作接口；跨窗口目标单独授权 | 先确定窗口生命周期/会话撤销和跨窗口确认模型，验证关闭竞态 |
| window.maximize | 计划公开（未实现） | 窗口布局 / `src/app/window_registry.h` | 窗口生命周期或跨窗口操作可能影响其他工作区 | 无 | 拟新增当前 owner 窗口操作接口；跨窗口目标单独授权 | 先确定窗口生命周期/会话撤销和跨窗口确认模型，验证关闭竞态 |
| sidebar.toggle | 计划公开（未实现） | 侧栏 / `src/app/actions/action_registry.cpp` | 当前实现操作宿主侧栏显示与分组状态 | 无 | 拟新增 owner 内侧栏显示枚举接口 | 枚举分组白名单、持久化影响和窄布局回归通过 |
| sidebar.section | 计划公开（未实现） | 侧栏 / `src/app/actions/action_registry.cpp` | 当前实现操作宿主侧栏显示与分组状态 | 无 | 拟新增 owner 内侧栏显示枚举接口 | 枚举分组白名单、持久化影响和窄布局回归通过 |
| favbar.toggle | 计划公开（未实现） | 收藏栏 / `src/app/favorites_bar.h` | 只需显示控制，不应连带授权收藏数据写入 | 无 | 拟新增 owner 内收藏栏显示开关 | 显示开关与收藏写入分离；主题/窄布局检查通过 |
| details.toggle | 计划公开（未实现） | 详情面板 / `src/app/actions/action_registry.cpp` | 面板显示可能触发所选文件读取 | 无 | 拟新增 owner 内详情显示接口 | 限定当前选择与受控预览读取，失效选择和云文件行为明确 |
| app.settings_page | 计划公开（未实现） | 应用界面 / `src/app/app_commands.h` | 当前入口打开宿主设置或帮助界面 | 无 | 拟新增受限宿主页面枚举接口 | 仅允许非敏感页面，禁止借页面导航自动修改权限 |
| app.shortcut_help | 计划公开（未实现） | 应用界面 / `src/app/app_commands.h` | 当前入口打开宿主设置或帮助界面 | 无 | 拟新增受限宿主页面枚举接口 | 仅允许非敏感页面，禁止借页面导航自动修改权限 |
| app.exit | 限制开放 | 应用界面 / `src/app/app_commands.h` | 权威动作风险为UserOnly，必须由用户本人触发 | 无 | 当前不开放；拟新增受限宿主页面枚举接口 | 仅允许非敏感页面，禁止借页面导航自动修改权限 |
| view.mode | 已公开映射（仅所列子集） | 视图 / `src/app/actions/action_registry.cpp` | 已由CapabilityService提供显式参数适配与调用者/owner校验；内部action本身不授予包 | view.setMode | 仅通过 view.setMode；不接受内部action ID | 包声明能力并启用；当前调用者代际有效；参数严格校验；仅schema枚举模式，不代表照片墙/全部视图选项已开放 |
| view.icon_size | 计划公开（未实现） | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| view.wall | 计划公开（未实现） | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| view.sort | 已公开映射（仅所列子集） | 视图 / `src/app/actions/action_registry.cpp` | 已通过显式能力适配作用于当前聚焦分栏的已加载普通目录；不接受任意条目或pane ID；排序可能保存目录视图偏好 | view.setSort | 仅通过 view.setSort；内部action ID不是权限 | 包声明并启用、caller/owner代际有效；非加载中的普通目录；虚拟页/搜索结果拒绝；column/direction严格枚举，已有文件夹优先规则仍生效 |
| view.group | 计划公开（未实现） | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| view.folders_first | 计划公开（未实现） | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| view.folder_sizes | 计划公开（未实现） | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| view.columns | 计划公开（未实现） | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| view.apply_all | 限制开放 | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 当前不开放；拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| view.filter | 计划公开（未实现） | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| view.compare | 计划公开（未实现） | 视图 / `src/app/actions/action_registry.cpp` | 改变视图参数或持久默认值，需要严格参数与作用域 | 无 | 拟新增 owner 内视图选项枚举接口；全局应用另列权限 | 定义合法排序/分组/列参数和当前目录作用域，区分临时与持久设置 |
| select.items | 计划公开（未实现） | 选择模型 / `src/app/actions/action_args.h` | 选择变化影响后续文件操作，不能用任意路径冒充条目 | 无 | 拟新增当前快照不透明条目的选择接口 | 验证条目所属快照、范围上限和快速切换后的失效拒绝 |
| select.all | 已公开映射（仅所列子集） | 选择模型 / `src/app/actions/action_args.h` | 已通过显式能力适配作用于当前聚焦分栏的已加载普通目录；不接受任意条目或pane ID | selection.selectAll | 仅通过 selection.selectAll；内部action ID不是权限 | 包声明并启用、caller/owner代际有效；非加载中的普通目录；虚拟页/搜索结果拒绝；仅当前过滤后可见项目参与全选/反选，其他分栏不变 |
| select.invert | 已公开映射（仅所列子集） | 选择模型 / `src/app/actions/action_args.h` | 已通过显式能力适配作用于当前聚焦分栏的已加载普通目录；不接受任意条目或pane ID | selection.invert | 仅通过 selection.invert；内部action ID不是权限 | 包声明并启用、caller/owner代际有效；非加载中的普通目录；虚拟页/搜索结果拒绝；仅当前过滤后可见项目参与全选/反选，其他分栏不变 |
| select.pattern | 计划公开（未实现） | 选择模型 / `src/app/actions/action_args.h` | 选择变化影响后续文件操作，不能用任意路径冒充条目 | 无 | 拟新增当前快照不透明条目的选择接口 | 验证条目所属快照、范围上限和快速切换后的失效拒绝 |
| select.clear | 已公开映射（仅所列子集） | 选择模型 / `src/app/actions/action_args.h` | 已通过显式能力适配作用于当前聚焦分栏的已加载普通目录；不接受任意条目或pane ID | selection.clear | 仅通过 selection.clear；内部action ID不是权限 | 包声明并启用、caller/owner代际有效；非加载中的普通目录；虚拟页/搜索结果拒绝；清空当前分栏选择，其他分栏不变 |
| edit.undo | 限制开放 | 撤销 / `src/app/app_ops_ui.h` | 撤销风险由历史操作决定，可能恢复或覆盖文件 | 无 | 当前不开放；不公开任意撤销栈；仅考虑包自有任务的可撤销操作 | 建立任务归属与撤销计划确认，验证用户操作不被其他包撤销 |
| file.copy | 限制开放 | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 当前不开放；拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.cut | 限制开放 | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 当前不开放；拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.copy_path | 限制开放 | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 当前不开放；拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.paste | 限制开放 | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 当前不开放；拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.copy_to | 已公开映射（仅所列子集） | 文件操作 / `src/app/app_ops_ui.h` | 仅映射当前选择传输到另一分栏的子集，内部任意目的地/跨窗口变体未公开 | files.copySelectionToOtherPane | 仅通过 files.copySelectionToOtherPane；不接受内部action ID | 包声明能力并启用；当前调用者代际有效；参数严格校验；源/目标准备通过且每次经原生确认，任务归属与取消受控 |
| file.move_to | 已公开映射（仅所列子集） | 文件操作 / `src/app/app_ops_ui.h` | 仅映射当前选择传输到另一分栏的子集，内部任意目的地/跨窗口变体未公开 | files.moveSelectionToOtherPane | 仅通过 files.moveSelectionToOtherPane；不接受内部action ID | 包声明能力并启用；当前调用者代际有效；参数严格校验；源/目标准备通过且每次经原生确认，任务归属与取消受控 |
| file.new_folder | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.new_file | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.rename | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.batch_rename | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.group_into_folder | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.dissolve | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.organize | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.find_duplicates | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.zip | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.recycle | 限制开放 | 文件操作 / `src/app/app_ops_ui.h` | 删除用户数据需要单独删除授权与逐次原生确认 | 无 | 当前不开放；拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.delete_permanent | 限制开放 | 文件操作 / `src/app/app_ops_ui.h` | 不可恢复删除必须保留用户专用确认边界 | 无 | 当前不开放；拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| recycle.restore | 限制开放 | 回收站 / `src/app/app_ops_ui.h` | 回收站影响用户已删除文件及恢复目标 | 无 | 当前不开放；不提供通用回收站写入入口；仅考虑包自有任务恢复 | 需要精确对象身份、恢复冲突计划与逐次原生确认 |
| recycle.empty | 限制开放 | 回收站 / `src/app/app_ops_ui.h` | 不可恢复删除必须保留用户专用确认边界 | 无 | 当前不开放；不提供通用回收站写入入口；仅考虑包自有任务恢复 | 需要精确对象身份、恢复冲突计划与逐次原生确认 |
| file.properties | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.context_menu | 限制开放 | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 当前不开放；拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.terminal | 限制开放 | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 当前不开放；拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| file.compute_size | 计划公开（未实现） | 文件操作 / `src/app/app_ops_ui.h` | 当前动作触及文件、剪贴板或宿主交互，需按操作拆分参数 | 无 | 拟新增当前目录条目句柄的受控文件请求，不接受任意路径 | 建立逐操作准备/确认/执行协议，验证冲突、取消、过期选择及审计 |
| shell.verbs | 限制开放 | Shell适配 / `src/app/context_menu.h` | Shell扩展和verb可能启动任意外部程序 | 无 | 当前不开放；保留用户Shell菜单；不将verb字符串作为增强包命令 | 仅在独立外部执行权限、目标校验与逐次确认协议完成后重新评审 |
| shell.run_verb | 限制开放 | Shell适配 / `src/app/context_menu.h` | 外部程序或远程副作用无法由宿主完整约束 | 无 | 当前不开放；保留用户Shell菜单；不将verb字符串作为增强包命令 | 仅在独立外部执行权限、目标校验与逐次确认协议完成后重新评审 |
| cloud.keep | 限制开放 | 云文件 / `src/fs/cloud_state.h` | 改变系统或设备状态，当前增强包不获得此权限 | 无 | 当前不开放；保留用户云文件操作入口 | 需供应商语义、流量/离线影响和可取消确认协议后重新评审 |
| cloud.free | 限制开放 | 云文件 / `src/fs/cloud_state.h` | 改变系统或设备状态，当前增强包不获得此权限 | 无 | 当前不开放；保留用户云文件操作入口 | 需供应商语义、流量/离线影响和可取消确认协议后重新评审 |
| tag.toggle | 计划公开（未实现） | 标签 / `src/app/tag_view.h` | 标签编辑属于用户共享元数据，不能跨包隐式修改 | 无 | 拟新增显式用户授权的标签对象接口 | 明确用户共享与包私有范围；验证标签ID、事务回滚和删除引用 |
| tag.clear | 计划公开（未实现） | 标签 / `src/app/tag_view.h` | 标签编辑属于用户共享元数据，不能跨包隐式修改 | 无 | 拟新增显式用户授权的标签对象接口 | 明确用户共享与包私有范围；验证标签ID、事务回滚和删除引用 |
| tag.panel | 计划公开（未实现） | 标签 / `src/app/tag_view.h` | 标签编辑属于用户共享元数据，不能跨包隐式修改 | 无 | 拟新增显式用户授权的标签对象接口 | 明确用户共享与包私有范围；验证标签ID、事务回滚和删除引用 |
| tag.create | 计划公开（未实现） | 标签 / `src/app/tag_view.h` | 标签编辑属于用户共享元数据，不能跨包隐式修改 | 无 | 拟新增显式用户授权的标签对象接口 | 明确用户共享与包私有范围；验证标签ID、事务回滚和删除引用 |
| tag.edit | 计划公开（未实现） | 标签 / `src/app/tag_view.h` | 标签编辑属于用户共享元数据，不能跨包隐式修改 | 无 | 拟新增显式用户授权的标签对象接口 | 明确用户共享与包私有范围；验证标签ID、事务回滚和删除引用 |
| tag.delete | 计划公开（未实现） | 标签 / `src/app/tag_view.h` | 标签编辑属于用户共享元数据，不能跨包隐式修改 | 无 | 拟新增显式用户授权的标签对象接口 | 明确用户共享与包私有范围；验证标签ID、事务回滚和删除引用 |
| tag.group | 计划公开（未实现） | 标签 / `src/app/tag_view.h` | 标签编辑属于用户共享元数据，不能跨包隐式修改 | 无 | 拟新增显式用户授权的标签对象接口 | 明确用户共享与包私有范围；验证标签ID、事务回滚和删除引用 |
| smartview.save | 计划公开（未实现） | 智能视图 / `src/app/tag_query.h` | 保存查询改变持久用户视图 | 无 | 拟新增受限查询AST的智能视图保存接口 | 定义无代码查询语法、包归属、重名处理和撤销 |
| fav.add | 计划公开（未实现） | 收藏 / `src/app/favorites_bar.h` | 收藏属于用户持久数据 | 无 | 拟新增条目句柄收藏接口 | 用户明确接受持久变更，验证去重、撤销与包卸载语义 |
| quick_access.pin | 计划公开（未实现） | 快速访问 / `src/app/places.h` | 固定项目改变用户共享导航入口 | 无 | 拟新增受限当前目录固定接口 | 限定当前条目并定义持久化与撤销；不得写入任意Shell位置 |
| recent.remove | 计划公开（未实现） | 最近记录 / `src/app/places.h` | 移除记录涉及用户历史隐私与持久数据 | 无 | 拟新增仅包自身来源历史记录清理接口 | 先建立记录来源标记，禁止清除其他用户活动 |
| workspace.pin | 计划公开（未实现） | 工作区 / `src/app/window_registry.h` | 固定工作区持久保存多窗口或目录信息 | 无 | 拟新增用户确认的当前工作区保存接口 | 定义目录信息脱敏、保存范围、覆盖确认和卸载保留行为 |
| alias.set | 计划公开（未实现） | 别名 / `src/app/folder_alias.h` | 别名影响持久路径解析或导入导出文件 | 无 | 拟新增受限别名对象接口，导入导出单独用户文件选择 | 验证目标目录身份、循环引用、冲突和受控文件选择 |
| alias.display | 计划公开（未实现） | 别名 / `src/app/folder_alias.h` | 别名影响持久路径解析或导入导出文件 | 无 | 拟新增受限别名对象接口，导入导出单独用户文件选择 | 验证目标目录身份、循环引用、冲突和受控文件选择 |
| alias.manage | 计划公开（未实现） | 别名 / `src/app/folder_alias.h` | 别名影响持久路径解析或导入导出文件 | 无 | 拟新增受限别名对象接口，导入导出单独用户文件选择 | 验证目标目录身份、循环引用、冲突和受控文件选择 |
| alias.export | 限制开放 | 别名 / `src/app/folder_alias.h` | 别名影响持久路径解析或导入导出文件 | 无 | 当前不开放；拟新增受限别名对象接口，导入导出单独用户文件选择 | 验证目标目录身份、循环引用、冲突和受控文件选择 |
| tray.add | 计划公开（未实现） | 暂存托盘 / `src/app/app_ops_ui.h` | 托盘可引用多个目录并触发批量文件操作 | 无 | 拟新增包自有托盘条目句柄接口 | 验证条目来源、失效和批量上限；传输/压缩须独立确认与取消 |
| tray.remove | 计划公开（未实现） | 暂存托盘 / `src/app/app_ops_ui.h` | 托盘可引用多个目录并触发批量文件操作 | 无 | 拟新增包自有托盘条目句柄接口 | 验证条目来源、失效和批量上限；传输/压缩须独立确认与取消 |
| tray.find_stale | 计划公开（未实现） | 暂存托盘 / `src/app/app_ops_ui.h` | 托盘可引用多个目录并触发批量文件操作 | 无 | 拟新增包自有托盘条目句柄接口 | 验证条目来源、失效和批量上限；传输/压缩须独立确认与取消 |
| tray.send_to | 计划公开（未实现） | 暂存托盘 / `src/app/app_ops_ui.h` | 托盘可引用多个目录并触发批量文件操作 | 无 | 拟新增包自有托盘条目句柄接口 | 验证条目来源、失效和批量上限；传输/压缩须独立确认与取消 |
| tray.zip | 计划公开（未实现） | 暂存托盘 / `src/app/app_ops_ui.h` | 托盘可引用多个目录并触发批量文件操作 | 无 | 拟新增包自有托盘条目句柄接口 | 验证条目来源、失效和批量上限；传输/压缩须独立确认与取消 |
| tray.compare | 计划公开（未实现） | 暂存托盘 / `src/app/app_ops_ui.h` | 托盘可引用多个目录并触发批量文件操作 | 无 | 拟新增包自有托盘条目句柄接口 | 验证条目来源、失效和批量上限；传输/压缩须独立确认与取消 |
| tray.text_diff | 计划公开（未实现） | 暂存托盘 / `src/app/app_ops_ui.h` | 托盘可引用多个目录并触发批量文件操作 | 无 | 拟新增包自有托盘条目句柄接口 | 验证条目来源、失效和批量上限；传输/压缩须独立确认与取消 |
| search.files | 计划公开（未实现） | 搜索 / `src/app/global_search_controller.h` | 搜索范围和内容结果可能泄露当前目录之外数据 | 无 | 拟新增授权范围内可取消搜索资源接口 | 限定scope、结果分页/预算/代际；全盘索引读取另行授权 |
| search.open | 计划公开（未实现） | 搜索 / `src/app/global_search_controller.h` | 搜索范围和内容结果可能泄露当前目录之外数据 | 无 | 拟新增授权范围内可取消搜索资源接口 | 限定scope、结果分页/预算/代际；全盘索引读取另行授权 |
| search.advanced | 计划公开（未实现） | 搜索 / `src/app/global_search_controller.h` | 搜索范围和内容结果可能泄露当前目录之外数据 | 无 | 拟新增授权范围内可取消搜索资源接口 | 限定scope、结果分页/预算/代际；全盘索引读取另行授权 |
| search.project | 计划公开（未实现） | 搜索 / `src/app/global_search_controller.h` | 搜索范围和内容结果可能泄露当前目录之外数据 | 无 | 拟新增授权范围内可取消搜索资源接口 | 限定scope、结果分页/预算/代际；全盘索引读取另行授权 |
| search.global | 计划公开（未实现） | 搜索 / `src/app/global_search_controller.h` | 搜索范围和内容结果可能泄露当前目录之外数据 | 无 | 拟新增授权范围内可取消搜索资源接口 | 限定scope、结果分页/预算/代际；全盘索引读取另行授权 |
| index.status | 计划公开（未实现） | 索引 / `src/app/global_search_controller.h` | 索引状态与系统服务安装的权限边界不同 | 无 | 拟新增脱敏只读索引状态接口；安装保持用户操作 | 状态不得泄露无权目录；服务安装仅经独立系统权限与用户确认 |
| index.install_full | 限制开放 | 索引 / `src/app/global_search_controller.h` | 改变系统或设备状态，当前增强包不获得此权限 | 无 | 当前不开放；拟新增脱敏只读索引状态接口；安装保持用户操作 | 状态不得泄露无权目录；服务安装仅经独立系统权限与用户确认 |
| preview.toggle | 计划公开（未实现） | 预览 / `src/app/app_commands.h` | 预览可能加载第三方解析器或触发云下载 | 无 | 拟新增当前选择受控预览接口，云下载保持单独确认 | 复用隔离预览宿主，验证取消/代际/文件读取授权 |
| preview.step | 计划公开（未实现） | 预览 / `src/app/app_commands.h` | 预览可能加载第三方解析器或触发云下载 | 无 | 拟新增当前选择受控预览接口，云下载保持单独确认 | 复用隔离预览宿主，验证取消/代际/文件读取授权 |
| preview.cloud_download | 限制开放 | 预览 / `src/app/app_commands.h` | 改变系统或设备状态，当前增强包不获得此权限 | 无 | 当前不开放；拟新增当前选择受控预览接口，云下载保持单独确认 | 复用隔离预览宿主，验证取消/代际/文件读取授权 |
| drive.eject | 限制开放 | 设备 / `src/app/drive_verbs.h` | 改变系统或设备状态，当前增强包不获得此权限 | 无 | 当前不开放；保留用户设备管理界面 | 设备身份、未完成I/O保护与原生确认方案完成后重新评审 |
| drive.disconnect | 限制开放 | 设备 / `src/app/drive_verbs.h` | 改变系统或设备状态，当前增强包不获得此权限 | 无 | 当前不开放；保留用户设备管理界面 | 设备身份、未完成I/O保护与原生确认方案完成后重新评审 |
| drive.unlock | 限制开放 | 设备 / `src/app/drive_verbs.h` | 权威动作风险为UserOnly，必须由用户本人触发 | 无 | 当前不开放；保留用户设备管理界面 | 设备身份、未完成I/O保护与原生确认方案完成后重新评审 |
| net.map_drive | 限制开放 | 网络位置 / `src/app/places.h` | 改变系统或设备状态，当前增强包不获得此权限 | 无 | 当前不开放；拟新增显式授权网络位置对象接口，不提供凭据字符串 | 拆分只读发现与持久连接；验证远程范围、取消和凭据由用户输入 |
| net.connect_server | 限制开放 | 网络位置 / `src/app/places.h` | 网络操作涉及远程凭据、发现范围及用户记录 | 无 | 当前不开放；拟新增显式授权网络位置对象接口，不提供凭据字符串 | 拆分只读发现与持久连接；验证远程范围、取消和凭据由用户输入 |
| net.place | 计划公开（未实现） | 网络位置 / `src/app/places.h` | 网络操作涉及远程凭据、发现范围及用户记录 | 无 | 拟新增显式授权网络位置对象接口，不提供凭据字符串 | 拆分只读发现与持久连接；验证远程范围、取消和凭据由用户输入 |
| net.recent | 计划公开（未实现） | 网络位置 / `src/app/places.h` | 网络操作涉及远程凭据、发现范围及用户记录 | 无 | 拟新增显式授权网络位置对象接口，不提供凭据字符串 | 拆分只读发现与持久连接；验证远程范围、取消和凭据由用户输入 |
| net.rescan | 计划公开（未实现） | 网络位置 / `src/app/places.h` | 网络操作涉及远程凭据、发现范围及用户记录 | 无 | 拟新增显式授权网络位置对象接口，不提供凭据字符串 | 拆分只读发现与持久连接；验证远程范围、取消和凭据由用户输入 |
| net.system_settings | 限制开放 | 网络位置 / `src/app/places.h` | 网络操作涉及远程凭据、发现范围及用户记录 | 无 | 当前不开放；拟新增显式授权网络位置对象接口，不提供凭据字符串 | 拆分只读发现与持久连接；验证远程范围、取消和凭据由用户输入 |
| git.refresh | 计划公开（未实现） | Git / `src/app/git_ui.h` | Git可能运行外部工具或访问远程仓库 | 无 | 保留现有用户Git入口，拟只读仓库状态资源单独设计 | 读状态先约束仓库范围；pull/push需外部执行与逐次确认模型 |
| git.pull | 限制开放 | Git / `src/app/git_ui.h` | 外部程序或远程副作用无法由宿主完整约束 | 无 | 当前不开放；保留现有用户Git入口，拟只读仓库状态资源单独设计 | 读状态先约束仓库范围；pull/push需外部执行与逐次确认模型 |
| git.push | 限制开放 | Git / `src/app/git_ui.h` | 外部程序或远程副作用无法由宿主完整约束 | 无 | 当前不开放；保留现有用户Git入口，拟只读仓库状态资源单独设计 | 读状态先约束仓库范围；pull/push需外部执行与逐次确认模型 |
| toolbar.open_with | 限制开放 | 工具栏 / `src/app/app_commands.h` | 外部程序或远程副作用无法由宿主完整约束 | 无 | 当前不开放；保留用户工具栏配置，增强包仅使用自身声明菜单 | 禁止持久化任意命令行；独立外部执行协议通过后重新评审 |
| toolbar.pin_app | 限制开放 | 工具栏 / `src/app/app_commands.h` | 工具配置可能保存外部可执行文件或改变全局菜单 | 无 | 当前不开放；保留用户工具栏配置，增强包仅使用自身声明菜单 | 禁止持久化任意命令行；独立外部执行协议通过后重新评审 |
| toolbar.customize | 限制开放 | 工具栏 / `src/app/app_commands.h` | 工具配置可能保存外部可执行文件或改变全局菜单 | 无 | 当前不开放；保留用户工具栏配置，增强包仅使用自身声明菜单 | 禁止持久化任意命令行；独立外部执行协议通过后重新评审 |
| toolbar.tools | 限制开放 | 工具栏 / `src/app/app_commands.h` | 工具配置可能保存外部可执行文件或改变全局菜单 | 无 | 当前不开放；保留用户工具栏配置，增强包仅使用自身声明菜单 | 禁止持久化任意命令行；独立外部执行协议通过后重新评审 |
| settings.read | 计划公开（未实现） | 全局设置 / `src/app/actions/action_settings.h` | 全局设置可改变Windows集成；包级settings资源不等价 | 无 | 保留全局写入限制；拟脱敏全局只读设置白名单 | 逐key分类系统副作用和隐私，用户授权与迁移/回滚测试通过 |
| settings.set | 限制开放 | 全局设置 / `src/app/actions/action_settings.h` | 全局设置可改变Windows集成；包级settings资源不等价 | 无 | 当前不开放；保留全局写入限制；拟脱敏全局只读设置白名单 | 逐key分类系统副作用和隐私，用户授权与迁移/回滚测试通过 |
| keymap.read | 计划公开（未实现） | 快捷键 / `src/app/keymap.h` | 快捷键配置影响全局输入与其他用户命令 | 无 | 拟新增仅本包命令快捷键申请接口 | 检测冲突、用户确认及卸载恢复，禁止覆盖系统/权限入口 |
| keymap.set | 计划公开（未实现） | 快捷键 / `src/app/keymap.h` | 快捷键配置影响全局输入与其他用户命令 | 无 | 拟新增仅本包命令快捷键申请接口 | 检测冲突、用户确认及卸载恢复，禁止覆盖系统/权限入口 |
| assistant.prefs | 限制开放 | AI权限与会话 / `src/app/ai_assistant_chat.h` | 权威动作风险为UserOnly，必须由用户本人触发 | 无 | 当前不开放；仅保留用户入口，不允许增强包替用户配置或提交 | 必须由用户直接操作，不通过增强包授权转授 |
| assistant.organize_here | 限制开放 | AI权限与会话 / `src/app/ai_assistant_chat.h` | 权威动作风险为UserOnly，必须由用户本人触发 | 无 | 当前不开放；仅保留用户入口，不允许增强包替用户配置或提交 | 必须由用户直接操作，不通过增强包授权转授 |

未发现注册行或明确例外的动作：0。该数量只衡量登记证据，不代表 runtime、AI 参数、撤销或确认流程已通过验收。

## 未接线与平台边界

增强包不得传内部 action ID 给 workflow.run；工作流只能引用本包已声明命令 ID，逐步复用能力服务。内部 run_actions 的权限与确认模型不被增强包继承。脚本、任意进程和任意路径访问没有因工作流开放。

Resource API 的清单权限与宿主 broker 接线须单独验证；仅有 resources schema 不代表请求入口已公开。audit/持久设置等接口的具体 payload 与调用者归属以最终宿主契约为准。
