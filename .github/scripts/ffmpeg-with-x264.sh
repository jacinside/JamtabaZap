#!/bin/bash
# The build images ship FFmpeg without libx264, and synchronized video relies on x264's
# options. Rebuilds the same FFmpeg version over the image's own, with libx264.
# usage: ffmpeg-with-x264.sh [cross-prefix]
set -euo pipefail
CROSS="${1:-}"
LIB=$(find / -name libavcodec.a -not -path '/proc/*' 2>/dev/null | head -1)
X264=$(find / -name x264.h -not -path '/proc/*' 2>/dev/null | head -1)
[ -n "$LIB" ] && [ -n "$X264" ] || { echo "libavcodec.a: '$LIB', x264.h: '$X264'"; exit 1; }
PREFIX=$(dirname "$(dirname "$LIB")")
X264_PREFIX=$(dirname "$(dirname "$X264")")
echo "FFmpeg prefix: $PREFIX, x264: $X264_PREFIX"
git clone --quiet --depth 1 -b n4.4.1 https://github.com/FFmpeg/FFmpeg.git /tmp/ffmpeg
cd /tmp/ffmpeg
ARGS=(--prefix="$PREFIX" --enable-gpl --enable-libx264 --enable-static --disable-shared --enable-pic
      --disable-programs --disable-doc --disable-debug --disable-x86asm
      --extra-cflags="-I$X264_PREFIX/include" --extra-ldflags="-L$X264_PREFIX/lib"
      --pkg-config-flags=--static)
if [ -n "$CROSS" ]; then
    ARGS+=(--enable-cross-compile --cross-prefix="$CROSS" --target-os=mingw32 --arch=x86_64 --pkg-config=true)
    # pkg-config is off when cross-compiling here, so x264 is checked by linking it directly
    sed -i 's/require_pkg_config libx264 x264 "stdint.h x264.h" x264_encoder_encode/require libx264 "stdint.h x264.h" x264_encoder_encode -lx264/' configure
else
    sed -i 's/require_pkg_config libx264 x264 "stdint.h x264.h" x264_encoder_encode/require libx264 "stdint.h x264.h" x264_encoder_encode -lx264 -lpthread -ldl -lm/' configure
fi
./configure "${ARGS[@]}" > /tmp/ffmpeg-configure.log 2>&1 || { tail -30 ffbuild/config.log; exit 1; }
grep -E 'libx264|^install prefix' /tmp/ffmpeg-configure.log | head -5 || true
make -j"$(nproc)" > /tmp/ffmpeg-make.log 2>&1 || { tail -30 /tmp/ffmpeg-make.log; exit 1; }
make install > /dev/null
grep -q libx264 "$PREFIX/lib/libavcodec.a" || { echo "libx264 didn't make it into libavcodec.a"; exit 1; }
echo "FFmpeg with libx264 installed in $PREFIX"
