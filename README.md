# Ratchet & Clank: Into the Nexus (PC Static Recompilation)

A native PC x86-64 static recompilation of **Ratchet & Clank: Into the Nexus** (PS3 USA Disc version, `BCUS99245` v1.00) powered by the `ps3recomp` runtime.

## Project Status: Work-in-Progress (WIP)
- **Boot & Intro:** Working (Safety screens, intro cinematics with full video and audio).
- **Audio:** Cutscene/movie audio is fully functional. In-game menu/gameplay BGM mixer is WIP.
- **Rendering:** Custom Vulkan RSX hardware renderer is functional up to 1080p. In-game 3D rendering and particle effects are under active development.
- **Controls:** DualSense and standard XInput gamepads supported.

## How It Works
Anyone with a legal copy of the USA PS3 game (`BCUS99245`) can build and run this:

### 1. Requirements
- Windows 10/11 (x86-64)
- Vulkan 1.3 capable GPU
- CMake 3.25+ and Ninja
- Clang / Zig C++ toolchain
- Legally dumped files from **Ratchet & Clank: Into the Nexus (USA)** (`BCUS99245`)

### 2. Building from Source
Clone the repository:
```cmd
git clone https://github.com/vagenam/nexus-recomp.git
cd nexus-recomp
```

Configure and build with CMake:
```cmd
cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j10
```
The compiled binary `NexusRecomp.exe` will be generated in `build-win/game/`.

### 3. Running
1. Copy `NexusRecomp.exe` into a `dist/` directory alongside `settings.txt`.
2. In `settings.txt`, set the path to your dumped PS3 game folder:
   ```ini
   GAME=C:\path\to\Ratchet & Clank - Into the Nexus (USA)\PS3_GAME
   RACACIT_WINDOW_SIZE=1920x1080
   RACACIT_DISPLAY_MODE=windowed
   ```
3. Launch `NexusRecomp.exe`.

## Legal Notice
This project is an independent clean-room static recompiler runtime and does not distribute copyrighted game assets, PS3 disc contents, or Sony PlayStation proprietary SDK files. To play the game, you must supply your own legally dumped files from *Ratchet & Clank: Into the Nexus* (`BCUS99245`).
