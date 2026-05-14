# SuperLightBattery

SuperLightBattery is a tiny, open source Windows battery indicator for Logitech LIGHTSPEED mice, starting with the Logitech PRO X Superlight family.

The goal is simple: show the mouse battery state without installing Logitech G HUB or any other heavy background software.

## Status

This repository is at the project setup stage. No working battery reader has been implemented yet.

The intended first version is a small native Windows tray application written in C with direct Win32 and HID API calls. It should stay idle almost all the time and query the mouse only when needed.

## Goals

- Show Logitech mouse battery state from the system tray.
- Use very little memory and effectively zero idle CPU.
- Avoid services, drivers, Electron, .NET runtimes, scripting runtimes, and vendor background software.
- Keep the implementation focused, auditable, and easy to build from source.
- License the project under MIT for broad reuse.

## Initial Target

- Windows 11
- Logitech LIGHTSPEED receiver
- Logitech PRO X Superlight / similar HID++ 2.0 devices

Support for more receivers, devices, and operating systems can be added later if it does not compromise the lightweight design.

## Planned Technical Direction

- Native C / Win32 application.
- Windows SetupAPI and HID APIs for device discovery and communication.
- Logitech HID++ battery queries for battery percentage or coarse battery level.
- Tray UI via `Shell_NotifyIconW`.
- On-demand refresh by default instead of continuous polling.

## Non-Goals

- Replacing Logitech G HUB as a full device manager.
- Changing DPI, lighting, button mappings, profiles, firmware, or pairing state.
- Collecting telemetry or sending data over the network.

## Disclaimer

SuperLightBattery is an independent open source project and is not affiliated with, endorsed by, or sponsored by Logitech.

## License

SuperLightBattery is licensed under the MIT License. See [LICENSE](LICENSE).
