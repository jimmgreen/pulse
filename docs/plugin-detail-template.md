# 插件详情模板（主程序所有）

主程序统一负责详情头部、管理动作和 tabs；插件仅提供包 metadata 和设置内容，不应自行绘制重复的详情头部或卸载按钮。

- `SettingsLayout::DetailHeader`、`LayoutExtensionDetailHeader` 和 `DrawExtensionDetailHeader` 是商店详情（包括 7-Zip）与 native focused 设置页共用的布局/绘制模板。名称、描述、图标、版本和启用状态来自包 metadata；没有远程目录时所有本地 installed 包仍生成 metadata 行。manifest 未提供的描述以现有 publisher 信息回退，不伪造产品描述。
- tabs 共用布局和绘制函数；常用设置始终显示，右键菜单由包能力决定，7-Zip 可增加其已有格式/关于内容页。窄宽度使用相同换行规则。
- 主程序提供 `StoreDetailEnable` 和 `StoreDetailMore`。native 页通过 focused package ID 在当前 installed 列表中解析目标，7-Zip 保留原 installed index。缺失 ID 不借用另一个包的索引。
- 更多菜单的卸载/回滚与 installed 列表使用同一管理菜单、`ExtensionStoreAction` 和 `ExtensionManagementUi::Action`，保留确认、generation 检查及服务端执行。详情页不自行删除包或源码目录。
- 详情头部不再显示 native-code 技术 pill；安装/发布者权限审核保持原路径。未匹配目录的本地版本 hash 保留现有含义，不冒称已发布版本。

定向验证：`build_st` 的 `pulse` 目标（SELFTEST=ON），运行 `python bench_data/plugin_detail_template/run_render.py`，只选择 `PULSE_TEST_NATIVE_HEADER`。测试使用真实 WindowViewModel、MakeSettingsLayout、HitTest、MainRenderer 和 compositor 离屏 PNG；不执行实际插件卸载。
