#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 0 ]]; then
    printf '사용법: INSTALL_PREFIX=... QT_PREFIX_PATH=... CMAKE_PREFIX_PATH=... ./install.sh\n' >&2
    exit 2
fi

source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$source_dir/build"
install_prefix="${INSTALL_PREFIX:-$HOME/.local/SDK/iiSocietySync}"
qt_prefix="${QT_PREFIX_PATH:-}"
if [[ -z "$qt_prefix" && -d "/Volumes/Storage/Qt/6.8.3/macos" ]]; then
    qt_prefix="/Volumes/Storage/Qt/6.8.3/macos"
fi
search_prefixes="${CMAKE_PREFIX_PATH:-}${CMAKE_PREFIX_PATH:+;}$HOME/.local/SDK"
if [[ -n "$qt_prefix" ]]; then
    search_prefixes="$qt_prefix${search_prefixes:+;$search_prefixes}"
fi

cmake -S "$source_dir" -B "$build_dir"     -DCMAKE_BUILD_TYPE=Release     -DBUILD_TESTING=ON     "-DCMAKE_INSTALL_PREFIX=$install_prefix"     "-DCMAKE_PREFIX_PATH=$search_prefixes"
cmake --build "$build_dir" --config Release --parallel
ctest --test-dir "$build_dir" -C Release --output-on-failure
cmake --install "$build_dir" --config Release

consumer_build_dir="$build_dir/consumer/build"
cmake -S "$source_dir/tests/consumer" -B "$consumer_build_dir"     -DCMAKE_BUILD_TYPE=Release     "-DiiSocietySync_DIR=$install_prefix/lib/cmake/iiSocietySync"     "-DCMAKE_PREFIX_PATH=$install_prefix${search_prefixes:+;$search_prefixes}"
cmake --build "$consumer_build_dir" --config Release --parallel
ctest --test-dir "$consumer_build_dir" -C Release --output-on-failure
