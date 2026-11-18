> 历史记录：JavaScript 实验实现已退役；以下命令和接口不适用于当前宿主。当前作者入口见 [C++ 快速开始](../extensions-sdk-quickstart.md)。

# 用真实 JavaScript 生成重命名计划

`script-rename-panel` 是显式开发示例，不代表生产第三方脚本隔离已验收。正常宿主默认不开启脚本。
示例源代码位于清单 `runtime.source`，原生面板仅提供输入和四个操作按钮；改算法不需要重新编译 Pulse。

先使用匹配版本的 Pulse、Pulse.ExtensionHost.exe 和 SDK，在测试目录中准备可丢弃文件。
SDK 的 `bin/Pulse.ExtensionHost.exe` 及 QuickJS 许可/锁定版本随包提供；实际宿主使用与其构建匹配的辅助程序。
缺少辅助程序或隔离不可用时必须失败，不回退普通进程。

```powershell
python tools/extensions/pulse_ext.py --pulse C:/Pulse/pulse.exe init C:/work/rename-demo --template script-rename-panel --id local.rename-demo --name "Rename demo"
python tools/extensions/pulse_ext.py --pulse C:/Pulse/pulse.exe dev C:/work/rename-demo/manifest.json --profile C:/work/rename-profile-01 --allow-scripts
```

`--profile` 必须是新的隔离目录；已安装包的 `run --allow-scripts` 也必须提供新 profile。
开关只传递宿主 `--extension-script-dev`；未指定时不启用源码执行。
CLI 在启动前检查 `isolatedJavaScriptRuntime`，但发现和清单验证不等同脚本成功运行。

示例读取当前目录第一批最多 64 个条目，跳过目录和重解析点，不递归、不继续下一页。
作者 JS 自行循环、分支、拆分扩展名，将文件名主干转小写，生成 `prefix + 两位编号 + "_" + 主干 + 原扩展名`。
编号取宿主当前列表顺序。输入 `demo-` 可得到 `demo-01_image alpha.jpg`；实际编号依当前排序而定。

1. **Preview** 调用 `files.list`，计算新名，再调用 `files.prepareRename`，保存短期 planId。屏幕预览截断到 240 个 Unicode 码点，不代表完整审核清单。
2. **Commit** 提交 planId，必须经过宿主原生确认；返回 taskId 时只显示 submitted。
3. **Status** 查询所属任务，区分完成、运行中和终止但未全部完成。
4. **Cancel** 请求取消所属任务；再查询真实结果，不声称回滚已完成重命名。

每次资源调用都先检查 `response.ok`。源码使用同步 `pulse.call(request)`，不返回 Promise；单次执行最多 32 次 broker 调用。
权限仅为 `files.readCurrentDirectory`、`files.renameCurrentDirectory`、`operations.own`。
UI 输入采用命令的 inputState 白名单；编辑、关闭、重开、重载或撤权后，旧脚本结果不覆盖新状态。输入法组合期间也不提交结果。

可将源码临时提取为 UTF-8 文件编辑，再写回清单；签名和包哈希最终覆盖写回后的 JSON。
以下保存为 `edit_source.py`，在示例目录运行（不会让宿主加载外部脚本文件）：

```python
import json
from pathlib import Path

manifest = Path("manifest.json")
data = json.loads(manifest.read_text(encoding="utf-8"))
source = Path("rename.js")
if not source.exists():
    source.write_text(data["runtime"]["source"], encoding="utf-8")
else:
    data["runtime"]["source"] = source.read_text(encoding="utf-8")
    manifest.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
```

第一次运行提取源码；编辑 `stem.toLowerCase()` 或分隔符后再运行写回。然后重新 validate/test/pack；已有签名必须重新生成。
本轮验收记录由实际宿主探针另行提供，不能把模板生成、类型检查或截图存在当成真实文件操作成功。

入口参数就是 `inputState` 白名单的字符串字典，例 `function preview(input) { var prefix = input.prefix; }`；不带 `commandId` 或嵌套 `state` 包装。

开发脚本诊断：`dev` 退出 JSON 的 `scriptDiagnostic` 指向隔离 profile 内的 `extension-script-status.json`。这是异步写入的单个最新完成结果，不是历史日志；没有运行脚本或写入被安全检查拒绝时文件可能不存在。字段仅为 `schemaVersion`、`packageId`、`commandId`、字符串 `runId`、`ok`，失败另有固定 `error.code`。不保存脚本源、输入值、文件路径或异常堆栈；未知错误归为 `script_failed`。关闭时短暂等待最后一次写入，不能把诊断文件的存在当作文件操作成功，实际任务仍用 `operations.get` 查询。
