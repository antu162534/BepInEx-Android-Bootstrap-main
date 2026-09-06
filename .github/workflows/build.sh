#!/usr/bin/env bash
set -euo pipefail

: "${ANDROID_NDK_ROOT:?ANDROID_NDK_ROOT must point to an Android NDK installation}"
: "${MOD_ROOT_EXTERNAL:=ON}"

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="$script_dir/build-arm64"
dist_dir="$script_dir/dist/lib/arm64-v8a"

cmake -S "$script_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DMOD_ROOT_EXTERNAL="$MOD_ROOT_EXTERNAL"
cmake --build "$build_dir" --parallel
mkdir -p "$dist_dir"
cp "$build_dir/libmodbootstrap.so" "$dist_dir/libmodbootstrap.so"
