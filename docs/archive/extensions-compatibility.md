# 宿主契约识别与迁移检查

`apiVersion=1` 表示清单语法版本，不保证每个 v1 宿主支持相同的组件、命令、资源操作和预算。作者必须以目标宿主的只读发现和原生验证结果为准；不得自动删掉权限、确认步骤或未知字段来让包通过。

## AI、MCP 与 CLI 共用的指纹

`describe_extension_api`、助手开发工具和 `pulse.exe --extension-api` 返回同一个生成对象。新增可选 `contractIdentity`：`formatVersion=1`、`algorithm=sha256`、`canonicalization=sorted-ascii-json-v1`、`scope=discovery-excluding-contractIdentity`、`sha256`。SDK 的 `schemas/extensions/host-api.json` 保留该对象，发布清单 `discoverySha256` 记录同一摘要。旧宿主没有这个字段时仍可发现和验证，不因此自动拒绝旧包。

算法排除根对象 `contractIdentity`，对其余完整 JSON 对象递归按 key 排序，用 ASCII 转义、无多余空白的 JSON 编码计算 SHA-256。数组顺序保留，语义说明、限制、Schema、模板目录和资源结果都在摘要内。当前权威实现为 SDK 中 `tools/extensions/contract_compat.py`；生成的宿主描述内嵌同一结果。

该指纹是内容身份，不是发布者签名、运行时行为证明或授权凭证。相同指纹不替代宿主二进制摘要，也不证明原生实现没有回归；不同指纹要求审阅差异，不自动判定为破坏性升级。

## 比较目标宿主与已验收基线

基线使用固定 SDK 中的原始 `schemas/extensions/host-api.json`，不能传 `inspect-sdk --json` 的外层 `{ok,api}` 对象。

```powershell
python tools/extensions/pulse_ext.py --pulse C:/path/pulse.exe --json compare-sdk C:/previous-sdk/schemas/extensions/host-api.json
python tools/extensions/pulse_ext.py --pulse C:/path/pulse.exe --json compare-sdk C:/previous-sdk/schemas/extensions/host-api.json --require-identical
```

比较只读取指定基线和目标宿主发现，不加载包、执行命令或修改设置。报告包括两侧摘要、版本、增删能力和绑定、JSON Pointer 差异路径；不重复输出字段内容。基线存在指纹却与内容不匹配时拒绝。输入限制为4MiB、深度64、20万个节点；最多256条差异，超出显式返回 `changesTruncated=true`，不能把截断列表当完整迁移清单。

普通模式成功完成比较返回0，`status=review-required` 表示存在变化；`--require-identical` 在变化时返回2及 `contract_changed`，供固定构建/CI使用。始终返回 `runtimeCompatibilityCertified=false`、`nativeManifestValidationRequired=true`、`grantsAuthority=false`，不把诊断结果当执行许可。

## v1 兼容与变更规则

- 当前开发预览继续保留既有 v1 输入的含义和冻结兼容 fixture；新能力采用显式发现，不改变已有参数含义。没有自动移除日期，不承诺尚未发布的 v2 支持。
- 未来需要删除或改义时，先指定新的 API 大版本及迁移示例，再实现显式版本拒绝与旧版本回归；不能用同一 v1 值静默替换协议。安全修正若必须收紧旧输入，发布说明必须列出触发输入、拒绝原因和可行替代方案。
- 选择宿主时先审阅 `compare-sdk`，再对实际清单执行 `test`/原生验证。无可用等价能力时返回缺口，不暗中降级为 Shell、内部 action 或 MCP 借包身份调用。
- 此处只落地发现身份、差异检查与维护规则；没有自动迁移、协议多版本执行器或对所有版本的兼容承诺。

对应定向检查为 `test_contract_compat.py`、原生 `pulse_extension_developer_contract_test` 及解压 SDK 作者验收。实际运行结果在本轮平台记录中另列。
