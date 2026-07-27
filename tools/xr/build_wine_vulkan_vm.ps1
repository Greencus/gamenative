$ErrorActionPreference = "Stop"

$script = "/mnt/c/Users/flori/Documents/Coding/gamenative/tools/xr/build_wine_vulkan_vm.sh"
& wsl.exe -d Ubuntu -- bash $script
if ($LASTEXITCODE -ne 0) {
    throw "Wine Vulkan VM build failed with exit code $LASTEXITCODE"
}
