#!/usr/bin/env python3
"""
End-to-end automated static recompilation & build pipeline for:
  Ratchet & Clank: Into the Nexus (BCUS99245 v1.00) -> Windows x86-64 (Xeon E5-2680 v4 AVX2/FMA3 + RX 9060 XT 4K/60)

Runs all stages in a single script so no interactive shell prompts are needed:
  Stage 1: Prepare patched ps3recomp SDK + external libs (Vulkan-Headers, glslang, zlib, SDL2)
  Stage 2: Lift PPU EBOOT.ELF (14,728 functions) + VMX alias fix + Insomniac PPU post-lift patches + HLE NIDs
  Stage 3: Lift SPU workloads (3 embedded SPU ELFs, 2 SPURS policy modules, igpm_rt, 207 igPm jobs & relocatable submodules)
  Stage 4: Generate game sources + toolchain & compile NexusRecomp.exe with zig cc (Broadwell AVX2/FMA3)
  Stage 5: Stage EBOOT.elf + settings.txt for zero-arg launch and run initial boot verification
"""

import concurrent.futures
import glob
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile

ROOT = r"c:\coding\nexus-recomp"
REPOS = r"c:\coding\repos"
VENV_SCRIPTS = r"c:\coding\.venv\Scripts"
PYTHON_EXE = os.path.join(VENV_SCRIPTS, "python.exe")
CMAKE_EXE = os.path.join(VENV_SCRIPTS, "cmake.exe")
NINJA_EXE = os.path.join(VENV_SCRIPTS, "ninja.exe")
ZIG_EXE = r"c:\coding\.venv\Lib\site-packages\ziglang\zig.exe"

GAME_ROOT = r"C:\coding\Put_PS3_Game_Here\Ratchet & Clank - Into the Nexus (USA) (En,Fr,Es)"
ELF_PATH = os.path.join(ROOT, "elf", "EBOOT.ELF")
OUT_DIR = os.path.join(ROOT, "out")
TOOLS_DIR = os.path.join(ROOT, "tools")
PS3R_DIR = os.path.join(TOOLS_DIR, "ps3recomp")
PS3R_TOOLS = os.path.join(PS3R_DIR, "tools")
GAME_DIR = os.path.join(ROOT, "game")
RECOMP_DIR = os.path.join(GAME_DIR, "src", "recomp")
SPU_DIR = os.path.join(GAME_DIR, "spu")
WIN_DIR = os.path.join(ROOT, "windows")
BUILD_DIR = os.path.join(ROOT, "build-win")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def apply_unified_patch_py(patch_path, target_dir):
    """Pure-Python unified diff applier that handles LF/CRLF transparently."""
    with open(patch_path, "rb") as f:
        raw_patch = f.read().replace(b"\r\n", b"\n").decode("utf-8", errors="replace")

    lines = raw_patch.split("\n")
    i = 0
    n = len(lines)
    files_patched = 0

    while i < n:
        line = lines[i]
        if not line.startswith("diff --git "):
            i += 1
            continue

        # Parse diff header
        m = re.match(r"diff --git a/(.*?) b/(.*?)$", line)
        if not m:
            i += 1
            continue
        rel_path = m.group(2)
        i += 1

        is_new = False
        is_deleted = False
        while i < n and not lines[i].startswith("@@ ") and not lines[i].startswith("diff --git "):
            if lines[i].startswith("new file mode"):
                is_new = True
            elif lines[i].startswith("deleted file mode"):
                is_deleted = True
            i += 1

        full_path = os.path.join(target_dir, rel_path.replace("/", os.sep))
        if is_deleted:
            if os.path.exists(full_path):
                os.remove(full_path)
            while i < n and not lines[i].startswith("diff --git "):
                i += 1
            files_patched += 1
            continue

        if is_new or not os.path.exists(full_path):
            orig_lines = []
        else:
            with open(full_path, "rb") as f:
                content = f.read().replace(b"\r\n", b"\n").decode("utf-8", errors="replace")
            if content.endswith("\n"):
                orig_lines = content[:-1].split("\n")
            elif content == "":
                orig_lines = []
            else:
                orig_lines = content.split("\n")

        # Apply hunks
        offset = 0
        while i < n and lines[i].startswith("@@ "):
            hm = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", lines[i])
            if not hm:
                raise RuntimeError(f"Malformed hunk header in {rel_path}: {lines[i]}")
            old_start = int(hm.group(1))
            i += 1

            old_chunk = []
            new_chunk = []
            while i < n and not lines[i].startswith("@@ ") and not lines[i].startswith("diff --git "):
                hl = lines[i]
                if hl == r"\ No newline at end of file":
                    i += 1
                    continue
                if hl == "" and i == n - 1:
                    i += 1
                    break
                prefix = hl[0] if hl else " "
                body = hl[1:] if hl else ""
                if prefix == " ":
                    old_chunk.append(body)
                    new_chunk.append(body)
                elif prefix == "-":
                    old_chunk.append(body)
                elif prefix == "+":
                    new_chunk.append(body)
                else:
                    break
                i += 1

            pos = max(0, old_start - 1 + offset)
            if not old_chunk:
                orig_lines[pos:pos] = new_chunk
                offset += len(new_chunk)
            else:
                # Locate old_chunk near pos
                found_at = None
                if orig_lines[pos : pos + len(old_chunk)] == old_chunk:
                    found_at = pos
                else:
                    # Search nearby window first, then full file
                    for delta in range(-200, 201):
                        cand = pos + delta
                        if 0 <= cand <= len(orig_lines) - len(old_chunk):
                            if orig_lines[cand : cand + len(old_chunk)] == old_chunk:
                                found_at = cand
                                break
                    if found_at is None:
                        # Strip trailing whitespace fallback
                        old_stripped = [x.rstrip() for x in old_chunk]
                        for cand in range(len(orig_lines) - len(old_chunk) + 1):
                            if [x.rstrip() for x in orig_lines[cand : cand + len(old_chunk)]] == old_stripped:
                                found_at = cand
                                break
                if found_at is None:
                    raise RuntimeError(
                        f"Failed to apply hunk in {rel_path} around line {old_start} (first line: {old_chunk[0]!r})"
                    )
                orig_lines[found_at : found_at + len(old_chunk)] = new_chunk
                offset = found_at - (old_start - 1) + (len(new_chunk) - len(old_chunk))

        os.makedirs(os.path.dirname(full_path), exist_ok=True)
        with open(full_path, "wb") as f:
            f.write(("\n".join(orig_lines) + "\n").encode("utf-8"))
        files_patched += 1

    return files_patched


