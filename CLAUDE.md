# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Live Haptics** is a Windows system tray application (C++17) that adds customizable haptic feedback to Logitech mice (e.g., MX Master 4) via direct HID++ 2.0 protocol communication over a Bolt (C548) or Unifying (C52B) receiver.

## Build Commands

Requires Visual Studio 2022 and CMake 3.20+. No test suite exists; testing is manual.

**Quick build (automated):**
```bat
build.bat
```
Finds VS2022, configures with NMake Makefiles, builds, and launches the executable automatically.

No lint or test commands are configured.

## Architecture

The application is layered: **HID protocol → haptic logic → UI**, driven by a global mouse hook with async worker threads for non-blocking haptic playback.

### Layers

**`src/hidpp/` — HID++ 2.0 Protocol**
- `HidDevice`: Wraps `hidapi` for raw HID read/write and device enumeration by VID/PID.
- `HidppDevice`: Implements HID++ 2.0 — feature discovery (ROOT, BATTERY, HAPTIC_FEEDBACK, THUMB_WHEEL, REPROG_CONTROLS_V4), packet encoding (short/long/very-long reports), waveform playback, battery polling, and a notification thread for hardware events (thumb wheel notches, gesture buttons).
- 16 waveforms: ROM IDs 0x00–0x0E (contiguous) plus 0x1B ("Whisper"); use `waveformName()` for safe lookup.
- Device connection tries Bolt receiver first (index 1–6), then Unifying.

**`src/haptics/` — Core Logic & Threading**
- `HapticController`: Owns the `HapticConfig` struct (7 event types, per-event waveform + cooldown delay), and two worker threads:
  - **Haptic thread**: Drains an async command queue, enforces per-event cooldowns, calls `HidppDevice::playHaptic()`.
  - **Manager thread**: Monitors connection/battery (~1 s interval), posts `WM_BATTERY`/`WM_CONNECTED` to `TrayApp`, and runs hover detection via Windows UIAutomation/MSAA.
- Hover detection inspects the element tree under the cursor for interactive regions (buttons, links, NC region hit-test codes).

**`src/ui/` — UI & System Tray**
- `TrayApp`: Main application controller. Owns a hidden Win32 message window, installs the global low-level mouse hook, manages the tray icon (dynamic status dot + battery %), and forwards mouse events to `HapticController`.
- `PopupWindow`: 540×820 px custom dark-mode config UI rendered entirely with GDI+ (no native controls). Shows 7 event-type cards, each with toggle, a 4×3 waveform grid, and a delay slider. Waveform preview on click. Callbacks immediately update `HapticController` config and call `Config::save()`.

**`src/config/` — Persistence**
- `Config`: Loads/saves `HapticConfig` as INI to `%APPDATA%\LiveHaptics\config.ini`.

### Startup & Data Flow

```
wWinMain → single-instance mutex check → TrayApp::init()
  → load Config → create message window → create PopupWindow (hidden)
  → instantiate HapticController → install mouse hook
  → create tray icon → start HapticController threads → message loop
```

**Mouse → Haptic:**
```
Global hook (WM_MOUSEWHEEL, WM_LBUTTONDOWN, …)
  → TrayApp::MouseHookProc() [skips events over PopupWindow]
  → HapticController::playScroll() / playLeftClick() / …
  → async queue → haptic thread → cooldown check
  → HidppDevice::playHaptic(waveformId) → HID++ packet → motor
```

**Config change:**
```
PopupWindow callback → HapticController config field → Config::save()
```

**Device status:**
```
Manager thread → HidppDevice::refreshBattery()
  → PostMessage WM_BATTERY / WM_CONNECTED → TrayApp → tray icon update
```

### Threading Model

4 threads: main (GUI/hook), haptic worker, device manager, HID++ notification listener. Coordination via `std::atomic` flags and `std::mutex`-protected queues.

## Key Technical Notes

- **Single-instance enforcement** via a named mutex in `main.cpp`.
- **DPI awareness** set to per-monitor v2 at startup.
- **Windows libraries linked:** gdiplus, gdi32, user32, shell32, comctl32, ole32, uuid, dwmapi, uxtheme, oleacc, uiautomationcore.
- **`hidapi`** is fetched as a static library at configure time (v0.14.0) via CMake FetchContent.
- Compiler: MSVC with `/W4` and `/MD` runtime.
