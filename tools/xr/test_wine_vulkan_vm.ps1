param(
    [string]$Prefix = $env:GN_WINE_XR_PREFIX
)

$ErrorActionPreference = "Stop"

$script = "/mnt/c/Users/flori/Documents/Coding/gamenative/tools/xr/test_wine_vulkan_vm.sh"
if ($Prefix) {
    & wsl.exe -d Ubuntu -- env "GN_WINE_XR_PREFIX=$Prefix" bash $script
} else {
    & wsl.exe -d Ubuntu -- bash $script
}
if ($LASTEXITCODE -ne 0) {
    throw "Wine Vulkan VM smoke test failed with exit code $LASTEXITCODE"
}