def download_and_extract_zip(url, dest_dir):
    if os.path.isdir(dest_dir) and os.listdir(dest_dir):
        log(f"  [cached] {os.path.basename(dest_dir)}")
        return
    log(f"  Downloading {url} -> {dest_dir} ...")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = resp.read()
    tmp_extract = dest_dir + "_tmp"
    shutil.rmtree(tmp_extract, ignore_errors=True)
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        zf.extractall(tmp_extract)
    entries = os.listdir(tmp_extract)
    inner = os.path.join(tmp_extract, entries[0]) if len(entries) == 1 else tmp_extract
    if os.path.exists(dest_dir):
        shutil.rmtree(dest_dir)
    shutil.move(inner, dest_dir)
    shutil.rmtree(tmp_extract, ignore_errors=True)


def stage1_prepare_sdk_and_deps():
    log("=== STAGE 1: Preparing Patched ps3recomp SDK & Dependencies ===")
    os.makedirs(TOOLS_DIR, exist_ok=True)

    marker = os.path.join(PS3R_DIR, ".racacit_patched")
    if not os.path.exists(marker):
        if os.path.exists(PS3R_DIR):
            shutil.rmtree(PS3R_DIR)
        log("Downloading sp00nznet/ps3recomp @ 5244db92669b54831c6d3ebb7a96d58c4dbfa6df ...")
        download_and_extract_zip(
            "https://github.com/sp00nznet/ps3recomp/archive/5244db92669b54831c6d3ebb7a96d58c4dbfa6df.zip",
            PS3R_DIR,
        )
        patch_file = os.path.join(REPOS, "RACiT", "patches", "ps3recomp-0001-racacit-runtime-fixes.patch")
        log("Applying ps3recomp-0001-racacit-runtime-fixes.patch ...")
        count = apply_unified_patch_py(patch_file, PS3R_DIR)
        with open(marker, "w") as f:
            f.write(f"patched {count} files\n")
        log(f"Successfully patched {count} files in ps3recomp!")
    else:
        log("ps3recomp already patched.")

    # Download exact pinned dependencies used by RACiT Windows build
    deps = [
        ("Vulkan-Headers", "https://github.com/KhronosGroup/Vulkan-Headers/archive/c46850864f4661461b0f6cb9922c058ffea4915e.zip"),
        ("glslang-src", "https://github.com/KhronosGroup/glslang/archive/2fbed5e1fb43a1c480fe020e3bb686a31010989a.zip"),
        ("zlib", "https://github.com/madler/zlib/archive/767c4c947852e143f582c85f14cf573411df1b35.zip"),
        ("SDL2-src", "https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.32.10.zip"),
    ]
    for name, url in deps:
        download_and_extract_zip(url, os.path.join(TOOLS_DIR, name))

    # Write zig toolchain files in windows/toolchain/
    tc_dir = os.path.join(WIN_DIR, "toolchain")
    os.makedirs(tc_dir, exist_ok=True)
    zig_Esc = ZIG_EXE.replace("\\", "/")
    with open(os.path.join(tc_dir, "zig-windows.cmake"), "w") as f:
        f.write(f"""set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(ZIG_EXE "{zig_Esc}")
set(CMAKE_C_COMPILER   "${{ZIG_EXE}}" cc  -target x86_64-windows-gnu)
set(CMAKE_CXX_COMPILER "${{ZIG_EXE}}" c++ -target x86_64-windows-gnu)
set(CMAKE_RC_COMPILER "{tc_dir.replace(chr(92), '/')}/zig-rc.cmd")
set(CMAKE_AR     "{tc_dir.replace(chr(92), '/')}/zig-ar.cmd")
set(CMAKE_RANLIB "{tc_dir.replace(chr(92), '/')}/zig-ranlib.cmd")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
""")
    for tool in ("rc", "ar", "ranlib"):
        with open(os.path.join(tc_dir, f"zig-{tool}.cmd"), "w") as f:
            f.write(f'@echo off\r\n"{ZIG_EXE}" {tool} %*\r\n')

    # Build static SDL2 if not already built
    sdl2_lib = os.path.join(TOOLS_DIR, "sdl2-win", "lib", "libSDL2.a")
    if not os.path.exists(sdl2_lib):
        log("Building static SDL2 with zig cc ...")
        sdl2_build = os.path.join(TOOLS_DIR, "sdl2-win-build")
        sdl2_install = os.path.join(TOOLS_DIR, "sdl2-win")
        env = os.environ.copy()
        env["PATH"] = VENV_SCRIPTS + os.pathsep + env.get("PATH", "")
        subprocess.run(
            [
                CMAKE_EXE,
                "-S", os.path.join(TOOLS_DIR, "SDL2-src"),
                "-B", sdl2_build,
                "-G", "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={NINJA_EXE}",
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(tc_dir, 'zig-windows.cmake')}",
                f"-DCMAKE_INSTALL_PREFIX={sdl2_install}",
                "-DSDL_SHARED=OFF",
                "-DSDL_STATIC=ON",
                "-DSDL_TEST=OFF",
            ],
            env=env,
            check=True,
        )
        subprocess.run(
            [CMAKE_EXE, "--build", sdl2_build, "--target", "install", "-j14"],
            env=env,
            check=True,
        )
        log("SDL2 static build complete!")
    else:
        log("SDL2 static library already built.")


