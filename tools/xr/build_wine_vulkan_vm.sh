#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
work_root="${GN_WINE_XR_WORK_ROOT:-$HOME/projects/gamenative-winevulkan-9.2}"
source_dir="$work_root/wine-9.2"
build_dir="$work_root/build64-x"
toolchain_root="${GN_WINE_TOOLCHAIN_ROOT:-$HOME/projects/gamenative-wine-toolchain/root}"
jobs="${GN_WINE_BUILD_JOBS:-4}"
patch_file="$repo_root/tools/xr/wine-9.2-xr-vulkan.patch"
asset_dir="$repo_root/app/src/main/assets/xr/wine/wine-9.2-x86_64/x86_64-unix"

if [[ ! -x "$source_dir/configure" ]]; then
    mkdir -p "$work_root"
    git clone --depth 1 --branch wine-9.2 \
        https://gitlab.winehq.org/wine/wine.git "$source_dir"
fi

if ! grep -q "GameNative's OpenXR unixlib" "$source_dir/dlls/winevulkan/vulkan.c"; then
    (cd "$source_dir" && git apply "$patch_file")
fi

if [[ ! -f "$build_dir/Makefile" ]]; then
    mkdir -p "$build_dir"
    (
        cd "$build_dir"
        CFLAGS=-std=gnu11 \
        CPPFLAGS="-I$toolchain_root/usr/include" \
        LDFLAGS="-L$toolchain_root/usr/lib/x86_64-linux-gnu" \
        "$source_dir/configure" \
            --enable-win64 \
            --without-alsa --without-capi --without-coreaudio --without-cups \
            --without-dbus --without-fontconfig --without-freetype \
            --without-gettext --without-gettextpo --without-gphoto \
            --without-gstreamer --without-inotify --without-krb5 \
            --without-mingw --without-netapi --without-opencl --without-opengl \
            --without-oss --without-pcap --without-pcsclite --without-pulse \
            --without-sane --without-sdl --without-udev --without-unwind \
            --without-usb --without-v4l2 --without-wayland
    )
fi

make -C "$build_dir" -j"$jobs" dlls/winevulkan/winevulkan.so

gcc -std=gnu11 -D__WINESRC__ -DWINE_UNIX_LIB \
    -I"$build_dir/dlls/winevulkan" -I"$source_dir/dlls/winevulkan" \
    -I"$build_dir/include" -I"$source_dir/include" \
    "$repo_root/tools/xr/winevulkan_layout_probe.c" \
    -o "$work_root/winevulkan_layout_probe"
layout="$("$work_root/winevulkan_layout_probe")"
echo "$layout"
if [[ "$layout" != *"handle=4088"* ]]; then
    echo "Unexpected Wine 9.2 wine_device layout; update the unixlib handle adapter" >&2
    exit 1
fi

mkdir -p "$asset_dir"
cp "$build_dir/dlls/winevulkan/winevulkan.so" "$asset_dir/winevulkan.so"
file "$asset_dir/winevulkan.so"
sha256sum "$asset_dir/winevulkan.so"
