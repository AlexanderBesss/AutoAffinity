# AutoAffinity

Watches for `cs2.exe` and `dota2.exe` and automatically sets them to **High priority** with **CPU cores 0 and 1 excluded** from their affinity mask. Settings are reapplied whenever the game restarts or if something changes them back.

## Features

- System tray icon — runs silently in the background
- Double-click the tray icon to open the log window
- Minimizing or closing the window hides it back to the tray
- Detects new process launches and drift (manual resets) within ~20 seconds
- Registers itself to run on Windows startup automatically
- Single-instance guard (safe to call from startup without duplicates)

## What it does to the processes

| Setting | Value |
|---|---|
| Priority class | `HIGH_PRIORITY_CLASS` |
| CPU affinity | All logical processors **except** 0 and 1 (physical core 0 / both HT threads) |

The idea is to dedicate physical core 0 to the OS and background tasks, leaving all remaining cores for the game.

## Requirements

- Windows 10/11 x64
- Must be run as **Administrator** (required to set priority and affinity on other processes)

## Build

Requires **Visual Studio 2022** with the *Desktop development with C++* workload.

```bat
build.bat
```

Output: `x64\Release\AutoAffinity.exe`

Alternatively, open `AutoAffinity.slnx` in Visual Studio, select **Release / x64**, and build.

## Usage

1. Run `AutoAffinity.exe` as Administrator
2. The app appears in the system tray
3. Start CS2 or Dota 2 — priority and affinity are applied within one poll cycle (~20s)
4. Double-click the tray icon to view the log
5. Right-click the tray icon to show/hide the window or exit
