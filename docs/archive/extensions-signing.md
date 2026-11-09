# 声明式包开发签名

签名使用 Windows CNG 的 RSA PKCS#1 v1.5 / SHA-256（RSA 2048–4096 位），覆盖固定域与格式前缀 `Pulse.Extension.Manifest.Signature\0v1\0` 加上原始 manifest 字节。JSON 空白变化也会使签名失效；没有重新序列化或自创密码算法。参考微软 [BCryptImportKeyPair](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptimportkeypair) 和 [BCryptVerifySignature](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptverifysignature)。

这是本地开发者身份固定与完整性验证，不是证书链、市场审核、撤销服务、时间戳或生产分发系统。签名不授予任何 capability/resource 权限；升级增加权限仍需独立显式接受。私钥由作者明确提供，工具不生成或信任用户长期密钥。测试仅在临时目录或内存生成临时密钥。

## 格式与策略

- 私钥输入是 Windows CNG `RSAFULLPRIVATEBLOB`，公钥是 `RSAPUBLICBLOB`；不接受任意 PEM 的隐式转换。
- 发布者 ID 是公钥 blob 的 SHA-256 小写十六进制 pin。
- detached 签名 JSON 仅含 formatVersion=1、algorithm=`RSA-PKCS1-SHA256`、publisher、signature（小写 hex）。
- 存储根的 signature-policy.json 仅含 formatVersion=1、requireSigned 布尔、publishers 数组（最多32项 `{id,publicKey}`，publicKey 为公钥 blob hex）。
- 没有策略时，旧 unsigned 开发包兼容；任何提供的签名都必须来自显式信任公钥且验证成功。requireSigned 拒绝 unsigned。
- 安装后的签名保存在 versions/<manifest-sha256>.sig.json。启动加载、启用及回退检查签名；空文件、坏签名、未知发布者失败关闭。

策略和存储使用当前用户文件权限；本实现不声称能抵御同一用户任意改写策略、删除策略或替换程序。生产环境的策略分发、ACL、密钥保护/轮换、撤销、证书链及市场审查仍须另行建设。

## 作者命令

先通过独立可信渠道核对公钥 SHA-256 pin，不从待安装包自动信任公钥。以下路径均为作者明确提供的文件：

```powershell
python tools/extensions/package_signing.py sign manifest.json --private-key author-private.cng --output manifest.sig.json
python tools/extensions/package_signing.py trust --store C:\Test\PulseStore --public-key author-public.cng --fingerprint <已核对的SHA256> --require-signed
python tools/extensions/package_signing.py verify manifest.json --signature manifest.sig.json --policy C:\Test\PulseStore\signature-policy.json
python tools/extensions/pulse_ext.py pack manifest.json --output package.zip
python tools/extensions/pulse_ext.py verify package.zip --signature manifest.sig.json --policy C:\Test\PulseStore\signature-policy.json
python tools/extensions/pulse_ext.py install package.zip --store C:\Test\PulseStore --signature manifest.sig.json
```

签名是 ZIP 外独立文件，现有 ZIP 两文件格式保持兼容。pack 保留原始 manifest 字节。签名验证身份不自动启用包、不运行包，也不跳过原生 manifest 校验。新增权限的升级仍会要求 `--accept-capabilities`；签名不替代这项决定。

## 本地公钥轮换与撤销

先独立核对新公钥指纹，用 `trust` 加入新公钥，再验签新版本。旧公钥在过渡期仍可信；确认新版本可用后，显式撤销旧指纹：

```powershell
python tools/extensions/package_signing.py trust --store C:\Test\PulseStore --public-key new-author-public.cng --fingerprint <新公钥的64位小写十六进制SHA256>
python tools/extensions/package_signing.py untrust --store C:\Test\PulseStore --fingerprint <旧公钥的64位小写十六进制SHA256>
```

两条命令均持有跨进程存储锁，并用同目录临时文件原子替换策略。`trust` 不带 `--require-signed` 时也保留已有 `requireSigned=true`；该开关只能强化策略。`untrust` 严格校验现有策略和指纹，只删除指定公钥，保留其他公钥与签名要求；未知指纹、非法指纹、策略损坏或存储忙时报错，不改写策略。撤销最后一个公钥也不放宽签名要求，因此 requireSigned 存储会拒绝所有包，直到显式加入可信公钥。

本地 pin 撤销与在线撤销服务不同。运行中 store 信任观察器的实现与验收状态见下节；策略写入完成与 UI 已消费撤销结果仍是两个时刻。旧公钥签署的版本在之后验签、加载或回退时会因未知发布者被拒绝；签名身份变化也不跳过能力/资源增权确认。恢复 pin 不自动恢复本会话授权，须重新启动验证。

