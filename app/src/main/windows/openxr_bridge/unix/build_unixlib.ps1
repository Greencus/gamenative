$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..\..\..")
$assetRoot = Join-Path $repoRoot "app\src\main\assets\xr\unix"
$source = Join-Path $PSScriptRoot "gamenative_openxr_unix.c"
$ndkRoot = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk\29.0.14206865"
$armClang = Join-Path $ndkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
$armSysroot = Join-Path $ndkRoot "toolchains\llvm\prebuilt\windows-x86_64\sysroot"

if (!(Test-Path $armClang)) {
    throw "Android NDK clang not found at $armClang"
}

$armOutputDir = Join-Path $assetRoot "aarch64"
New-Item -ItemType Directory -Force -Path $armOutputDir | Out-Null
& $armClang --target=aarch64-linux-android29 --sysroot=$armSysroot `
    -std=c11 -O2 -g -fPIC -fvisibility=hidden -shared `
    -Wall -Wextra -Werror `
    -I (Join-Path $ndkRoot "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include") `
    -o (Join-Path $armOutputDir "gamenative_openxr.so") `
    $source -ldl
if ($LASTEXITCODE -ne 0) {
    throw "aarch64 unixlib build failed with exit code $LASTEXITCODE"
}

$wslScript = "/mnt/c/Users/flori/Documents/Coding/gamenative/app/src/main/windows/openxr_bridge/unix/build_unixlib.sh"
& wsl -d Ubuntu -- bash $wslScript
if ($LASTEXITCODE -ne 0) {
    throw "x86_64 unixlib build failed with exit code $LASTEXITCODE"
}
