# 发布和自动更新

源码与发布页：<https://github.com/jimmgreen/pulse>。

从 1.0.3 开始，客户端启动约 15 秒后检查更新，持续运行时每 6 小时再检查一次。检查在后台进行；有新版本时显示可点击的提示，同一运行会话不会重复提示同一个版本。用户点击后才下载安装包；下载期间可在设置中取消。安装包校验通过后启动安装向导，保留 Windows 管理员确认。正在执行文件操作或迁移索引时不启动安装。

1.0.2 及更早的安装包未配置更新源，需要手动安装一次 1.0.3 或更新版本。

## 发布新版本

1. 修改 `version.txt`，例如改为 `1.0.4`。
2. 在 `docs/releases/1.0.4.md` 写本次更新内容。
3. 提交、推送代码，再推送同名版本标签：

```powershell
git add version.txt docs/releases/1.0.4.md
git commit -m "release: prepare 1.0.4"
git push origin main
git tag v1.0.4
git push origin v1.0.4
```

GitHub Actions 的 **Build and publish Pulse** 工作流会构建、测试并打包两种版本。两者全部通过后，才发布 Release 并设为最新版本。单纯提交源码或上传 Actions 构建产物不会触发客户端更新。

| 客户端 | 安装包 | 更新清单 |
| --- | --- | --- |
| Windows 10 / Windows 11 x64 | `PulseSetup-版本.exe` | `update-manifest.json` |
| Windows 8.1 x64 | `PulseSetup-版本-win81.exe` | `update-manifest-win81.json` |

Release 正文自动提供上述系统说明与两个下载入口。不要重复覆盖已经公开发布的版本；修复发布问题时递增版本号。

## 更新校验

更新清单和安装包默认依次尝试 `https://ghproxy.net/`、`https://gh-proxy.com/` 加原始 Release 文件 URL，最后回退到 GitHub。无需开关或配置。网络错误或非 200 响应时切换到下一来源，每个来源最多请求一次；取消、写入失败和校验失败不会触发来源切换。切换前清空已下载内容，避免拼接不同来源的响应。

只对不含凭据或查询参数的 GitHub Release 文件地址启用公共加速，其他自定义更新域名仍直接访问。ghproxy.net 和 gh-proxy.com 是第三方公共服务，可用性由其运营方决定；未来官方对象存储/CDN 可通过现有清单地址配置接入。

清单采用 ECDSA P-256 签名，客户端内置的公钥位于 `cmake/update-public-key.txt`。签名覆盖版本号、最低系统版本、下载地址和安装包 SHA-256。客户端拒绝未通过签名、校验和不匹配、旧版本或不适用的系统版本；下载只允许 HTTPS，支持 GitHub 的 HTTPS 重定向。

签名私钥保存在仓库的 Actions Secret `PULSE_UPDATE_PRIVATE_KEY` 中，备份应保存在仓库之外，不进入 Git。后续发布沿用此密钥和公钥，避免已安装客户端无法验证新版。

CI 使用 v143 和静态 VC 运行库，LumaText 使用校验过的固定 SDK，定义见 `cmake/lumatext-sdk.json`。这样构建不依赖本机路径或私有 LumaText 仓库。SDK 仅供构建使用；普通用户下载 Release 顶部对应系统的安装包。
