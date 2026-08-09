param(
    [Parameter(Mandatory = $true)]
    [string] $Path
)

$ErrorActionPreference = "Stop"

$resolved = Resolve-Path -LiteralPath $Path
$bytes = [IO.File]::ReadAllBytes($resolved)
if ($bytes.Length -lt 0x60 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
    throw "Not a valid PE image: $resolved"
}

$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ($peOffset -lt 0x60 -or $peOffset + 4 -gt $bytes.Length -or
    $bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45) {
    throw "Invalid PE header offset in: $resolved"
}

$signature = [Text.Encoding]::ASCII.GetBytes("Wine builtin DLL`0")
[Array]::Copy($signature, 0, $bytes, 0x40, $signature.Length)
[Array]::Clear($bytes, 0x40 + $signature.Length, 32 - $signature.Length)
[IO.File]::WriteAllBytes($resolved, $bytes)

$written = [Text.Encoding]::ASCII.GetString($bytes, 0x40, $signature.Length)
if ($written -ne "Wine builtin DLL`0") {
    throw "Failed to stamp Wine builtin signature in: $resolved"
}

Write-Output "Stamped Wine builtin PE signature: $resolved"