def patch_nexus_ppu_lift(recomp_dir):
    """
    Scan lifted PPU C++ files for generic Insomniac Engine patterns:
      1. Guest setjmp/longjmp (Lua error recovery)
      2. Hoisted RSX GCM ctrl->get (+0x4) before spin-wait label while ctrl->put (+0x0) is read inside
      3. 2.5s GPU sit-and-spin watchdog constant (0x0BE420E0 -> 0x5F2EDC00)
    """
    decls = '''
/* nexus: guest setjmp/longjmp support */
#include "ps3_setjmp.h"
typedef struct RacJbFrame { jmp_buf env; uint32_t guest_buf; struct RacJbFrame* prev; } RacJbFrame;
extern "C" void racacit_jb_push(RacJbFrame* f, uint32_t guest_buf);
extern "C" void racacit_jb_pop(RacJbFrame* f);
extern "C" void racacit_longjmp(uint32_t guest_buf);
'''
    cpp_files = sorted(glob.glob(os.path.join(recomp_dir, "ppu_recomp_*.cpp")))

    # Pass 1: guest setjmp and longjmp functions in Into the Nexus (BCUS99245)
    setjmp_fn = "func_00A084CC"
    longjmp_fn = "func_00A085F0"
    fn_re = re.compile(r"void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\) \{\n(.*?)\n\}\n", re.DOTALL)

    log(f"  Detected guest setjmp={setjmp_fn}, longjmp={longjmp_fn}")

    # Pass 2: apply patches across all chunks
    patched_files = 0
    hoisted_get_fixes = 0
    watchdog_fixes = 0
    setjmp_callers_patched = 0

    hoisted_re = re.compile(
        r"(        ctx->gpr\[(\d+)\] = vm_read32\(ctx->gpr\[(\d+)\] \+ 0x4\);\n)"
        r"(loc_[0-9A-Fa-f]{8}:\n)"
        r"((?:        [^\n]+\n){1,10}?        ctx->gpr\[\d+\] = vm_read32\(ctx->gpr\[\3\] \+ 0x0\);\n)"
    )

    for p in cpp_files:
        src = open(p, "r", encoding="latin-1").read()
        changed = False
        needs_jb_decls = False

        # 1a. Patch longjmp function itself
        if longjmp_fn and f"void {longjmp_fn}(ppu_context* ctx)" in src:
            for m in fn_re.finditer(src):
                if m.group(1) == longjmp_fn and "racacit_longjmp" not in m.group(2):
                    fn_full = m.group(0)
                    fn_new = fn_full.replace(
                        f"void {longjmp_fn}(ppu_context* ctx) {{\n",
                        f"void {longjmp_fn}(ppu_context* ctx) {{\n        uint32_t _jb = (uint32_t)ctx->gpr[3];\n",
                        1,
                    )
                    first_ret = fn_new.index("        return;")
                    fn_new = fn_new[:first_ret] + "        racacit_longjmp(_jb);\n" + fn_new[first_ret:]
                    src = src[: m.start()] + fn_new + src[m.end() :]
                    changed = True
                    needs_jb_decls = True
                    break

        # 1b. Patch callers of setjmp_fn (e.g., luaD_rawrunprotected)
        if setjmp_fn and f"{setjmp_fn}(ctx);" in src:
            new_parts = []
            last_idx = 0
            for m in fn_re.finditer(src):
                fname, body = m.group(1), m.group(2)
                if f"{setjmp_fn}(ctx); DRAIN_TRAMPOLINE(ctx);" in body and "racacit_jb_push" not in body:
                    call_re = re.compile(
                        rf"(ctx->lr = 0x[0-9A-Fa-f]+; {setjmp_fn}\(ctx\); DRAIN_TRAMPOLINE\(ctx\);)"
                    )
                    if call_re.search(body) and body.count("        return;") == 1:
                        fn_text = m.group(0)
                        fn_text = fn_text.replace(
                            f"void {fname}(ppu_context* ctx) {{\n",
                            f"void {fname}(ppu_context* ctx) {{\n        RacJbFrame _jbf;\n",
                            1,
                        )
                        fn_text = call_re.sub(
                            lambda cm: (
                                cm.group(1).split(";")[0]
                                + ";\n        racacit_jb_push(&_jbf, (uint32_t)ctx->gpr[3]);\n"
                                + f"        if (setjmp(_jbf.env) == 0) {{ {setjmp_fn}(ctx); DRAIN_TRAMPOLINE(ctx); }}\n"
                                + "        else { g_trampoline_fn = 0; }"
                            ),
                            fn_text,
                            count=1,
                        )
                        tail = fn_text.rindex("        return;")
                        fn_text = fn_text[:tail] + "        racacit_jb_pop(&_jbf);\n" + fn_text[tail:]
                        new_parts.append(src[last_idx : m.start()])
                        new_parts.append(fn_text)
                        last_idx = m.end()
                        changed = True
                        needs_jb_decls = True
                        setjmp_callers_patched += 1
            if needs_jb_decls and new_parts:
                new_parts.append(src[last_idx:])
                src = "".join(new_parts)

        # 2. Hoisted GCM get fix disabled: regex matched C++ hash tables (Scaleform, etc.) and broke them
        # (GCM FIFO get/put spin loops are handled in HLE cellGcmSys.c instead)

        # 3. Extend 2.5s GPU watchdog timeout (0x0BE420E0 -> 0x5F2EDC00)
        if "(uint32_t)0xBE4 << 16" in src and "| 0x20E0" in src:
            src = src.replace("(uint32_t)0xBE4 << 16", "(uint32_t)0x5F2E << 16").replace(
                "| 0x20E0", "| 0xDC00"
            )
            watchdog_fixes += 1
            changed = True

        if needs_jb_decls and "nexus: guest setjmp/longjmp support" not in src:
            inc_pos = src.index("\n", src.index("#include")) + 1
            src = src[:inc_pos] + decls + src[inc_pos:]
            changed = True

        if changed:
            with open(p, "w", encoding="latin-1") as f:
                f.write(src)
            patched_files += 1

    log(
        f"  PPU post-lift patches applied to {patched_files} file(s): "
        f"setjmp callers={setjmp_callers_patched}, hoisted GCM get={hoisted_get_fixes}, GPU watchdog={watchdog_fixes}"
    )


def stage2_lift_ppu():
    log("=== STAGE 2: Lifting PPU Code (14,728 functions) ===")
    os.makedirs(RECOMP_DIR, exist_ok=True)
    header_path = os.path.join(RECOMP_DIR, "ppu_recomp.h")
    if not os.path.exists(header_path):
        env = os.environ.copy()
        env["PS3RECOMP_CHUNK_LINES"] = "100000"
        log("Running ppu_lifter.py (chunk size: 100,000 lines) ...")
        subprocess.run(
            [
                PYTHON_EXE,
                "-I",
                os.path.join(PS3R_TOOLS, "ppu_lifter.py"),
                ELF_PATH,
                "--functions", os.path.join(OUT_DIR, "EBOOT.functions.json"),
                "--hle-stubs", os.path.join(OUT_DIR, "EBOOT.imports.json"),
                "--output", RECOMP_DIR,
            ],
            env=env,
            check=True,
        )
    else:
        log("PPU code already lifted in game/src/recomp.")

    nid_cpp = os.path.join(RECOMP_DIR, "ppu_hle_nids.cpp")
    if not os.path.exists(nid_cpp):
        log("Running Insomniac PPU post-lift patcher ...")
        patch_nexus_ppu_lift(RECOMP_DIR)
    log("Generating HLE NID table (ppu_hle_nids.cpp) ...")
    subprocess.run(
        [
            PYTHON_EXE,
            "-I",
            os.path.join(PS3R_TOOLS, "gen_hle_nids.py"),
            "--out", nid_cpp,
            "--all",
        ],
        check=True,
    )


