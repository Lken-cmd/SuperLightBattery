# SuperLightBattery

A tiny Windows battery indicator for Logitech LIGHTSPEED mice — no G HUB, no service, no telemetry.

SuperLightBattery shows the battery level of a Logitech LIGHTSPEED mouse (PRO X Superlight family and similar HID++ 2.0 devices) in the system tray. Two small executables, statically linked, no background framework. Tested on Windows 11.

## Install

1. Download the latest release zip from the [Releases](https://github.com/Lken-cmd/SuperLightBattery/releases) page.
2. Unzip it anywhere. Keep `SuperLightBattery.exe` and `SuperLightBatteryInstaller.exe` next to each other.
3. Run `SuperLightBatteryInstaller.exe`.
4. Click **Install**. The tray icon appears, and the app starts automatically at every logon from then on.

No admin rights are required. Files go to `%LOCALAPPDATA%\SuperLightBattery` and one value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

## Use

The tray icon shows battery state as a vertical fill on a battery silhouette.

- **Green** above 50 %, **amber** 21–50 %, **red** at 20 % and below.
- A yellow lightning bolt overlay means the mouse reports it is charging.
- An empty grey bar means the percent is unknown (voltage-only firmware, coarse state, or no device).

| Action on the icon | What happens |
| --- | --- |
| Hover | Tooltip with device name, percent, state, and DPI. Triggers a refresh at most once a minute. |
| Right-click | Opens the menu and triggers an immediate refresh. |
| Double-click | Triggers an immediate refresh. |
| Refresh menu item | Same as double-click. |
| Exit menu item | Closes the tray app. The startup entry stays in place, so it will run again at next logon unless you uninstall. |

There is no background polling. The tray sleeps in `GetMessage` until you interact with the icon or a HID device-change notification fires.

## Uninstall

Run the installer again and click **Uninstall**, or:

```powershell
SuperLightBatteryInstaller.exe --uninstall
```

This deletes the files, removes the registry entry, and stops the running tray. If a file is locked because the tray is still active, the installer schedules it for deletion at next reboot.

## CLI

For scripting and one-off checks, `SuperLightBattery.exe` has a small CLI:

```powershell
SuperLightBattery.exe --list-devices   # list candidate Logitech HID interfaces
SuperLightBattery.exe --probe          # test HID++ communication and battery feature
SuperLightBattery.exe --once           # print a single JSON snapshot and exit
SuperLightBattery.exe --tray           # run the tray app (default)
```

`--once` example output:

```json
{
  "battery": {
    "percent": 46,
    "state": "Discharging",
    "charging": false,
    "source": "Unified battery 0x1004"
  },
  "device": {
    "name": "PRO X Wireless",
    "hid_product": "USB Receiver",
    "transport": "receiver",
    "vid": "VID_046D",
    "pid": "PID_C547",
    "hidpp_device_index": "0x01"
  },
  "dpi": {
    "current": 1600,
    "min": 100,
    "max": 25600
  },
  "profile": {
    "index": 1,
    "dpi_slot": 2
  }
}
```

Notes:

- `battery.percent` is firmware-reported when available. Voltage is never converted into a fake percentage.
- `battery.source` names the HID++ feature or register that supplied the value.
- `device.name` is the firmware-reported name; `device.hid_product` is the generic Windows HID product string.
- Unsupported sections come back as `null`.

`--list-devices` and `--probe` deliberately do not print serial numbers, USB instance IDs, HID device paths, captures, or logs.

## Build from source

You need MSVC C++ build tools (Visual Studio 2019 or 2022, x64) and PowerShell.

```powershell
.\build.ps1
```

The build writes:

```text
out\SuperLightBattery.exe
out\SuperLightBatteryInstaller.exe
```

Both binaries are statically linked against the MSVC CRT (`/MT`), come in around 190 KB and 160 KB, and depend only on core Windows DLLs (`kernel32`, `user32`, `gdi32`, `shell32`, `setupapi`, `hid`, `advapi32`, `comctl32`). The icon is regenerated from `assets/generate-icon.ps1` if missing.

## Troubleshooting

**The tray icon shows "unavailable" and the menu has nothing useful.**
Run `SuperLightBattery.exe --probe` from a terminal. It will list which Logitech HID collections are detected and which (if any) answer HID++. Make sure the receiver is plugged in and the mouse is paired and on.

**A console window flashes when I run `SuperLightBattery.exe --tray` directly.**
The tray binary is a console-subsystem app; the autostart entry hides the console by going through `SuperLightBatteryInstaller.exe --launch-tray`, which spawns the tray with `CREATE_NO_WINDOW`. For a console-free manual launch, use the installer or `--launch-tray`.

**Icon looks blurry on a high-DPI display.**
The tray icon renders at the shell's requested pixel size with 4× supersampling and is per-monitor DPI aware. If it still looks soft, open an issue and include your DPI scaling (Settings → System → Display → Scale).

**Installer says "SuperLightBattery.exe must sit next to the installer."**
The two executables need to be in the same folder. The release zip ships them that way already; re-download the full archive if they got separated.

**Tray won't exit during uninstall.**
The installer posts `WM_CLOSE` to the tray window and waits briefly. If the tray was unresponsive at that moment, exit it manually from its right-click menu and run uninstall again. The locked files were already scheduled for deletion at next reboot.

## Releases

Tagged releases (`v*`) trigger a GitHub Actions build on `windows-latest` that zips both binaries plus the README and LICENSE and publishes a GitHub Release.

```powershell
git tag v1.0.0
git push origin v1.0.0
```

## What it doesn't do

- Replace Logitech G HUB. It only reads battery, DPI, and profile state.
- Change DPI, lighting, button mappings, profiles, firmware, or pairing.
- Talk to the network. No telemetry, no auto-update, no analytics.

## Disclaimer

SuperLightBattery is an independent open source project and is not affiliated with, endorsed by, or sponsored by Logitech. "Logitech", "LIGHTSPEED", and "PRO X Superlight" are trademarks of their respective owner and are used here only to describe device compatibility.

## License

SuperLightBattery is licensed under the MIT License. See [LICENSE](LICENSE).
