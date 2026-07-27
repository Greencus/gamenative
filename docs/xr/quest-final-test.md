# GameNativeVR Quest final test

This is the only remaining validation gate after local and VM builds pass.

## Testing from another computer

The Quest does not need to be connected to the build machine. Copy these two
files to any Windows computer that can connect to the headset:

- `app/build/outputs/apk/modernXr/debug/app-modernXr-debug.apk`
- `tools/xr/run_quest_hardware_test.ps1`

Install current Android SDK Platform Tools so `adb` is available, enable
Developer Mode for the headset, connect and authorize it, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\run_quest_hardware_test.ps1 `
  -ApkPath .\app-modernXr-debug.apk
```

The script installs the APK, guides the manual headset portion, and creates a
timestamped `quest-xr-evidence-*.zip`. Return that ZIP for diagnosis and final
sign-off. The other computer does not need this repository, Android Studio,
Wine, or the VM.

If the Quest already supports and exposes Android wireless debugging, the same
script also works after pairing it with `adb pair IP:PAIRING_PORT` and connecting
with `adb connect IP:DEBUG_PORT`. The headset and test computer must be able to
reach each other on the same network. Do not rely on this option if the Quest
does not expose a wireless-debugging pairing screen; use a physical connection
to the other computer instead.

## Install and start

```powershell
adb devices -l
adb install -r app\build\outputs\apk\modernXr\debug\app-modernXr-debug.apk
adb logcat -c
adb logcat GameNativeVR:I AndroidRuntime:E *:S
```

Launch one native OpenXR title first, then one OpenComposite/OpenVR title. Use
the default `wine-9.2-x86_64` container for the producer test because the
winevulkan host patch is version-pinned.

## Observe

The in-headset overlay must show an active bridge, current session state,
tracking/input updates, registered left/right buffers, submitted frames, and no
continually increasing drop/error counter.

From a second terminal:

```powershell
adb shell am broadcast -a app.gamenative.xr.STATUS
adb shell run-as app.gamenative cat files/imagefs/home/xuser/.wine/drive_c/gamenative/xr/bridge.log
```

The exact container prefix can differ; if `run-as` cannot find the log, use the
path shown by GameNative's container diagnostics.

## Pass criteria

- Head rotation and translation are correct in VIEW and LOCAL spaces.
- Recenter raises a reference-space-change event and changes LOCAL, not STAGE.
- STAGE bounds match the Quest guardian/play-area bounds when available.
- Both controllers update pose, buttons, triggers, sticks, and active profiles.
- Haptics fire on the requested hand and stop cleanly.
- `xrRequestExitSession` produces STOPPING, then EXITING after `xrEndSession`.
- Both eyes show the submitted projection pose/FOV, crop, and array layer.
- No eye inversion, stale frames, tearing, or repeated image reuse at 72/90 Hz.
- A flat title renders on the theater quad and exits back to the app.
- Steam VR launch selection and a non-Steam OpenComposite replacement both load.
- Repeating launch/exit three times produces no deadlock or native crash.

If producer creation fails, collect:

```powershell
adb logcat -d -v threadtime > quest-xr-logcat.txt
adb shell am broadcast -a app.gamenative.xr.STATUS > quest-xr-status.txt
```
