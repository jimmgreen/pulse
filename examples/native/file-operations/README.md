# C++ 文件操作示例

示例只包含公开 SDK 头。DLL 组合 Pulse 原生按钮和文字，自己选择动作、保留请求 ID、展示状态；宿主负责捕获当前选择、异步预检、原生确认、执行和取消。

在 x64 MSVC 开发环境中运行：

```powershell
cmake -S examples/native/file-operations -B build-native-file-operations -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-file-operations
```

使用作者 CLI 的 `native-manifest`、`pack`、`verify`、`install --allow-native` 和 `enable --allow-native` 安装，再通过正常增强包菜单打开。详情见 [原生包指南](../../../docs/native-packages.md)。

复制/移动要求主窗口显示两个窗格；回收站操作只需要当前窗格的 1–64 个普通文件选择。每次提交都会显示宿主确认。示例使用“刷新自己的状态”手动轮询，终态读取后释放请求；“取消”只发出尽力取消请求。停用时取消自己的未完成请求并撤销 UI 订阅。

不支持永久删除、目录递归、任意路径或任意目录目标。回收策略拒绝任何选中项时，本次请求在执行前失败；执行中状态变化仍可能形成部分结果。`COMPLETED` 才表示全部源项完成；取消不撤销已经完成的文件效果。完整契约见 [文件操作接口](../../../sdk/native/README.md)。

AI 作者提示：复用本示例的请求寿命与错误处理，在 UI 线程调用 `pulse.fileops`；业务算法放在 DLL。不要把 `submit` 接受、显示确认或任务排队解释为文件完成。
