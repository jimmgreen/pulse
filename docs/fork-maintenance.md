# ClauxQz4/pulse 维护与发布

此 fork 的主程序更新源、Windows / Windows 8.1 构建配置、内容搜索与 AI 增强包、发布脚本均指向 `ClauxQz4/pulse`。

已有 FFmpeg、图像、RAW 预览包仍使用上游已发布且固定 SHA-256 的独立版本。发布检查可从上游读取这些包并核对内容，但只向本 fork 上传；以后在本 fork 构建的预览包也可与这些旧包共存。

## 更新签名

- 算法：ECDSA P-256 + SHA-256，沿用 Pulse 更新清单格式。
- 仓库只保存公钥：`cmake/update-public-key.txt`。
- 本机私钥保存在仓库外：`%USERPROFILE%\.pulse-maintainer\keys\pulse-update-p256.pem`。请另做安全备份，不要提交、公开分享或放进安装包。
- GitHub 仓库 Settings → Secrets and variables → Actions 中的 `PULSE_UPDATE_PRIVATE_KEY` 必须保存该 PEM 私钥完整内容。首次发布前确认 Secret 已同步；设置页面看不到已保存的值。
- 构建中嵌入的公钥必须与 Secret 对应。`create_update_manifest.ps1` 会检查，不匹配时停止发布。
- 这是更新清单签名，不是 Windows Authenticode 厂商证书。

常规更新应继续使用同一密钥。换密钥需要同时更新公钥和已安装客户端的信任，不能只替换 GitHub Secret。

## 本地打包

在安装了 MSVC v143、Windows SDK、CMake 3.25+、Ninja、Python 和 PowerShell 7.2+ 的环境中：

```powershell
.\build_installer.bat
$version = (Get-Content version.txt -Raw).Trim()
.\scripts\create_update_manifest.ps1 `
  -Installer "dist/PulseSetup-$version.exe" `
  -DownloadPage "https://github.com/ClauxQz4/pulse/releases/download/v$version/PulseSetup-$version.exe" `
  -PrivateKey "$env:USERPROFILE\.pulse-maintainer\keys\pulse-update-p256.pem" `
  -ExpectedPublicKey (Get-Content cmake/update-public-key.txt -Raw).Trim() `
  -MinimumWindowsBuild 10240
```

构建目录若曾用于旧公钥或上游更新源，应重新配置，显式设置 `PULSE_UPDATE_PUBLIC_KEY_HEX` 和 `PULSE_UPDATE_MANIFEST_URL`，或使用新构建目录；CMake 缓存不会因公钥文件变化而自动覆盖已有值。

## GitHub Actions 发布

1. 首次启用 fork 的 Actions，并设置上述签名 Secret。
2. 修改 `version.txt` 为未发布的递增三段版本号，提交所有改动。
3. 将代码和对应版本标签推到 `origin`：

```powershell
git push origin main
$version = (Get-Content version.txt -Raw).Trim()
git tag "v$version"
git push origin "v$version"
```

`Build and publish Pulse` 工作流会构建两个 Windows 通道、运行检查、签名更新清单并发布。不要把预览包的独立版本标为 Latest。客户端使用 Latest 主程序版本的更新清单。

发布失败时查看 Actions 日志，不要跳过签名、公钥匹配、安装包哈希和增强包内容校验。对于已公开的版本，用新版本号发布修正，不覆盖旧版本。
