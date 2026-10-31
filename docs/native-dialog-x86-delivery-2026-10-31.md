# x86 交付验证（2026-10-31）

交付版本 native-x86-20261031-r2，独立x86包。未修改真实用户COM注册。

## 改动

- x86 启动器、RuntimeUiProof32R8、Auto32、选择器及主程序、Shell/预览/操作辅助组件。
- 32/64位注册视图和配置隔离；控制器共用名称但位于各自包内。
- Python/C++包验证严格检查架构和自动组件对，不允许混用Auto32/Auto64。
- LumaText以既有SDK对应提交94e913c13b20b9f7f7dee01c9f0b10f831506379独立构建x86 /MT；HarfBuzz/FreeType锁定哈希核验。隔离源码配置门禁补丁与复现脚本位于bench_data/x86-deps，不改原库。
- PDFium同版本155.0.8044.0官方x86归档，SHA256 658da1a7ee0196fc1d366a7b0f2a79b61a51a809852e3fda49f68aef09768df9；CMake通过PULSE_PDFIUM_ROOT选择独立依赖。
- 修正x86下PDF读取的size_t/DWORD类型冲突及卡顿采样的寄存器架构；x86无帧指针时仅保留可取得的部分栈。
- 实测发现GetTokenInformation(TokenIsRestricted)返回87，握手后链路仍拒绝普通进程。四处改用公开IsTokenRestricted接口，查询错误仍拒绝。该API检查restricting SIDs，不把普通UAC过滤等同于受限SID列表：https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-istokenrestricted 。

## 实际通过

- x86交付组件Release构建，文字库实际DLL加载、ABI与context创建释放。
- 选择器真实交互桌面ready-close：41项，0失败。
- x86 Legacy A/W入口：19项通过；14类其他common dialog错误来源：58项通过。令牌修复后Legacy定向再次通过。
- Python包验证：17项通过。C++包验证x86/x64各65项通过；两架构隔离注册回归通过。
- 普通/无效/受限SID令牌定向：x86/x64各4项通过。
- 实际Auto32 DLL的未修饰导出与Open/Save工厂直接调用通过。这不等同于注册表COM自动激活通过。
- 最终包36个文件及依赖闭包验证通过。
- 从最终包的PulseDialogLauncher启动自有x86 smoke进程：bundle_valid=1，process_started=1，握手成功；运行时Query ready=1。
- 该进程实际观察到本进程线程可见PulseFolderPickerWindow：COM打开和GetOpenFileNameW打开均通过，取消返回正确、无选择结果，进程退出。不是只看握手判定成功。
- 最终报告：bench_data/x86-managed-delivery/report-final.log；初次ready失败证据保留report-1/2，修复后report-3和final通过。

## 未通过与限制

- 隔离HKCU/HKCR中的CoCreateInstance仍创建原生对象，没有加载目标shim；保留原断言和失败结果。查询隔离HKCR能读到正确DLL，不能仅据此推断真实用户注册错误。没有改写真实COM注册来绕过限制。
- pulse_pdf_unicode_test的“mixed first page and supplementary-only next page retain their boundary”失败；相同用例在x64对照也失败，保留为既有问题，没有删除或放宽。其余17项通过，包括真实PDF读取与独立文档宿主。不能宣称完整PDF测试通过。
- 未在用户另一台电脑的迅雷上实测导入完成；本次真实窗口验证覆盖打开/取消，不能当作迅雷导入成功或x86所有应用验收。
- x86自动接管仍需要用户运行Enable-x86并重启应用；旧式入口需Launch-x86。所有不支持选项继续原生回退。

## 构建入口

scripts/vcvars.bat x86；build-x86为独立Ninja Release目录，PULSE_WITH_LUMATEXT=ON，LumaText_DIR指向bench_data/x86-deps/lumatext-sdk/lib/cmake/LumaText，PULSE_PDFIUM_ROOT指向bench_data/x86-deps/pdfium。
只构建交付组件及对应测试，未运行全量历史回归。
