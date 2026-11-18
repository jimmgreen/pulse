# 2026-10-28 修复版本核对

核对时间：2026-10-29，北京时间。只读核对历史记录、当前源码、安装包及已安装文件；本次没有重跑功能测试，也没有再次修改产品代码。

## 结论

昨天任务“看看现在的 批量重命名是不是只能替换？能否直接输入要改成的文件名直接改呢”（01a17ff8-664b-7990-a3e6-073537ab8492）中，下面列出的实际修复均位于 `C:/Users/SS/Desktop/pulse/build/ai_pack_wt`，其 version.txt 为 1.0.58。不能因为聊天默认 cwd 是根目录，就判断实际操作都在 1.0.56；已对照每轮命令的 cwd、最终交付路径和当前代码。

目录树动画改动仍在 `src/ui/ui_pane_list.cpp`：list_context 包含 tree revision，row_key 优先使用完整路径。顶部固定父目录折叠改动仍在 `src/app/app_details_tree.cpp` 与 `src/ui/ui_pane_list.cpp`。主程序编译时间为 10 月 28 日 23:09，晚于这两项源文件修改时间 18:51。

## 昨天修复清单及记录中的验证边界

| 项目 | 位置/状态 | 昨天记录的验证情况（非本次重测） |
|---|---|---|
| 目录树展开触发其他行闪动 | 58 版 ui_pane_list.cpp | 59 项动画测试；未完成实际界面复现确认 |
| 固定父目录箭头不能折叠 | 58 版 app_details_tree.cpp、ui_pane_list.cpp | 8 项命中测试；完整交互未验证 |
| 双栏移动/复制到目标栏无效 | 58 版 pane_transfer_target.h、app_commands.cpp | 7 项目标选择测试；未实测完整移动 |
| GIF 静态预览/解码预算 | 58 版 gif_decoder.cpp | 用户文件 87 帧解码；用户随后反馈解决 |
| 扩展名按钮无效 | 58 版 app_commands.h、app_commands.cpp | 编号隔离与显示测试；用户随后反馈可用 |
| 快速搜索图标依赖字体 | 58 版 global_search_window.cpp | 图标检查；目视验证不完整 |
| 双击 Ctrl 搜索闪烁 | 58 版 global_search_window.cpp、controller | 多轮修改和日志；用户反馈未退出旧进程，随后确认解决 |
| 双屏搜索跟随鼠标屏幕 | 同上 | 编译通过；双屏实测未完成 |
| 映射盘容量被网络盘提示遮住 | 58 版 ui_pane_list.cpp | 编译通过；真实映射盘未实测 |
| folder_sizes/places 临时文件反复写入 | 58 版 folder_size_store.cpp、places.cpp | 定向测试通过；用户机器持续循环需复测 |
| OneDrive 路径格式导致去重遗漏 | 58 版 cloud_roots.cpp、app_model.cpp | 已修改；原截图是否同根目录未证实 |
| 地址栏 Enter 系统提示音 | 58 版 app_hosted_edit.cpp | 编译通过；未听音实测 |
| 地址补全、箭头子目录、文件路径及引号处理 | 58 版 address_completion.h、address_path_open.cpp、breadcrumb_menu.cpp | 19 项检查；真实断线网络盘未实测 |
| 小字号扩展名字形对齐、子目录菜单大小/速度 | 58 版 lumatext_renderer.cpp、breadcrumb_menu.cpp | 三种缩放、深色及渲染对照；本机目录加载测试 |
| 托盘窗口位置/尺寸恢复 | 58 版 tray_controller.cpp | 定向测试及用户后续日志；不能据此排除全部多窗口问题 |
| 托盘唤起额外恢复已关闭窗口 | 58 版 app_main.cpp | 20 项关闭历史测试；完整托盘操作未实测 |
| 管理员权限下新建文件夹 | 58 版 elevated_host、ops、app_ops_ui.cpp | 定向协议/窗口测试；真实 UAC 完整操作未实测 |
| Excel 鼠标松开后继续框选 | 58 版 preview_handler_host.cpp | 改动已编译；原记录未完成完整交互验证 |
| 表格预览漏填充色 | 58 版 table_document.cpp、table_view.cpp | 用户文件 12 表、3386 个填充单元格，深浅主题渲染检查 |
| 动态右键、图标、打开方式数量 | 58 版 shell_host、context_menu_controller.cpp | 原生条目比对；小米互传仍需目标机器验证 |
| 菜单隐藏设置部分入口失效 | 58 版 app_commands.cpp、quick_access.cpp 等 | 系统菜单 5 项定向测试；未逐个目视验证 |

尚未交付修复的历史问题包括：混合 DPI 主窗口错位（等待缩放与复现证据）、右键压缩卡住（尚未确认具体命令）。三栏侧键问题当时只完成分析。这些不能算成“已修复但漏合入”。

## 安装包与当前安装核验

`C:/Users/SS/Desktop/pulse/dist/PulseSetup-1.0.58-start-pin-fix.exe` 中以下五个文件均与 58 工作目录 build 中的文件逐字节一致，且本次读取的 `C:/Program Files/Pulse` 已安装文件也全部一致：

- pulse.exe
- Pulse.Preview.exe
- pulse_elevated.exe
- pulse_shell.exe
- pulse_start_pin.dll

因此没有发现上述目录树等修复因打包取错旧目录而遗漏。若当前仍出现相关现象，应作为已合入代码的剩余功能问题继续复现，不能再用“旧版本未合入”解释。

根目录仍为 1.0.56，且另一个 Shell 接管/文件对话框重构任务仍在该目录活动；该任务属于尚在进行的独立改造，本报告不把它的未完成改动混入 58 版。
