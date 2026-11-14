#!/bin/bash
# Copyright (c) 2026 Javier Cordero
# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the Linux build into one AppImage that carries the libraries most distros lack
# (ICU in particular: the build machine's version is not the one newer distros ship).
# Usage: linux-appimage.sh <JamtabaZap executable> <VstScanner executable> <output .AppImage>
set -euo pipefail

app=$(realpath "$1")
scanner=$(realpath "$2")
out=$(realpath -m "$3")
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

# The tools run extracted, with no FUSE needed on the build machine.
export APPIMAGE_EXTRACT_AND_RUN=1
curl -fsSL -o linuxdeploy https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
curl -fsSL -o appimagetool https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
chmod +x linuxdeploy appimagetool
# Clear the AppImage magic bytes in their ELF header: emulated x86 (Docker on Apple Silicon) won't run them otherwise.
for tool in linuxdeploy appimagetool; do printf '\0\0\0' | dd of="$tool" bs=1 seek=8 conv=notrunc status=none; done

cat > JamtabaZap.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=JamtabaZap
GenericName=Online music jam sessions
Comment=Play music online with synchronized video
Exec=JamtabaZap
Icon=JamtabaZap
Terminal=false
Categories=AudioVideo;Audio;
EOF
cp "$repo/installers/linux/Jamtaba2.png" JamtabaZap.png

# Qt 5 loads OpenSSL 1.1 at run time for HTTPS; newer distros only ship OpenSSL 3.
ssl=$(ldconfig -p | awk '$1 == "libssl.so.1.1" && /x86-64/ {print $NF; exit}')
crypto=$(ldconfig -p | awk '$1 == "libcrypto.so.1.1" && /x86-64/ {print $NF; exit}')
[ -n "$ssl" ] && [ -n "$crypto" ] || { echo "OpenSSL 1.1 isn't installed on this machine"; exit 1; }

# Qt is linked statically, so linuxdeploy only has to gather the system libraries.
# VstScanner sits next to JamtabaZap: the app looks for it in its own folder.
./linuxdeploy --appdir AppDir \
    --executable "$app" --executable "$scanner" \
    --library "$ssl" --library "$crypto" \
    --desktop-file JamtabaZap.desktop --icon-file JamtabaZap.png

# appimagetool's runtime is static, so the AppImage needs no libfuse2 on the user's machine.
ARCH=x86_64 ./appimagetool --no-appstream AppDir "$out"
echo "made $out"
