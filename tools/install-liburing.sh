#!/bin/sh
# Build and install a pinned liburing release. Used by CI and by
# cibuildwheel (inside the manylinux container); distro packages are often
# too old (Ubuntu 22.04 ships 2.1).
set -eu
VERSION="${LIBURING_VERSION:-2.15}"
# The tarball ends up inside the published wheels, so verify it. Overriding
# LIBURING_VERSION requires passing its LIBURING_SHA256 too.
if [ "$VERSION" = "2.15" ]; then
    SHA256="${LIBURING_SHA256:-8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a}"
else
    SHA256="${LIBURING_SHA256:?set LIBURING_SHA256 for liburing $VERSION}"
fi
PREFIX="${PREFIX:-/usr/local}"
# Root (containers) needs no sudo; set SUDO= to install to a writable PREFIX.
if [ -z "${SUDO+set}" ]; then
    SUDO=""
    [ "$(id -u)" -eq 0 ] || SUDO="sudo"
fi
work="$(mktemp -d)"
curl -sSfL -o "$work/liburing.tar.gz" \
    "https://github.com/axboe/liburing/archive/refs/tags/liburing-${VERSION}.tar.gz"
echo "$SHA256  $work/liburing.tar.gz" | sha256sum -c -
tar xzf "$work/liburing.tar.gz" -C "$work"
cd "$work/liburing-liburing-${VERSION}"
./configure --prefix="$PREFIX"
make -j"$(nproc)" -C src
$SUDO make -C src install
$SUDO ldconfig 2>/dev/null || true   # not needed for private prefixes
rm -rf "$work"
