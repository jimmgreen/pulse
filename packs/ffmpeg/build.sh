#!/usr/bin/env bash
# Builds the Pulse FFmpeg preview pack: a decode-only, LGPL-2.1+ FFmpeg.
#
#   ffmpeg.exe / ffprobe.exe, statically linked, no DLLs next to them.
#   Decoders, demuxers and parsers: all of FFmpeg's built-in ones, plus dav1d
#   (BSD-2) for software AV1. Encoders and muxers: only what Pulse pipes back
#   (BMP frames, raw PCM / WAV for playback). No network, no devices, no GPL
#   or nonfree components; --enable-gpl / --enable-nonfree are never passed.
#
# Runs in MSYS2 MINGW64 (see .github/workflows/preview-pack-ffmpeg.yml).
# Usage: packs/ffmpeg/build.sh <ffmpeg-version> <output-dir>
set -Eeuo pipefail
trap 'echo "FFmpeg pack failed at line $LINENO: $BASH_COMMAND" >&2' ERR

VERSION="${1:?ffmpeg version, e.g. 7.1.1}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo "Invalid FFmpeg release version" >&2; exit 1; }
OUT="$(realpath -m "${2:?output directory}")"
WORK="${WORK:-$PWD/build-pack-ffmpeg}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$WORK" "$OUT"
# MSYS2 does not necessarily ship upstream copyright notices with libass.
# Resolve the exact installed upstream version before starting the costly build.
pacman -Ql mingw-w64-x86_64-libass > "$WORK/libass-package-files.txt"
LIBASS_VERSION="$(pkg-config --modversion libass)"
[[ "$LIBASS_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid libass version: $LIBASS_VERSION" >&2; exit 1; }
LIBASS_LICENSE_URL="https://raw.githubusercontent.com/libass/libass/${LIBASS_VERSION}/COPYING"
curl --fail --location --retry 3 "$LIBASS_LICENSE_URL" -o "$OUT/LICENSE-libass.txt"
test -s "$OUT/LICENSE-libass.txt"
grep -q 'ISC License' "$OUT/LICENSE-libass.txt"
grep -q 'libass contributors' "$OUT/LICENSE-libass.txt"
cd "$WORK"

TARBALL="ffmpeg-${VERSION}.tar.xz"
if [ ! -f "$TARBALL" ]; then
    curl -fsSLO "https://ffmpeg.org/releases/${TARBALL}"
    curl -fsSLO "https://ffmpeg.org/releases/${TARBALL}.asc" || true
fi
SOURCE_DIR="$(realpath -m "$WORK/ffmpeg-${VERSION}")"
[[ "$SOURCE_DIR" == "$(pwd)/ffmpeg-${VERSION}" ]] || { echo "Source directory outside build workspace" >&2; exit 1; }
rm -rf "$SOURCE_DIR"
tar xf "$TARBALL"
cd "ffmpeg-${VERSION}"

# Filters: scaling / pixel formats / rotation for thumbnails, tiling for
# storyboards, resampling for audio playback.
FILTERS=scale,setsar,setpts,subtitles,format,null,anull,transpose,hflip,vflip,select,fps,tile,pad,thumbnail,aresample,aformat,volume,atempo,trim,atrim

# libass's static HarfBuzz/Graphite dependency contains C++ objects. Use the
# C++ linker driver so its runtime follows all archives in the link command.
./configure \
    --prefix="$WORK/prefix" \
    --target-os=mingw32 --arch=x86_64 \
    --pkg-config-flags=--static \
    --ld=g++ \
    --extra-ldflags="-static -static-libgcc -static-libstdc++" \
    --enable-static --disable-shared \
    --disable-autodetect --enable-w32threads --enable-zlib --enable-libdav1d --enable-libass \
    --disable-everything \
    --disable-network --disable-devices --disable-hwaccels --disable-doc --disable-debug \
    --disable-ffplay --enable-ffmpeg --enable-ffprobe \
    --enable-protocol=file,pipe \
    --enable-demuxers --enable-parsers --enable-decoders --enable-bsfs \
    --enable-encoder=bmp,rawvideo,pcm_s16le,pcm_f32le,wrapped_avframe \
    --enable-muxer=image2pipe,rawvideo,wav,pcm_s16le,pcm_f32le,null \
    --enable-filter="$FILTERS" \
    --enable-swscale --enable-swresample \
    --extra-version=pulse-pack

# Hard guarantees, checked on the generated configuration.
grep -q '^#define CONFIG_GPL 0' config.h
grep -q '^#define CONFIG_NONFREE 0' config.h
grep -q '^#define CONFIG_NETWORK 0' config.h

make -j"$JOBS"
make install
set -x
cp "$WORK/prefix/bin/ffmpeg.exe" "$WORK/prefix/bin/ffprobe.exe" "$OUT/"
strip "$OUT/ffmpeg.exe" "$OUT/ffprobe.exe"

# Licence material that has to travel with the binaries.
cp COPYING.LGPLv2.1 "$OUT/LICENSE-FFmpeg-LGPL-2.1.txt"
DAV1D_LICENSE="$(find /mingw64/share -ipath '*dav1d*' -iname 'COPYING*' -o -ipath '*dav1d*' -iname 'LICENSE*' | head -n1 || true)"
[ -n "$DAV1D_LICENSE" ] && cp "$DAV1D_LICENSE" "$OUT/LICENSE-dav1d.txt"
# Preserve all installed dependency notices, including transitive static libraries
# (libass, fontconfig, FreeType, FriBidi, HarfBuzz and their dependencies).
find /mingw64/share/licenses -type f -print0 | sort -z | while IFS= read -r -d '' notice; do
    printf '\n===== %s =====\n' "${notice#/mingw64/share/licenses/}"
    cat "$notice"
done > "$OUT/LICENSE-dependencies.txt"
test -s "$OUT/LICENSE-dependencies.txt"
pacman -Q > "$OUT/DEPENDENCIES.txt"
cat > "$OUT/SOURCE.txt" <<EOF
Pulse FFmpeg preview pack
FFmpeg ${VERSION}, built from the unmodified release tarball:
  https://ffmpeg.org/releases/${TARBALL}
Build script and configuration:
  packs/ffmpeg/build.sh in the Pulse repository
License: GNU Lesser General Public License 2.1 or later (no GPL or nonfree parts).
Static dependency package versions are recorded in DEPENDENCIES.txt and their
notices in LICENSE-dependencies.txt. MSYS2 source recipes and source URLs:
  https://github.com/msys2/MINGW-packages
The corresponding FFmpeg source tarball is published with this pack release.
libass ${LIBASS_VERSION} copyright notice: ${LIBASS_LICENSE_URL}
EOF
"$OUT/ffmpeg.exe" -hide_banner -buildconf > "$OUT/BUILDCONF.txt" 2>&1
echo "built: $(ls -la "$OUT")"

# Playback requires these filters in the shipped binary, not just a developer FFmpeg.
"$OUT/ffmpeg.exe" -hide_banner -filters > "$WORK/pack-filters.txt"
for filter in fps scale format atempo trim atrim setpts subtitles; do
    grep -Eq "[[:space:]]${filter}[[:space:]]" "$WORK/pack-filters.txt"
done
