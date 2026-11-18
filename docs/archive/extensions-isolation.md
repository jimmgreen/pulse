# 通用隔离探针：独立实验参考

本页不定义增强包运行路线。JavaScript 执行层已删除；当前 C++ DLL 在宿主进程内运行，边界见 [平台契约](extensions-platform-contract.md)。

`src/extension_host/isolation.*`、`probe_main.cpp` 和相关探针保留为独立的 Windows 隔离研究基础，不加载增强包源码，也不进入原生 SDK 作者流程。它们不是桌面磁贴插件的前置条件。

旧 AppContainer/回环实验的环境、未决结论与诊断保存在 `docs/archive/extensions-isolation-poc.md`，不能拿旧探针结果证明当前原生代码具有沙箱隔离。主路线图不再把“等待脚本隔离完成”列为交付阶段。
