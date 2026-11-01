#!/usr/bin/env bash
# Build the application, its firmware installation bundle and the portable archive.
set -euo pipefail
cd "$(dirname "$0")/.."

build_dir="${BUILD_DIR:-$PWD/build/linux}"
options=("BUILD_DIR=$build_dir" "QMAKE=${QMAKE:-/opt/Qt/6.12.0/gcc_64/bin/qmake}"
         GCS_BUILD_CONF=release PACKAGE_TAGN=x86_64)

make -j"${JOBS:-4}" "${options[@]}" arm_sdk_install glc6_download breakpad_install
make -j"${JOBS:-4}" "${options[@]}" gcs
make "${options[@]}" package_ground_compress

cd "$build_dir/package-linux_x86_64"
sha256sum droning_linux_x86_64.tar.xz > SHA256SUMS
printf 'Package: %s/droning_linux_x86_64.tar.xz\n' "$PWD"
