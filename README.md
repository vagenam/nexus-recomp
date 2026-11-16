# Project Raccoon — Ratchet & Clank: Into the Nexus PC Recomp (`v0.3.1`)

**Project Raccoon** is a native Windows x86-64 static recompilation of **Ratchet & Clank: Into the Nexus** (PS3 USA Disc version, `BCUS99245` v1.00) powered by the `ps3recomp` runtime.

> **Note:** This project is created with Artificial Intelligence (AI).
> For a deep dive into the engineering, profiling, and gameplay fixes, see **[TECH_WRITEUP.md](TECH_WRITEUP.md)**.

---

## Current Project Status (`v0.3.1`)
- **Testing Scope:** The game has **not** been tested thoroughly yet — only the **first level** (Nebulox 7) has been tested, and it plays alright there. Later levels and full playthrough stability are unverified.
- **Test Hardware:** Tested on an **Intel Xeon E5-2680 v4** (older server CPU, not optimal for single-thread/PPU-heavy emulation) and an **AMD Radeon RX 9060 XT 16 GB**. Systems with newer, faster desktop CPUs will likely get noticeably better performance.
- **Shipping Defaults:** Ships in **stock PS3 720p** (`1.00x`, stock texture filtering & shadow resolution) with **unlocked FPS**, from my testing increasing resolution to 4k and maxing out shadow resolution had next to no negative impact on fps but it might be different for you
- **Graphics (RSX → Vulkan 1.3):** Supports up to **4K (`3.00x`, `3840×2160`)** internal resolution scaling, anisotropic/trilinear filtering, and VRAM budget management.
- **Audio:** Full FMV, menu, and in-game `cellAudio` mixing.
- **Controls:** Keyboard & Mouse + SDL2 Gamepads (PlayStation DualSense / DualShock 4 with native HIDAPI haptics/speaker/triggers/lightbar, Xbox / XInput).

---

## Known Issues
- **Untested Beyond Level 1:** Only the first level (Nebulox 7) has been tested so far; later planets/levels and full playthrough stability are unverified.
- **Graphical Glitches:** Some rendering and shader glitches still remain in certain scenes.
- **Performance Dips:** Framerate is not locked 60 FPS everywhere yet and can dip in heavy scenes (especially on older server/multi-core CPUs such as Xeon E5-2680 v4).
- **Audio Slowdown:** Audio can experience slowdown during startup or heavy CPU/SPU load.
- **Bolt & Crate Debris Caps (Workaround):** Physical bolt spawns (`func_004E9F98`, value-scaled) and crate-plank debris Mobys (`func_00784E20`, `cls=0x55`) are capped at 3 per burst during active gameplay to prevent SPU `igAnim` stalls.
- **4K Resolution Cap:** Internal resolution is capped at `3.00x` (4K `3840×2160`); `5K (4.00x)` is disabled to avoid Vulkan `VK_ERROR_OUT_OF_DEVICE_MEMORY` stalls.

---

