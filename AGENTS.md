# Repository agent instructions

## Completion notifications

When a user task is fully completed, send exactly one short Hark notification before the final response:

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Users\flori\Documents\Coding\PushMe\notify-hark.ps1" -Body "<short completion summary>"
```

Do not send notifications for intermediate updates, partial progress, or blocked tasks.

## APK delivery

After every successful local APK build from this repository, send the resulting APK to the Tailscale device `lily` using Tailscale Taildrop:

```powershell
& "C:\Program Files\Tailscale\tailscale.exe" file cp "<absolute-apk-path>" "lily:"
```

If the transfer cannot be completed, report that explicitly instead of silently skipping it. This is a file transfer only; do not use ADB or attempt remote installation.
