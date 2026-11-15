# Technical Writeup — Project Raccoon: Into the Nexus (`v0.3`)

This document chronicles the complete engineering journey of **Project Raccoon (`v0.3`)** — from initial bring-up to a smooth, playable Windows x86-64 static recompilation of *Ratchet & Clank: Into the Nexus* (`BCUS99245` v1.00).

---

## 1. Recompilation Architecture & Toolchain Evolution

### 1.1 Static Lifting (`ps3recomp` + `build_pipeline.py`)
- **PPU (`14,728` functions):** `EBOOT.BIN` (`BCUS99245` v1.00, 15.5 MB ELF) is statically lifted into C++ (`game/src/recomp/ppu_recomp_*.cpp`) with VMX alias fixes, Insomniac post-lift patches, and HLE NID bindings.
- **SPU & SPURS (`207` jobs):** Lifts **3 embedded SPU ELFs**, **2 SPURS policy modules**, `igpm_rt`, and **207 `igPm` jobs + relocatable submodules** running on persistent host worker threads (`spurs_lane_thread`) with priority-aware `cellSpursModulePollStatus` and `HOST_PLACEMENT=auto` hybrid P/E-core & LLC topology detection (`lv2_register.c`).
- **RSX → Vulkan 1.3:** Guest RSX command buffers are drained by `rsx_draw_engine.c` and translated to Vulkan 1.3 by `rsx_vulkan_backend.c`, with an integrated SDL2/Vulkan overlay (`rsx_host_ui.c`, `F1` / `Esc`) and up to **4K (`3.00x`, `3840×2160`)** internal resolution scaling.

### 1.2 Toolchain Upgrade: Zig `-O3` → Clang 23.1.4 + PGO (`-march=haswell`)
- **Initial Limitation:** Early builds used Zig `0.16.0` (`-O3`), whose `zigc.lib` lacked `libclang_rt.profile` and hit undefined symbols (`frexpf`, `wmemchr`, `isnan`) under ThinLTO.
- **The Upgrade (`build-win-clang-pgo`):** Migrated the build from `zig cc` to **`LLVM-MinGW UCRT` (Clang 23.1.4)** with `-O3 -march=haswell` (**AVX2 + FMA + BMI + BMI2**) and **Profile-Guided Optimization (`-fprofile-use=pgo.profdata`)** trained on live gameplay (`-static` link, without ThinLTO).
- **CPU Guard & Placement:** Added a pre-CRT `racacit_cpu_ok()` CPUID check (`game/main.cpp`) matching `-march=haswell` (`x86-64-v3`), `SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)`, and an in-game F1 **CPU Profile** selector (`General` vs `Xeon / Multi-core`) paired with `HOST_PLACEMENT=auto` topology detection.
- **Impact:** Combined with the runtime optimizations below, Nebulox 7 benchmark performance climbed from **`~44.2 FPS`** (early Zig `-O3` baseline) to **`54.24–57.02 FPS`** (`avg_ft = 17.64–18.59 ms`).

---

## 2. Cross-Recompilation Lineage: What We Pulled, Kept & Improved

Rather than starting in a vacuum, we dissected five sibling PS3 static recompilations — **`RACiT`**, **`ratchet-tod-recomp`**, **`infamous2-recomp`**, **`resistance-recomp`**, and **`Soulbrandt`** — adopting what worked, discarding what didn't fit *Into the Nexus*, and improving the core runtime where our telemetry exposed bottlenecks:

