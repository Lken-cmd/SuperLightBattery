# Agent Instructions

## Implementation Defaults

- Prefer C with direct Win32, SetupAPI, and HID APIs.

Optimize for:

- Very low idle CPU usage.
- Small memory footprint.
- Native Windows APIs.
- Small, readable, auditable code.
- No network access unless a future task explicitly requires it.

## Public Repository Hygiene For Agents

This project is intended to be public open source. Do not commit private or sensitive information.

Before preparing any patch or commit:

- Inspect the full diff.
- Check staged files deliberately.
- Remove local usernames, local absolute paths, machine names, serial numbers, USB instance IDs, hardware captures, logs, dumps, tokens, API keys, and private notes.
- Do not commit generated binaries, build directories, package caches, `.env` files, crash dumps, debug symbol files, or packet captures.
- Prefer generic examples over real local device identifiers.

## Licensing Rules

- The project license is MIT.
- Do not copy GPL, proprietary, or otherwise incompatible source code into this repository.
- Community projects and operating-system drivers may be used for behavioral understanding, but implementation code must be original or taken only from MIT-compatible sources with attribution where required.
- Do not include proprietary Logitech documents, SDKs, binaries, firmware, captures, or assets unless their license clearly allows redistribution in an MIT project.
- Do not decompile Logitech G HUB or other proprietary software for implementation details. Use public documentation, operating-system APIs, compatible open source references, and clean-room observations of locally owned hardware behavior instead.