def fnv1a64_spu_workload(data):
    """Matches runtime/spu/spu_workload.c (offset basis 1469598103934665603)."""
    h = 1469598103934665603
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_elf_load_segs(d):
    phoff, = struct.unpack(">Q", d[0x20:0x28])
    phentsize, phnum = struct.unpack(">HH", d[0x36:0x3A])
    out = []
    for i in range(phnum):
        p = d[phoff + i * phentsize : phoff + (i + 1) * phentsize]
        ptype, _, off, vaddr, _, filesz, _, _ = struct.unpack(">IIQQQQQQ", p)
        if ptype == 1 and filesz:
            out.append((vaddr, off, filesz))
    return out


def read_guest_bytes(d, segs, ea, size):
    for va, off, fs in segs:
        if va <= ea and ea + size <= va + fs:
            return d[off + ea - va : off + ea - va + size]
    return None


def lift_one_spu_job(args):
    """Worker for parallel SPU job lifting."""
    idx, kind, ea, size, base, raw, extra_funcs, use_regions, use_reloc, out_raw, spu_dir = args
    name = f"{kind}_{ea:08X}" if ea >= 0x1000000 else f"{kind}_{ea:06X}"
    dst = os.path.join(spu_dir, name)
    c_file = os.path.join(dst, "spu_recomp.c")
    prefix = name + "_"

    if not os.path.exists(c_file):
        os.makedirs(dst, exist_ok=True)
        bin_p = os.path.join(out_raw, name + ".bin")
        elf_p = os.path.join(out_raw, name + ".elf")
        with open(bin_p, "wb") as f:
            f.write(raw)
        subprocess.run(
            [
                PYTHON_EXE,
                "-I",
                os.path.join(PS3R_TOOLS, "wrap_spu_elf.py"),
                bin_p,
                "--out", elf_p,
                "--base", hex(base),
                "--entry", hex(base),
            ],
            check=True,
            capture_output=True,
        )
        cmd = [
            PYTHON_EXE,
            "-I",
            os.path.join(PS3R_TOOLS, "spu_lifter.py"),
            "--auto-functions",
            elf_p,
            "--base", hex(base),
            "--symbol-prefix", prefix,
            "--output", dst,
        ]
        if extra_funcs:
            cmd += ["--extra-funcs", ",".join(hex(x) for x in sorted(extra_funcs))]
        if use_regions:
            cmd.append("--regions")
        if use_reloc:
            cmd.append("--reloc")

        r = subprocess.run(cmd, capture_output=True, text=True)
        with open(os.path.join(out_raw, name + ".lift.log"), "w", encoding="latin-1") as f:
            f.write(r.stdout + r.stderr)
        if r.returncode != 0 or not os.path.exists(c_file):
            return (idx, name, ea, size, base, prefix, False)

        with open(c_file, "r", encoding="latin-1") as f:
            src = f.read()
        for h in ("spu_context.h", "spu_helpers.h"):
            src = src.replace(f'"../../runtime/spu/{h}"', f'"{h}"')
        with open(c_file, "w", encoding="latin-1") as f:
            f.write(src)

    return (idx, name, ea, size, base, prefix, True)


def write_if_changed(path, text):
    if os.path.exists(path):
        try:
            if open(path, "r", encoding="latin-1").read() == text:
                return
        except OSError:
            pass
    with open(path, "w", encoding="latin-1") as f:
        f.write(text)


