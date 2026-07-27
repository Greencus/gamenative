param(
    [Parameter(Mandatory = $true)]
    [string]$ApkPath,

    [string]$OutputDirectory = (Join-Path $PWD ("quest-xr-evidence-" + (Get-Date -Format "yyyyMMdd-HHmmss"))),

    [switch]$SkipInstall
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$adbCommand = Get-Command adb -ErrorAction Stop
$adb = $adbCommand.Source
$resolvedApk = (Resolve-Path -LiteralPath $ApkPath).Path

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$resolvedOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path

function Invoke-AdbCapture {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$OutputFile,

        [switch]$AllowFailure
    )

    $lines = & $adb @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $lines | Out-File -LiteralPath (Join-Path $resolvedOutput $OutputFile) -Encoding utf8

    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "adb $($Arguments -join ' ') failed with exit code $exitCode. See $OutputFile."
    }

    return $lines
}

$deviceOutput = & $adb devices -l 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "adb devices failed: $($deviceOutput -join [Environment]::NewLine)"
}
$deviceOutput | Out-File -LiteralPath (Join-Path $resolvedOutput "devices.txt") -Encoding utf8

$connected = @(
    $deviceOutput |
        Where-Object { $_ -match "^(\S+)\s+device(?:\s|$)" } |
        ForEach-Object { $Matches[1] }
)
if ($connected.Count -ne 1) {
    throw "Expected exactly one authorized Quest, found $($connected.Count). Check the headset prompt and run 'adb devices -l'."
}

$serial = $connected[0]
$target = @("-s", $serial)

if (-not $SkipInstall) {
    Write-Host "Installing $resolvedApk on $serial ..."
    Invoke-AdbCapture -Arguments ($target + @("install", "-r", $resolvedApk)) -OutputFile "install.txt" | Write-Host
}

Invoke-AdbCapture -Arguments ($target + @("shell", "getprop")) -OutputFile "device-properties.txt" | Out-Null
Invoke-AdbCapture -Arguments ($target + @("shell", "dumpsys", "package", "app.gamenative")) -OutputFile "package.txt" -AllowFailure | Out-Null
Invoke-AdbCapture -Arguments ($target + @("logcat", "-c")) -OutputFile "logcat-clear.txt" | Out-Null

Write-Host ""
Write-Host "The Quest is ready for the GameNativeVR test."
Write-Host "1. Launch GameNative and a native OpenXR title."
Write-Host "2. Exercise head tracking, both controllers, recenter, haptics, and session exit."
Write-Host "3. Launch an OpenComposite/OpenVR title with the wine-9.2-x86_64 container."
Write-Host "4. Check both eyes at 72/90 Hz, then run a flat title in theater mode."
Write-Host "5. Repeat launch and exit three times."
Write-Host ""
Read-Host "Press Enter after the test (or immediately after a failure) to collect evidence"

Invoke-AdbCapture -Arguments ($target + @("shell", "am", "broadcast", "-a", "app.gamenative.xr.STATUS")) -OutputFile "status-broadcast.txt" -AllowFailure | Out-Null
Start-Sleep -Seconds 1
Invoke-AdbCapture -Arguments ($target + @("shell", "run-as", "app.gamenative", "cat", "files/imagefs/home/xuser/.wine/drive_c/gamenative/xr/bridge.log")) -OutputFile "bridge.log" -AllowFailure | Out-Null
Invoke-AdbCapture -Arguments ($target + @("logcat", "-d", "-v", "threadtime")) -OutputFile "logcat.txt" -AllowFailure | Out-Null

$filteredLog = Get-Content -LiteralPath (Join-Path $resolvedOutput "logcat.txt") |
    Select-String -Pattern "GameNativeVR|AndroidRuntime|FATAL EXCEPTION|libc.*Fatal signal"
$filteredLog | ForEach-Object { $_.Line } |
    Out-File -LiteralPath (Join-Path $resolvedOutput "xr-logcat.txt") -Encoding utf8

Copy-Item -LiteralPath (Join-Path $PSScriptRoot "..\..\docs\xr\quest-final-test.md") `
    -Destination (Join-Path $resolvedOutput "quest-final-test.md") -ErrorAction SilentlyContinue

$zipPath = "$resolvedOutput.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $resolvedOutput "*") -DestinationPath $zipPath -CompressionLevel Optimal

Write-Host ""
Write-Host "Evidence collected: $zipPath"
Write-Host "Return that ZIP together with any visible headset symptoms."
