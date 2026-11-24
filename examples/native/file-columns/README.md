# file-columns：给“详细信息”视图加列和图标角标

演示实验接口 `pulse.columns`（`include/pulse/columns.h`），用 `pulse/app.hpp` 约 120 行：

| 内容 | 写法 |
|---|---|
| 「行数」列：文本文件的行数，右对齐，只问指定扩展名 | `AddColumn(ColumnSpec{id, title, extensions, width_dip, align, applies_to}, fn)` |
| 「编码」列：UTF-8 / UTF-8 BOM / UTF-16 / ASCII / binary，按结果着色 | 返回 `ColumnText{text, tone}`，`tone` 取 `PULSE_COLUMN_TONE_*` |
| 角标：24 小时内改过的文件，图标右上角显示 sparkle | `AddBadge(spec, fn)`，返回 `BadgeIcon{"sparkle", PULSE_COLUMN_TONE_ACCENT}` |
| 右键「重新统计行数」 | `RefreshColumns("lines", path)` 丢掉缓存、重新询问 |
| 切换语言后列名跟着变 | `OnLanguageChanged()` 里 `SetColumnTitle(id, Text(...))` |

## 在 Pulse 里的样子

- 只在本地文件夹出现（不含搜索结果、此电脑、网络路径和虚拟文件系统）。
- 列排在内置列之后，从名称列的空余宽度里分；放不下的从最后一列开始隐藏。用户可以拖动分隔线改宽度（双击恢复默认），也可以在列标题右键菜单里逐个隐藏或显示，菜单里会注明「扩展列 · 包 id」。
- 角标画在“详细信息”、列表和图标视图的图标右上角，菜单里同样可以关掉。每个文件只显示一个角标，几个包都给角标时先注册的优先。
- 列只用于显示，不参与排序。

## 回调怎么写

- 回调运行在宿主工作线程，每次只问屏幕上可见的行，按文件版本（路径、大小、修改时间）缓存。保持快速，不要调用 UI，也不要等待 UI 线程。
- 返回 `std::nullopt` 表示留空；文本最长 256 字节。
- `ColumnItem` 给出 `path`（UTF-8，只在回调期间有效）、`is_dir`、`size_bytes`、`modified_unix_ms`。
- 每个包最多 4 列、4 个角标。角标图标可以是内置名（`lock`、`star`、`warn`、`check`、`sparkle`…）、本包用 `Icons()` 注册的图标名，或 `<package>/<name>`。
- 老版本 Pulse 没有这个接口时 `Columns()` 返回空指针，插件照常加载。

## 构建与试运行

```powershell
python tools/extensions/pulse_ext.py --json new C:/PulseAuthor/cols --id com.example.cols --name "Cols" --template file-columns
python tools/extensions/pulse_ext.py --json build C:/PulseAuthor/cols --config Release
python tools/extensions/pulse_ext.py --json test C:/PulseAuthor/cols --smoke-commands --strict-resources --selection C:/PulseAuthor/cols/README.md
```

`test` 的报告里 `columnProviders` 列出每个列和角标，以及宿主拿 `--selection` 给的文件在工作线程上问到的值。

`settings.own` 是因为 `App` 启动时总会查询设置服务；`files.readDirectory` 说明本包会读取列表里文件的内容。
