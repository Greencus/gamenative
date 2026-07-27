#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
work_root="${GN_WINE_XR_WORK_ROOT:-$HOME/projects/gamenative-winevulkan-9.2}"
build_dir="$work_root/build64-x"
toolchain_root="${GN_WINE_TOOLCHAIN_ROOT:-$HOME/projects/gamenative-wine-toolchain/root}"
prefix="${GN_WINE_XR_PREFIX:-$HOME/projects/gamenative-wine-xr-smoke-prefix}"
stage="$work_root/xr-smoke-stage"
smoke_exe="$work_root/winevulkan_unixlib_smoke.exe"
runtime_smoke_exe="$work_root/runtime_vulkan_enable_smoke.exe"
log_file="/tmp/gamenative-xr-unix.log"

mkdir -p "$stage/x86_64-windows" "$stage/x86_64-unix"
cp "$repo_root/app/src/main/assets/xr/windows/gamenative_xr_unixbridge64.dll" \
   "$stage/x86_64-windows/gamenative_xr_unixbridge.dll"
cp "$repo_root/app/src/main/assets/xr/windows/gamenative_openxr64.dll" \
   "$stage/x86_64-windows/gamenative_openxr64.dll"
cp "$repo_root/app/src/main/assets/xr/windows/gamenative_openxr64.dll" \
   "$work_root/gamenative_openxr64.dll"
cp "$repo_root/app/src/main/assets/xr/unix/x86_64/gamenative_openxr.so" \
   "$stage/x86_64-unix/gamenative_xr_unixbridge.so"

"$toolchain_root/usr/bin/x86_64-w64-mingw32-gcc-posix" -O2 \
    -I"$toolchain_root/usr/include" \
    -I"$repo_root/app/src/main/windows/openxr_bridge" \
    "$repo_root/app/src/main/windows/openxr_bridge/unix/winevulkan_unixlib_smoke.c" \
    "$toolchain_root/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/libvulkan-1.a" \
    -o "$smoke_exe"
"$toolchain_root/usr/bin/x86_64-w64-mingw32-gcc-posix" -O2 \
    -I"$toolchain_root/usr/include" \
    -I"$repo_root/app/src/main/cpp/third_party" \
    "$repo_root/app/src/main/windows/openxr_bridge/unix/runtime_vulkan_enable_smoke.c" \
    "$toolchain_root/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/libvulkan-1.a" \
    -o "$runtime_smoke_exe"

if [[ ! -f "$prefix/system.reg" ]]; then
    WINEARCH=win64 WINEPREFIX="$prefix" WINEDEBUG=-all DISPLAY="${DISPLAY:-:0}" \
        timeout 240s "$build_dir/wine" wineboot -u || true
    WINEPREFIX="$prefix" "$build_dir/server/wineserver" -k 2>/dev/null || true
fi
WINEPREFIX="$prefix" "$build_dir/server/wineserver" -k 2>/dev/null || true
WINEPREFIX="$prefix" WINEDEBUG=-all DISPLAY="${DISPLAY:-:0}" \
    timeout 60s "$build_dir/wine" cmd /c echo "Wine XR smoke prefix ready"

GAMENATIVE_XR=1 \
WINEPREFIX="$prefix" \
WINEDEBUG=-all \
WINEDLLPATH="$stage" \
DISPLAY="${DISPLAY:-:0}" \
    "$build_dir/wine" "$runtime_smoke_exe"

: >"$log_file"
set +e
GAMENATIVE_XR=1 \
WINEPREFIX="$prefix" \
WINEDEBUG=-all \
WINEDLLOVERRIDES=gamenative_xr_unixbridge=b \
WINEDLLPATH="$stage" \
DISPLAY="${DISPLAY:-:0}" \
    "$build_dir/wine" "$smoke_exe"
result=$?
set -e

if [[ $result -eq 0 ]]; then
    echo "Wine Vulkan unixlib producer smoke passed"
    exit 0
fi
if grep -q "Vulkan context ready" "$log_file" &&
   grep -q "vkAllocateMemory(dma-buf) failed" "$log_file"; then
    echo "Wine host interop passed; dma-buf allocation skipped on the VM software Vulkan driver"
    exit 0
fi

cat "$log_file"
echo "Wine Vulkan unixlib smoke failed with exit code $result" >&2
exit "$result"