def stage3_lift_spu():
    log("=== STAGE 3: Lifting All SPU Workloads, Policy Modules & Jobs ===")
    os.makedirs(SPU_DIR, exist_ok=True)
    elf_bytes = open(ELF_PATH, "rb").read()
    segs = parse_elf_load_segs(elf_bytes)

    # 3A. Lift the 3 embedded SPU ELFs via build_spu_workloads.py
    workloads_c = os.path.join(SPU_DIR, "spu_workloads.c")
    spu_images_dir = os.path.join(OUT_DIR, "spu_images")
    if not os.path.exists(workloads_c):
        log("Lifting embedded SPU ELF images via build_spu_workloads.py ...")
        subprocess.run(
            [
                PYTHON_EXE,
                "-I",
                os.path.join(PS3R_TOOLS, "build_spu_workloads.py"),
                "--images", spu_images_dir,
                "--lifted", SPU_DIR,
                "--out", workloads_c,
                "--register-fn", "nexus_spu_register_all",
                "--constructor",
                "--title", "nexus",
            ],
            check=True,
        )

    # Discover exact guest EA, size, and LS base of the 3 embedded SPU ELFs for WWS staged overlays
    elf_overlays = []
    spu_elf_files = sorted(glob.glob(os.path.join(spu_images_dir, "*.elf")))
    for img_idx, spu_elf_p in enumerate(spu_elf_files, start=1):
        # Filename is spu_XXXX_at_OOOOOOOO.elf where OOOOOOOO is file offset in EBOOT.ELF
        m = re.search(r"_at_([0-9A-Fa-f]+)\.elf$", os.path.basename(spu_elf_p))
        if not m:
            continue
        file_off = int(m.group(1), 16)
        sd = open(spu_elf_p, "rb").read()
        e_phoff, = struct.unpack(">I", sd[28:32])
        e_phentsize, e_phnum = struct.unpack(">HH", sd[42:46])
        for pi in range(e_phnum):
            ph = sd[e_phoff + pi * e_phentsize : e_phoff + (pi + 1) * e_phentsize]
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack(">6I", ph[:24])
            if p_type == 1 and p_filesz > 0 and p_vaddr >= 0x100:
                # Convert EBOOT file offset (file_off + p_offset) to guest EA
                for va, seg_off, seg_fs in segs:
                    if seg_off <= file_off + p_offset < seg_off + seg_fs:
                        guest_ea = va + (file_off + p_offset - seg_off)
                        elf_overlays.append((img_idx, guest_ea, p_filesz, p_vaddr))
                        log(
                            f"  SPU ELF overlay #{img_idx} ({os.path.basename(spu_elf_p)}): "
                            f"guest EA 0x{guest_ea:08X}, size 0x{p_filesz:X}, LS base 0x{p_vaddr:X}"
                        )
                        break
                break

    # 3B. Lift SPURS Policy Modules (pm_wwsjob at guest VA 0x00D3D680, pm_igpm at guest VA 0x00C14800)
    pm_out_raw = os.path.join(OUT_DIR, "policy_modules")
    os.makedirs(pm_out_raw, exist_ok=True)
    pm_specs = [
        # (name, guest_va, max_lift_size, min_fp_size, max_fp_size, image_id)
        ("pm_wwsjob", 0x00D3D680, 0x3100, 0x30C0, 0x3100, 100),
        ("pm_igpm",   0x00C14800, 0x230,  0x200,  0x280,  101),
    ]
    pm_decls = []
    pm_regs = []
    for name, gva, lift_sz, min_sz, max_sz, img_id in pm_specs:
        raw_fp = read_guest_bytes(elf_bytes, segs, gva, max_sz)
        raw = raw_fp[:lift_sz]
        bin_p = os.path.join(pm_out_raw, name + ".bin")
        elf_p = os.path.join(pm_out_raw, name + ".elf")
        with open(bin_p, "wb") as f:
            f.write(raw)
        subprocess.run(
            [
                PYTHON_EXE,
                "-I",
                os.path.join(PS3R_TOOLS, "wrap_spu_elf.py"),
                bin_p,
                "--out", elf_p,
                "--base", "0xA00",
                "--entry", "0xA00",
            ],
            check=True,
            capture_output=True,
        )
        dst = os.path.join(SPU_DIR, name)
        os.makedirs(dst, exist_ok=True)
        prefix = name + "_"
        subprocess.run(
            [
                PYTHON_EXE,
                "-I",
                os.path.join(PS3R_TOOLS, "spu_lifter.py"),
                "--auto-functions",
                elf_p,
                "--base", "0xA00",
                "--symbol-prefix", prefix,
                "--output", dst,
            ],
            check=True,
            capture_output=True,
        )
        c_file = os.path.join(dst, "spu_recomp.c")
        src = open(c_file, "r", encoding="latin-1").read()
        for h in ("spu_context.h", "spu_helpers.h"):
            src = src.replace(f'"../../runtime/spu/{h}"', f'"{h}"')
        open(c_file, "w", encoding="latin-1").write(src)

        entry_fn = f"{prefix}spu_func_00000A00"
        pm_decls.append(f"extern void {entry_fn}(spu_context*);")
        pm_decls.append(f"extern void {prefix}spu_recomp_register(void);")
        pm_regs.append(f"    spu_begin_image({img_id}); {prefix}spu_recomp_register();")
        # Register FNV-1a-64 fingerprints for every 16-byte-aligned size in [min_sz..max_sz]
        # so cellSpursAddWorkload matches regardless of exact sizePm padding!
        for sz in range(min_sz, max_sz + 1, 16):
            fp = fnv1a64_spu_workload(raw_fp[:sz])
            pm_regs.append(f'    spu_workload_register_img(0x{fp:016X}ULL, {entry_fn}, {img_id}, "{name}");')
        log(f"  Lifted policy module {name} (VA 0x{gva:08X}, registered sizes 0x{min_sz:X}..0x{max_sz:X} -> image {img_id})")

    write_if_changed(
        os.path.join(SPU_DIR, "policy_modules.c"),
        '/* Generated by build_pipeline.py -- raw SPURS policy modules */\n'
        '#include "spu_workload.h"\n\n'
        'extern void spu_begin_image(int image_id);\n\n'
        + "\n".join(pm_decls)
        + "\n\nvoid racacit_pm_register_all(void)\n{\n"
        + "\n".join(pm_regs)
        + "\n    spu_begin_image(0);\n}\n\n"
        "__attribute__((constructor)) static void racacit_pm_register_ctor(void)\n"
        "{\n    racacit_pm_register_all();\n}\n",
    )

    # 3C. Lift igpm_rt (guest VA 0x00C14A80, size 0x580, LS base 0x3F580, image 150)
    jobs_out_raw = os.path.join(OUT_DIR, "spu_jobs")
    os.makedirs(jobs_out_raw, exist_ok=True)
    igpm_rt_va = 0x00C14A80
    igpm_rt_sz = 0x580
    igpm_rt_base = 0x3F580
    igpm_rt_img = 150
    igpm_raw = read_guest_bytes(elf_bytes, segs, igpm_rt_va, igpm_rt_sz)
    igpm_bin = os.path.join(jobs_out_raw, "igpm_rt.bin")
    igpm_elf = os.path.join(jobs_out_raw, "igpm_rt.elf")
    open(igpm_bin, "wb").write(igpm_raw)
    subprocess.run(
        [
            PYTHON_EXE,
            "-I",
            os.path.join(PS3R_TOOLS, "wrap_spu_elf.py"),
            igpm_bin,
            "--out", igpm_elf,
            "--base", hex(igpm_rt_base),
            "--entry", hex(igpm_rt_base),
        ],
        check=True,
        capture_output=True,
    )
    igpm_dst = os.path.join(SPU_DIR, "igpm_rt")
    os.makedirs(igpm_dst, exist_ok=True)
    subprocess.run(
        [
            PYTHON_EXE,
            "-I",
            os.path.join(PS3R_TOOLS, "spu_lifter.py"),
            "--auto-functions",
            igpm_elf,
            "--base", hex(igpm_rt_base),
            "--symbol-prefix", "igpm_rt_",
            "--output", igpm_dst,
            "--extra-funcs", "0x3F5A0,0x3F5C0",
            "--code-end", "0x3FA00",
        ],
        check=True,
        capture_output=True,
    )
    igpm_c = os.path.join(igpm_dst, "spu_recomp.c")
    src = open(igpm_c, "r", encoding="latin-1").read()
    for h in ("spu_context.h", "spu_helpers.h"):
        src = src.replace(f'"../../runtime/spu/{h}"', f'"{h}"')
    # Apply patch_igpm_clear optimization (Into the Nexus LS-clear loop at 0x3F8B8..0x3F8C0)
    old_clear = (
        "void igpm_rt_spu_func_0003F8C0(spu_context* SPU_RESTRICT ctx) {\n"
        "        ctx->gpr[4] = spu_ai(ctx->gpr[4], -1);\n"
        "        ctx->gpr[56] = spu_ceqi(ctx->gpr[4], -1);\n"
        "        if (ctx->gpr[56]._u32[0] == 0) { ctx->pc = 0x3F8B8; g_spu_trampoline_fn = igpm_rt_spu_func_0003F8B8; return; }\n"
    )
    new_clear = (
        "void igpm_rt_spu_func_0003F8C0(spu_context* SPU_RESTRICT ctx) {\n"
        "        for (;;) {\n"
        "            ctx->gpr[4] = spu_ai(ctx->gpr[4], -1);\n"
        "            ctx->gpr[56] = spu_ceqi(ctx->gpr[4], -1);\n"
        "            if (ctx->gpr[56]._u32[0] != 0) break;\n"
        "            spu_ls_write128(ctx, ctx->gpr[3]._u32[0] + 0x0, ctx->gpr[5]);\n"
        "            ctx->gpr[3] = spu_ai(ctx->gpr[3], 16);\n"
        "        }\n"
    )
    if old_clear in src:
        src = src.replace(old_clear, new_clear, 1)
        log("  Applied fast LS-clear loop optimization to igpm_rt!")
    open(igpm_c, "w", encoding="latin-1").write(src)

    write_if_changed(
        os.path.join(SPU_DIR, "spu_extra.c"),
        "/* Generated by build_pipeline.py -- igpm_rt at LS 0x3F580 */\n"
        "extern void spu_begin_image(int image_id);\n"
        "extern void spu_register_overlay_image(int image_id, unsigned base, const unsigned char* bytes, unsigned size);\n"
        "extern unsigned char* vm_base;\n\n"
        "extern void igpm_rt_spu_recomp_register(void);\n\n"
        "__attribute__((constructor(201))) static void racacit_extra_register_ctor(void)\n{\n"
        f"    spu_begin_image({igpm_rt_img}); igpm_rt_spu_recomp_register(); spu_begin_image(0);\n"
        "}\n\n"
        "void racacit_register_extra_overlays(void)\n{\n"
        f"    spu_register_overlay_image({igpm_rt_img}, 0x{igpm_rt_base:X}, vm_base + 0x{igpm_rt_va:X}u, 0x{igpm_rt_sz:X});\n"
        "}\n",
    )

    # 3D. Load GUID-keyed entry hints from RACiT/scripts/spu_starts.txt
    guid_starts = {}
    racit_starts_path = os.path.join(REPOS, "RACiT", "scripts", "spu_starts.txt")
    if os.path.exists(racit_starts_path):
        for line in open(racit_starts_path, "r", encoding="latin-1"):
            parts = line.split("#")[0].split()
            if len(parts) >= 2:
                guid_starts.setdefault(parts[0].lower(), set()).add(int(parts[1], 16))

    # Find all GUID-headed blocks across all PT_LOAD segments
    # In PH[0], raw jobs live between 0x00C15000 (after igpm_rt) and 0x00D19B00 (first SPU ELF)
    # In PH[3], extra GUID-headed job/sub-modules live in 0x10000000..0x1004C800
    ph0_heads = []
    for a in range(0x00C15000, 0x00D19B00, 16):
        b = read_guest_bytes(elf_bytes, segs, a, 16)
        if b:
            ws = struct.unpack(">4I", b)
            if all((w >> 25) == 0x21 and (w & 0x7F) == 2 for w in ws):
                ph0_heads.append(a)

    ph3_heads = []
    for a in range(0x10000000, 0x1004C800, 16):
        b = read_guest_bytes(elf_bytes, segs, a, 16)
        if b:
            ws = struct.unpack(">4I", b)
            if all((w >> 25) == 0x21 and (w & 0x7F) == 2 for w in ws):
                ph3_heads.append(a)

    log(f"  Found {len(ph0_heads)} GUID-headed SPU jobs in PH[0] and {len(ph3_heads)} in PH[3].")

    # Prepare parallel lift tasks for fixed jobs (LS base 0x100) + relocatable submodules (LS base 0x10000, --reloc)
    tasks = []
    all_job_heads = ph0_heads + ph3_heads
    for i, a in enumerate(ph0_heads):
        nxt = ph0_heads[i + 1] if i + 1 < len(ph0_heads) else 0x00D19B00
        size = min(nxt - a, 0x30000)
        raw = read_guest_bytes(elf_bytes, segs, a, size)
        entry = (struct.unpack(">I", raw[0x18:0x1C])[0] & 0xFFFF) << 2
        extra = {entry} if 0x100 < entry < 0x100 + size else set()
        guid_hex = raw[:16].hex().lower()
        extra |= {s for s in guid_starts.get(guid_hex, ()) if 0x120 <= s < 0x100 + size}
        tasks.append((i, "job", a, size, 0x100, raw, extra, True, False, jobs_out_raw, SPU_DIR))

    for j, a in enumerate(ph3_heads):
        nxt = ph3_heads[j + 1] if j + 1 < len(ph3_heads) else 0x1004C800
        size = min(nxt - a, 0x30000)
        raw = read_guest_bytes(elf_bytes, segs, a, size)
        entry = (struct.unpack(">I", raw[0x18:0x1C])[0] & 0xFFFF) << 2
        extra = {entry} if 0x100 < entry < 0x100 + size else set()
        tasks.append((len(ph0_heads) + j, "job", a, size, 0x100, raw, extra, True, False, jobs_out_raw, SPU_DIR))
        # Also lift PH[3] modules as relocatable sub-modules at base 0x10000 so parent jobs loading them at any LS base run natively
        sub_entry = ((struct.unpack(">I", raw[0x18:0x1C])[0] & 0xFFFF) << 2)
        sub_extra = {0x10000 + sub_entry} if 0x20 <= sub_entry < size else {0x10020}
        tasks.append((1000 + j, "sub", a, size, 0x10000, raw, sub_extra, False, True, jobs_out_raw, SPU_DIR))

    log(f"  Lifting {len(tasks)} SPU job/submodule images across 14 Xeon cores ...")
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=14) as ex:
        for res in ex.map(lift_one_spu_job, tasks):
            results.append(res)

    job_decls, job_blobs, job_regs = [], [], []
    sub_decls, sub_blobs, sub_regs = [], [], []
    ok_jobs = 0
    ok_subs = 0

    for idx, name, ea, size, base, prefix, ok in sorted(results, key=lambda x: x[0]):
        if not ok:
            log(f"  [WARN] SPU lift skipped for {name}")
            continue
        raw = read_guest_bytes(elf_bytes, segs, ea, size)
        if idx < 1000:
            img_id = 200 + idx
            job_decls.append(f"extern void {prefix}spu_recomp_register(void);")
            job_blobs.append(
                f"static const unsigned char {name}_bytes[{size}] = {{"
                + ",".join(str(x) for x in raw)
                + "};"
            )
            job_regs.append(
                f"    spu_begin_image({img_id}); {prefix}spu_recomp_register();\n"
                f"    spu_register_overlay_image({img_id}, 0x{base:X}, {name}_bytes, {size});"
            )
            ok_jobs += 1
        else:
            img_id = 500 + (idx - 1000)
            sub_decls.append(f"extern void {prefix}spu_recomp_register(void);")
            sub_blobs.append(
                f"static const unsigned char {name}_bytes[{size}] = {{"
                + ",".join(str(x) for x in raw)
                + "};"
            )
            sub_regs.append(
                f"    spu_begin_image({img_id}); {prefix}spu_recomp_register();\n"
                f"    spu_register_overlay_image_at({img_id}, 0x{base:X}, {name}_bytes, {size}, 0x30);\n"
                f"    spu_mark_overlay_relocatable({img_id});"
            )
            ok_subs += 1

    log(f"  Successfully lifted {ok_jobs} fixed SPU jobs and {ok_subs} relocatable SPU sub-modules!")

    write_if_changed(
        os.path.join(SPU_DIR, "spu_jobs.c"),
        "/* Generated by build_pipeline.py -- igPm SPU jobs & WWS ELF overlays */\n"
        "extern void spu_begin_image(int image_id);\n"
        "extern void spu_register_overlay_image(int image_id, unsigned base, const unsigned char* bytes, unsigned size);\n\n"
        + "\n".join(job_decls)
        + "\n\n"
        + "\n".join(job_blobs)
        + "\n\nvoid racacit_jobs_register_all(void)\n{\n"
        + "\n".join(job_regs)
        + "\n    spu_begin_image(0);\n}\n\n"
        "extern unsigned char* vm_base;\n"
        "void racacit_register_elf_overlays(void)\n{\n"
        + "".join(
            f"    spu_register_overlay_image({img_idx}, 0x{ls_base:X}, vm_base + 0x{gea:X}u, 0x{fsz:X});\n"
            for img_idx, gea, fsz, ls_base in elf_overlays
        )
        + "}\n\n"
        "__attribute__((constructor(200))) static void racacit_jobs_register_ctor(void)\n"
        "{\n    racacit_jobs_register_all();\n}\n",
    )

    write_if_changed(
        os.path.join(SPU_DIR, "spu_sub.c"),
        "/* Generated by build_pipeline.py -- relocatable SPU sub-modules */\n"
        "extern void spu_begin_image(int image_id);\n"
        "extern void spu_register_overlay_image_at(int image_id, unsigned base, const unsigned char* bytes, unsigned size, unsigned anchor);\n"
        "extern void spu_mark_overlay_relocatable(int image_id);\n\n"
        + "\n".join(sub_decls)
        + "\n\n"
        + "\n".join(sub_blobs)
        + "\n\n__attribute__((constructor(202))) static void racacit_sub_register_ctor(void)\n{\n"
        + "\n".join(sub_regs)
        + "\n    spu_begin_image(0);\n}\n",
    )


