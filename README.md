# SuperLightBattery

A tiny Windows battery indicator for the Logitech PRO X Superlight, no G HUB, no service, no telemetry.

SuperLightBattery shows the battery level of an original Logitech PRO X Superlight in the system tray. Small native binaries, statically linked, no background framework. Tested on Windows 11.

## Compatibility

Tested with the original Logitech PRO X Superlight.

Other Logitech LIGHTSPEED / HID++ mice, including the PRO X Superlight 2, may or may not work. Reports are welcome.

## Install

1. Download the latest release zip from the [Releases](https://github.com/Lken-cmd/SuperLightBattery/releases) page.
2. Unzip it anywhere. Keep `SuperLightBattery.exe`, `SuperLightBatteryLauncher.exe`, and `SuperLightBatteryInstaller.exe` next to each other.
3. Run `SuperLightBatteryInstaller.exe`.
4. Click **Install**. The tray icon appears, and the app starts automatically at every logon from then on.

No admin rights are required. Files go to `%LOCALAPPDATA%\SuperLightBattery` and a `SuperLightBattery` shortcut is added to the current user's Startup folder.

## Use

The tray icon shows battery state as a vertical fill on a battery silhouette.

- **Green** above 50 %, **amber** 21–50 %, **red** at 20 % and below.
- A black lightning bolt overlay means the mouse reports it is charging.
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

This deletes the files, removes the startup shortcut and stops the running tray.

## CLI

For scripting and one-off checks, `SuperLightBattery.exe` has a small CLI:

```powershell
SuperLightBattery.exe --list-devices   # list candidate Logitech HID interfaces
SuperLightBattery.exe --probe          # test HID++ communication and battery feature
SuperLightBattery.exe --once           # print a single JSON snapshot and exit
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

- `battery.percent` is firmware-reported.
- `battery.source` names the HID++ feature or register that supplied the value.
- `device.name` is the firmware-reported name; `device.hid_product` is the generic Windows HID product string.
- Unsupported sections come back as `null`.

## Build from source

You need MSVC C++ build tools (Visual Studio 2019 or 2022, x64) and PowerShell.

```powershell
.\build.ps1
```

The build writes:

```text
out\SuperLightBattery.exe
out\SuperLightBatteryLauncher.exe
out\SuperLightBatteryInstaller.exe
```

The app and installer are statically linked against the MSVC CRT (`/MT`); the small startup launcher is CRT-free. The binaries depend only on core Windows DLLs (`kernel32`, `user32`, `gdi32`, `shell32`, `setupapi`, `hid`, `advapi32`, `comctl32`, `ole32`). The icon is regenerated from `assets/generate-icon.ps1` if missing.

## Releases

Tagged releases (`v*`) trigger a GitHub Actions build on `windows-latest` that zips the binaries plus the README and LICENSE and publishes a GitHub Release.

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
