$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..\..\..")
$ndkRoot = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk\29.0.14206865"
$clang = Join-Path $ndkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
$buildDir = Join-Path $repoRoot "app\build\openxr_bridge"
$smokeExe64 = Join-Path $buildDir "unixcall_smoke64.exe"
$smokeExe32 = Join-Path $buildDir "unixcall_smoke32.exe"

& $clang --target=x86_64-w64-windows-gnu -nostdlib "-Wl,-e,mainCRTStartup" `
    -o $smokeExe64 `
    (Join-Path $PSScriptRoot "unixcall_smoke.c") `
    (Join-Path $buildDir "libkernel32_x64.a")
if ($LASTEXITCODE -ne 0) {
    throw "x64 unix-call smoke executable build failed with exit code $LASTEXITCODE"
}
& $clang --target=i686-w64-windows-gnu -nostdlib "-Wl,-e,mainCRTStartup" `
    -o $smokeExe32 `
    (Join-Path $PSScriptRoot "unixcall_smoke.c") `
    (Join-Path $buildDir "libkernel32_x86.a")
if ($LASTEXITCODE -ne 0) {
    throw "x86 unix-call smoke executable build failed with exit code $LASTEXITCODE"
}

$unixAsset = "/mnt/c/Users/flori/Documents/Coding/gamenative/app/src/main/assets/xr/unix/x86_64/gamenative_openxr.so"
$helperAsset64 = "/mnt/c/Users/flori/Documents/Coding/gamenative/app/src/main/assets/xr/windows/gamenative_xr_unixbridge64.dll"
$helperAsset32 = "/mnt/c/Users/flori/Documents/Coding/gamenative/app/src/main/assets/xr/windows/gamenative_xr_unixbridge32.dll"
$wineRoot = "/home/flori/projects/gamenative-wine-toolchain/root/usr/lib/x86_64-linux-gnu/wine"
$wineRunner = "$wineRoot/wine"
$smokeWsl64 = "/mnt/c/Users/flori/Documents/Coding/gamenative/app/build/openxr_bridge/unixcall_smoke64.exe"
$smokeWsl32 = "/mnt/c/Users/flori/Documents/Coding/gamenative/app/build/openxr_bridge/unixcall_smoke32.exe"

& wsl -d Ubuntu -- cp $unixAsset "$wineRoot/x86_64-unix/gamenative_xr_unixbridge.so"
if ($LASTEXITCODE -ne 0) { throw "Could not stage unixlib in Wine VM" }
& wsl -d Ubuntu -- cp $helperAsset64 "$wineRoot/x86_64-windows/gamenative_xr_unixbridge.dll"
if ($LASTEXITCODE -ne 0) { throw "Could not stage Wine PE companion in VM" }
& wsl -d Ubuntu -- cp $helperAsset32 "$wineRoot/i386-windows/gamenative_xr_unixbridge.dll"
if ($LASTEXITCODE -ne 0) { throw "Could not stage x86 Wine PE companion in VM" }
& wsl -d Ubuntu -- cp $helperAsset64 "/home/flori/projects/gamenative-wine-smoke-prefix9/drive_c/windows/system32/gamenative_xr_unixbridge.dll"
if ($LASTEXITCODE -ne 0) { throw "Could not stage x64 Wine PE companion in smoke prefix" }
& wsl -d Ubuntu -- cp $helperAsset32 "/home/flori/projects/gamenative-wine-smoke-prefix9/drive_c/windows/syswow64/gamenative_xr_unixbridge.dll"
if ($LASTEXITCODE -ne 0) { throw "Could not stage x86 Wine PE companion in smoke prefix" }

foreach ($smokeWsl in @($smokeWsl64, $smokeWsl32)) {
    & wsl -d Ubuntu -- env `
        "WINEPREFIX=/home/flori/projects/gamenative-wine-smoke-prefix9" `
        "WINEDEBUG=-all" `
        "WINEDLLOVERRIDES=gamenative_xr_unixbridge=b" `
        "WINEDLLPATH=$wineRoot/x86_64-unix`:$wineRoot/x86_64-windows`:$wineRoot/i386-windows" `
        "LD_LIBRARY_PATH=$wineRoot/x86_64-unix" `
        $wineRunner $smokeWsl
    if ($LASTEXITCODE -ne 0) {
        throw "Wine unix-call smoke test failed for $smokeWsl with exit code $LASTEXITCODE"
    }
}

Write-Host "Wine unix-call smoke tests passed (x64 and x86)"
