# GameNativeVR — True Stereo OpenXR Bridge Design

Status: **Design / pre-implementation.** This document is the agreed starting point for
replacing the TCP-stubbed OpenXR bridge with a real, GPU-buffer-sharing pipeline that can
present true per-eye stereo from a Windows OpenXR game running under Wine/Box64 on a
Meta Quest. Nothing here is validated on hardware yet; every milestone below has an explicit
"how we know it works" gate, and most require a Quest to close.

---

## 1. Where we are today

| Layer | File(s) | State |
|---|---|---|
| PE OpenXR runtime (Win32, inside Wine) | `app/src/main/windows/openxr_bridge/gamenative_openxr_bridge.c` | Implements the OpenXR API surface a Windows game links against. Talks to Android over **TCP** (`127.0.0.1:38476`). Returns poses + frame timing. **`xrCreateSwapchain` returns `XR_ERROR_FEATURE_UNSUPPORTED`.** |
| Control server (Android) | `app/src/main/java/app/gamenative/xr/XrBridgeServer.kt` | TCP line protocol. `CREATE_SWAPCHAIN` → `ERR unsupported_swapchain_transport`. |
| Quest compositor (Android, native) | `app/src/main/cpp/xr_runtime/quest_xr.cpp` | Full, working native OpenXR app (GLES) on the **real** Quest runtime. Today it only composites a `SurfaceTexture` "theater screen" as a quad layer. |
| Runtime wiring | `app/src/main/java/app/gamenative/xr/XrRuntimeManager.kt` | Writes the OpenXR runtime JSON + Wine registry, exports env vars. Already exports an **unused** `GAMENATIVE_XR_SOCKET=/tmp/gamenative-xr.sock`. |
| Game Vulkan backend | `libvortekrenderer.so` (prebuilt) | Vulkan wrapper ICD. Forwards the game's Vulkan over a unix socket to a host `VkDevice` it owns. **No source in this repo.** |
| Buffer transport that already works | `com/winlator/xserver/extensions/DRI3Extension.java`, `app/src/main/cpp/extras/gpu_image.c` | The game's rendered frames cross the process boundary as real **`AHardwareBuffer` handles over a unix socket** (`AHardwareBuffer_recvHandleFromUnixSocket`). |
| Compositor-side AHB import (source) | `app/src/main/cpp/winlator/VulkanRendererContext.cpp` (enables `VK_ANDROID_external_memory_android_hardware_buffer`), `VulkanRendererScanout.cpp` | Source-available Vulkan device that imports AHBs for scanout. This is the model for how `quest_xr` will import eye buffers. |

### Root cause (confirmed)

1. **TCP cannot carry GPU handles.** `AHardwareBuffer`/dma-buf sharing requires `SCM_RIGHTS` / `AHardwareBuffer_sendHandleToUnixSocket` over an `AF_UNIX` socket. The bridge uses TCP.
2. **The PE DLL is Win32.** It has no `AHardwareBuffer`, no `AF_UNIX`, no `SCM_RIGHTS`. No change *inside the PE DLL* can ever produce a shareable buffer. The allocation must happen on the **native (Linux/ELF, in-container) side of Wine** — this is exactly Proton's PE-DLL + unixlib split.
3. **The game's GPU images live inside Vortek's host `VkDevice`**, and Vortek is a closed blob. We cannot ask it to allocate exportable XR swapchain images or to hand their handles to a third party.

Therefore true stereo requires (a) a native unix-side XR component, and (b) a **source-available Vulkan path** in which eye images can be allocated as exportable `AHardwareBuffer`/dma-buf.

---

## 2. Target architecture

```
 ┌─────────────────────────── Wine / Box64 (in container) ───────────────────────────┐
 │                                                                                    │
 │  Windows OpenXR game                                                               │
 │      │ xrCreateSwapchain / xrAcquire / xrEndFrame                                  │
 │      ▼                                                                             │
 │  gamenative_openxr64.dll  (PE, thin)                                               │
 │      │ __wine_unix_call  (unixlib thunk — NOT a socket)                            │
 │      ▼                                                                             │
 │  gamenative_openxr.so     (NEW unix-side component, ELF)                           │
 │      │  - owns/shares the game's native VkDevice (winevulkan native interop)       │
 │      │  - allocates per-eye VkImages backed by AHardwareBuffer (exportable)        │
 │      │  - the game renders each eye directly into these images (zero-copy)         │
 │      ▼                                                                             │
 │  AF_UNIX socket  /tmp/gamenative-xr.sock                                           │
 │      │  control plane: pose/timing/lifecycle (replaces TCP)                        │
 │      │  data plane:    AHardwareBuffer_sendHandleToUnixSocket(eyeBuffer)           │
 └──────┼─────────────────────────────────────────────────────────────────────────────┘
        │
 ┌──────▼──────────────────────── Android app process ───────────────────────────────┐
 │  XrBridgeServer (AF_UNIX)  ──►  QuestVrActivity / quest_xr.cpp                      │
 │      - recvHandleFromUnixSocket → AHardwareBuffer per eye                           │
 │      - import as GL texture (EGLImage) or Vk image                                  │
 │      - blit into the REAL Quest projection-layer swapchain images, per eye          │
 │      - xrEndFrame with XrCompositionLayerProjection (2 views)                       │
 └────────────────────────────────────────────────────────────────────────────────────┘
```

