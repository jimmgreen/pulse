# Native Windows ARM64 and preview packs

The touchscreen UI is shared application code, not an ARM64-only feature. CPU
architecture is used only where native binaries require it. Automatic touch
layout is based on digitizer capability. Checkbox policy, selection geometry,
and the frame-driven inertia model are built for the normal x64 LumaText and
DirectWrite configurations, the Windows 8.1 x64 compatibility configuration,
and ARM64. Windows 8.1 CI runs on Windows Server 2022; its import audit is not a
substitute for execution on Windows 8.1. Gesture model tests do not replace
real-touchscreen verification.

## Optional pack builds

All four pack workflows support native `x64` and `arm64`. The latter uses the
public-repository `windows-11-arm` runner, not an x64 runner attempting to start
ARM64 executables. Build tools may themselves use Windows emulation (MSYS2 Unix
tools do); the compiler target and packaged EXE/DLL PE machine must match the
requested native architecture before the pack's original smoke tests run.

| Pack | Workflow | Native checks retained |
| --- | --- | --- |
| Images | [preview-pack-images.yml](../.github/workflows/preview-pack-images.yml) | Pixel/model tests, HEVC/AV1 availability, staged DLL closure, decoder-only licence rules |
| RAW | [preview-pack-raw.yml](../.github/workflows/preview-pack-raw.yml) | Malformed/missing inputs, bounded output, synthetic DNG metadata, embedded preview and explicit demosaic |
| FFmpeg | [preview-pack-ffmpeg.yml](../.github/workflows/preview-pack-ffmpeg.yml) | Required codecs/demuxers/muxers, WAV probe, PCM, accurate seeking, tempo and video output, self-contained imports |
| Archive | [preview-pack-archive.yml](../.github/workflows/preview-pack-archive.yml) | Independently pinned official installer digest, 7-Zip identity/version, both PE machines and native format enumeration |

Dispatch Images/RAW with `pack_version`, `architecture=all|x64|arm64`, and
`publish`. FFmpeg uses `ffmpeg_version`, `architecture=both|x64|arm64`, and
`publish`. Archive uses the official `version`, the independently verified
installer SHA-256 values, `architecture=all|x64|arm64`, and `publish`. The
Archive default digests apply **only to 26.03**; changing the version requires
new independently verified digests.

`publish=false` keeps downloadable Actions artifacts without advertising a new
installable release. Explicit publication creates architecture-separated tags:
`pack-<key>-<version>` for x64, and `pack-<key>-<version>-arm64` for ARM64. The
release is created at the built commit and is never marked as the latest main
application update. Existing release assets must not be overwritten: choose a
new version. Installed file names stay unchanged; separate release tags prevent
EXE/DLL and compressed-asset collisions. Original x64 download conventions are
preserved.

Images keep libheif/libde265 as replaceable LGPL DLLs; RAW keeps LibRaw as a
replaceable DLL; FFmpeg stays decode-only LGPL with GPL/nonfree/network disabled.
Architecture support must not be obtained by dropping required decoders,
licence files, dependency checks, or existing smoke tests.

## Download selection and coexistence

Catalog selection follows the **compiled Pulse process**, not its host OS:

- Native ARM64 Pulse uses [the ARM64 catalog](../src/app/pack_catalog_arm64_generated.h).
- x64 Pulse, including x64 Pulse under Windows ARM emulation, retains [the original x64 catalog](../src/app/pack_catalog_generated.h).
- An unpublished native package has no downloadable entry. It must not silently
  fall back to another architecture's catalog or invented hashes.

Only after a workflow succeeds and a release actually exists may its generated
manifest/catalog be imported. Both compressed and uncompressed sizes and
SHA-256 digests remain compiled into Pulse. Fork-built ARM64 catalog entries
reference the fork's verified releases; upstream maintainers may rebuild and
publish equivalent upstream assets and regenerate the hashes. Replacing only
the repository URL without publishing byte-identical payloads is not valid.

[The ARM64 catalog importer](../scripts/import_arm64_pack_catalog.py) requires all
four published native manifests, checks their GitHub asset digests and every
compressed payload's size/digest metadata, then writes a complete catalog in one
step. It never manufactures a missing entry or executes a downloaded helper:

```powershell
python scripts/import_arm64_pack_catalog.py --repository OWNER/pulse `
  --ffmpeg-version 7.1.1 --images-version 1.0.3 --raw-version 1.0.3 --archive-version 26.03
```

The subsequent native online regression verifies actual downloaded bytes,
decompression and EXE/DLL machine values; metadata checks alone are not runtime
verification.

The original x64 installation root remains `%LOCALAPPDATA%\Pulse\packs`.
ARM64 tools and their `installed.json` markers live under
`%LOCALAPPDATA%\Pulse\packs\arm64`, so a legacy x64 pack cannot be treated as a
native ARM64 installation. No old pack is deleted or migrated automatically.
User preferences remain shared in `%LOCALAPPDATA%\Pulse\packs\packs.json`,
including enabled flags, explicit custom FFmpeg, download source, automatic
updates, and uninstall preference. A user-selected custom FFmpeg remains an
explicit choice and is not silently replaced.

## Focused runtime verification

[The native routing workflow](../.github/workflows/preview-pack-runtime.yml)
builds only the affected pack tests and preview-host dependencies on x64 and
ARM64. Its isolated-profile tests cover catalog selection, legacy-root
isolation, shared settings, first-save parent creation, coexistence and cache
invalidation. It retains the related module tests and their pre-existing
integration prerequisites rather than converting skips to passes.

After **all four native entries are published**, manually dispatch with
`install_published_packs=true`. This exercises the production downloader,
hash verification, LZMS decompression and commit into a new isolated root,
checks every downloaded EXE/DLL machine, and runs the published RAW and archive
packs through the preview IPC host using generated fixtures. This does not
claim a full media/image integration run when their encoder/camera fixtures
are unavailable. The shipped FFmpeg intentionally cannot encode those fixture
formats; its original native decoder/PCM/seek smoke tests run separately in the
pack build.

All compilation and executable verification for this change are performed in
the GitHub fork. No full historical selftest suite or local installer run is
required for these scoped checks.
