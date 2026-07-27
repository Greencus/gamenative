# GameNativeVR implementation status

Last updated: 2026-07-24

The revised missing-feature list is implemented in the source tree. Items that
depend on a real Android/Quest Vulkan driver or OpenXR runtime remain explicitly
hardware-pending because the Quest was not connected for this build.

| Area | Implementation | Validation |
|---|---|---|
| Wine unixlib | `__wine_unix_call` companion, x64/x86 PE modules, x86_64/aarch64 Unix modules, Wine 9.2 host-handle adapter, and a version-pinned winevulkan patch | x64/x86 basic calls pass in Wine; a real Windows VkDevice reaches the Unix module in the VM |
| Producer swapchains | Create/destroy/enumerate/acquire/wait/release, triple buffering, array layers, Vulkan images, D3D11 DXVK resources, and D3D12 vkd3d resources | Windows DLLs compile; VM reaches exportable allocation, then llvmpipe skips because it cannot allocate the dma-buf image |
| Transport | AHB plus dma-buf registration, one-to-four modifier planes with per-plane offsets/pitches, FourCC mapping, indexed buffer ring, and per-image EGL import caching | Native synthetic AHB producer rendered projection layers on Quest previously; dma-buf/modifier variants await the connected Quest |
| Synchronization | Producer sync-file acquire fence, compositor EGL native-fence wait, compositor release fence, producer wait before reuse; blocking fallbacks are explicit | Compiles on both sides; 72/90 Hz behavior awaits Quest |
| Frame submission | Projection-layer parsing, layer-space composition, submitted poses/FOVs, two view subimages, array indices, sub-rectangle UV cropping, released-image selection, and per-eye submission | Compile-verified; real application presentation awaits Quest |
| Graphics bindings | Vulkan enable and enable2 device queries/creation, real adapter LUID, D3D11/DXVK native interop, D3D12/vkd3d native interop | Vulkan instance/device creation and D3D11/D3D12 graphics-requirement/LUID smokes pass in Wine; resource interop awaits Quest/game coverage |
| 32-bit runtime | Separate x64/x86 runtime manifests and native/WOW6432 registry views | x86 runtime and OpenComposite load smokes pass in Wine |
| Actions | Attach-once, suggested-profile precedence, active-action-set and subaction filtering | Wine runtime smoke verifies attach-once, profile precedence, and unattached active-set rejection; controller hardware awaits Quest |
| Spaces | Pose offsets, base-space composition in `xrLocateSpace` and `xrLocateViews`, VIEW/LOCAL/STAGE distinction, stage bounds, position/orientation recenter detection, linear/angular velocity | Wine runtime smoke verifies composition/velocity/bounds-unavailable behavior; tracking and real bounds await Quest |
| Lifecycle/events | STOPPING to EXITING flow, loss pending, events lost, interaction-profile changed, reference-space changed | Wine runtime smoke verifies STOPPING and EXITING |
| OpenComposite | Steam and non-Steam executable resolution, per-game x64/x86 replacement, backup/restore, update metadata, atomic verified copies | x64 and x86 DLL load smokes pass in Wine |
| Diagnostics | `app.gamenative.xr.STATUS` broadcast, bridge/Unix logs, in-app status overlay, active swapchain count, submitted-frame count, graphics API, and last command/error | Build-verified; adb query awaits Quest |
| Theater/launch | Flat-game SurfaceTexture theater, Steam VR selection paths, OpenComposite launch wiring | Build-verified; visual/game launch checks await Quest |

## Wine interop decision

Windows applications must not be told to enable Linux-only Vulkan extensions.
GameNative therefore leaves the application extension list unchanged and ships a
Wine 9.2 `winevulkan.so` patch that conditionally enables the host-only dma-buf
and sync-file extensions when `GAMENATIVE_XR=1`.

The stock module is preserved as `winevulkan.so.gamenative-stock` before the
patched module is installed. The patch is deliberately pinned to
`wine-9.2-x86_64`; other Wine versions retain tracking/input support but log that
producer interop is unsupported until a matching Wine build is supplied.

Rebuild and smoke-test it in the Ubuntu VM with:

```powershell
tools\xr\build_wine_vulkan_vm.ps1
tools\xr\test_wine_vulkan_vm.ps1
```

The VM uses llvmpipe. A result stating that host interop passed but dma-buf
allocation was skipped is expected there: the Windows VkDevice, Wine host
handles, Unix call, and FD entry points were validated, while the software
driver cannot provide the exportable image required by the Quest path.

## Final hardware gate

The task is not complete until both rows below pass on a connected Quest:

1. Control plane: head pose, predicted timing, controller actions, interaction
   profile changes, haptics, exit/recenter events, and diagnostics reach a
   Windows OpenXR application.
2. Producer/display plane: Vulkan or DXVK swapchains register as dma-bufs,
   stereo projection layers display correctly, acquire/release fences remain
   tear-free at headset refresh rate, and teardown/relaunch does not leak or
   deadlock.
