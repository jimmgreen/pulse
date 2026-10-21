# FFmpeg preview pack

The [pack workflow](../../.github/workflows/preview-pack-ffmpeg.yml) builds the same decode-only LGPL configuration for two native Windows targets:

| Pack architecture | GitHub runner | MSYS2 environment | Compiler |
| --- | --- | --- | --- |
| `x64` | `windows-2022` | `MINGW64` | GCC, `x86_64` |
| `arm64` | `windows-11-arm` | `CLANGARM64` | Clang/LLVM, `aarch64` |

`windows-11-arm` is a GitHub-hosted ARM64 runner available to public repositories. MSYS2's Unix tools (including Bash and Make) still use Windows x64 emulation on ARM; the CLANGARM64 compiler, dependencies, produced executables, and executable smoke tests are ARM64. This is not an x64 runner pretending to test an ARM target. CI checks the runner/OS architecture, compiler target, and both executable PE machine fields (`0x8664` / `0xaa64`).

Official support references:

- [GitHub ARM64 hosted runners](https://github.blog/changelog/2025-08-07-arm64-hosted-runners-for-public-repositories-are-now-generally-available/)
- [MSYS2 ARM64 support](https://www.msys2.org/docs/arm64/)
- [FFmpeg native Windows/MSYS2 compilation](https://ffmpeg.org/platform.html#Native-Windows-compilation-using-MSYS2)
- [CLANGARM64 Clang](https://packages.msys2.org/packages/mingw-w64-clang-aarch64-clang), [LLVM tools](https://packages.msys2.org/packages/mingw-w64-clang-aarch64-llvm-tools), [dav1d](https://packages.msys2.org/packages/mingw-w64-clang-aarch64-dav1d), and [zlib](https://packages.msys2.org/packages/mingw-w64-clang-aarch64-zlib) (both libraries provide static archives)

## Dispatch and publication

Manual inputs:

- `ffmpeg_version`: numeric FFmpeg release, default `7.1.1`.
- `architecture`: `both` (default), `x64`, or `arm64`.
- `publish`: `false` (default) uploads complete downloadable build artifacts without creating tags or releases; `true` also publishes architecture-specific releases at the workflow's exact commit.

A push of tag `pack-ffmpeg-<version>` builds/publishes x64 only. A push of tag `pack-ffmpeg-<version>-arm64` builds/publishes ARM64 only. The existing development-branch push trigger builds both architectures but does not publish. Each matrix leg runs independently (`fail-fast: false`).

| Output | x64 (legacy names preserved) | ARM64 |
| --- | --- | --- |
| Artifact | `pack-ffmpeg` | `pack-ffmpeg-arm64` |
| Release tag | `pack-ffmpeg-<version>` | `pack-ffmpeg-<version>-arm64` |
| ZIP | `pulse-pack-ffmpeg-<version>-win64.zip` | `pulse-pack-ffmpeg-<version>-arm64.zip` |
| Manifest | `pulse-pack-ffmpeg-<version>.json` | `pulse-pack-ffmpeg-<version>-arm64.json` |
| Generated catalog fragment | `pack-catalog.inc` | `pack-catalog-arm64.inc` |
| Catalog file array | `kMediaPackFiles` | `kMediaPackArm64Files` |
| Catalog release | `kMediaPackRelease` | `kMediaPackArm64Release` |

The installed names (`ffmpeg.exe`, `ffprobe.exe`, licenses, configuration/source text) and downloadable `<name>.lzms` assets are identical across architectures. Their **independent release tags** prevent collisions and preserve the existing x64 download URL convention. Generated catalog bases use the repository running the workflow, so fork builds point at the fork's releases. Copy the ARM64 fragment into the application catalog only after the referenced release is actually published and its hashes are verified; this package does not modify the application's catalog or architecture selector.

To add ARM64 to a version whose x64 release already exists, dispatch with `architecture=arm64` and `publish=true`, or push only the ARM64 tag. Publication does not replace existing releases/assets and always uses `--latest=false`, so the optional pack cannot become Pulse's main application update release.

## Configuration and verification

[The build script](build.sh) accepts `<ffmpeg-version> <output-dir> [x64|arm64]`; omitting the last argument retains the x64 behavior. Use it only inside the corresponding MSYS2 environment on a matching Windows host. It rejects a mismatched environment/compiler target and keeps separate architecture build directories. Clang uses LLVM archiving, resource, symbol, and strip tools rather than GCC runtime flags. The script requires the dav1d license instead of silently omitting it.

Both targets retain all built-in decoders, demuxers, parsers and bitstream filters plus software AV1 through dav1d. Encoders/muxers are limited to the original BMP/raw video and PCM/WAV preview outputs. Network, devices, hardware accelerators, GPL and nonfree components remain disabled. Filter and generated-configuration checks remain in the build script.

CI runs the original required codec/demuxer/muxer checks and PCM, accurate input/output seeking, float PCM, `atempo`, and video-frame smoke tests on the matching host. It also probes the generated WAV with the shipped `ffprobe.exe`. PE imports are inspected with GNU objdump (x64) or LLVM objdump (ARM64); unexpected external MinGW/LLVM/dav1d/zlib runtime dependencies fail packaging. OS-provided DLLs are permitted. Toolchain/package versions, configure logs, and import listings are uploaded as `pack-ffmpeg-diagnostics-<arch>`, including on failure where available.

Changing the workflow is not evidence that a native build or runtime smoke test passed. A successful matching CI run and published, verified assets are required before catalog integration.
