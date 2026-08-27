# Signed release flow

Update checks are disabled unless both CMake cache values are supplied:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DPULSE_UPDATE_MANIFEST_URL=https://example.com/pulse/update-manifest.json `
  -DPULSE_UPDATE_PUBLIC_KEY_HEX=04...
```

Keep the ECDSA P-256 manifest private key offline and outside the repository.
The public key printed by `create_update_manifest.ps1` is the value compiled
into Pulse. A formal release also requires a Windows code-signing certificate
available to SignTool and an HTTPS timestamp service.

Run the complete release pipeline from an x64 developer prompt:

```powershell
pwsh scripts/package_release.ps1 `
  -CodeSigningThumbprint <certificate-sha1> `
  -TimestampUrl https://timestamp.example.com `
  -DownloadPage https://example.com/pulse/download `
  -ManifestPrivateKey D:\offline\pulse-update-private.pem
```

The pipeline builds and signs the four executables and installer, archives
PDBs outside the installer, writes `dist/update-manifest.json`, and creates
`dist/hashes.json`. Never add the manifest private key or PDBs to the installer.
