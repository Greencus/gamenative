$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..\..\..")
$ndkRoot = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk\29.0.14206865"
$clang = Join-Path $ndkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
$buildDir = Join-Path $repoRoot "app\build\openxr_bridge"
$cacheDir = Join-Path $buildDir "opencomposite"
$probe64 = Join-Path $buildDir "opencomposite_load_smoke64.exe"
$probe32 = Join-Path $buildDir "opencomposite_load_smoke32.exe"
$dll64 = Join-Path $cacheDir "openvr_api_x64.dll"
$dll32 = Join-Path $cacheDir "openvr_api_x86.dll"

New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null

& $clang --target=x86_64-w64-windows-gnu -nostdlib "-Wl,-e,mainCRTStartup" `
    -o $probe64 `
    (Join-Path $PSScriptRoot "opencomposite_load_smoke.c") `
    (Join-Path $buildDir "libkernel32_x64.a")
if ($LASTEXITCODE -ne 0) { throw "x64 OpenComposite probe build failed" }

& $clang --target=i686-w64-windows-gnu -nostdlib "-Wl,-e,mainCRTStartup" `
    -o $probe32 `
    (Join-Path $PSScriptRoot "opencomposite_load_smoke.c") `
    (Join-Path $buildDir "libkernel32_x86.a")
if ($LASTEXITCODE -ne 0) { throw "x86 OpenComposite probe build failed" }

if (!(Test-Path $dll64)) {
    Invoke-WebRequest `
        -Uri "https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr" `
        -OutFile $dll64
}
if (!(Test-Path $dll32)) {
    Invoke-WebRequest `
        -Uri "https://znix.xyz/OpenComposite/download.php?arch=x86&branch=openxr" `
        -OutFile $dll32
}

$wineRoot = "/home/flori/projects/gamenative-wine-toolchain/root/usr/lib/x86_64-linux-gnu/wine"
$wineRunner = "$wineRoot/wine"
& wsl -d Ubuntu -- cp "$wineRoot/x86_64-windows/zlib1.dll" `
    "/home/flori/projects/gamenative-wine-smoke-prefix9/drive_c/windows/system32/zlib1.dll"
if ($LASTEXITCODE -ne 0) { throw "Could not stage x64 zlib1.dll in Wine smoke prefix" }
& wsl -d Ubuntu -- cp "$wineRoot/i386-windows/zlib1.dll" `
    "/home/flori/projects/gamenative-wine-smoke-prefix9/drive_c/windows/syswow64/zlib1.dll"
if ($LASTEXITCODE -ne 0) { throw "Could not stage x86 zlib1.dll in Wine smoke prefix" }
$commonEnvironment = @(
    "WINEPREFIX=/home/flori/projects/gamenative-wine-smoke-prefix9",
    "WINEDEBUG=-all",
    "WINEDLLPATH=$wineRoot/x86_64-unix`:$wineRoot/x86_64-windows`:$wineRoot/i386-windows",
    "LD_LIBRARY_PATH=$wineRoot/x86_64-unix"
)
$cases = @(
    @{
        Probe = "/mnt/c/Users/flori/Documents/Coding/gamenative/app/build/openxr_bridge/opencomposite_load_smoke64.exe"
        Dll = "Z:/mnt/c/Users/flori/Documents/Coding/gamenative/app/build/openxr_bridge/opencomposite/openvr_api_x64.dll"
    },
    @{
        Probe = "/mnt/c/Users/flori/Documents/Coding/gamenative/app/build/openxr_bridge/opencomposite_load_smoke32.exe"
        Dll = "Z:/mnt/c/Users/flori/Documents/Coding/gamenative/app/build/openxr_bridge/opencomposite/openvr_api_x86.dll"
    }
)

foreach ($case in $cases) {
    & wsl -d Ubuntu -- env `
        $commonEnvironment `
        "GAMENATIVE_OPENCOMPOSITE_PROBE=$($case.Dll)" `
        $wineRunner $case.Probe
    if ($LASTEXITCODE -ne 0) {
        throw "OpenComposite load verification failed for $($case.Dll) with exit code $LASTEXITCODE"
    }
}

Write-Host "OpenComposite load verification passed (x64 and x86)"
