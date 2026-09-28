# ShortcutGrouper

A Windows 11 shell extension that adds a **"Объединить в группу"** (Group Shortcuts) button to the context menu command bar. When you select two or more `.lnk` shortcut files on the desktop, this button lets you group them into a visual folder widget.

## Features

- **Desktop folder widgets** — iOS/Android-style visual overlays on the Windows desktop showing grouped shortcut icons
- **Drag-and-drop to add** — drag `.lnk` files onto a widget to add them to the group
- **Draggable widgets** — reposition widgets anywhere on the desktop; positions are saved
- **Click-to-expand popup** — click a widget to see the full shortcut list; click an item to launch it
- **System tray icon** — persistent background process with tray icon for refresh, settings, and exit
- **Acrylic blur effects** — modern Windows visual effects via `SetWindowCompositionAttribute`
- **Direct2D/DirectWrite rendering** — custom hardware-accelerated rendering engine
- **IPC single instance** — second launches send commands to the running instance
- **MSIX sparse package** — modern packaging and distribution support
- **"Open With" menu suppression** — removes unwanted "Open With" items from the desktop context menu
- **Undo support** via Windows `IFileOperation`
- **Logging** to `%TEMP%\DesktopGroupManager.log`

## Requirements

- **OS:** Windows 10 version 1607 or later (Windows 11 recommended: new context menu, Mica, rounded corners). Older systems (Windows 7/8.x, Windows 10 pre-1607) should largely work via traditional registration, but are untested.
- **Build tools:** Visual Studio 2022 (v143+) or CMake 3.20+
- **Dependencies:** Bundled in `vendor/`:
  - [nlohmann/json](https://github.com/nlohmann/json) (header-only)

## Building

```bash
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build Release
cmake --build build --config Release
```

Build artifacts will be in `build/bin/Release/`:
- `ShellExtension.dll` — the COM shell extension
- `GroupManager.exe` — the grouping application

> The legacy `ShortcutGrouper.sln` / `*.vcxproj` build is stale
> (references removed files, misses new ones) — use CMake.

## Installation

### Option A: Setup.exe (Recommended)

```powershell
.\installer\build-setup.ps1 -Version 1.0.1.0
```

Produces `ShortcutGrouper-Setup-<version>.exe` (IExpress self-extractor,
no extra tools needed). Run it: installs to `%ProgramFiles%`,
registers the sparse package + COM DLL, creates Start Menu shortcuts
(incl. Uninstall). UAC prompt appears once.

### Option B: MSIX Sparse Package

1. Generate a self-signed certificate:
   ```powershell
   .\scripts\setup-cert.ps1
   ```
2. Build and install the sparse package:
   ```powershell
   .\scripts\install-sparse.ps1
   ```

### Option B: Traditional Registration

1. Copy `ShellExtension.dll` and `GroupManager.exe` to the same directory.
2. **Register the shell extension** (run as Administrator):
   ```bash
   regsvr32 ShellExtension.dll
   ```
   Or double-click `scripts\register.bat`.

3. Restart Explorer (optional, but recommended):
   ```bash
   taskkill /f /im explorer.exe
   start explorer.exe
   ```

## Usage

1. Select **two or more** `.lnk` shortcut files on your desktop.
2. Right-click to open the context menu.
3. Click the **"Объединить в группу"** button in the command bar.
4. Enter a name for the group (or accept the default).
5. A folder widget is created on your desktop showing the grouped shortcuts.
6. **Drag** the widget to reposition it. **Click** it to expand and see all shortcuts.
7. **Drag additional `.lnk` files** onto a widget to add them to the group.
8. Group metadata is saved to `%APPDATA%\DesktopGroupManager\groups.json`.

### Tray Icon

- Right-click the tray icon to access **Refresh widgets**, **Settings**, or **Exit**.

## Uninstallation

### Option B: MSIX Sparse Package

```powershell
.\scripts\uninstall.ps1
```

### Option B: Traditional Registration

1. **Unregister** (run as Administrator):
   ```bash
   regsvr32 /u ShellExtension.dll
   ```
   Or double-click `scripts\unregister.bat`.

2. Delete the installed files.

## Project Structure

```
ShortcutGrouper/
├── CMakeLists.txt                 # The build (VS2022 generator, x64)
├── LICENSE
├── README.md
├── src/
│   ├── ShellExtension/              # COM DLL for context menu integration
│   │   ├── dllmain.cpp
│   │   ├── GroupCommand.h/cpp       # IExplorerCommand "Group Shortcuts"
│   │   ├── ClassFactory.h/cpp
│   │   ├── Registration.h/cpp       # HKCR + HKCU\Classes registration
│   │   ├── LogHelper.h/cpp
│   │   ├── DesktopOpenWithFilter.h/cpp
│   │   └── ShellExtension.def
│   ├── GroupManager/                # GUI application with desktop widgets
│   │   ├── main.cpp                 # WinMain: singleton, IPC, tray, timers
│   │   ├── DesktopWidget.h/cpp      # Desktop folder widget overlay + OLE DnD
│   │   ├── WidgetManager.h/cpp      # Widget lifecycle & group CRUD
│   │   ├── WidgetTypes.h            # Shared data structures, AppData helper
│   │   ├── Renderer.h/cpp           # Direct2D rendering engine + caches
│   │   ├── PopupWindow.h/cpp        # Click-to-expand popup, shell menu
│   │   ├── SearchWindow.h/cpp       # Global shortcut search
│   │   ├── SettingsDialog.h/cpp     # Settings UI + autostart (Run)
│   │   ├── ModernMenu.h/cpp         # Windows 11-style menus
│   │   ├── Marquee.h/cpp            # Desktop rubber-band selection
│   │   ├── DesktopGrid.h/cpp        # Desktop icon grid probing (ListView IPC)
│   │   ├── ColorDialog.h/cpp        # Glass color picker
│   │   ├── BlurHelper.h/cpp         # Acrylic blur effects
│   │   ├── IconHelper.h/cpp         # Shortcut icon extraction
│   │   ├── TrayIcon.h/cpp           # System tray icon
│   │   ├── UninstallHelper.h/cpp    # App uninstaller lookup & launch
│   │   └── OpenWithDetector.h/cpp   # Drag-and-drop gesture detection
│   ├── Common/                      # Shared utilities (Common.lib)
│   │   ├── Lang.h/cpp               # RU/EN localization
│   │   ├── DpiHelper.h/cpp          # Per-monitor DPI helpers
│   │   ├── ColorUtils.h/cpp
│   │   ├── Logger.h/cpp             # Unified debug.log writer
│   │   ├── AppPaths.h               # AppData paths (KnownFolder API)
│   │   ├── Reg.h                    # Registry DWORD/string helpers
│   │   └── ShellQuote.h             # CommandLineToArgvW quoting, PS escaping
│   └── AppxManifest.xml             # MSIX sparse manifest
├── tools/
│   └── create_msix.cpp              # Legacy MSIX packaging tool (unused by build)
├── docs/
│   └── desktop-bags-backup.reg      # User's Explorer Bags backup
├── vendor/
│   └── include/nlohmann/            # Bundled nlohmann/json
├── cert/
│   └── sparse.pfx                   # Code-signing certificate (git-ignored)
├── resources/
│   ├── app.rc
│   ├── resource.h
│   └── icon.ico
└── scripts/
    ├── register.bat                 # Traditional COM registration
    ├── unregister.bat               # Traditional COM unregistration
    ├── setup-cert.ps1               # Generate self-signed certificate
    ├── build-sparse.ps1             # Build MSIX sparse package
    ├── install-sparse.ps1           # Full installer (cert + sparse + COM)
    ├── uninstall.ps1                # Clean uninstaller
    └── debug-menu.ps1               # Diagnostic checks (incl. autostart)
└── installer/
    ├── Install.ps1                  # Setup payload: Program Files install
    ├── Uninstall.ps1                # Setup payload: clean removal
    ├── install.bat                  # Setup entry point (self-elevates)
    └── build-setup.ps1              # Assembles Setup.exe via IExpress
```

> The project builds with CMake only (VS2022 opens `CMakeLists.txt` natively
> via File → Open → CMake). Legacy `.sln` / `*.vcxproj` files were removed.

## Architecture

### Shell Extension (`ShellExtension.dll`)

Implements `IExplorerCommand` — the Windows 11 modern shell extension interface. The extension:

- Registers under `HKCR\lnkfile\shell\GroupShortcuts`
- Validates that exactly `.lnk` files are selected
- Shows the button only when 2+ shortcuts are selected
- Launches `GroupManager.exe` with the selected file paths

### Desktop Open With Filter (`DesktopOpenWithFilter`)

A separate COM class that suppresses "Open With" menu items on the desktop right-click context menu, working around the Windows 11 XAML flyout behavior.

### Group Manager (`GroupManager.exe`)

A persistent background application that:

- Runs as a **system tray application** with a message loop
- Receives commands from the shell extension via **IPC** (`WM_COPYDATA`)
- Creates **desktop folder widgets** rendered via Direct2D behind desktop icons
- Supports **drag-and-drop** to add shortcuts to groups
- Supports **click-to-expand** popup menus showing all shortcuts in a group
- Persists group data (positions, shortcuts) to JSON
- Uses acrylic blur effects for the popup window
- Single-instance: subsequent launches communicate with the existing process

### Desktop Widgets

- Rendered as `WS_POPUP` tool windows parented to the `WorkerW` desktop window
- Each widget displays a 2×2 grid of mini-icons from the group's shortcuts
- Widgets are draggable; positions are saved to JSON
- Clicking a widget opens a layered popup with the full shortcut list

### MSIX Packaging

- `scripts\build-sparse.ps1` builds the MSIX (`MakeAppx` + full-manifest
  injection + `SignTool`); legacy `tools\create_msix.cpp` is not used
- `src\AppxManifest.xml` declares the `desktop4:ContextMenu` extension
- Sparse package installation enables modern Windows 11 context menu integration
- Staging lives in `%ProgramData%\ShortcutGrouper\sparse` (not `%TEMP%`)

## Troubleshooting

- **Button doesn't appear:** Ensure you have 2+ `.lnk` files selected. Restart Explorer after registration.
- **Widgets not showing:** Run `scripts\debug-menu.ps1` to check registration status.
- **Autostart not working:** `debug-menu.ps1` section `[8]` shows `Run` value and
  `StartupApproved` block; re-enable the toggle in app Settings, or delete the
  `DesktopGroupManager` value under `StartupApproved\Run`.
- **Errors:** Check `%APPDATA%\DesktopGroupManager\debug.log` for diagnostic output.
- **Registration fails:** Run `register.bat` as Administrator, or use `install-sparse.ps1`.
- **Certificate issues:** Run `setup-cert.ps1` and ensure Developer Mode is enabled.

## License

MIT License. See [LICENSE](LICENSE) for details.
