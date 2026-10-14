#!/bin/sh
# Build and install a pinned liburing release. Used by CI and by
# cibuildwheel (inside the manylinux container); distro packages are often
# too old (Ubuntu 22.04 ships 2.1).
set -eu
VERSION="${LIBURING_VERSION:-2.15}"
PREFIX="${PREFIX:-/usr/local}"
# Root (containers) needs no sudo; set SUDO= to install to a writable PREFIX.
if [ -z "${SUDO+set}" ]; then
    SUDO=""
    [ "$(id -u)" -eq 0 ] || SUDO="sudo"
fi
work="$(mktemp -d)"
curl -sSfL "https://github.com/axboe/liburing/archive/refs/tags/liburing-${VERSION}.tar.gz" \
    | tar xz -C "$work"
cd "$work/liburing-liburing-${VERSION}"
./configure --prefix="$PREFIX"
make -j"$(nproc)" -C src
$SUDO make -C src install
$SUDO ldconfig 2>/dev/null || true   # not needed for private prefixes
rm -rf "$work"
