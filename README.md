# Ratchet & Clank: Into the Nexus (PC Static Recompilation)

A native PC x86-64 static recompilation of **Ratchet & Clank: Into the Nexus** (PS3 USA Disc version, `BCUS99245` v1.00) powered by the `ps3recomp` runtime.

> **Note:** This project is created entirely with Artificial Intelligence (AI).

## Project Status: Work-in-Progress (WIP)
- **Boot & Intro:** Working (Safety screens, intro cinematics, and title screen boot).
- **Audio:** 
  - FMV / cutscene movies have full audio.
  - **Menu and gameplay currently have no audio** (in-game audio mixer pipeline is under active development).
- **Controls:** Gamepads supported via SDL2 (PlayStation DualSense / DualShock 4 with native HIDAPI, Xbox / XInput controllers).
- **Gameplay:** Early WIP / unstable. Boots into playable gameplay, but rendering and effects are currently broken, frame rate is unstable, and crashes occur during early gameplay.

## Requirements
- Windows 10/11 (x86-64)
- Vulkan 1.3 capable GPU
- CMake 3.25+ and Ninja
- Clang / Zig C++ toolchain
- Legally dumped files from **Ratchet & Clank: Into the Nexus (USA)** (`BCUS99245`)

## Building from Source
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

## Running
1. Copy `NexusRecomp.exe` into a `dist/` directory alongside `settings.txt`.
2. In `settings.txt`, set the path to your dumped PS3 game folder:
   ```ini
   GAME=C:\path\to\Ratchet & Clank - Into the Nexus (USA)\PS3_GAME
   RACACIT_WINDOW_SIZE=1920x1080
   RACACIT_DISPLAY_MODE=windowed
   RACACIT_CPU_MODE=0
   ```
3. Launch `NexusRecomp.exe`.

## In-Game Settings Menu (F1 / Esc)
Press **F1** (or **Esc**) during gameplay or menus to open the settings overlay.

- **Display:** Toggle Fullscreen/Borderless/Windowed, Window Size, VSync, Aspect Ratio, and FPS counter.
- **Performance:**
  - **CPU Profile:**
    - `General (Default)` (`RACACIT_CPU_MODE=0`): SPU lanes yield after a short 30 µs spin. Recommended for most modern 4-, 6-, and 8-core consumer CPUs (AMD Ryzen, Intel Core) so worker threads don't starve other processes or threads.
    - `Xeon e5 2680v4` (`RACACIT_CPU_MODE=1`): SPU lanes spin longer (200 µs), reducing thread sleep and wake latency overhead on high-core-count, lower-clock workstation/server CPUs (e.g., Intel Xeon E5).
- **Controls:** Configure DualSense / gamepad features and keyboard/mouse settings.

## Legal Notice
This project is an independent clean-room static recompiler runtime and does not distribute copyrighted game assets, PS3 disc contents, or Sony PlayStation proprietary SDK files. To play the game, you must supply your own legally dumped files from *Ratchet & Clank: Into the Nexus* (`BCUS99245`).
