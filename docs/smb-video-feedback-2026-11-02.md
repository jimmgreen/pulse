# 飞牛 NAS / SMB 视频卡顿反馈（2026-11-02）

## 用户信息与日志边界

用户反馈通过 SMB 挂载飞牛 NAS 上的网盘：浏览目录正常，双击视频及空格预览均卡顿，并称容易导致 Pulse 崩溃。来源为 Desktop/New folder (2)/User，内嵌 Pulse-diagnostics/User 是重复导出内容，不叠加计数。只读用户日志。

support-report：1,570条运行记录，6种失败模式。诊断包不含崩溃转储、异常地址或堆栈，因此不能宣称已定位/修复用户主程序崩溃根因。日志记录版本1.0.58，build 20261027T200036Z-0B84E1AE；不能仅凭时间戳推断全部代码差异。

## 可确认的异常

- preview_client_failure stage5/error1460：43次，末次约4,031ms，首次约8,032ms，表明预览请求超时。
- stage2/error231：86次，记录host exit3；preview-33356启动后立即process_stop。与创建唯一命名管道失败退出的路径相符。
- preview_handler_failure 0x80040154：39次，表示处理器未注册；不能把它当成崩溃异常。
- 四条外部打开请求均error0，Shell调用耗时579/641/234/2,281ms；只能证明启动交接成功，不能证明播放器首帧或网络吞吐正常。请求运行在后台STA，非UI线程。
- 主进程PID30432仍有08:02:03 UTC的健康记录；现有采样最大private约541MiB，不能据此确定或排除其他时刻的崩溃。

## 修复

旧预览host的强制终止是异步的；StopChild仅有2秒有界等待。SMB内核I/O尚未取消完成时，旧host可能仍拥有原命名管道。替代host复用同lane名称，FILE_FLAG_FIRST_PIPE_INSTANCE会失败退出3，使之后的请求继续失败。

替代host使用lane token + 实际host PID独立名称，客户端连接CreateProcess返回的PID对应端点。保留用户范围管道安全属性和已有服务器PID身份检查；旧调用方式保留兼容。没有延长读取超时，也没有将网络访问放入UI线程。

现有限制：普通失败已有次数受限的退避重试；跨不同文件/缓存重置的无限内核I/O阻塞，并没有新增全局遗留进程数限制。本修改解决重启连接冲突，不证明NAS网络本身或外部播放器性能已解决。

## 验证

scripts/test_preview_restart.py已实跑通过：legacy占用失败负对照复现exit3；保持旧连接不释放，三个新host均通过服务器PID及真实协议响应字段校验。本机没有用户的飞牛NAS挂载环境，该用例精确复现旧管道未释放的条件，不冒充真实NAS测试。

## 后续完整日志补充

用户追加 Desktop/Diagnostics，包含993条保留记录，Crashes目录为空。累计计数替换前一包（同一会话），不叠加：连接忙/host exit3为291次，响应超时为86次。08:28:18 UTC外部打开返回成功，但耗时11,234ms；主进程相同PID仍在08:31:42完成目录导航。新的成功预览响应也有5,140ms、7,375ms，说明并非所有慢请求都失败。

双击路径复查发现启动前会重复GetFileAttributesW查询远程父目录，仅用于决定工作目录。改为对UNC/映射网络盘及已识别的远程类盘直接使用父路径，交由原Shell打开机制验证目标；本地路径的有效目录检查保持不变。增加working_dir_ms与shell_ms诊断字段，后续能够区分工作目录准备和系统打开耗时。现有日志没有这两个字段，不能断言11.2秒全部由该次查询造成。

## 最终验证结果

- pulse、pulse_preview、pulse_picker_ui_proof、pulse_elevated受影响目标构建成功。构建曾报告app_input.cpp既有/其他改动中的pointer_moved未使用警告，本次未修改该文件，不扩大修复。
- pulse_open_item_directory_test：12/12通过，UNC、扩展UNC、映射盘、带空格中文名称不调用属性查询；本地存在/不存在/非目录及Shell路径规则通过。
- scripts/test_preview_restart.py：真实预览进程的旧命名冲突负对照，以及新命名三代共存、合法响应、进程身份与请求代际检查均通过；测试进程全部清理，日志写入独立bench_data目录。
- 发布载荷检查与静态运行库检查通过，测试开关保持关闭。
- 没有更改NAS、用户文件或偏好，没有在不存在的用户NAS连接上声称实测。

交付安装包：dist\PulseSetup-1.0.58-test-smb-preview-20261102.exe。46个载荷文件逐项哈希及版本目录闭包检查通过，4个受影响生产二进制均与最新build一致。SHA256：5f1219b218872eae11021337e1568dc1da6c35a5c49777d2d115840a88adb224。尚未替换本机已安装版本。
