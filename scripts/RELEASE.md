# 发布流程

正式发布使用 GitHub Actions，同时生成 Windows 10 / 11 和 Windows 8.1 两种安装包。版本、标签、签名密钥和更新清单配置见 [自动更新与发布](../docs/automatic-updates.md)。

本地验证使用 `build_release_ci.ps1`。`package_release.ps1` 为可选的 Authenticode 签名流程，需要自行配置代码签名证书；更新清单的 ECDSA 签名与 Windows 代码签名用途不同。
