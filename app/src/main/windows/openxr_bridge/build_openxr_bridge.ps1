$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..\..")
$ndkRoot = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk\29.0.14206865"
$clang = Join-Path $ndkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
$dlltool = Join-Path $ndkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-dlltool.exe"

if (!(Test-Path $clang)) {
    throw "clang.exe not found at $clang"
}
if (!(Test-Path $dlltool)) {
    throw "llvm-dlltool.exe not found at $dlltool"
}

$source = Join-Path $PSScriptRoot "gamenative_openxr_bridge.c"
$includeDir = Join-Path $repoRoot "app\src\main\cpp\third_party\openxr"
$assetDir = Join-Path $repoRoot "app\src\main\assets\xr\windows"
$buildDir = Join-Path $repoRoot "app\build\openxr_bridge"
New-Item -ItemType Directory -Force -Path $assetDir | Out-Null
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$ws2x64 = Join-Path $buildDir "libws2_32_x64.a"
$ws2x86 = Join-Path $buildDir "libws2_32_x86.a"
$k32x64 = Join-Path $buildDir "libkernel32_x64.a"
$k32x86 = Join-Path $buildDir "libkernel32_x86.a"
$ntx64 = Join-Path $buildDir "libntdll_x64.a"
$ntx86 = Join-Path $buildDir "libntdll_x86.a"
$dxgix64 = Join-Path $buildDir "libdxgi_x64.a"
$dxgix86 = Join-Path $buildDir "libdxgi_x86.a"

function Invoke-CheckedNative {
    param([scriptblock]$Command, [string]$Description)
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

Invoke-CheckedNative { & $dlltool -m i386:x86-64 -d (Join-Path $PSScriptRoot "ws2_32_x64.def") -l $ws2x64 } "x64 ws2 import library"
Invoke-CheckedNative { & $dlltool -m i386 -k -d (Join-Path $PSScriptRoot "ws2_32_x86.def") -l $ws2x86 } "x86 ws2 import library"
Invoke-CheckedNative { & $dlltool -m i386:x86-64 -d (Join-Path $PSScriptRoot "kernel32_x64.def") -l $k32x64 } "x64 kernel32 import library"
Invoke-CheckedNative { & $dlltool -m i386 -k -d (Join-Path $PSScriptRoot "kernel32_x86.def") -l $k32x86 } "x86 kernel32 import library"
Invoke-CheckedNative { & $dlltool -m i386:x86-64 -d (Join-Path $PSScriptRoot "ntdll_x64.def") -l $ntx64 } "x64 ntdll import library"
Invoke-CheckedNative { & $dlltool -m i386 -k -d (Join-Path $PSScriptRoot "ntdll_x86.def") -l $ntx86 } "x86 ntdll import library"
Invoke-CheckedNative { & $dlltool -m i386:x86-64 -d (Join-Path $PSScriptRoot "dxgi_x64.def") -l $dxgix64 } "x64 dxgi import library"
Invoke-CheckedNative { & $dlltool -m i386 -k -d (Join-Path $PSScriptRoot "dxgi_x86.def") -l $dxgix86 } "x86 dxgi import library"

Invoke-CheckedNative { & $clang --target=x86_64-w64-windows-gnu -shared -nostdlib "-Wl,-e,DllMain" `
    -I $includeDir `
    -o (Join-Path $assetDir "gamenative_openxr64.dll") `
    $source `
    (Join-Path $PSScriptRoot "gamenative_openxr_bridge_x64.def") `
    $ws2x64 `
    $k32x64 `
    $ntx64 `
    $dxgix64 } "x64 OpenXR bridge"

Invoke-CheckedNative { & $clang --target=i686-w64-windows-gnu -shared -nostdlib "-Wl,-e,DllMain" `
    -I $includeDir `
    -o (Join-Path $assetDir "gamenative_openxr32.dll") `
    $source `
    (Join-Path $PSScriptRoot "gamenative_openxr_bridge_x86.def") `
    $ws2x86 `
    $k32x86 `
    $ntx86 `
    $dxgix86 } "x86 OpenXR bridge"
