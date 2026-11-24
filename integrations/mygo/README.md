# MyGo v0.7.4：在 Pulse 中显示文件

适用于能重新编译调用方的开发者，不修改已安装的 Alma 或 Explorer。
在使用 v0.7.4 的 MyGo 源码根目录应用 pulse-reveal-v0.7.4.patch，重新编译调用方，
为该应用进程设置 MYGO_PULSE_EXECUTABLE 为支持 --reveal-item 的 pulse.exe 的绝对路径。
无需注册系统默认应用。不要把命令行参数写进变量，也不要指向旧版 Pulse。

未设置时沿用 Explorer；设置后，本地盘符路径通过独立参数交给 Pulse。
UNC、盘符根目录等继续使用 Explorer。目标已删除时不弹空窗口。
该接口用于“在文件夹中显示”，不是打开/保存对话框；调用方无需等待 Pulse 窗口关闭。
