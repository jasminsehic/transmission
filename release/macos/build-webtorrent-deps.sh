#!/bin/bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
prefix="$root/build-xcode-webtorrent"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
jobs="$(sysctl -n hw.logicalcpu)"
sdk="$(xcrun --sdk macosx --show-sdk-path)"
deployment_target=12.0

fetch() {
    git init -q "$3"
    git -C "$3" fetch --depth 1 "$1" "$2"
    git -C "$3" checkout -q --detach FETCH_HEAD
}

fetch https://github.com/openssl/openssl.git c1eeb9406b6142148f267594197d853403d10208 "$work/openssl"
fetch https://github.com/curl/curl.git 11b991232fbcaa88e2b1faecac224416b0001e35 "$work/curl"

mkdir -p "$prefix/lib" "$prefix/include"

for arch in arm64 x86_64; do
    build="$work/$arch"
    mkdir -p "$build/openssl"
    (
        cd "$build/openssl"
        export MACOSX_DEPLOYMENT_TARGET="$deployment_target"
        export SDKROOT="$sdk"
        export CC="$(xcrun --sdk macosx --find clang)"
        "$work/openssl/Configure" "darwin64-$arch-cc" \
            --prefix="$build/ssl" --libdir=lib no-shared no-tests
        make -j"$jobs"
        make install_sw
    )

    cmake -S "$work/curl" -B "$build/curl" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
        -DCMAKE_INSTALL_PREFIX="$build/curl-install" \
        -DOPENSSL_ROOT_DIR="$build/ssl" -DOPENSSL_USE_STATIC_LIBS=ON \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF \
        -DBUILD_TESTING=OFF -DCURL_USE_OPENSSL=ON -DCURL_DISABLE_WEBSOCKETS=OFF \
        -DCURL_CA_BUNDLE=/etc/ssl/cert.pem -DCURL_USE_PKGCONFIG=OFF \
        -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF \
        -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=ON \
        -DCURL_DISABLE_LDAP=ON -DCURL_USE_GSSAPI=OFF -DUSE_LIBIDN2=OFF
    cmake --build "$build/curl" --parallel "$jobs"
    cmake --install "$build/curl"
    "$build/curl-install/bin/curl-config" --protocols | grep -qx WS
    "$build/curl-install/bin/curl-config" --protocols | grep -qx WSS

    cmake -S "$root/third-party/libdatachannel" -B "$build/rtc" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
        -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$build/archives" \
        -DOPENSSL_ROOT_DIR="$build/ssl" -DOPENSSL_USE_STATIC_LIBS=ON \
        -DBUILD_SHARED_LIBS=OFF -DPREFER_SYSTEM_LIB=OFF \
        -DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON
    cmake --build "$build/rtc" --target datachannel-static --parallel "$jobs"

    xcrun libtool -static -o "$build/libwebtorrent-deps.a" \
        "$build/archives/libdatachannel-static.a" \
        "$build/archives/libjuice-static.a" \
        "$build/archives/libusrsctp.a" \
        "$build/curl-install/lib/libcurl.a" \
        "$build/ssl/lib/libssl.a" "$build/ssl/lib/libcrypto.a"
done

cp -R "$work/arm64/curl-install/include/curl" "$prefix/include/"
xcrun lipo -create \
    "$work/arm64/libwebtorrent-deps.a" "$work/x86_64/libwebtorrent-deps.a" \
    -output "$prefix/lib/libwebtorrent-deps.a"
xcrun lipo "$prefix/lib/libwebtorrent-deps.a" -verify_arch arm64 x86_64
