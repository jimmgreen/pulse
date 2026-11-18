# XMind x64 打开参数修复（2026-10-31 r3）

## 问题与改动
用户日志 pointer_bits=64、activation enabled=1、provider hr=0；observe_contract reason=7（MissingValidationOptions）、options=4672/0x1240，随后回原生。0x1240含FILEMUSTEXIST、FORCEFILESYSTEM和ALLOWMULTISELECT，缺PATHMUSTEXIST。

现在只为 LocalOpenV1、普通文件、FILEMUSTEXIST 且没有 NOVALIDATE 的组合允许缺 PATHMUSTEXIST。契约和生产 RuntimeContractCertified 同步；不改调用者选项，不放宽普通策略、Save、缺FILEMUSTEXIST或NOVALIDATE缺PATH的边界。现有后台检查仍验证祖先与目标、OPEN_EXISTING及访问权限，多选失败不返回部分结果。

官方选项语义：https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_fileopendialogoptions

## 通过的验证
- pulse_file_dialog_contract_test --xmind-open-contract：12项通过，包括精确0x1240、单选0x1040、实际访问检查标志、多选，以及拒绝边界。
- pulse_file_picker_model_test --local-open：27项通过，包含新增的不存在父目录、多选有效前缀后不存在父目录、缺失文件、共享拒绝与权限检查。
- 受影响的RuntimeUiProof64和Auto64构建成功；未重跑全部历史测试。
- pulse_dialog_auto_com_activation_test ABS_DLL --xmind-ui ABS_FIXTURE：在隔离HKCU中绑定实际Auto64 DLL，直接其DllGetClassObject创建对象；精确0x1240、空文件名、XMind/All过滤器、index1，实际可见PulseFolderPickerWindow，取消返回800704C7且无选择结果。
- build目录和最终交付目录各一次上述实际DLL显示检查均通过（visible=1 expired=0）。这是DLL集成验证，不是全局注册表激活测试，也不等于XMind客户端已现场验收。
- 最终包36个组件/许可文件，架构、哈希及依赖闭包验证通过。

## 保留的失败与验证限制
- 原生System32对照 exact0x1240：单选返回一个自有现存文件，多选返回两个；但ClearClientData E_FAIL及CloseDesktop清理失败，因此整项不计通过。
- 原生缺失文件/缺失父目录对照：未触发FileOk，错误提示无法被驱动正常关闭，最终watchdog超时；未删除断言或记作通过，日志和夹具保留。
- 本机真实64位注册仍指向不存在的旧R42包，使受管测试在原生CoCreateInstance阶段返回8007007E。没有改动用户真实注册；采用隔离配置的实际Auto64 DLL测试验证本修复。用户需从新包Enable.cmd更新启用路径并重启XMind。
- 原有x86交付中的公开令牌查询兼容修复也已包含在本次64位DLL中。

日志：bench_data/xmind-worker.log、xmind-auto-ui.log、xmind-final-package-ui.log、xmind-native-single/multi/missing/missing-parent.log。修改位于file_dialog_contract.cpp、file_dialog_runtime.h及相应测试。
