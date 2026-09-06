#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 0 ]]; then
    echo "Usage: INSTALL_PREFIX=... QT_PREFIX_PATH=... CMAKE_PREFIX_PATH=... ./install.sh" >&2
    exit 2
fi

source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$source_dir/build"
install_prefix="${INSTALL_PREFIX:-$HOME/.local/SDK/iiLocalLLM}"
qt_prefix="${QT_PREFIX_PATH:-}"
if [[ -z "$qt_prefix" && -d "/Volumes/Storage/Qt/6.8.3/macos" ]]; then
    qt_prefix="/Volumes/Storage/Qt/6.8.3/macos"
fi
prefix_path="${CMAKE_PREFIX_PATH:-}"
if [[ -n "$qt_prefix" ]]; then
    prefix_path="$qt_prefix${prefix_path:+;$prefix_path}"
fi

cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DCMAKE_INSTALL_PREFIX="$install_prefix" \
    -DCMAKE_PREFIX_PATH="$prefix_path"
cmake --build "$build_dir" --config Release --parallel
ctest --test-dir "$build_dir" -C Release --output-on-failure
cmake --install "$build_dir" --config Release

consumer_build_dir="$build_dir/consumer/build"
cmake -S "$source_dir/tests/consumer" -B "$consumer_build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$install_prefix${prefix_path:+;$prefix_path}" \
    -DiiLocalLLM_DIR="$install_prefix/lib/cmake/iiLocalLLM"
cmake --build "$consumer_build_dir" --config Release --parallel
ctest --test-dir "$consumer_build_dir" -C Release --output-on-failure
