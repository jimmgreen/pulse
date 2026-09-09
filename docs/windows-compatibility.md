# Windows 兼容性

Pulse 提供两种 x64 安装包：

| 版本 | 系统 |
| --- | --- |
| Windows 版（64位） | Windows 10 / Windows 11 |
| Windows 8.1 兼容版（64位） | Windows 8.1 |

两者功能一致。依赖较新系统接口的窗口材质等视觉效果，会在不可用时使用兼容显示方式。

Windows 8.1 发布构建使用 Visual Studio 2022 v143 和静态 VC 运行库。内部构建选项 `PULSE_WIN81_CANDIDATE` 用于选择兼容工具链及对应更新源；发布文件名为 `PulseSetup-版本-win81.exe`。

运行 `pwsh ./scripts/build_release_ci.ps1 -Channel win81` 可完成构建、回归测试、导入检查和安装包生成。静态导入检查用于发现不兼容的直接依赖，不能替代实际系统上的运行验证。
