# Ratchet & Clank: Into the Nexus (PC Static Recompilation)

A native PC x86-64 static recompilation of **Ratchet & Clank: Into the Nexus** (PS3, `BCUS99245` / `NPEA00454`) powered by the `ps3recomp` runtime.

## Features
- **Native x86-64 Execution:** Recompiled directly from PowerPC/SPU bytecode to native C++ and compiled with Clang/LLVM.
- **Vulkan Graphics Engine:** Custom RSX hardware rendering backend with Vulkan swapchain support.
- **SPURS & SPU Multithreading:** SPU job and policy module runtime with parallel worker threads.
- **Custom Audio Pipeline:** `cellAudio` streaming and mixing architecture.

## Requirements
- 64-bit Windows 10/11
- Vulkan 1.3 compatible GPU
- CMake 3.25+ & Ninja
- Clang / Zig toolchain

## Building
1. Place a legally dumped `EBOOT.ELF` from your PS3 game disc into the `elf/` directory.
2. Run the lift & build pipeline:
   ```cmd
   python build_pipeline.py
   ```
3. Or build directly via CMake:
   ```cmd
   cmake --build build-win -j10
   ```
4. Output executable will be placed in `dist/NexusRecomp.exe`.

## Legal Notice
This project is an independent clean-room static recompiler runtime and does not contain copyrighted game assets, proprietary binaries, or Sony PlayStation SDK libraries. To play the game, you must legally own a copy of *Ratchet & Clank: Into the Nexus* on PlayStation 3.
