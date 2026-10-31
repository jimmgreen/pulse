# 闲置与隐藏窗口绘制阶段（2026-10-30）

在用户指定的 `pulse58-integrated-fixes/pulse` 的1.0.58集成版本继续优化，保留上一批可见区文字绘制、logo和其他未提交改动。本批解决的是隐藏/最小化窗口的无效绘制和帧线程唤醒，**没有证明常驻内存大幅下降，也不是启动提速阶段**。

## 定位和实现

现有UiTimerPacer已经会降到可见静置500ms、隐藏1000ms，直接继续拉长间隔可能延迟后台结果。新增只在测试构建存在的累计计时后，初始基线中可见静置5～30秒主UI tick合计约43ms，标签发现约9ms、操作窗口处理约10ms；不值得为这些小成本大改状态缓存。

实际复现两个问题：

1. WM_PAINT无可见性判断，最小化后的失效请求仍执行Render/Present。安静基线30秒也出现2～3次隐藏绘制。
2. 滚动/动画未结束时，UI定时器仍Arm帧线程；帧消息发现窗口最小化又Disarm。带未完成滚动的定向对照复现30秒额外唤醒30次。

产品改动限定在 `app_main.cpp`：普通隐藏/最小化窗口验证绘制区域后跳过Render；动画和滚动启动帧入口检查可见性；隐藏/最小化立即停帧并切到已有低频定时器。显示时主动Invalidate，恢复绘制最新模型；显式shot/menushot离屏捕获继续允许绘制。保留布局、缩略图和绘图资源，避免为了内存截图让恢复变慢。文件操作、导航结果、索引通知等定时处理仍继续，没有整体停止消息循环。

没有修改帧时钟算法、文件操作逻辑，也没有强制清工作集。

## 实测结果和边界

真实主窗口、独立配置、两万文件fixture，完整20000→0→20000目录往返，随后可见静置30秒、最小化30秒、恢复。通过同一个测试exe的 `PULSE_TEST_HIDDEN_RENDER_BASELINE` 开关交替执行旧/新路径；测量期间不构建。搜索服务关闭，因此不是完整服务配置的总CPU/内存。

|30秒最小化场景|旧路径|新路径|
|---|---:|---:|
|安静时实际绘制帧|2|0|
|持续重绘请求时实际绘制帧|125|0|
|未完成滚动时额外帧消息|30|0|
|安静时后台UI tick次数|30|30|

持续重绘场景由探针每250ms显式请求重绘，属于有界压力测试，**不是声称正常闲置每30秒都绘制125帧**。该组主进程累计CPU时间140.625→78.125ms，绝对差62.5ms；安静组31.25→46.875ms，未见稳定CPU下降，不把相对百分比包装成大收益。

恢复到两万条列表：安静组34.565→41.789ms，重绘压力组68.489→34.197ms，未完成滚动组71.849→57.274ms。每种只有一对，说明功能恢复与耗时量级，不能承诺稳定恢复提速；原始不利样本保留。

私有驻留样本只显示小幅波动，不声称确定节省。现有外部QueryWorkingSet按50ms轮询，最小化末尾和恢复事件接近时可能在同一次轮询取样，不能据此解释瞬时内存释放。没有把总工作集当成私有堆，也没有将本阶段局部数据与File Pilot比较。

原始资料：`bench_data/idle_delivery_20261030/`。
- `baseline_1/`：第一轮定位，含ticks、presents、tick_ms、tags_ms、ops_ms累计值；恢复后探针早触发造成多等5秒，后续已按剩余时间安排，原始数据保留。
- `quiet_before/after`、`repaint_before/after`及`comparison.json`：安静/压力对照。
- `motion_before/after`、`navigation_after`及`regression.json`：帧唤醒和背景导航检查。

## 验证

- `pulse_frame_pump_test`：59项通过、0失败，日志 `frame_pump_test.log`，覆盖静置/隐藏降频、重新启动帧、停止后不再发消息和动画时钟。
- 4个性能对照进程和3个补充回归进程均正常退出。新路径最小化期间presents增量均0。
- 最小化期间正常NavigateTo空目录，后台模型完成更新，恢复时显示0条的最新目录；没有停掉后台结果处理。
- 旧/新未完成滚动测试恢复后均显示20000条，新路径隐藏帧消息增量0。
- 最终普通隐藏（SW_HIDE）期间导航、SW_SHOW后显示最新目录通过：隐藏绘制和帧消息均0，显示后0条最新目录，恢复样本31.080ms，进程退出0。记录`hide_navigation_final/verified.json`；这是相同窗口可见性分支测试，未实际点击托盘按钮。
- 本轮没有改变布局/颜色，也没有做人工鼠标、实际托盘按钮点击或截图逐像素验收；没有运行无关全量selftest。

测试入口扩展在 `src/app/startup_perf_probe.h` 和 `scripts/measure_startup_idle.py`。例如：

```powershell
python scripts/measure_startup_idle.py --exe build-perf-isolated/pulse.exe --output bench_data/idle_delivery_20261030/recheck --initial bench_data/listing_filter_perf_20261030 --target bench_data/startup_idle_20261030/empty --minimize --hidden-scroll
```

输出目录必须不存在。`--hidden-baseline`启用旧路径；`--hidden-repaint`加入压力请求；`--hidden-navigate`在隐藏期间导航到target；`--hide`改测SW_HIDE/SW_SHOW。不得将人为压力样本与普通静置混为一谈。

## 后续入口

长时间主窗口私有内存、完整服务配置仍缺少稳定的下降证据，不应继续机械增加缓存淘汰或延长轮询。下一批应针对实际驻留的大对象做归因，并同时测恢复/返回目录；启动图形首次提交、复杂彩色文字准备成本仍是已有待办。此处隐藏绘制与重复帧唤醒已修复，不再重复实施。

## 已交付产物

正式pulse目标构建成功（`build/hidden_frames_release_build.log`），发布载荷确认不含selftest dispatcher。独立包`dist/PulseSetup-1.0.58-idle-frames.exe`，10,150,432字节（9.68MiB），完整XZ解压及33个文件与build逐字节/SHA256核对通过。只生成和核对，未安装、未重启真实索引服务。

- 安装包SHA256：`d0b19bc05ff3abb825c8d83ef60767c77a4e3ff300972ee52a0f66ba2be5e474`。
- 正式主程序SHA256：`639f2d9fc311a93c19105decc4034f6fb81a0d50a5e53d33de8da300520d29e0`。
- 核对记录：`build/hidden_frames_package_verified.json`。

保留其他未提交改动；本阶段验证不等于其他模块的完整验收。
