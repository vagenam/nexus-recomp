# Project Raccoon — Ratchet & Clank: Into the Nexus PC Recomp (`v0.3`)

**Project Raccoon** is a native Windows x86-64 static recompilation of **Ratchet & Clank: Into the Nexus** (PS3 USA Disc version, `BCUS99245` v1.00) powered by the `ps3recomp` runtime.

> **Note:** This project is created with Artificial Intelligence (AI).
> For a deep dive into the engineering, profiling, and gameplay fixes, see **[TECH_WRITEUP.md](TECH_WRITEUP.md)**.

---

## Current Project Status (`v0.3`)
- **Testing Scope:** The game has **not** been tested thoroughly yet — only the **first level** (Nebulox 7) has been tested, and it plays alright there. Later levels and full playthrough stability are unverified.
- **Test Hardware:** Tested on an **Intel Xeon E5-2680 v4** (older server CPU, not optimal for single-thread/PPU-heavy emulation) and an **AMD Radeon RX 9060 XT 16 GB**. Systems with newer, faster desktop CPUs will likely get noticeably better performance.
- **Known Issues:**
  - Some **graphical glitches** still remain.
  - **Performance** is not perfect yet and can dip in heavy scenes.
  - **Audio** still experiences slowdown in places (such as startup/heavy load).
- **Shipping Defaults:** Ships in **stock PS3 720p** (`1.00x`, stock texture filtering & shadow resolution) with **unlocked FPS** (`RACACIT_FPS_LIMIT=0`) so you can cap the framerate or raise resolution/settings yourself in the **F1 / Esc** menu.
- **Graphics (RSX → Vulkan 1.3):** Supports up to **4K (`3.00x`, `3840×2160`)** internal resolution scaling, anisotropic/trilinear filtering, and VRAM budget management.
- **Audio:** Full FMV, menu, and in-game `cellAudio` mixing.
- **Controls:** Keyboard & Mouse + SDL2 Gamepads (PlayStation DualSense / DualShock 4 with native HIDAPI haptics/speaker/triggers/lightbar, Xbox / XInput).

---


## Legal Notice
This project is an independent clean-room static recompiler runtime and does not distribute copyrighted game assets, PS3 disc contents, or Sony PlayStation proprietary SDK files. To play the game, you must supply your own legally dumped files from *Ratchet & Clank: Into the Nexus* (`BCUS99245`).
