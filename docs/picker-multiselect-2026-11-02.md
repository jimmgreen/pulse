# 多选后关闭窗口但未打开文件（2026-11-02）

## 原因与修复

用户反馈 Photoshop、Cursor 单选正常，多选后选择窗口关闭但没有打开文件。
用户本机 Cursor 的本次诊断（PID 41292）记录 `Show=0x8007065B`、`win32=1627`、`status=3`、`flags=1`，未进入 FileOk。
最小日志保存于 `bench_data/picker-multiselect-20261102/cursor-failure-excerpt.jsonl`。

列表多选将文件名编辑框隐藏并显示文件名标签。`FlushPaintedEdits` 要求出现新呈现帧，但此前只依赖可见编辑框请求重绘；编辑框隐藏时，`UpdateWindow` 没有待重绘区域，随后误报呈现失败，关闭窗口且没有交付结果。

`src/ui/folder_picker_dialog.cpp` 的最小修复是在 `UpdateWindow` 前显式请求父窗口重绘。保留编辑框错误检查和实际呈现帧检查，不吞掉真正的呈现错误，不改变选择结果、校验或取消语义。

## 验证

- 原始真实 UI 的十个 JPG 列表选择：修复前 56 项中 2 项失败，`Show=0x8007065B`，未到 FileOk（原始输出在工具会话，没有独立修复前日志）。
- 同一用例修复后 61 项全部通过，完整返回十条有序路径：`bench_data/picker-multiselect-20261102/ten-list-after.log`。
- 原双文件输入提交：47 项通过。
- 真实取消：41 项通过，日志 `cancel.log`。
- 单文件提交在 `open-mask-sequence` 中成功：FileOk 与终态接受通过；但该测试另有一项过滤器配置断言失败，总计 70 项、1 项失败，保留在 `single-submit.log`，不将整个用例记为通过，不扩展到无关过滤器修改。
- 独立真实调用方：系统 comdlg32 类工厂 → Pulse wrapper → 实际 UI → Show → GetResults，列表十选返回成功，十条路径及顺序完整，无原生回退，调用方窗口恢复。测试文件仅用于验证路径交付，不验证 Photoshop 图片解码。
- 调用方列表测试日志：`bench_data/wrapper-multi-list-final-2c10ab034eb64db295bfa72520ea392e.log`；输入多文件名对照：`bench_data/wrapper-multi-typed-fixed-94e04e72db8844d0b1a6f80fefaaf315.log`。两者均 exit 0 / failures 0。
- 受影响的主程序、UI DLL 与定向测试目标构建通过；不运行全量历史回归。

没有直接验证 Photoshop。操作 Cursor 窗口被自动审批拒绝，未绕过；本机失败证据来自用户自行操作后的日志。独立测试未修改用户文件、注册或偏好。

## DialogBridge 副本说明

当前不可变私有组件目录用于让已运行的调用程序继续使用旧组件，安装根目录可独立升级。不能直接删除当前依赖：

- `pulse.exe`：当前宿主协议要求；将来可以拆成专用小宿主，但本次不扩大重构范围。
- `lumatext.dll`：UI DLL 和宿主的直接依赖。
- `Pulse.Preview.exe`、`pdfium.dll`：缩略图和 PDF 预览。
- `pulse_shell.exe`、`pulse_elevated.exe`：Shell 与提权操作辅助进程。
- `manifest.json`：该目录的完整性清单，其哈希决定目录身份。

不能直接以根目录硬链接代替，避免共享文件对象的锁与原地修改破坏旧组件不可变性。
安装修复后，已加载旧组件的 Photoshop/Cursor 需要重新启动才能使用新代码；这不等于安装时必须关闭所有调用程序。

## 测试安装包

`dist/PulseSetup-1.0.58-test-multiselect-20261102.exe`，12,797,872 字节。

SHA256：`8a979619da75f70d63948b96ed06772d579b6622b1480bc02fcce4a734780de8`。
新 bundle：`34735e66708493fa20aa9fad805ae9f37a5669c9a3cba315176fc12a363d019b`。
压缩载荷哈希、72 个文件逐项哈希及私有组件依赖完整性校验均通过。