## Requirements
- **Windows 10 / 11 (x86-64)**
- **CPU with AVX2 support** (Intel Haswell / AMD Zen or newer)
- **Vulkan 1.3 capable GPU**
- **No manual tool installs required in release zips:** The prepackaged release zip includes `RecompBuilder.exe` with a prebundled portable toolchain (`tools\.portable\`). When running from a clean git clone, `RecompBuilder` or `build_pipeline.py` will automatically download and verify pinned toolchain dependencies (Python 3.12 embeddable, CMake, Ninja, Zig).
- **Your Own Game Dump (Zero Copyrighted Code/Assets Shipped):**
  1. **Extracted `PS3_GAME` folder** from **Ratchet & Clank: Into the Nexus (USA)** (`BCUS99245` v1.00) — **must be the extracted `PS3_GAME` folder** (containing `USRDIR\` and `PARAM.SFO`), **NOT** a `.pkg`, **NOT** an `.iso` (extract `.iso`/`.pkg` first if needed).
     *(Note: during setup, your decrypted `EBOOT.elf` is copied into `PS3_GAME\USRDIR\EBOOT.elf` so the recomp runtime can locate it).*
  2. **Decrypted `EBOOT.ELF` from RPCS3:** In RPCS3, go to **Utilities → Decrypt PS3 Binaries**, select `PS3_GAME\USRDIR\EBOOT.BIN`, and save the decrypted `EBOOT.ELF` (SHA-256 is verified up front against `BCUS99245` v1.00).

---

## Quick Start (`RecompBuilder.exe`)
> **Note:** This Quick Start guide is for users downloading the ready-to-use **Releases zip** (`Project-Raccoon-v0.3.1-win64.zip`), which comes with `RecompBuilder.exe` and offline toolchain prebundled. If you are building from a Git clone, compile `tools/RecompBuilder.cpp` or run `build_pipeline.py` / `build.bat`.

We do **not** ship a precompiled `NexusRecomp.exe` or any copyrighted game code/assets. `RecompBuilder.exe` is a native Windows GUI app that handles everything automatically:

1. Double-click **`RecompBuilder.exe`** (no console commands or manual tool installs).
2. Drop your **extracted `PS3_GAME` folder** into `Put_Game_Folder_Here\` (or click **Browse...** — must contain `USRDIR\` and `PARAM.SFO`, not `.iso` or `.pkg`).
3. Drop your **RPCS3-decrypted `EBOOT.ELF`** into `Put_ELF_Here\` (or click **Browse...**).
4. Click **Build & Setup** — it verifies your dump & ELF, lifts & compiles your executable into **`RecompFolder\NexusRecomp.exe`**, and writes **`RecompFolder\settings.txt`** (stock 720p, unlocked FPS, FPS counter on).
5. Click **Play** (or launch **`RecompFolder\NexusRecomp.exe`** / the Desktop shortcut).

---

## `v0.3.1` Changelog
- **Fixed Installer & Setup:** Resolved issues where v0.3 release / setup failed on fresh clones or external paths (untracked stale CMake caches and hardcoded directories).
- **ELF & Game Dump Verification:** Added up-front SHA-256 verification of `EBOOT.ELF` and title-ID validation of `PARAM.SFO` (`BCUS99245` v1.00), preventing cryptic lifter crashes when an incompatible dump is provided.
- **Lift Stamp Invalidation:** The pipeline and builder now track lift stamps (`out/lift_stamp.txt`), correctly invalidating outdated or stale lifts instead of silently skipping them.
- **Pinned & Hash-Verified Toolchain:** Pin exact versions of `cmake`, `ninja`, and `ziglang` with SHA-256 hash checks via `tools/toolchain-requirements.txt`.
- **Dynamic Threading / Hardware Scaling:** Replaced hardcoded core counts (`-j14`/`-j10`) with `os.cpu_count()` and `%NUMBER_OF_PROCESSORS%` (configurable via `NEXUS_JOBS`).
- **Cleaned Repo Bloat & Licenses:** Untracked vendored redundant source trees (saving ~170MB), added root `LICENSE` (MIT), and updated `THIRD_PARTY_NOTICES.txt` with Vulkan-Headers, glslang, zlib, and LGPL-2.1 `at3_standalone`.

---
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
- Test the whole game, make sure its playable from start to the end
---

## Reporting Bugs
If you run into crashes, graphical glitches, or gameplay issues, please report them using **[GitHub Issues](https://github.com/vagenam/nexus-recomp/issues)**.

---

## Support
If for some reason someone wants to, you can **[buy me a coffee](https://buymeacoffee.com/vagenam)**.
The recomp is completely free and always will. 
I do this bc i love ps3 era games, hate that they are stuck on ps3 and bc ratchet is one of my fav childhood series ;)


---

## Credits
- **[`ps3recomp`](https://github.com/sp00nznet/ps3recomp)** — Core PS3 PPU/SPU/RSX static recompilation runtime and tooling
- **[`RACiT`](https://github.com/HighDrexler/RACiT)** — *Ratchet & Clank: A Crack in Time* recompilation research & runtime foundations
- **[`ratchet-tod-recomp`](https://github.com/Re-TeRo17/ratchet-tod-recomp)** — *Ratchet & Clank: Tools of Destruction* recompilation insights
- **[`infamous2-recomp`](https://github.com/bhuiyah/infamous2-recomp)** — *inFAMOUS 2* recompilation & SPURS/RSX research
- **[`resistance-recomp`](https://github.com/erikomis/resistance-recomp)** — *Resistance* recompilation & Insomniac/PS3 runtime patterns
- **[`Soulbrandt`](https://github.com/JonathanDC64/Soulbrandt)** — Community research and tooling contributions

---

## Legal
- This project is **not affiliated with, endorsed, or sponsored by Sony Interactive Entertainment or Insomniac Games.**
- *Ratchet & Clank*, *Ratchet & Clank: Into the Nexus*, *PlayStation*, *PS3*, *DualShock*, and *DualSense* are trademarks of Sony Interactive Entertainment; all other trademarks belong to their respective owners and are used solely to describe compatibility.
- **No game code, assets, Sony firmware, or decryption keys are included**, nor anything derived from them. This repository and its releases contain only original runtime/host code, build scripts, and documentation.
- **You must own the game** (`BCUS99245`) and dump/decrypt it yourself for your own personal use. `RecompBuilder.exe` translates your own copy locally on your PC. **Do not share** the built executable (`RecompFolder\NexusRecomp.exe`), lifted sources (`game\src\recomp\`, `game\spu\`), `elf\EBOOT.ELF`, or your `PS3_GAME` dump, as they contain the game's code or data.
- See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for third-party open-source licenses.
