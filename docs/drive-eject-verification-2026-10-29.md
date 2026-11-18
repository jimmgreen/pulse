# 镜像弹出验证（2026-10-29）

工作区：pulse58-integrated-fixes，v1.0.58。保留其他已有修改，未制作安装包或免安装包。

## 当前实现

- VHD/VHDX 通过存储依赖识别。后台缓存统一普通盘符、扩展 DOS 路径及大小写，设备变化时失效；执行时重新核验卷 GUID、镜像文件身份及物理设备。
- 普通右键、重试和动作入口统一先释放目标镜像全部分区上的目录监听、大小扫描、缩略图和预览；等待实际后台任务和子进程退出，超时拒绝弹出。成功或失败后恢复相关功能，新窗口继承暂停状态。
- 新索引程序支持临时卷暂停租约，断开连接恢复。旧服务通过能力协商快速返回不支持，继续交由系统安全移除检查，不停止用户服务。
- 不再以强制锁定所有卷作为前置条件。依据已核验的精确虚拟设备能力选择系统移除协商，不向父设备扩大范围；真正文件占用会拒绝移除。需要权限时使用已有授权助手，只在线程临时令牌中启用所需权限。
- 协商后使用预先绑定并核对物理映射的镜像句柄卸载。最终卸载失败则恢复精确设备节点；成功后检查镜像加载状态及物理路径消失，不能仅凭盘符消失报告成功。

## 已验证及修复过程

1. 实际界面发现旧索引服务导致 ISO 弹出超时 1460；能力协商修复后定向检查 15/15 通过。
2. 实际界面发现扩展 DOS 路径与菜单缓存键不一致，VHDX 缺少弹出入口；路径规范化修复及回归通过，重新构建后实际右键可见“弹出”。
3. 原先预锁卷方案在 Everything 持有原始卷句柄时失败；原生 Explorer 对同一隔离 VHDX 成功弹出，证明不能用预锁卷结果直接判定无法安全移除。只读句柄快照受权限限制，不能判定唯一占用者。
4. 单用 CM_Request_Device_Eject 得到 IllegalDeviceRequest；据设备能力改用匹配的协商接口。测试中直接 Shell eject 会绕过已打开文件，未作为生产回退方案。
5. QueryAndRemove 候选：真实双分区 VHDX、Everything 保持运行；兄弟分区文件打开时拒绝且两卷保留，关闭后普通令牌通过授权助手卸载，物理路径消失且 Attached=False。故障注入验证协商后卸载失败可恢复两个原卷。日志：build/drive_eject_query_medium_test.log、build/drive_eject_query_recovery_test.log。
6. ISO 复验曾发现 ERROR_INVALID_PARAMETER（87），只读 API 对照定位为打开参数不兼容。改为真实光驱设备类型识别后的 V1 参数（不靠扩展名），保留身份和物理映射核验。最终新 ISO 实测：占用 2404、释放后 0、TestExit=0、Attached=False，Everything 保持运行。日志：build/drive_eject_iso_api_probe.log、build/drive_eject_query_iso_test.log。

## 直接依赖验证

- 授权协议 19 项、授权会话 --drive-eject 3 项通过，含预取消、无效盘符及错误卷身份。
- 目录监听和大小扫描释放 15 项通过；缩略图 --volume 多卷独立暂停/恢复通过。
- 新索引 host 在真实隔离 ISO 上 6 项通过：原目录句柄存在、暂停确认后释放、断连恢复、正常退出与夹具清理。真实 USN 卷根释放尚未实测。
- FFmpeg --cancel 定向检查 3/0。早期完整媒体测试意外运行后 56/6；不改测试的当前版本与仅撤回新增 Wait 的隔离基线均 63/0，原六失败未复现，根因未确定。日志：drive-eject-ffmpeg-test.log、drive-eject-ffmpeg-current-repeat.log、drive-eject-ffmpeg-baseline.log。

## 验证限制及清理

- 最终主界面点击验证未完成：界面控制工具 list_windows 连续超时，重置后最终重试仍失败。先前验证过菜单出现及失败后的浏览/预览恢复，不能代替最终成功路径。
- 本机普通进程为 Medium 令牌，但管理员同意策略自动通过；未修改安全设置，无法实测原生授权框点“否”。内部预取消通过不等同于该界面测试。
- 没有反馈用户原始镜像或现场，不能认定每次占用都由 Pulse 造成。实体光驱和永久挂起的第三方预览提供程序未实测。
- 曾因测试根指向系统卷被自动审批拒绝，停止该路径，改用身份核验后的新建镜像。未修改用户文件或停止用户服务。
- 本轮主界面夹具 pulse-eject-isolated-16340-119247031.vhdx 已通过夹具退出清理，进程已退出且镜像文件不存在。其他最终夹具由测试脚本负责清理并另行确认。

接口依据：[CM_Query_And_Remove_SubTreeW](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_query_and_remove_subtreew)、[CM_Request_Device_EjectW](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_request_device_ejectw)、[DetachVirtualDisk](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-detachvirtualdisk)。

## 最终冻结版验证结果（18:35 后）

- 最终源码 virtual_disk_eject.cpp 18:32:48；授权助手 18:33:36；主程序 18:35:25。
- 主程序目标构建 Exit0，无新增警告。日志：build/drive-eject-verified-main-build.log。
- 最终双分区 VHDX 普通权限授权流程 4 PASS：占用拒绝、错误文件身份拒绝、兄弟分区打开文件拒绝、关闭后卸载且物理路径不存在；另外系统 Attached=False。菜单缓存普通/扩展/小写路径及失效检查 Exit0。日志：build/drive_eject_final_medium_test.log。
- 最终故障注入测试确认两个原卷恢复可读，通过。日志：build/drive_eject_final_recovery_test.log。注入仅在独立测试库启用，生产库无该开关。
- 最终授权协议 19 PASS、授权会话 --drive-eject 3 PASS；未知或缺失测试参数 Exit64，不创建夹具。相关 diff-check 通过。
- 最终 ISO 占用拒绝及释放成功通过，Attached=False。Everything 全程保持运行。
- 所有本轮临时 VHDX 和 ISO 已清理；未停止或修改用户索引服务。
- 仍保留上述界面工具超时、真实授权框取消及实体光驱验证限制；最终界面点击未标为通过。
