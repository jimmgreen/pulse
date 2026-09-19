# 系统预览处理器：拖动窗口时的预览跟随

详情面板预览 `.xlsx` / `.docx` / `.pdf` 这类文件时，真正画图的是系统预览处理器：它被承载在 `pulse.exe` 的 STA 里，画面放进一个 TOPMOST 的 overlay 窗口（`PulsePreviewHandlerHost`）。拖动窗口时这个 overlay 曾经明显"在后追"——窗口已经移开，预览还停在原处，过一会儿才跳过去。

## 根因

`PlaceOverlay()` 原来每一步移动都调用 `SetWindowPos(..., HWND_TOPMOST, ...)`。overlay 里是处理器自己的跨进程子窗口（Office / PDF 的处理器多为 `LocalServer32`），重新声明 topmost band 会让窗口管理器与那个窗口的线程握手，公寓因此卡住；等它缓过来，overlay 直接跳到最新位置。

注意 `Sync` / `Reposition` 调用本身只要 0.0 ms（老的"调用是否阻塞"断言全是绿的），所以必须看**公寓有没有真的应用**这次移动，而不是看调用耗时。

## 修法

`src/ui/preview_handler_host.cpp` 的 `PlaceOverlay()`：**纯移动不再改 z 序**（`SWP_NOZORDER`），只有首次显示或尺寸变化时才重新置顶。

## 定向验证

- `pulse_preview_handler_probe.exe --selftest <真实 xlsx>` 的 `the overlay follows a moving owner`：把宿主窗口连续移动 16 次（每次 12 px，中间不等待），要求每一步都在 40 ms 内落到新位置。
  - 修复前：16/16 步全部超时——overlay 停在原地，公寓一次移动都没应用。
  - 修复后：worst ~1–2 ms、average < 1 ms、placements 16/16。
- 测试钩子（`PULSE_PREVIEW_HANDLER_TESTING`，只对 probe 目标定义）：
  `overlay_window_for_test()`（公寓的 overlay 句柄）、`ResetOverlayPlaceCountsForTest()` /
  `OverlayPlaceCallsForTest()` / `OverlayPlaceDoneForTest()`（`PlaceOverlay` 的进入与返回计数，
  用来区分"没被调用"和"调用了没返回"——后者才是卡在窗口管理器里）。
- 命令：`pulse_preview_handler_probe.exe --selftest "…\ceshi.xlsx"`。

验证边界：这条回归用例需要机器上有能承载该扩展名的系统预览处理器；没有处理器时用例打印
`[SKIP] overlay follow needs a file with a system preview handler` 并跳过。用例也不覆盖 Office 冷启动时
"provider 还没画完就拖动窗口"的那一段（那一档由慢打开预算的两个用例负责）。
