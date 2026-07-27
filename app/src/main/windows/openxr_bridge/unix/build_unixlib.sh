#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../../../../../.." && pwd)"
toolchain_root="${GN_WINE_TOOLCHAIN_ROOT:-$HOME/projects/gamenative-wine-toolchain/root}"
out_dir="$repo_root/app/src/main/assets/xr/unix/x86_64"
windows_out_dir="$repo_root/app/src/main/assets/xr/windows"

mkdir -p "$out_dir" "$windows_out_dir"
gcc -std=c11 -O2 -g -fPIC -fvisibility=hidden -shared \
    -Wall -Wextra -Werror \
    -I"$toolchain_root/usr/include" \
    -I"$toolchain_root/usr/include/libdrm" \
    -o "$out_dir/gamenative_openxr.so" \
    "$script_dir/gamenative_openxr_unix.c" \
    -ldl -lpthread

file "$out_dir/gamenative_openxr.so"

# Build the tiny builtin PE companions with Wine's own toolchain. Wine 9.x
# associates the matching Unix library only when this module comes from its
# architecture-specific *-windows directory.
wine_bin_dir="$toolchain_root/usr/lib/x86_64-linux-gnu/wine"
wine_lib_dir="$toolchain_root/usr/lib/x86_64-linux-gnu/wine"
tools_dir="$(mktemp -d)"
build_dir="$(mktemp -d)"
cleanup() {
    rm -rf -- "$tools_dir" "$build_dir"
}
trap cleanup EXIT

ln -s "$wine_bin_dir/winegcc" "$tools_dir/winegcc"
ln -s "$wine_bin_dir/winebuild" "$tools_dir/winebuild"
ln -s "$toolchain_root/usr/bin/x86_64-w64-mingw32-gcc-posix" \
    "$tools_dir/x86_64-w64-mingw32-gcc"
ln -s "$toolchain_root/usr/bin/x86_64-w64-mingw32-gcc-posix" \
    "$tools_dir/x86_64-w64-mingw32-g++"
ln -s "$toolchain_root/usr/bin/i686-w64-mingw32-gcc-posix" \
    "$tools_dir/i686-w64-mingw32-gcc"
ln -s "$toolchain_root/usr/bin/i686-w64-mingw32-gcc-posix" \
    "$tools_dir/i686-w64-mingw32-g++"

winegcc_path="$tools_dir/winegcc"
winebuild_path="$tools_dir/winebuild"
build_path="$tools_dir:$toolchain_root/usr/bin:/usr/bin:/bin"
helper_source="$script_dir/gamenative_xr_unixbridge.c"
helper_spec="$script_dir/gamenative_xr_unixbridge.spec"

(
    cd "$build_dir"
    PATH="$build_path" WINEBUILD="$winebuild_path" "$winegcc_path" \
        --target x86_64-w64-mingw32 -m64 -shared \
        -Wl,--wine-builtin \
        -o gamenative_xr_unixbridge.dll \
        "$helper_source" "$helper_spec" \
        -L"$wine_lib_dir/x86_64-windows"
    cp gamenative_xr_unixbridge.dll \
        "$windows_out_dir/gamenative_xr_unixbridge64.dll"

    PATH="$build_path" WINEBUILD="$winebuild_path" "$winegcc_path" \
        --target i686-w64-mingw32 -m32 -shared \
        -Wl,--wine-builtin \
        -o gamenative_xr_unixbridge.dll \
        "$helper_source" "$helper_spec" \
        -L"$wine_lib_dir/i386-windows"
    cp gamenative_xr_unixbridge.dll \
        "$windows_out_dir/gamenative_xr_unixbridge32.dll"
)

file "$windows_out_dir/gamenative_xr_unixbridge64.dll"
file "$windows_out_dir/gamenative_xr_unixbridge32.dll"