Key properties:
- **Control plane and data plane both move to `/tmp/gamenative-xr.sock`** (already stubbed in `XrRuntimeManager`). TCP (`XrBridgeServer`) is retired.
- The PE↔unix boundary uses the **Wine unixlib mechanism** (`__wine_unix_call`), not a socket — this is how the PE side reaches native Vulkan/`AHardwareBuffer` APIs at all.
- Eye images are **allocated once, reused per frame**, and the game renders into them directly — no per-frame GPU copy on the Wine side.

---

## 3. The Vortek-replacement decision (the load-bearing unknown)

True stereo needs the game's eye `VkImage`s to be **exportable as `AHardwareBuffer`/dma-buf**. Three candidate ways to get there, in order of realism:

### Option A — Turnip (in-container Mesa Adreno) + dma-buf export  ✅ recommended
- The game runs on Turnip *inside* the container (already a supported Winlator graphics driver). Turnip talks directly to the kernel DRM and can allocate images with `VK_EXT_external_memory_dma_buf` / `VK_KHR_external_memory_fd`.
- The same DRI3 path (`pixmapFromFd` / `pixmapFromHardwareBuffer`) already proves dma-buf/AHB fds from the in-container GPU stack reach the Android side and import correctly.
- The new unix-side `gamenative_openxr.so` shares the game's Vulkan device and allocates eye images with dma-buf export, then converts/passes them to `quest_xr`.
- **Risk:** requires the game session to use Turnip, not Vortek, when XR is enabled. Needs a container-config switch and validation that DXVK→Turnip is stable for the target titles.

### Option B — Extend the source-available `vulkan_renderer` device into the game ICD
- Unlikely to be practical: `vulkan_renderer` is a compositor/scanout device, not a Wine-facing ICD. Reusing it as the game's Vulkan backend is a much larger rewrite than Option A. **Not pursued unless A is blocked.**

### Option C — Reverse-engineer / replace the Vortek wire protocol with our own server
- Build a source server that speaks Vortek's client protocol and exposes AHB export. High effort, brittle against the prebuilt client. **Last resort.**

**Decision for this design: pursue Option A.** Milestones below assume Turnip + dma-buf/AHB export. If hardware testing in M3 shows Turnip can't export usable buffers for the in-use driver, we fall back to re-evaluating B/C.

> Open question to resolve before M2: does this project's Wine build expose **winevulkan native-handle interop** (the ability for the unixlib to obtain the native `VkDevice`/`VkImage`/`VkInstance` behind the PE handles)? Proton ships patches for this. If Winlator's Wine does **not**, the unix side must create its **own** `VkDevice` on the same physical device and the game must render into images it then imports — adding a device-to-device share step. This is the single biggest schedule risk.

---

## 4. Wire protocol (`/tmp/gamenative-xr.sock`)

