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

## Requirements
- **Windows 10 / 11 (x86-64)**
- **CPU with AVX2 support** (Intel Haswell / AMD Zen or newer; compiled with `-march=haswell`)
- **Vulkan 1.3 capable GPU**
- **No manual CMake/Ninja/Python install needed:** `RecompBuilder.exe` uses the prebundled offline toolchain (`tools\.portable\`) or auto-bootstraps it on first run.
- **Your Own Game Dump (Zero Copyrighted Code/Assets Shipped):**
  1. **Extracted `PS3_GAME` folder** from **Ratchet & Clank: Into the Nexus (USA)** (`BCUS99245` v1.00) — **must be the extracted `PS3_GAME` folder** (containing `USRDIR\` and `PARAM.SFO`), **NOT** a `.pkg`, **NOT** an `.iso` (extract `.iso`/`.pkg` first if needed).
  2. **Decrypted `EBOOT.ELF` from RPCS3:** In RPCS3, go to **Utilities → Decrypt PS3 Binaries**, select `PS3_GAME\USRDIR\EBOOT.BIN`, and save the decrypted `EBOOT.ELF`.

---

## Quick Start (`RecompBuilder.exe`)
We do **not** ship a precompiled `NexusRecomp.exe` or any copyrighted game code/assets. `RecompBuilder.exe` is a native Windows GUI app that handles everything automatically:

1. Double-click **`RecompBuilder.exe`** (no console commands or manual tool installs).
2. Drop your **extracted `PS3_GAME` folder** into `Put_Game_Folder_Here\` (or click **Browse...** — must contain `USRDIR\` and `PARAM.SFO`, not `.iso` or `.pkg`).
3. Drop your **RPCS3-decrypted `EBOOT.ELF`** into `Put_ELF_Here\` (or click **Browse...**).
4. Click **Build & Setup** — it uses the prebundled portable toolchain (`tools\.portable\`) to lift & compile your executable into **`RecompFolder\NexusRecomp.exe`** and writes **`RecompFolder\settings.txt`** (stock 720p, unlocked FPS, FPS counter on).
5. Click **Play** (or launch **`RecompFolder\NexusRecomp.exe`** / the Desktop shortcut).

---

## In-Game Settings Menu (`F1` / `Esc`)
Press **F1** (or **Esc**) during gameplay or menus to open the settings overlay:
- **Display:** Fullscreen / Borderless / Windowed, Window Size, VSync, Aspect Ratio, FPS Counter, Internal Resolution (`0.50x` up to `3.00x` 4K).
- **Controls:** DualSense haptics/triggers/speaker/lightbar, R1/R2 swap, and Keyboard & Mouse sensitivity.

---

## `v0.3` Changelog
- **Dynamic Bolt Spawn Cap (`func_004E9F98`):** Caps physical bolt spawns at 3 per burst while scaling each bolt's value so economy/progression remains 100% exact.
- **Crate Debris Moby Burst Cap (`func_00784E20`):** Caps runaway crate-plank Moby bursts (`cls=0x55`) during active gameplay (after the first 2s of level load), eliminating the `igAnim` (`0x00588B80`) SPU stall and 8.5 FPS hang.
- **SPU / SPURS Channel & Drain Optimizations:** AVX2 `spu_mem_ne`, 64-entry `_Thread_local` overlay cache, hoisted `w0/w1` checks, `spu_lr_wait` `SleepConditionVariableSRW` backoff, power-of-2 ring (`RING=32`), and guarded `yz_lockstep_tick`.
- **RSX VRAM Budget & 4K Cap:** Clamped `rt_budget` + `z_budget` to stay within 80% of card VRAM, lowered proactive eviction to 84%, and capped internal resolution at `4K (3.00x)` (removing `5K / 4.00x` which caused `VkResult -2` OOM stalls).
- **GUI Builder & Offline Toolchain (`RecompBuilder.exe`):** Native Win32 GUI setup/builder with `Put_Game_Folder_Here\` & `Put_ELF_Here\` auto-detection, prebundled portable toolchain, `RecompFolder\` output, and Desktop shortcut creation.

---

## Roadmap
- Fix all remaining visual and graphical glitches
- Achieve stable **60 FPS** across full gameplay
- Optional graphics enhancements (e.g., **SMAA**, improved shadows, and possibly **FSR**)
- Nicer, more polished installer experience

---

## Reporting Bugs
If you run into crashes, graphical glitches, or gameplay issues, please report them using **[GitHub Issues](https://github.com/vagenam/nexus-recomp/issues)**.

---

## Support
If for some reason someone wants to support this project, you can **[buy me a coffee](https://buymeacoffee.com/vagenam)**.

---

## Credits
- **`ps3recomp`** — Core PS3 PPU/SPU/RSX static recompilation runtime and tooling
- **`RACiT`** — *Ratchet & Clank: A Crack in Time* recompilation research & runtime foundations
- **`ratchet-tod-recomp`** — *Ratchet & Clank: Tools of Destruction* recompilation insights
- **`infamous2-recomp`** — *inFAMOUS 2* recompilation & SPURS/RSX research
- **`resistance-recomp`** — *Resistance* recompilation & Insomniac/PS3 runtime patterns
- **`Soulbrandt`** — Community research and tooling contributions

---

## Legal Notice
This project is an independent clean-room static recompiler runtime and does not distribute copyrighted game assets, PS3 disc contents, or Sony PlayStation proprietary SDK files. To play the game, you must supply your own legally dumped files from *Ratchet & Clank: Into the Nexus* (`BCUS99245`).
