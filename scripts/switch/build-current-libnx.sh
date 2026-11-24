#!/bin/bash
# Build chiaki-ng.nro for Switch against the CURRENT devkitPro/libnx.
#   docker run --rm -v "$PWD:/build/chiaki" -w /build/chiaki \
#     devkitpro/devkita64:latest bash scripts/switch/build-current-libnx.sh
set -eo pipefail
git config --global --add safe.directory '*'

apt-get update -qq
apt-get install -y -qq cmake ninja-build python3 python3-protobuf protobuf-compiler pkg-config >/dev/null

dkp-pacman -Sy --noconfirm --needed \
	switch-glfw switch-mesa switch-libdrm_nouveau switch-sdl2 \
	switch-mbedtls switch-libopus switch-zlib switch-bzip2 switch-pkg-config \
	switch-curl switch-libjson-c switch-miniupnpc

# Prebuilt ffmpeg (+dav1d), same as the chiaki-switch build
PKG_URL=https://github.com/eborjaa/chiaki-switch/raw/libnx-4.12
for PKG in switch-dav1d-1.2.1-1-any.pkg.tar.zst switch-ffmpeg-6.1-5-any.pkg.tar.zst; do
	[ -f "$PKG" ] || curl -fsSL -o "$PKG" "$PKG_URL/$PKG"
	dkp-pacman -U --noconfirm "$PKG"
done
dkp-pacman -Q libnx devkitA64

rm -fv third-party/nanopb/generator/proto/nanopb_pb2.py
rm -rf build_switch

cmake -B build_switch -GNinja \
	-DCMAKE_PREFIX_PATH=/opt/devkitpro/portlibs/switch/ \
	-DCHIAKI_USE_SYSTEM_CURL=OFF \
	-DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
	-DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF \
	-DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DENABLE_IPV6=OFF -DENABLE_THREADED_RESOLVER=OFF \
	-DCURL_DISABLE_SOCKETPAIR=ON -DHAVE_SOCKETPAIR=OFF -DHAVE_BASENAME=OFF \
	-DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake \
	-DCHIAKI_ENABLE_TESTS=OFF \
	-DCHIAKI_ENABLE_CLI=OFF \
	-DCHIAKI_ENABLE_GUI=OFF \
	-DCHIAKI_ENABLE_ANDROID=OFF \
	-DCHIAKI_ENABLE_BOREALIS=ON \
	-DCHIAKI_LIB_ENABLE_MBEDTLS=ON \
	-DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
	-DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF

ninja -C build_switch -k 0
find build_switch -name '*.nro' -exec ls -l {} \;