ASCII control lines (reuse today's `XrBridgeServer` grammar) **plus** binary frames for buffer handoff.

Control (newline-delimited, unchanged semantics):
```
HELLO                          -> OK GameNativeVR 2
GET_SYSTEM / GET_VIEWS         -> as today
BEGIN_SESSION / END_SESSION    -> OK
WAIT_FRAME                     -> OK time= period= render=
LOCATE_VIEWS                   -> OK flags= + per-eye pose/fov (extend to full quat+pos+fov per eye)
BEGIN_FRAME / END_FRAME        -> OK
```

Data plane (implemented in `xr_transport.cpp`). The consumer accepts **two** buffer kinds; a
producer picks one per eye and shares the handle **once**, then emits a tiny `FRAME` line per
presented frame:

```
# AHardwareBuffer kind (used by the in-process test producer; bionic-side allocators):
BUFFER eye=<0|1> index=<i> w=<px> h=<px>
    -> OK                            ; consumer is ready
    <AHardwareBuffer handle>         ; AHardwareBuffer_sendHandleToUnixSocket on the SAME fd
    -> OK stored

# dma-buf kind (real game path; Turnip/glibc side exports a single-plane dma-buf):
DMABUF eye=<0|1> index=<i> w=<px> h=<px> fourcc=<drm> stride=<bytes> offset=<bytes> modifier=<u64>
    -> OK                            ; consumer is ready
    <1 data byte + dma-buf fd>       ; sent as SCM_RIGHTS ancillary data on the SAME fd
    -> OK stored
    # fourcc/modifier may be hex (0x...) or decimal; modifier=0 means LINEAR.
    # consumer imports via eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, ...) (single plane for now).

# Per presented frame (both kinds):
FRAME eye=<0|1> index=<i>            -> OK
```
- Handles are transferred **once**; per-frame is only a `FRAME` line + (future) sync fence (§6).
- Multi-planar / multi-fd dma-bufs are not yet supported (single plane covers RGBA/BGRA color).

---

## 5. Component-by-component work

1. **`gamenative_openxr_bridge.c` (PE, thin)** — replace socket/`winsock` code with `__wine_unix_call` thunks to the unixlib. Implement real `xrCreateSwapchain`, `xrEnumerateSwapchainImages`, `xrAcquire/Wait/ReleaseSwapchainImage`, and wire `xrEndFrame` to `SUBMIT_FRAME`. `xrEnumerateSwapchainImages` returns the `VkImage` handles the unixlib allocated.
2. **`gamenative_openxr.so` (NEW, `app/src/main/windows/openxr_bridge/unix/`)** — the unixlib. Owns the AF_UNIX connection, allocates AHB-backed eye `VkImage`s on the game's Vulkan device (Option A), sends handles to Android, drives frame submit, returns native handles up to the PE side. Built by the Wine/Proton-style build; see `build_openxr_bridge.ps1`.
3. **`XrBridgeServer.kt`** — convert `ServerSocket`(TCP) → `LocalServerSocket`/`AF_UNIX` at `/tmp/gamenative-xr.sock`; add binary handle-receive (`AHardwareBuffer_recvHandleFromUnixSocket` via a small JNI helper, mirroring `gpu_image.c`). Maintain a registry of per-eye imported `AHardwareBuffer`s.
4. **`quest_xr.cpp`** — add a real projection-layer path: for each eye, import the shared `AHardwareBuffer` → `EGLImageKHR` → GL texture, blit into the eye's real Quest swapchain image, submit `XrCompositionLayerProjection` with 2 views (the eye-swapchain machinery already exists in `createEyeSwapchains`/`renderEye`). Replace the placeholder `drawTheaterScreen` with a textured blit of the imported eye buffer.
5. **`XrRuntimeManager.kt`** — drop `GAMENATIVE_XR_BRIDGE_HOST/PORT`; keep/centralize `GAMENATIVE_XR_SOCKET`. Force the Turnip graphics driver for the container when XR is enabled (Option A).
6. **JNI glue** — small native helper for `AHardwareBuffer_sendHandleToUnixSocket`/`recvHandleFromUnixSocket` on the Kotlin side (reuse the pattern in `gpu_image.c`).

---

## 6. Synchronization (don't skip)

Cross-process GPU sharing without sync = tearing/corruption. Plan:
- Each eye image carries an **acquire/release fence** (`VK_KHR_external_fence_fd` / `EGL_ANDROID_native_fence_sync`).
- Game side signals a fence when it finishes rendering an eye; the fd travels with `SUBMIT_FRAME`. `quest_xr` waits on it before sampling.
- `quest_xr` signals release; the unix side waits before letting the game re-acquire that index.
- Use ≥2 images per eye (double buffer) to avoid stalls.

---

## 7. Milestones & validation gates

| M | Deliverable | Validation gate |
|---|---|---|
| **M0** | This doc + protocol v2 frozen | Review sign-off |
| **M1** | AF_UNIX transport: retire TCP; `XrBridgeServer` + PE thunks exchange control lines over `/tmp/gamenative-xr.sock`; round-trip an `AHardwareBuffer` handle end-to-end (alloc dummy AHB unix-side → send → import in `XrBridgeServer` → describe). | Logcat shows a non-null imported `AHardwareBuffer` with expected w/h/format. **No headset needed.** |

### Implementation status (M1, consumer side — landed)

The **Quest-compositor / consumer half** of the zero-copy transport is implemented and wired
into the build (`externalNativeBuild` → `app/src/main/cpp/xr_runtime/CMakeLists.txt`):

- `app/src/main/cpp/xr_runtime/xr_transport.{h,cpp}` — AF_UNIX listener on
  `GAMENATIVE_XR_SOCKET` (default `/tmp/gamenative-xr.sock`). Receives per-eye
  `AHardwareBuffer` handles via `AHardwareBuffer_recvHandleFromUnixSocket` (handle shared
  **once** at `BUFFER`; per frame only a tiny `FRAME eye= index=` line). Thread-safe latest-frame
  slots; no GL on this thread.
- `quest_xr.cpp` — imports each eye buffer to a `GL_TEXTURE_2D` via `eglCreateImageKHR`
  (reusing the EGLImage across frames when the buffer is unchanged → steady-state cost is one
  texture bind, **zero copy**), blits into the real Quest eye swapchains, and submits an
  `XrCompositionLayerProjection`. Falls back to the existing theater quad when no producer is
  connected, so this is **non-regressing** until the producer lands.

**Test producer (landed) — validates the path without Wine:**
`app/src/main/cpp/xr_runtime/xr_test_producer.{h,cpp}` allocates two `AHardwareBuffer`s, writes a
per-eye test pattern (reddish left / bluish blue right, vertical gradient to expose flips), and
drives the exact wire protocol the real producer will use (`HELLO` → `BUFFER` + handle per eye →
`FRAME` per eye at ~72 Hz). It runs in-process against `quest_xr`'s own socket (AF_UNIX loopback).

Whole `gamenative_xr` target (consumer + compositor + test producer) **compiles and links** with
NDK 27.3.13750724 / CMake 3.22.1 for arm64-v8a (verified).

On-device smoke test (no Wine needed):
```
# build & install the modernXr flavor, launch the GameNativeVR activity, then:
adb shell setprop debug.gamenative.xr.testproducer 1
# (or set env GAMENATIVE_XR_TEST_PRODUCER=1 in the app process)
```
**Gate:** a reddish gradient in the left eye and a bluish gradient in the right eye, head-tracked,
proves transport → import → per-eye projection layer end-to-end.

> **✅ M1 GATE PASSED on Quest hardware (2026-07-23).** The per-eye test pattern rendered in the
> headset. This validates, on real hardware: the abstract-namespace AF_UNIX transport, the
> `AHardwareBuffer` handle handoff, `eglCreateImageKHR` import, the per-eye blit into the real Quest
> eye swapchains, and `XrCompositionLayerProjection` submission. The **compositor/consumer half of
> M3 is therefore already proven** — when the M2 producer lands, it plugs into a validated sink.
> Still unproven on hardware: the **dma-buf** variant of the import path (the test producer uses
> AHardwareBuffer; the real game path will use dma-buf), and fence-based sync (§6), which does not
> exist yet.
>
> Note: the test producer is opt-in as of the same date
> (`adb shell setprop debug.gamenative.xr.testproducer 1`), because while connected it outranks the
> theater quad and would hide flat games.

**dma-buf import path (consumer side — landed).** `xr_transport` now also accepts the `DMABUF`
message: it receives a single-plane dma-buf fd via `SCM_RIGHTS` and `quest_xr` imports it through
`eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, …)` with the DRM FourCC/stride/offset/modifier. This is
path (a) below, so the real producer can hand over Turnip's dma-bufs directly with no copy. The whole
target compiles clean (no warnings) for arm64. Validated by compilation; on-device exercise needs a
producer that emits real dma-bufs.

**Still required for the real game pipeline / M2+:**
1. **Game-connected producer** — the test fill must be replaced by the game's actual GPU output.
   The transport now supports **both** kinds (AHB and dma-buf), so the producer can use path (a):
   pass the game's **dma-buf fd** (already how the DRI3 path moves game buffers). Multi-plane dma-bufs
   and an explicit format→FourCC mapping still need adding when wiring real swapchain formats.
2. **winevulkan native interop** (design §8 risk #1) — whether the unix side can share the game's
   `VkDevice` vs. re-create one. Gates how eye images are sourced.
3. **Sync (design §6)** — fences not yet exchanged; currently relies on `glFlush`. Add acquire/release
   fence fds before M4.
4. **TCP retirement** — `XrBridgeServer` (control plane) is intentionally left on TCP for now to avoid
   regressing pose/timing; unify onto the AF_UNIX socket when the producer exists.
### Implementation status (M5 pulled forward — control plane v2, landed 2026-07)

Tracking, timing, session state, controller input, haptics and diagnostics do **not**
depend on the GPU swapchain transport, so they were implemented first over the existing
TCP control plane (they will move to the AF_UNIX socket together with the data plane):

- **Protocol v2** (`XrBridgeServer.kt` ↔ `gamenative_openxr_bridge.c`). Floats travel as
  fixed-point micro-units (`value * 1e6`, signed decimal) so the CRT-less PE DLL only
  parses integers. New/extended commands:
  - `GET_VIEWS` → real recommended per-eye size (from the Quest runtime's config views).
  - `WAIT_FRAME` → `time= period= render= state=` (Quest-side `XrSessionState`; the DLL
    forwards STOPPING to the game).
  - `LOCATE_VIEWS` → full per-eye quaternion + position + 4-angle FOV
    (`lqx…lfd rqx…rfd`), fed from `xrLocateViews` on the real Quest runtime each frame.
  - `GET_INPUT hand=<0|1>` → `active buttons tr sq sx sy` + grip and aim poses. Backed by
    a real Quest Touch action set in `quest_xr.cpp` (trigger, squeeze, thumbstick + click,
    A/B/X/Y, menu, grip/aim spaces), synced per frame.
  - `HAPTIC hand= amp= dur= freq=` → routed to `xrApplyHapticFeedback` on the Quest.
  - `STATUS` → one-line diagnostics (connected, session state, gfx binding, swapchain
    request count, last command/error).
- **PE runtime hardening** for OpenComposite/real games: real path table with round-trip
  `xrStringToPath`/`xrPathToString`, real action/action-set/space handle tables, suggested
  bindings are parsed to map each action to a hand + Touch component, `xrLocateSpace`
  returns real grip/aim/head poses, action state getters honor subaction paths and report
  `changedSinceLastSync`/`lastChangeTime`, and a spinlock serializes multi-threaded access
  to the shared socket.
- **Diagnostics** (§8 of the finish-line plan): the DLL logs negotiation, instance
  creation + requested extensions, graphics binding choice, swapchain attempts, session
  transitions and every unsupported-function request via `OutputDebugStringA` and — when
  `GAMENATIVE_XR_LOG=1` (now exported by `XrRuntimeManager`) — appends to
  `C:\gamenative\xr\bridge.log` inside the container. Android side:
`adb logcat -s GameNativeVR XrBridgeServer` shows listener state, client
  connect/disconnect, gfx binding, swapchain rejections and unknown commands.

Verified by compilation: PE DLLs (x64 + x86, NDK clang) and the `gamenative_xr`
arm64 target + Kotlin (modernXr flavor) all build clean. On-device validation (controller
input reaching an OpenXR sample under Wine) still needs a Quest.

| **M2** | Unix-side eye-image allocation (Option A): `gamenative_openxr.so` allocates 2× dma-buf/AHB eye images on the game's Vulkan device and exports them. PE `xrCreateSwapchain`/`xrEnumerateSwapchainImages` return real `VkImage`s. | A trivial OpenXR test app (or `hello_xr`) under Wine creates swapchains and clears them to a solid color without error. **No headset needed for API success; needs container.** |
| **M3** | `quest_xr` imports eye buffers and presents a projection layer. | **On Quest:** the cleared color from M2 appears per-eye in the headset, head-tracked. First true end-to-end frame. |
| **M4** | Real game: per-eye render + fence sync + double buffering. | **On Quest:** a real OpenXR title renders stereo, no tearing, stable frame timing. |
| **M5** | Input (controllers/haptics) over the same socket; polish. | **On Quest:** controller poses/buttons reach the game. |

---

## 8. Risks / open questions

1. **winevulkan native interop** in this project's Wine build (see §3 open question) — gates whether the unix side shares vs. re-creates the device. **Resolve first in M2.**
2. **Turnip viability** for target titles via DXVK — gates Option A.
3. **Format negotiation** — the game requests D3D/Vulkan formats; we must map to an AHB format both Turnip and the Quest runtime accept (start with `R8G8B8A8_UNORM` / `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM`).
4. **Foveation / resolution** — out of scope until M4+.
5. **Hardware dependency** — M3+ cannot be validated in CI or this environment; needs a Quest.

---

## 9. Out of scope (for now)
- D3D11/D3D12 swapchains (route via DXVK→Vulkan; the runtime advertises Vulkan to OpenComposite).
- Passthrough/AR blend modes, hand tracking, dynamic foveation.