| Source Project | What We Pulled / Studied | What Worked Out of the Box | What We Changed or Improved |
|---|---|---|---|
| **`RACiT`** (*A Crack in Time*) | Initial `sp00nznet/ps3recomp @ 5244db9` + `racacit-runtime-fixes` patch, `build_pipeline.py` structure, `game/` scaffolding (`spu_programs.h`, `vertex_layouts.h`, `gcm_command_replay.h`), `igPm` SPURS job extraction, and the Vulkan RSX backend. | Insomniac `igPm` job/submodule model (`207` jobs), SDL2/Vulkan overlay (`F1`), and `rsx_draw_engine.c` + `rsx_fp_decompiler.c` foundations. | Purged hardcoded *ACiT* guest addresses (`0x00880000`, `0x00FB34C0`, leftover `RACiT` hooks), fixed `0x00FB1840` New Game hang, fixed `SPU_RdMachStat` audio mixing, fixed `FP_SRC2_ABS` green door locks & `HLSL parse` colors, moved from `zig -O3` to **Clang 23.1.4 + PGO (`-march=haswell`)**, and added `func_004E9F98` (bolt cap) + `func_00784E20` (crate debris cap). |
| **`ratchet-tod-recomp`** (*Tools of Destruction*) | Newer `soyjxck/ps3recomp` runtime patterns, `HOST_PLACEMENT` hybrid P/E-core & LLC topology detection, `SPU_BRANCH_ALIGN`, and `rsx_draw_engine.c` updates. | `HOST_PLACEMENT=auto` (`lv2_register.c`) and `SPU_BRANCH_ALIGN` transferred cleanly and improved thread scheduling across hybrid Intel/AMD CPUs. | Kept and hardened **Vulkan 1.3** instead of switching to D3D12, fixing the Vulkan-specific `rsx_vk_zpass_clear` synchronous stall (`gpu_wait` `5.2–6.6 ms → 0.0 ms`), cutting `VK_BT_SLOTS` `6 → 4` (`-96 MB`), and adding the 80% VRAM clamp + 84% pressure flush. |
| **`infamous2-recomp`** (*inFAMOUS 2*) | `HOST_PLACEMENT` core isolation, `rsx_rthread.h` dedicated render-thread offload architecture, and `cellSpurs` / SPU worker scheduling patterns. | Core isolation concepts and SPU worker backoff patterns (`spu_lr_wait` `SleepConditionVariableSRW` + `MFC_LLR_LOST_EVENT` `YieldProcessor() + 1 ms`) cut wasted CPU spin. | Telemetry proved Vulkan `submit` was already `0.2 ms` (`gpu_wait = 0.0 ms`), so instead of rewriting around a D3D12 render-thread offload, we eliminated the *actual* CPU/RSX stalls: WriteWatch `8 ms` coalescing, `eng_geom_get` cap (`24`), and Morton texture decode optimization (`40 → 0` `[tex-slow]` hits). |
| **`resistance-recomp`** (*Resistance*) | `1 ms` `frame_clock` loop, `120 Hz` vblank decoupling, `GCM_PUT_LIVE` / command-stream handling, and `spu_channels.c` / `spu_drain.c` hot-path tuning. | `GCM_PUT_LIVE`, `spu_mem_ne` AVX2 vectorization, 64-entry `_Thread_local` SPU overlay cache, hoisted `w0/w1` checks, and power-of-2 `RING=32` bitmasking worked great. | Verified *Into the Nexus* is delta-time driven (`RACACIT_VBLANK_HZ=120` runs at `1.0×` speed cleanly) without `game.self` dual-binary split complexity, and combined it with `256 KB` `cellFsOpen` buffering + `RSX_PARK_WATCH` off. |
| **`Soulbrandt`** (*Demon's Souls*) | User-facing `setup` builder tooling (`tools/setup`), `clang-cl`/LLVM build pipeline ideas, and frame-pacing / vblank decoupling research. | Validated the value of a one-click user setup wizard and LLVM PGO/LTO optimization on lifted PPU/SPU C++ code. | Replaced Python-dependent setup scripts with **`tools/RecompBuilder.cpp` → `dist/RecompBuilder.exe`** (a zero-dependency static C++ Windows utility with native `IFileOpenDialog`, AVX2 CPUID check, and `IShellLinkW` shortcut creation), and kept `frame_clock` immediate-present without artificial pacing delays. |

---

## 3. Bring-Up, Audio & Shader Accuracy Fixes

- **Boot & `RACiT` Purge:** Fixed the `RSX_BACKEND_THREAD` boot freeze and stripped leftover `RACiT` fallback paths/hooks (`build_pipeline.py`, `main.cpp`, `stubs.cpp`) so the title screen and Start Menu boot cleanly.
- **New Game Hang:** Resolved the post-cutscene infinite retry stall at `0x00FB1840` when entering gameplay.
- **Full `cellAudio` Mixing:** Fixed `SPU_RdMachStat` bit 0 in `spu_channels.c`, unlocking 100% menu, FMV, and in-game audio mixing (previously cutscenes-only).
- **Black Door Locks → Bright Green (`rsx_fp_decompiler.c`):** Fixed `FP_SRC2_ABS` (`1u << 18`) in the RSX fragment program decompiler so unlocked door locks render bright green instead of black, alongside `ARL` (`dst_tmp & 1u`), `BRB`/`CLB`, `GCM_PUT_LIVE`, 28-bit vertex offset wrap, and `OP_LG2`/`OP_NRM` NaN guards.
- **HLSL Parse Color Bug (`rsx_fp_decompiler.c`):** Fixed a fragment shader translation error (`HLSL parse: ERROR`) that caused Vulkan to skip lighting/composite passes and leave raw G-buffer normals / corrupted colors on screen.
- **Occlusion Queries & Lens Flares (`rsx_be_proxy.c`):** Routed RSX occlusion queries (`0x17C8` / `0x17CC` / `0x1800`) through `rsx_be_proxy.c` (`VK_ZPASS_QUERIES=8192`, `VK_QUERY_RESULT_WAIT_BIT`), fixing missing/broken lens flares and sun occlusion.
- **UI Prompt Alpha / Masking:** Fixed `rsx_compact_be_f16()` to eliminate the semi-transparent bounding box around `□` and `△` interaction prompts.

---

## 4. Incremental FPS & Stutter Elimination (The Smoothness Pass)

### 4.1 WriteWatch (`ww`) Narrowing & Coalescing (`rsx_writewatch.c`, `cellGcmSys.c`)
- Removed the unused `0x40000000` range (`1,646` / `14M` checks), narrowed `0x20000000`/`0xC0000000` `GetWriteWatch` scans to `[s_range_lo..s_range_hi]` on 31 of 32 polls, and coalesced `rsx_ww_poll()` to at most once per `8 ms` (with `GCM_SEMA_WW_POLL=0`).
- **Impact:** WriteWatch overhead dropped from **`4.5–13.5 ms` (3×/frame)** down to **`1.3–2.1 ms` (1×/frame)**.

### 4.2 Eliminating the Synchronous ZPASS GPU Stall (`rsx_vulkan_backend.c`)
- **The Bottleneck:** When the RSX hit semaphore release `0x1D70` after `NV4097_GET_REPORT`, `rsx_be_zpass_flush()` / `rsx_vk_zpass_clear(0xFFFFFFFFu)` called `vk_bt_submit(1)` synchronously, stalling the RSX thread for **`5.2–6.6 ms/frame`** (`gpu_wait`).
- **The Fix:** Switched to non-blocking 1-frame-lagged / deferred ZPASS report submission (`gpu_wait: 5.2–6.6 ms → 0.0 ms`) while preserving `wait=1` query slot integrity so lens flares never flicker.
- **Impact:** Nebulox 7 benchmark jumped from **`54.24 FPS → 57.02 FPS`** (`min_fps 41.3 → 46.3`, `>35 ms = 0`, `>50 ms = 0`). Reduced `VK_BT_SLOTS` `6 → 4` (saving **96 MB** host-coherent RAM) and added pre-acquire scene submit (`+12.7%` standing FPS).

### 4.3 Streaming, Compaction & Texture Decode Hitch Fixes (`120–206 ms` → Gone)
- **Streaming / Compaction Hitches (`rsx_draw_engine.c`, `rsx_vulkan_backend.c`):** Disabled `RSX_PARK_WATCH` by default, enabled direct upload for tight compressed textures, raised the `PX_BIG` inline ring threshold to `4 MB`, and expanded `VK_BT_ARENA` to `64 MB`, eliminating `120–206 ms` streaming hitches.
- **Morton Texture Decode Hitches (`rsx_texture_layout.c`):** `[tex-slow]` profiling caught 40 `50–110 ms` `tx` hitches inside `rsx_texture_decode` doing per-texel Morton bit-interleaving on large textures. Optimizing the decode path dropped `[tex-slow]` and `[px-stall]` from **40 hits → 0 hits** (`tx` stayed at `1.1–1.8 ms`).
- **120-Frame Capture Hitch:** Fixed `RSX_ENG_STATS=0` env parsing (`"0"` was treated as truthy, triggering periodic `41–86 ms` 120-frame capture spikes).

### 4.4 RSX Draw Engine Hot-Path & PSO Prewarming (`rsx_draw_engine.c`)
- Capped `eng_geom_get()` promotions at `24` per frame (`bld` spikes dropped from **`5.0–8.7 ms` to `0.2–0.7 ms`**).
- Removed 4 redundant per-draw stack `memset`s (`textures`, `samplers`, `vtextures`, `vsamplers`), gated unconditional `ps3_qpc_us()` timers in `sink_end_inner`, skipped redundant target/pipe/viewport/vtex binds and duplicate `ps_constants` `memcmp`, and added a two-size `snapshot_spare` depth cache.
- Added `vk_plist_open` + `pipelinecache.bin` + `shadercache.bin` PSO prewarming, `256 KB` `_IOFBF` read buffer in `cellFsOpen` (`cellFs.c`), and `4×` shadow map resolution (`RACACIT_SHADOW_RES=4`, up to `2048×2048`) with zero FPS penalty.

---

## 5. Gameplay Entity & Debris Fixes (Bolt & Crate Fixes)

### 5.1 Dynamic Bolt Spawn Cap with Exact Value Scaling (`func_004E9F98`)
- **The Problem:** Breaking large crate stacks or defeating groups of enemies spawns bursts of 20–40+ individual bolt Mobys, causing PPU/SPU and draw-call spikes.
- **The Fix (`game/src/recomp/ppu_recomp_036.cpp` & `ppu_recomp_014.cpp`):** Inside `func_004E9F98`, physical bolt spawns are capped at **3 bolts per burst** while **scaling each spawned bolt's value** so the player receives the exact total bolt payout intended by the game.

### 5.2 Crate Debris Moby Burst Cap (`func_00784E20` / `lr=0x002C64A0`)
- **The Problem:** Smashing a single stack of wooden crates on the opening ship caused FPS to plummet to **8.5–14.9 FPS** (`117.6 ms` max frame time) while 50+ splintered 3D wooden crate planks flew through the air.
- **Root-Cause Investigation:**
  - `[frame-waits]` telemetry showed `igAnim` (`lr=0x00588B80`) — the SPU animation/skinning wait — spiking from **`31 µs/call` (`1.6 ms/s`)** in normal scenes to **`5,214 µs/call` (`171.9 ms/s`)** during crate destruction.
  - Instrumenting `func_00685088` (`MobySpawn`) and `func_005A75BC` (`FxSpawn`) proved `igPhysBreakable` (`func_007391E0`) was *not* the culprit (`0` clone calls). Instead, the burst came from `func_00784E20` (`CLevel::SpawnMoby` wrapper at `lr=0x00784E3C` calling `func_00685088`), invoked from `lr=0x002C64A0` with `cls=0x55`, spawning dozens of 3D plank Mobys in `<250 ms`.
- **The Fix (`game/src/recomp/ppu_recomp_021.cpp`):**
  - Added a level-table-aware per-class burst cap inside `func_00784E20`.
  - It monitors the active level's Moby class table pointer (`vm_read32(0x00F8D600)`) and leaves the first **2,000 ms** after level load untouched so level-init Mobys spawn normally.
  - During active gameplay, any single Moby class spawned more than **3 times within 350 ms** is capped (returning `0` cleanly, which `func_00784E20` and its callers already handle).
  - **Result:** Crate stacks smash cleanly with zero hang and no `igAnim` SPU stall.

---

## 6. SPU & SPURS Channel / Drain Optimizations
In `tools/ps3recomp/runtime/spu/spu_channels.c` and `spu_drain.c`:
- **AVX2 `spu_mem_ne`:** Vectorized SPU local-store dirty/change checks using AVX2 intrinsics.
- **64-Entry `_Thread_local` Overlay Cache:** Eliminated repeated global lookups for relocatable SPU overlay images across SPURS worker loops.
- **Hoisted `w0/w1` Checks & Guarded Restores:** Hoisted `w0/w1` checks and guarded `spu_tr_note` / `spu_irq_regs_maybe_restore` to cut per-channel hot-path overhead.
- **`spu_lr_wait` & `MFC_LLR_LOST_EVENT` Backoff:** Lowered `spu_ch_wait` `MFC_LLR_LOST_EVENT` (`0x400`) sleep from `10 ms` to `YieldProcessor()` + `1 ms`, and added `SleepConditionVariableSRW` backoff in `spu_lr_wait` after 4 timeouts instead of busy-spinning.
- **Power-of-2 Ring & Lockstep Guard:** Replaced modulo indexing with bitmasking (`RING=32`) and guarded `yz_lockstep_tick`.

---

## 7. RSX VRAM Budget & 4K Cap
- **The Problem:** At `4.00x` internal resolution (`5120×2880`, 5K — 16× native 720p pixel count), `rsx_draw_engine.c` scaled `rt_budget` and `z_budget` to `3,072 MB` each (`6,144 MB` hard limit each). Combined with `2,048 MB` texture budget, total hard limits reached `14,336 MB`, exceeding a 16 GB GPU's `14,021 MB` Vulkan budget and triggering `VkResult -2` (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) + `1,000+ ms` emergency GPU waits.
- **The Fix:**
  - **`tools/ps3recomp/libs/video/rsx_draw_engine.c`:** Clamped `rt_budget` and `z_budget` against `g_rsx_vram_budget_mb` so `tex_budget + 2*rt_budget + 2*z_budget <= 80%` of card VRAM.
  - **`tools/ps3recomp/libs/video/rsx_vulkan_backend.c`:** Lowered the proactive VRAM pressure trigger from `95%` (every 5s) to **`84%` (every 1s)** so stale targets/textures evict smoothly before an allocation ever fails.
  - **`tools/ps3recomp/libs/video/rsx_host_ui.c` & `rsx_vulkan_backend.c`:** Removed `5K (4.00x)` and capped internal resolution at **`4K (3.00x`, `3840×2160`)**, cutting pixel load by 44% while keeping image quality sharp.

---

## 8. Credits
This project stands on the shoulders of the PS3 static recompilation community:
- **`ps3recomp`** — Core PS3 PPU/SPU/RSX static recompilation runtime and tooling
- **`RACiT`** — *Ratchet & Clank: A Crack in Time* recompilation research & runtime foundations
- **`ratchet-tod-recomp`** — *Ratchet & Clank: Tools of Destruction* recompilation insights
- **`infamous2-recomp`** — *inFAMOUS 2* recompilation & SPURS/RSX research
- **`resistance-recomp`** — *Resistance* recompilation & Insomniac/PS3 runtime patterns
- **`Soulbrandt`** — Community research and tooling contributions