def stage4_write_sources_and_build():
    log("=== STAGE 4: Writing Game Harness & Compiling NexusRecomp.exe ===")
    os.makedirs(GAME_DIR, exist_ok=True)
    os.makedirs(WIN_DIR, exist_ok=True)

    # Copy and adapt main.cpp, stubs.cpp, button_prompts.cpp, discord_rpc.cpp from RACiT/game
    racit_game = os.path.join(REPOS, "RACiT", "game")
    for fname in ("main.cpp", "stubs.cpp", "button_prompts.cpp", "discord_rpc.cpp"):
        src = open(os.path.join(racit_game, fname), "r", encoding="latin-1").read()
        src = src.replace("Ratchet & Clank: A Crack in Time", "Ratchet & Clank: Into the Nexus")
        src = src.replace("RaCACiT.exe", "NexusRecomp.exe")
        if fname == "stubs.cpp":
            # In stubs.cpp, func_00937A90, func_0095D30C, func_005004BC are ACiT-specific functions
            # only called by optional ACiT armor-skin/lua-file hooks; provide weak stubs so link succeeds on Nexus
            weak_stubs = """
__attribute__((weak)) void func_00937A90(ppu_context*) {}
__attribute__((weak)) void func_0095D30C(ppu_context*) {}
__attribute__((weak)) void func_005004BC(ppu_context*) {}
"""
            src = src + "\n" + weak_stubs
        if fname == "main.cpp":
            # Default 4K resolution scale (3x 720p = 3840x2160) & high-res shadows for AMD Radeon RX 9060 XT 16GB
            old_defaults = 'if (!getenv("PS3_TITLE")) _putenv_s("PS3_TITLE", "Ratchet & Clank: Into the Nexus");'
            new_defaults = (
                old_defaults
                + '\n    if (!getenv("RSX_RES_SCALE")) _putenv_s("RSX_RES_SCALE", "3");'
                + '\n    if (!getenv("RSX_SHADOW_SCALE")) _putenv_s("RSX_SHADOW_SCALE", "200");'
            )
            src = src.replace(old_defaults, new_defaults, 1)
        write_if_changed(os.path.join(GAME_DIR, fname), src)

    # Write game/CMakeLists.txt tuned for Intel Xeon E5-2680 v4 (Broadwell AVX2/FMA3/BMI2)
    game_cmake = open(os.path.join(racit_game, "CMakeLists.txt"), "r", encoding="latin-1").read()
    game_cmake = game_cmake.replace(
        'set(RACACIT_SIMD_FLAGS "-mavx2;-mfma;-mbmi2")',
        'set(RACACIT_SIMD_FLAGS "-march=broadwell;-mtune=broadwell;-mavx2;-mfma;-mbmi;-mbmi2")',
    )
    game_cmake = game_cmake.replace(
        "OUTPUT_NAME RaCACiT",
        "OUTPUT_NAME NexusRecomp",
    )
    write_if_changed(os.path.join(GAME_DIR, "CMakeLists.txt"), game_cmake)

    # Write windows/CMakeLists.txt
    win_cmake = """cmake_minimum_required(VERSION 3.22)
project(nexus_windows C CXX)

get_filename_component(NEXUS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT MSVC)
    add_compile_options(-fomit-frame-pointer -fno-math-errno -march=broadwell -mtune=broadwell)
endif()

list(APPEND CMAKE_PREFIX_PATH "${NEXUS_ROOT}/tools/sdl2-win")
find_package(SDL2 REQUIRED CONFIG)

set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL             ON  CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL          OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
add_subdirectory("${NEXUS_ROOT}/tools/glslang-src" "${CMAKE_BINARY_DIR}/glslang" EXCLUDE_FROM_ALL)
foreach(t glslang SPIRV glslang-default-resource-limits)
    if(TARGET ${t} AND NOT TARGET glslang::${t})
        add_library(glslang::${t} ALIAS ${t})
    endif()
endforeach()

set(PS3RECOMP_RSX_VULKAN   ON  CACHE BOOL "" FORCE)
set(PS3RECOMP_WIN_USE_SDL2 ON  CACHE BOOL "" FORCE)
set(PS3RECOMP_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(PS3RECOMP_DIR "${NEXUS_ROOT}/tools/ps3recomp" CACHE PATH "" FORCE)
set(RECOMP_DIR "${NEXUS_ROOT}/game/src/recomp" CACHE PATH "" FORCE)
set(SPU_DIR    "${NEXUS_ROOT}/game/spu"        CACHE PATH "" FORCE)
set(PS3RECOMP_VULKAN_INCLUDE_DIR "${NEXUS_ROOT}/tools/Vulkan-Headers/include" CACHE PATH "" FORCE)
add_subdirectory("${NEXUS_ROOT}/game" "${CMAKE_BINARY_DIR}/game")
"""
    write_if_changed(os.path.join(WIN_DIR, "CMakeLists.txt"), win_cmake)

    env = os.environ.copy()
    env["PATH"] = VENV_SCRIPTS + os.pathsep + env.get("PATH", "")

    if not os.path.exists(os.path.join(BUILD_DIR, "build.ninja")):
        log("Configuring CMake build (build-win) ...")
        tc_file = os.path.join(WIN_DIR, "toolchain", "zig-windows.cmake")
        sdl2_cmake_dir = os.path.join(TOOLS_DIR, "sdl2-win", "lib", "cmake", "SDL2")
        subprocess.run(
            [
                CMAKE_EXE,
                "-S", WIN_DIR,
                "-B", BUILD_DIR,
                "-G", "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={NINJA_EXE}",
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_TOOLCHAIN_FILE={tc_file}",
                f"-DSDL2_DIR={sdl2_cmake_dir}",
                f"-DPython3_EXECUTABLE={PYTHON_EXE}",
            ],
            env=env,
            check=True,
        )

    log("Building NexusRecomp.exe with Ninja (-j10 for 32GB RAM safety across Xeon cores) ...")
    subprocess.run(
        [CMAKE_EXE, "--build", BUILD_DIR, "-j10"],
        env=env,
        check=True,
    )


