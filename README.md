# Clipboard Manager

A lightweight, cross-platform clipboard history manager written in C++.  
Copy anything — the manager quietly records it so you can retrieve it later.

## Features (v0.1)

- Monitors the clipboard in the background (polls every 500 ms)
- Stores up to 50 history entries in memory
- Interactive terminal UI: view history, clear it, quit cleanly
- Cross-platform: **macOS**, **Windows**, **Linux**

## Requirements

| Platform | Toolchain | Extra |
|----------|-----------|-------|
| macOS | Xcode Command Line Tools (`xcode-select --install`) | — |
| Windows | MSVC (Visual Studio) or MinGW | — |
| Linux | GCC / Clang | `xclip` (`sudo apt install xclip`) |

All platforms need **CMake ≥ 3.16**.

## Build & Install

```bash
# 1. Clone
git clone https://github.com/YOUR_USERNAME/clipboard-manager.git
cd clipboard-manager

# 2. Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 3. Compile
cmake --build build --parallel

# 4. Install globally (makes `clipboard-manager` available system-wide)
sudo cmake --install build          # macOS / Linux  → /usr/local/bin
cmake --install build               # Windows (run as Administrator)
```

After installing, run from any terminal:
```bash
clipboard-manager
```

To uninstall, delete the binary:
```bash
sudo rm /usr/local/bin/clipboard-manager   # macOS / Linux
```

### Run without installing

```bash
./build/clipboard-manager          # macOS / Linux
build\Release\clipboard-manager.exe  # Windows
```

## Usage

```
╔══════════════════════════════════╗
║      Clipboard Manager v0.1      ║
╚══════════════════════════════════╝

Commands (type and press Enter):
  h  – show history
  c  – clear history
  q  – quit
```

Copy any text in any app while the manager is running, then press `h` to see your history.

## Project Structure

```
clipboard-manager/
├── src/
│   ├── main.cpp              # Entry point, command loop
│   ├── ClipboardManager.h    # Public API
│   ├── ClipboardManager.cpp  # Polling loop & history management
│   └── platform/
│       ├── clipboard_mac.mm  # macOS  – NSPasteboard (Objective-C++)
│       ├── clipboard_win.cpp # Windows – Win32 OpenClipboard
│       └── clipboard_linux.cpp # Linux – xclip via popen()
├── .vscode/
│   ├── tasks.json            # Ctrl+Shift+B → builds the project
│   ├── launch.json           # F5 → debugs the project
│   └── settings.json         # Editor & CMake preferences
├── CMakeLists.txt
└── README.md
```

## Recommended VS Code Extensions

- **C/C++** (`ms-vscode.cpptools`) — IntelliSense and debugging
- **CMake Tools** (`ms-vscode.cmake-tools`) — CMake integration
- **CodeLLDB** (`vadimchill.codelldb`) — native debugger (macOS/Linux)

Install them from the Extensions panel (`Cmd+Shift+X`).

## Roadmap

- [ ] Persistent storage (save history to disk across restarts)  
- [ ] Search / fuzzy-filter history  
- [ ] System tray icon (macOS menu bar / Windows tray)  
- [ ] De-duplicate consecutive identical copies  
- [ ] Support images and rich content  

## Contributing

Pull requests are welcome! Please open an issue first to discuss what you'd like to change.

## License

MIT