## 2026-11-04 实现与验证状态

已实现 package_signature.cpp 的 CNG 验签、原生 package_store 启动加载门禁、Python sign/trust/verify、CLI归档验证/安装的 detached签名参数，以及启用/回退检查。能力和资源升级差异继续独立确认。

本次可核实的定向结果：真实CNG签名与CLI端到端5项、Python存储15项、作者CLI64项、打包10项通过。CLI端到端使用临时密钥，覆盖签名、错误pin拒绝且不写信任策略、正确pin显式信任及验签。原生签名与包存储定向测试已通过，日志 platform-signature-01.log、platform-package_store-03.log。真实宿主签名启动验收12项通过，见 bench_data/platform-signature-host-20261104-01/checks.json：有效签名、显式增权升级、回退、坏签名、撤销发布者、缺失必需签名及恢复信任；签名由Python CNG生成，原生加载器验证同一原始字节。测试私钥仅在进程内存，不留下长期密钥。没有为本轮P0–P5签名功能发布新归档，既有UI固定ZIP不回写。

资源服务与工作流的原生定向测试已通过，真实宿主检查执行中；它们不是签名安全的替代证明。P3边界测试仍处理中，P4只具隔离PoC，不能因包已签名就允许第三方脚本。


## 仍需完成的开发与验收

1. 已完成当前原生验签和真实启动矩阵；后续签名格式或存储修改须重跑对应检查。原生链接目录测试存在环境SKIP，不能算通过。运行中策略观察器及本节定向原生/双窗口验收已完成，不以启动矩阵代替。
2. 补来源与权限差异的可审阅展示、策略配置错误诊断和恢复说明；验证签名sidecar/manifest/registry切换在中断时不会激活未验证版本。
3. 完成发布者连续性、密钥轮换/撤销和策略保护方案，再确定生产信任来源；同用户可改策略的开发存储不是受保护的企业信任根。
4. 完成受控外部作者试用、签名升级/回退工作流和最终SDK独立解压验收。生产证书链、时间戳、在线撤销、自动更新和市场仍未实现，不作为当前开发签名的隐含能力。

以上P5退出项尚未全部满足；不要将“存在签名实现”或某个测试集合通过表述为P5/整个平台已经交付。

## 安装前审阅及中断恢复

`pulse_ext verify` 返回 authorizationReview，列出包名、全部命令能力、资源权限、工作流数量及文件操作仍需原生确认的提示；signature 单独返回 signed/trusted/publisher，不把 ZIP 完整性当作来源信任。grantsAuthority 永远为false：审阅/验签本身不启用、不授权、不执行。workflow.run 的提示是保守声明；实际是否出现文件确认取决于其步骤。

签名存储定向故障测试已覆盖签名落盘前中断、registry切换前中断：原已验证activeRevision保持不变，重试后可切换并回退。日志 platform-signing-python-02.log（6项）。这不是真实断电或文件系统硬件持久性测试。残留不可变版本不会自动激活；修复原因后重试同一包，不能手工把registry指向未经验证内容。

新增本地信任管理定向检查：`platform-signing-python-03.log` 中 7 项全部通过，保留上述中断恢复用例。新增真实临时 CNG 双密钥轮换、撤销旧 pin 后旧签拒绝/新签通过、requireSigned 不降级，以及非法/未知 pin、损坏策略、跨进程存储锁竞争时策略字节不变。该结果验证本地 CLI 和验签边界，不替代生产密钥轮换、在线撤销或运行中会话撤权机制。

## 运行中本地信任撤销（开发预览）

仅对 `--extension-store` 已启动包启用。启动验证保留原始 manifest 和 detached signature 快照；单个后台观察器约每秒读取一次当前 `signature-policy.json`（最多 64 KiB），复用同一原生 CNG 验签函数逐包判断。不读取变化后的 registry、不重新加载新版本、不授予新增权限，普通 CLI enable/disable 仍在下次启动生效。

观察器固定祖先目录句柄并拒绝重解析点、策略硬链接；短暂读取冲突最多重试三次、间隔 100 ms，可被关闭唤醒。读取持续失败时无法建立信任，保守撤销；格式损坏遵循真实验签器拒绝。策略缺失时，已签名包失去 publisher pin，而 unsigned 开发包按原有无策略规则仍可使用；不会因单个签名失败而批量停用其他仍可信包。