def stage5_stage_and_verify():
    log("=== STAGE 5: Staging Executable, Game Config & Running Boot Smoke Test ===")
    # Extract ASCII-clean diagnostic slice around 0x005F4000..0x005FA000 from ppu_recomp_022.cpp
    p22 = os.path.join(RECOMP_DIR, "ppu_recomp_022.cpp")
    diag_p = os.path.join(OUT_DIR, "diag_005F.txt")
    if os.path.exists(p22) and not os.path.exists(diag_p):
        raw22 = open(p22, "rb").read()
        clean22 = bytes(b if 32 <= b < 127 or b in (9, 10, 13) else ord(".") for b in raw22).decode("ascii")
        i0 = clean22.find("void func_005F4")
        i1 = clean22.find("void func_005FA")
        if i0 >= 0:
            open(diag_p, "w", encoding="ascii").write(clean22[i0 : i1 if i1 > i0 else i0 + 80000])

    exe_src = os.path.join(BUILD_DIR, "game", "NexusRecomp.exe")
    if not os.path.exists(exe_src):
        raise RuntimeError(f"Expected executable not found at {exe_src}")

    # Copy decrypted EBOOT.ELF into the user's PS3_GAME/USRDIR/EBOOT.elf so the runner finds it automatically
    usrdir_eboot = os.path.join(GAME_ROOT, "PS3_GAME", "USRDIR", "EBOOT.elf")
    if not os.path.exists(usrdir_eboot):
        log(f"Copying decrypted EBOOT.elf to {usrdir_eboot} ...")
        shutil.copy2(ELF_PATH, usrdir_eboot)

    # Also create a clean distribution folder at c:\coding\nexus-recomp\dist\
    dist_dir = os.path.join(ROOT, "dist")
    os.makedirs(dist_dir, exist_ok=True)
    dist_exe = os.path.join(dist_dir, "NexusRecomp.exe")
    shutil.copy2(exe_src, dist_exe)

    settings_path = os.path.join(dist_dir, "settings.txt")
    with open(settings_path, "w", encoding="latin-1") as f:
        f.write(
            f"# Ratchet & Clank: Into the Nexus (PC Static Recompilation)\n"
            f"# Tuned for Intel Xeon E5-2680 v4 (AVX2/FMA3) + AMD Radeon RX 9060 XT 16GB (4K 60fps)\n"
            f"GAME={GAME_ROOT}\n"
            f"RSX_RES_SCALE=3\n"
            f"RACACIT_RES_SCALE=3.0\n"
            f"RSX_SHADOW_SCALE=200\n"
            f"RACACIT_ANISO=16\n"
            f"RACACIT_TRILINEAR=1\n"
            f"RACACIT_FPS=1\n"
            f"RACACIT_PRESENT_PACE=1\n"
            f"RACACIT_CPU_MODE=max\n"
            f"RACACIT_UPSCALE=sharp\n"
            f"RACACIT_SHARPNESS=5\n"
            f"RACACIT_KBM=1\n"
        )
    log(f"Staged ready-to-play build at: {dist_exe}")

    # Run a 35-second automated boot verification test with PAD_AUTOPRESS=1 to inspect log.txt
    log("Running 35-second boot verification test of NexusRecomp.exe (with PAD_AUTOPRESS=1) ...")
    log_path = os.path.join(dist_dir, "log.txt")
    if os.path.exists(log_path):
        os.remove(log_path)

    test_env = os.environ.copy()
    test_env["PAD_AUTOPRESS"] = "1"
    proc = subprocess.Popen(
        [dist_exe, usrdir_eboot],
        cwd=dist_dir,
        env=test_env,
    )
    time.sleep(35)
    still_alive = proc.poll() is None
    if still_alive:
        log("NexusRecomp.exe is running Alive after 35 seconds! Terminating smoke test ...")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    else:
        log(f"NexusRecomp.exe exited early with code {proc.returncode}")

    if os.path.exists(log_path):
        with open(log_path, "r", encoding="latin-1", errors="replace") as f:
            log_text = f.read()
        lines = log_text.splitlines()
        log(f"Captured {len(lines)} lines in {log_path}. First 40 lines:")
        for ln in lines[:40]:
            print("  ", ln)
        if len(lines) > 40:
            log("Last 40 lines:")
            for ln in lines[-40:]:
                print("  ", ln)


def main():
    t0 = time.time()
    stage1_prepare_sdk_and_deps()
    stage2_lift_ppu()
    stage3_lift_spu()
    stage4_write_sources_and_build()
    stage5_stage_and_verify()
    log(f"=== ALL STAGES COMPLETE in {(time.time() - t0) / 60.0:.1f} minutes! ===")


if __name__ == "__main__":
    main()
