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
set -euo pipefail

VERSION="${1:?ffmpeg version, e.g. 7.1.1}"
OUT="$(realpath -m "${2:?output directory}")"
WORK="${WORK:-$PWD/build-pack-ffmpeg}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$WORK" "$OUT"
cd "$WORK"

TARBALL="ffmpeg-${VERSION}.tar.xz"
if [ ! -f "$TARBALL" ]; then
    curl -fsSLO "https://ffmpeg.org/releases/${TARBALL}"
    curl -fsSLO "https://ffmpeg.org/releases/${TARBALL}.asc" || true
fi
rm -rf "ffmpeg-${VERSION}"
tar xf "$TARBALL"
cd "ffmpeg-${VERSION}"

# Filters: scaling / pixel formats / rotation for thumbnails, tiling for
# storyboards, resampling for audio playback.
FILTERS=scale,setsar,format,null,anull,transpose,hflip,vflip,select,fps,tile,pad,thumbnail,aresample,aformat,volume,atempo,trim,atrim

./configure \
    --prefix="$WORK/prefix" \
    --target-os=mingw32 --arch=x86_64 \
    --pkg-config-flags=--static \
    --extra-ldflags="-static -static-libgcc" \
    --enable-static --disable-shared \
    --disable-autodetect --enable-w32threads --enable-zlib --enable-libdav1d \
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
cp "$WORK/prefix/bin/ffmpeg.exe" "$WORK/prefix/bin/ffprobe.exe" "$OUT/"
strip "$OUT/ffmpeg.exe" "$OUT/ffprobe.exe"

# Licence material that has to travel with the binaries.
cp COPYING.LGPLv2.1 "$OUT/LICENSE-FFmpeg-LGPL-2.1.txt"
DAV1D_LICENSE="$(find /mingw64/share -ipath '*dav1d*' -iname 'COPYING*' -o -ipath '*dav1d*' -iname 'LICENSE*' | head -n1 || true)"
[ -n "$DAV1D_LICENSE" ] && cp "$DAV1D_LICENSE" "$OUT/LICENSE-dav1d.txt"
cat > "$OUT/SOURCE.txt" <<EOF
Pulse FFmpeg preview pack
FFmpeg ${VERSION}, built from the unmodified release tarball:
  https://ffmpeg.org/releases/${TARBALL}
Build script and configuration:
  packs/ffmpeg/build.sh in the Pulse repository
License: GNU Lesser General Public License 2.1 or later (no GPL or nonfree parts).
EOF
"$OUT/ffmpeg.exe" -hide_banner -buildconf > "$OUT/BUILDCONF.txt" 2>&1
echo "built: $(ls -la "$OUT")"

# Playback requires these filters in the shipped binary, not just a developer FFmpeg.
"$OUT/ffmpeg.exe" -hide_banner -filters > "$WORK/pack-filters.txt"
for filter in fps scale format atempo trim atrim; do
    grep -Eq "[[:space:]]${filter}[[:space:]]" "$WORK/pack-filters.txt"
done
