# 原生对话框契约实现与验证（2026-10-31）

本文记录当前工作树的验证，不代表旧测试包、已安装副本或全部第三方应用已经获得这些能力。未改动系统注册；原生对照使用 System32 comdlg32 工厂，避免现有自动注册影响。

## 已实现并验证

- 同一 COM 对象重复 Show；每次清理结果、取消状态，保留配置与订阅。`--repeat-show-contract` 106 项通过。
- 文件夹单选/多选及非空初始名称；保留 PICKFOLDERS 下 SetFileTypes 的 E_UNEXPECTED，仅对该已知无效过滤调用豁免全局拒绝。生产 RuntimeContractCertified、协议、UI 和 Shell 结果数组贯通。契约 10 项、包装器 28 项通过。
- 真实目录多选：鼠标选择两目录，首次 FileOk 否决，第二次接受；精确两条路径与同一 HWND，23 项通过。名称行布局 20 张图、零失败。
- 显示期间 SetFolder：外部请求先加载后提交，失败/否决保留旧状态；回调内请求排队，完成通知后才更新原生镜像。外部真实 UI 37 项、回调真实 UI 42 项、包装器 24 项通过。原生对照确认回调内返回 S_OK 时 GetFolder 仍为旧目录，随后才发生导航事件。
- Legacy GetOpenFileName/GetSaveFileName A/W：真实 runtime 路由、System32 原生 COM 工厂、参数/结果适配及 CommDlgExtendedError；支持集外在显示前回原始 OFN，显示后不重放。入口定向 19 项通过，包含多选编码、偏移、缓冲不足、取消及对象复用。
- Legacy 宿主实际读写：用户依次确认 OpenW/SaveW/OpenA/SaveA，四场均 `path_matches=1 host_consumed=1 native=0 native_api=0 cleanup=1`，最终 `LEGACY_UI_ALL cases=4 failures=0`。Open 读回夹具内容；Save 仅在路径精确匹配自有目标后创建并读回核对。
- 原生 A/W 当前目录对照四组通过：带 NOCHANGEDIR 保留原目录，不带时转入选择目录。当前 Legacy 支持集要求该标志。

## 用户存储策略

用户路径策略与可信模块加载分离：普通 UNC、扩展 UNC、固定盘和可移动盘可进入后台权限/身份探测，保留祖先 reparse、offline、recall 拒绝与最终句柄路径一致性检查。可信 DLL 加载规则未放宽。43 项注入策略及真实 provider 配置、25 项 worker 访问回归、41 项真实 UI ready-close 检查通过。

没有真实 UNC 共享或 USB 的实机认证；映射盘最终路径若变成 UNC，当前可能保守拒绝。Save 的初始目录、快捷位置和动态导航保留原 LocalSave 边界，不因 Open 扩展而隐式开放 UNC Save。

## 跨 common dialog 错误来源

用户明确授权后，已应用 `bench_data/legacy-common-dialog-error.proposed.patch`。14 个颜色、字体、打印、页面设置、查找/替换 A/W 入口仅清理 Pulse 错误来源，继续调用原生函数，保留 BOOL/HRESULT/HWND、LastError 以及异常传播。

`--other-common-dialog-errors` 58 项通过，涵盖取消、原生错误、嵌套调用、C++/SEH 展开和线程隔离；`--legacy-only` 19 项回归通过。普通运行时及界面运行时构建无新增警告。两个一次性自有进程分别加载实际 DLL，均得到 query=S_OK、installed_entries=0xF、ready=0，确认完整 Detours 事务安装成功且未启用宿主接管配置。未注入其他进程、未修改系统注册。

日志：`build-f04/other-common-dialog-errors.log`、`build-f04/legacy-after-error-hooks.log`。测试未实际打印，也不代表所有第三方厂商对话框都已实测。

## 验证边界

- 测试构建目录为 build-f04，真实界面需要 PULSE_WITH_LUMATEXT=ON。关闭该组件的构建在 Show 前返回 ERROR_NOT_SUPPORTED，不算功能通过。
- 自动键盘测试因前台 guard 拒绝未通过；鼠标手工模式独立验证结果，没有删除或弱化原键盘测试。
- 默认独立桌面的 UI 初始化及 ready-close 对照均返回 0x8007065B；导航真实 UI 在交互桌面通过。
- 原生文件夹初始名称对照的结果路径正确，但 ClearClientData 返回 E_FAIL；整项不列为通过，保留原始日志。
- Legacy 仍只支持明确的 Explorer、隐藏只读控件、NOCHANGEDIR 等选项组合。hook/template、自定义 filter 和未实现选项不计为替换支持。
- x86、管理员/AppContainer、云占位、任意 Shell namespace、全部动态控件/属性、不可变版本身份与升级恢复未因此自动获得认证。不得将取消、原生回退、仅显示界面计作完整替换成功。

## 定向复查

使用 scripts/vcvars.bat 配置环境，只构建相关目标。入口包括：

- pulse_file_dialog_inprocess_wrapper_test --repeat-show-contract / --folders / --navigate-contract
- pulse_file_dialog_contract_test --folders
- pulse_picker_inprocess_v2_test <绝对UI DLL路径> navigate-external或navigate-callback --interactive
- pulse_picker_inprocess_v2_input_test <绝对UI DLL路径> --folders --manual-multi
- pulse_file_dialog_runtime_entry_test --legacy-only / --legacy-ui-all / --other-common-dialog-errors

真实交互测试只使用 bench_data 内本进程创建的夹具。需要人工或授权的电脑控制操作；不要仅启动进程就记录成功。