UI 定时器只消费内存结果，包身份属于本次启动快照，结果带观察器代际。失信包标为本会话不可恢复并通过现有授权注册表停用：旧 caller binding/ticket 失效，依赖这些校验的确认、工作流、受控操作与资源请求拒绝继续授权。已经提交的文件变更不回滚，正在执行的底层操作按现有取消检查边界停止，不承诺在策略写入瞬间原子停止全部副作用。恢复 pin 或调用普通 enable 不会复活该会话包，须新启动再次验证。

store 模式每个活动窗口有消费定时器，共享唯一后台观察器。首窗口关闭不停止其他窗口的信任检查；最后窗口关闭后取消等待、请求取消同步 I/O，最多等待 200 ms。正常完成则 join；异常阻塞时线程仅保留自己的 shared 快照/句柄，不持有 runtime、owner 或 UI 指针，延后回收且不再发布结果。旧线程实际退出前占用唯一观察器槽位，新监测启动失败会保守撤权，不能假报监测正常或累积线程。UI 不执行文件或签名 I/O，旧代际结果不会应用到新启动。普通开发热重载仍使用原有窗口路径。固定码调试输出不包含 policy、密钥或 manifest 内容。本地驱动阻塞可能延后私有线程回收，但不会等待该 I/O 完成才允许 UI 关闭。策略文件允许共享删除；实际 Windows 验证仍发现 legacy replace 在目标读句柄存续时返回 error 5，目录 pin 对照不触发该错误。因此作者 CLI 的原子提交仅对 Win32 5/32/33 作最多 50 次尝试、间隔 10 ms（等待总计至多 490 ms），复用同一已刷新临时文件，不删除旧策略，不放宽读取边界。其他错误或预算耗尽立即失败并清理临时文件；实际系统 I/O 时长不包含在等待预算内。祖先目录仍固定并拒绝重解析。

新增独立 `extension_store_trust_test.cpp` 使用临时 CNG 密钥与隔离 store，覆盖运行中撤 pin、有效旧授权拒绝、其他包保留、registry 变化不加载、恢复 pin 不复活、显式新启动、缺失/损坏策略，以及并发原子替换、注入阻塞读取后的 200 ms 关闭边界、旧 worker 槽位不累积和迟到结果不入 runtime。workspace-shutdown-native-01.log实际22项通过，含正常关闭排空审计及关闭后不能恢复授权。

真实双窗口验收协调脚本为：

```powershell
python tools/extensions/live_trust_revocation_check.py --pulse build-extensions/pulse.exe --output bench_data/store-trust-live-01
```

脚本只使用新建隔离目录和内存临时私钥；由宿主 probe 握手触发真实 `untrust`/`trust` CLI，再校验关闭首窗口后仍撤权、未撤包命令实际执行、恢复策略不复活及下一进程重新验签。第08轮真实双窗口及重启21项检查全部通过，原生22项通过；Python语法检查不代替这些验收。`untrust` 现有 `reloadRequired` 字段继续表示重新获得授权需重新加载，不是在线 PKI 或即时撤销完成凭据。

新增有界写入的两项定向 Python 测试已实际通过：真实 Windows 目标读句柄短暂占用后成功、持续占用耗尽仍保留旧策略及清理临时文件；精确 50/49 次尝试/等待预算及其他错误立即失败。原生并发回归实际通过，明确验证同一有界 writer 策略，不将 OS 单次 replace 误称为永不冲突。完整Python签名模块9项通过，见workspace-signing-python-04.log。

真实双窗口store-trust-host-20261104-05/06/07各21项脚本检查通过，包括首窗关闭后剩余窗口实际撤权、未受影响包执行、恢复pin不复活和下一进程重验。第05轮五张应用内截图已逐张检查。此前02–04重启退出访问冲突不能被重跑通过抵消：低扰动重复启动在第8次复现，堆栈已符号化为SeedShellVerbCache→EnumerateStaticVerbs→l10n::Get，宿主缓存后台线程跨退出的修复与最终验证另行记录。

退出缺陷最终修复：PrefetchStaticVerbs、SeedShellVerbCache与SeedCtxLayout的后台任务改为受管线程；停止监测时取消后续阶段并等待当前任务结束，最后窗口与主入口提前返回均显式收尾。不能中断的Shell/图标调用仍受既有最后窗口退出看门狗限制，不能把本地测试称为慢网络图标的正常关闭保证。pulse_shell_cache_readers_test实际4项通过；修复后store-trust-host-20261104-08全部21项通过，store-trust-restart-stress-20261104-02连续20次真实重启各6项通过（含真实窗口销毁完成）、退出0、store摘要不变。原崩溃报告、低扰动堆栈及符号化结果保留。
