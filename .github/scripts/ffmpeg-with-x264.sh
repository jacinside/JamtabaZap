#!/bin/bash
# Copyright (c) 2026 Javier Cordero
# SPDX-License-Identifier: GPL-3.0-or-later
# The build images ship FFmpeg without libx264, and synchronized video relies on x264's
# options. Rebuilds the same FFmpeg version over the image's own, with libx264.
# The images' own libx264 has no assembly (plain C, several times slower: HD can't keep up on
# a laptop), so x264 is built here too, with its assembly, over the image's.
# usage: ffmpeg-with-x264.sh [cross-prefix]
set -euo pipefail
CROSS="${1:-}"
LIB=$(find / -name libavcodec.a -not -path '/proc/*' 2>/dev/null | head -1)
X264=$(find / -name x264.h -not -path '/proc/*' 2>/dev/null | head -1)
[ -n "$LIB" ] && [ -n "$X264" ] || { echo "libavcodec.a: '$LIB', x264.h: '$X264'"; exit 1; }
PREFIX=$(dirname "$(dirname "$LIB")")
X264_PREFIX=$(dirname "$(dirname "$X264")")
echo "FFmpeg prefix: $PREFIX, x264: $X264_PREFIX"
# Without nasm, FFmpeg builds without its assembly: decoding and color conversion take far more CPU.
apt-get update -qq && apt-get install -y -qq nasm > /dev/null
nasm -v

# VideoLAN's server sometimes refuses CI runners: GitHub's mirror has the same commit
git clone --quiet -b stable https://code.videolan.org/videolan/x264.git /tmp/x264 \
    || git clone --quiet -b stable https://github.com/mirror/x264.git /tmp/x264
(cd /tmp/x264 && git checkout --quiet b35605ace3ddf7c1a5d67a2eb553f034aef41d55)
X264_ARGS=(--prefix="$X264_PREFIX" --enable-static --disable-cli --enable-pic)
[ -n "$CROSS" ] && X264_ARGS+=(--host=x86_64-w64-mingw32 --cross-prefix="$CROSS")
(cd /tmp/x264 && ./configure "${X264_ARGS[@]}" > /tmp/x264-configure.log 2>&1 \
    || { tail -30 /tmp/x264-configure.log; exit 1; })
grep -E '^asm:' /tmp/x264/config.log /tmp/x264-configure.log 2>/dev/null | head -2 || true
grep -q '^#define HAVE_X86_INLINE_ASM 1\|^#define HAVE_MMX 1' /tmp/x264/config.h || { echo "x264 would build without assembly"; exit 1; }
make -C /tmp/x264 -j"$(nproc)" > /tmp/x264-make.log 2>&1 || { tail -30 /tmp/x264-make.log; exit 1; }
make -C /tmp/x264 install-lib-static > /dev/null
# counted, not grep -q: with pipefail, grep leaving early fails the pipe
AVX2=$("${CROSS}nm" "$X264_PREFIX/lib/libx264.a" 2>/dev/null | grep -c -i 'satd.*avx2' || true)
[ "$AVX2" -gt 0 ] || { echo "libx264.a has no AVX2 code"; exit 1; }
echo "libx264.a: $AVX2 AVX2 functions"
echo "x264 with assembly installed in $X264_PREFIX"

git clone --quiet --depth 1 -b n4.4.1 https://github.com/FFmpeg/FFmpeg.git /tmp/ffmpeg
cd /tmp/ffmpeg
ARGS=(--prefix="$PREFIX" --enable-gpl --enable-libx264 --enable-static --disable-shared --enable-pic
      --disable-programs --disable-doc --disable-debug
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
grep -E 'libx264|^install prefix|^x86 assembler' /tmp/ffmpeg-configure.log | head -5 || true
grep -q '^x86 assembler.*nasm' /tmp/ffmpeg-configure.log || { echo "FFmpeg would build without assembly"; exit 1; }
make -j"$(nproc)" > /tmp/ffmpeg-make.log 2>&1 || { tail -30 /tmp/ffmpeg-make.log; exit 1; }
make install > /dev/null
grep -q libx264 "$PREFIX/lib/libavcodec.a" || { echo "libx264 didn't make it into libavcodec.a"; exit 1; }
echo "FFmpeg with libx264 installed in $PREFIX"
