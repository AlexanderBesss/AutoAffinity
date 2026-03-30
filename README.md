# AutoAffinity

Watches for **cs2.exe** and **dota2.exe** and keeps them on **High priority** with **CPU cores 0 and 1 excluded** from affinity. Settings are reapplied automatically if the game restarts or if something changes them back.

## Usage

Run `AutoAffinity.exe` as **Administrator**. The app sits in the system tray.

- **Double-click** tray icon — open log window
- **Right-click** tray icon — show/hide or exit

## Build

Requires Visual Studio 2022 with the *Desktop development with C++* workload.

```bat
build.bat
```

Output: `x64\Release\AutoAffinity.exe`
