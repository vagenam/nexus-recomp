#!/usr/bin/env python3
"""
End-to-end automated static recompilation & build pipeline for:
  Ratchet & Clank: Into the Nexus (BCUS99245 v1.00) -> Windows x86-64 (AVX2/FMA3, Vulkan 1.3)

Runs all stages in a single script so no interactive shell prompts are needed:
  Stage 0: Verify inputs (EBOOT.ELF SHA-256 + PARAM.SFO title ID / version)
  Stage 1: Check vendored ps3recomp SDK + download pinned, hash-verified libs (Vulkan-Headers, glslang, zlib, SDL2)
  Stage 2: Lift PPU EBOOT.ELF (14,728 functions) + VMX alias fix + Insomniac PPU post-lift patches + HLE NIDs
  Stage 3: Lift SPU workloads (3 embedded SPU ELFs, 2 SPURS policy modules, igpm_rt, 207 igPm jobs & relocatable submodules)
  Stage 4: Generate game sources + toolchain & compile NexusRecomp.exe with zig cc (Broadwell AVX2/FMA3)
  Stage 5: Stage EBOOT.elf + settings.txt for zero-arg launch and run initial boot verification
"""

import concurrent.futures
import glob
import hashlib
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

ROOT = os.path.dirname(os.path.abspath(__file__))
PORTABLE_DIR = os.path.join(ROOT, "tools", ".portable")
VENV_SCRIPTS = os.path.dirname(sys.executable)
PYTHON_EXE = sys.executable
CMAKE_EXE = shutil.which("cmake") or os.path.join(PORTABLE_DIR, "Lib", "site-packages", "cmake", "data", "bin", "cmake.exe")
NINJA_EXE = shutil.which("ninja") or os.path.join(PORTABLE_DIR, "Scripts", "ninja.exe")
ZIG_EXE = shutil.which("zig") or os.path.join(PORTABLE_DIR, "Lib", "site-packages", "ziglang", "zig.exe")

def _game_root_from(g):
    g = g.strip().strip('"')
    return os.path.dirname(g) if os.path.basename(g.rstrip("\\/")).upper() == "PS3_GAME" else g


def _find_game_root():
    env_g = os.environ.get("NEXUS_PS3_GAME", "").strip()
    if env_g:
        return _game_root_from(env_g)
    for settings_p in (os.path.join(ROOT, "RecompFolder", "settings.txt"),
                       os.path.join(ROOT, "dist", "settings.txt")):
        if os.path.exists(settings_p):
            with open(settings_p, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if line.startswith("GAME=") and line[5:].strip():
                        return _game_root_from(line[5:])
    return None

GAME_ROOT = _find_game_root()

# ---------------------------------------------------------------------------
# Supported input. Every hardcoded guest address in this pipeline and in the
# game harness (0x00D3D680, 0x00C14A80, func_00A084CC, ...) is only valid for
# this exact binary, so anything else is rejected up front instead of failing
# minutes later in Stage 3.
# ---------------------------------------------------------------------------
EXPECTED_TITLE_ID = "BCUS99245"
EXPECTED_APP_VER = "01.00"
# SHA-256 of EBOOT.BIN (BCUS99245 v1.00) decrypted with RPCS3 "Decrypt PS3 Binaries"
EXPECTED_ELF_SHA256 = "ca1a1a293d211095fc834eaaa10cd5fa9248b0e0328919fdeadeed1675c52c3f"
# Bump when the lift/post-lift patching logic changes so old lifts are redone.
LIFT_RECIPE_VERSION = "3"

# Parallelism: default to all logical CPUs; override with NEXUS_JOBS=<n>
# (lower it if the compile runs out of RAM -- lifted TUs are large).
JOBS = max(1, int(os.environ.get("NEXUS_JOBS", "0") or 0) or (os.cpu_count() or 4))
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


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_param_sfo(path):
    """Minimal PARAM.SFO reader -> {key: str|int}."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x00PSF":
        raise ValueError("not a PARAM.SFO file (bad magic)")
    key_tab, data_tab, count = struct.unpack("<III", d[8:20])
    out = {}
    for i in range(count):
        k_off, fmt, d_len, _d_max, d_off = struct.unpack("<HHIII", d[20 + i * 16 : 36 + i * 16])
        k_start = key_tab + k_off
        key = d[k_start : d.index(b"\x00", k_start)].decode("ascii", "replace")
        raw = d[data_tab + d_off : data_tab + d_off + d_len]
        if fmt == 0x0404:
            out[key] = struct.unpack("<I", raw[:4])[0]
        else:
            out[key] = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
    return out


class InputError(Exception):
    pass


def stage0_verify_inputs():
    """Fail fast if the ELF or the game dump is not BCUS99245 v1.00."""
    log("=== STAGE 0: Verifying EBOOT.ELF & PS3_GAME ===")
    allow_unknown = os.environ.get("NEXUS_ALLOW_UNKNOWN_ELF", "0") == "1"
    problems = []

    if GAME_ROOT is None:
        problems.append("PS3_GAME folder not set (set NEXUS_PS3_GAME or GAME= in RecompFolder\\settings.txt).")
    else:
        ps3_game = os.path.join(GAME_ROOT, "PS3_GAME")
        sfo = os.path.join(ps3_game, "PARAM.SFO")
        if not os.path.isdir(os.path.join(ps3_game, "USRDIR")) or not os.path.isfile(sfo):
            problems.append(f"{ps3_game} must contain both USRDIR\\ and PARAM.SFO (extracted PS3_GAME folder, not .iso/.pkg).")
        else:
            try:
                p = read_param_sfo(sfo)
            except (OSError, ValueError, struct.error) as e:
                problems.append(f"Could not read {sfo}: {e}")
            else:
                tid = p.get("TITLE_ID", "?")
                ver = p.get("APP_VER", p.get("VERSION", "?"))
                if tid != EXPECTED_TITLE_ID:
                    problems.append(f"PARAM.SFO TITLE_ID is {tid}, expected {EXPECTED_TITLE_ID} (Ratchet & Clank: Into the Nexus, USA disc).")
                else:
                    log(f"  PARAM.SFO: TITLE_ID={tid} APP_VER={ver} [OK]")
                    if ver != EXPECTED_APP_VER:
                        log(f"  [WARN] APP_VER {ver} != {EXPECTED_APP_VER}; only the v1.00 disc EBOOT is supported.")

    if not os.path.isfile(ELF_PATH):
        problems.append(f"Decrypted EBOOT.ELF not found at {ELF_PATH}.")
    else:
        with open(ELF_PATH, "rb") as f:
            magic = f.read(4)
        if magic != b"\x7fELF":
            problems.append(f"{ELF_PATH} is not a decrypted ELF (encrypted SELF/EBOOT.BIN?). Decrypt it with RPCS3: Utilities -> Decrypt PS3 Binaries.")
        else:
            digest = sha256_file(ELF_PATH)
            if digest == EXPECTED_ELF_SHA256:
                log(f"  EBOOT.ELF SHA-256 {digest[:16]}... [OK] (BCUS99245 v1.00)")
            elif allow_unknown:
                log(f"  [WARN] EBOOT.ELF SHA-256 {digest} is unknown; continuing because NEXUS_ALLOW_UNKNOWN_ELF=1.")
            else:
                problems.append(
                    f"EBOOT.ELF SHA-256 is {digest}, expected {EXPECTED_ELF_SHA256}.\n"
                    "    Only the RPCS3-decrypted EBOOT.BIN of BCUS99245 v1.00 (disc, no update) is supported --\n"
                    "    the pipeline relies on fixed v1.00 addresses. (Override: NEXUS_ALLOW_UNKNOWN_ELF=1, unsupported.)"
                )

    if problems:
        for pr in problems:
            log(f"  [ERROR] {pr}")
        raise InputError("Input verification failed; see errors above.")


def download_and_extract_zip(url, dest_dir, sha256=None, stamp=None):
    """Download a pinned zip, verify its SHA-256, extract to dest_dir.

    A `.pinned` stamp file records which archive the folder came from, so a
    folder left over from a different pin (or a partial extract) is replaced.
    """
    stamp_p = os.path.join(dest_dir, ".pinned")
    want = stamp or sha256 or url
    if os.path.isfile(stamp_p) and open(stamp_p, "r").read().strip() == want:
        log(f"  [cached] {os.path.basename(dest_dir)}")
        return
    log(f"  Downloading {url} -> {dest_dir} ...")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=300) as resp:
        data = resp.read()
    if sha256:
        got = hashlib.sha256(data).hexdigest()
        if got != sha256:
            raise RuntimeError(f"SHA-256 mismatch for {url}: got {got}, expected {sha256}")
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
    with open(stamp_p, "w") as f:
        f.write(want + "\n")


# Pinned third-party sources (downloaded, never vendored in git). SHA-256 is of
# the GitHub archive zip.
PINNED_DEPS = [
    ("Vulkan-Headers", "https://github.com/KhronosGroup/Vulkan-Headers/archive/c46850864f4661461b0f6cb9922c058ffea4915e.zip",
     "45fd1cbfba537884f8a54d5dc2a69cb18de42fd94e84dc6a9bf66b5df829d79a"),
    ("glslang-src", "https://github.com/KhronosGroup/glslang/archive/2fbed5e1fb43a1c480fe020e3bb686a31010989a.zip",
     "6c241ca6d3b88ac602e572401f95de1c247110d900d1bbb3e302c4f3714515cc"),
    ("zlib", "https://github.com/madler/zlib/archive/767c4c947852e143f582c85f14cf573411df1b35.zip",
     "7416fc424ed245c961e998e149ffca387c572db63db25a86bf17f81306ed4201"),
    ("SDL2-src", "https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.32.10.zip",
     "7a3c207b8509edc487d658df357ad764cd852d68fe248d307b25c0741d52fdf0"),
]


def _cmake_cache_source(build_dir):
    cache = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.exists(cache):
        return None
    with open(cache, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if line.startswith("CMAKE_HOME_DIRECTORY:"):
                return line.split("=", 1)[1].strip()
    return None


def _drop_stale_cmake_dir(build_dir, source_dir):
    """CMake refuses a cache created for another source path (e.g. a moved clone)."""
    src = _cmake_cache_source(build_dir)
    if src is not None and os.path.normcase(os.path.abspath(src)) != os.path.normcase(os.path.abspath(source_dir)):
        log(f"  Removing stale CMake build dir {build_dir} (was configured for {src}) ...")
        shutil.rmtree(build_dir, ignore_errors=True)


def stage1_prepare_sdk_and_deps():
    log("=== STAGE 1: Checking ps3recomp SDK & Preparing Dependencies ===")
    os.makedirs(TOOLS_DIR, exist_ok=True)

    # tools/ps3recomp is vendored in this repo: upstream sp00nznet/ps3recomp
    # @ 5244db9 + the RACiT runtime-fixes patch (MIT) + Nexus-specific runtime
    # changes. It is the source of truth and is never re-downloaded, because a
    # fresh upstream copy would lose the Nexus changes.
    for need in ("CMakeLists.txt", os.path.join("tools", "ppu_lifter.py"), os.path.join("tools", "spu_lifter.py")):
        if not os.path.exists(os.path.join(PS3R_DIR, need)):
            raise FileNotFoundError(
                f"Vendored ps3recomp is incomplete (missing tools/ps3recomp/{need.replace(os.sep, '/')}). "
                "Re-clone the repository or run `git checkout -- tools/ps3recomp`."
            )
    log("ps3recomp SDK present (vendored).")

    for name, url, sha in PINNED_DEPS:
        download_and_extract_zip(url, os.path.join(TOOLS_DIR, name), sha256=sha)

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
        _drop_stale_cmake_dir(sdl2_build, os.path.join(TOOLS_DIR, "SDL2-src"))
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
            [CMAKE_EXE, "--build", sdl2_build, "--target", "install", f"-j{JOBS}"],
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

        # 3. Extend 2.5s GPU watchdog timeout ONLY inside func_005F6DC8 (0x0BE420E0 -> 0x5F2EDC00)
        # Do NOT replace file-wide -- 0xBE420E0 also appears as a math constant (e.g. func_0063B65C).
        wd_m = re.search(
            r"(void func_005F6DC8\(ppu_context\* ctx\) \{.*?\n\})",
            src,
            re.DOTALL,
        )
        if wd_m and "(uint32_t)0xBE4 << 16" in wd_m.group(1) and "| 0x20E0" in wd_m.group(1):
            fixed_wd = wd_m.group(1).replace("(uint32_t)0xBE4 << 16", "(uint32_t)0x5F2E << 16").replace(
                "| 0x20E0", "| 0xDC00"
            )
            src = src[: wd_m.start(1)] + fixed_wd + src[wd_m.end(1) :]
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


LIFT_STAMP = os.path.join(OUT_DIR, "lift_stamp.txt")


def _expected_lift_stamp():
    return f"elf_sha256={sha256_file(ELF_PATH)}\nrecipe={LIFT_RECIPE_VERSION}\n"


def invalidate_stale_lift():
    """Wipe generated lift output if it was produced from a different ELF / recipe.

    Previously only the existence of ppu_recomp.h was checked, so a stale
    elf/EBOOT.ELF or an old lift was silently reused.
    """
    want = _expected_lift_stamp()
    have = open(LIFT_STAMP, "r").read() if os.path.exists(LIFT_STAMP) else None
    if have == want:
        return False
    if have is None and not os.path.exists(os.path.join(RECOMP_DIR, "ppu_recomp.h")) and not os.path.isdir(SPU_DIR):
        return False  # nothing generated yet
    log("Lift output is stale (different EBOOT.ELF or lift recipe) -- removing generated sources ...")
    for d in (OUT_DIR, RECOMP_DIR, SPU_DIR):
        shutil.rmtree(d, ignore_errors=True)
    return True


def write_lift_stamp():
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(LIFT_STAMP, "w") as f:
        f.write(_expected_lift_stamp())


def stage2_lift_ppu():
    log("=== STAGE 2: Lifting PPU Code (14,728 functions) ===")
    invalidate_stale_lift()
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(RECOMP_DIR, exist_ok=True)
    func_json = os.path.join(OUT_DIR, "EBOOT.functions.json")
    imp_json = os.path.join(OUT_DIR, "EBOOT.imports.json")
    if not os.path.exists(func_json) or not os.path.exists(imp_json):
        log("Running ppu_loader.py to extract functions & imports ...")
        subprocess.run(
            [
                PYTHON_EXE,
                "-I",
                os.path.join(PS3R_TOOLS, "ppu_loader.py"),
                ELF_PATH,
                "--output", OUT_DIR,
            ],
            check=True,
        )
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
    if not os.path.exists(spu_images_dir) or not any(f.endswith(".elf") for f in os.listdir(spu_images_dir)):
        log("Extracting embedded SPU ELF images via extract_spu_images.py ...")
        subprocess.run(
            [
                PYTHON_EXE,
                "-I",
                os.path.join(PS3R_TOOLS, "extract_spu_images.py"),
                ELF_PATH,
                "--output", spu_images_dir,
            ],
            check=True,
        )
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

    # 3D. GUID-keyed entry hints (Nexus uses its own EBOOT.elf SPU tables)
    guid_starts = {}

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

    log(f"  Lifting {len(tasks)} SPU job/submodule images across {JOBS} worker threads ...")
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=JOBS) as ex:
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

    # Verify Nexus game harness sources are present (no external RACiT copy)
    for fname in ("main.cpp", "stubs.cpp", "button_prompts.cpp", "discord_rpc.cpp"):
        dst_game_file = os.path.join(GAME_DIR, fname)
        if not os.path.exists(dst_game_file):
            raise FileNotFoundError(f"Missing Nexus game source: {dst_game_file}")

    # Use existing game/CMakeLists.txt and windows/CMakeLists.txt from the repo
    if not os.path.exists(os.path.join(GAME_DIR, "CMakeLists.txt")) or not os.path.exists(os.path.join(WIN_DIR, "CMakeLists.txt")):
        raise FileNotFoundError("Missing game/CMakeLists.txt or windows/CMakeLists.txt")

    env = os.environ.copy()
    env["PATH"] = VENV_SCRIPTS + os.pathsep + env.get("PATH", "")

    _drop_stale_cmake_dir(BUILD_DIR, WIN_DIR)
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

    log(f"Building NexusRecomp.exe with Ninja (-j{JOBS}; set NEXUS_JOBS lower if you run out of RAM) ...")
    subprocess.run(
        [CMAKE_EXE, "--build", BUILD_DIR, f"-j{JOBS}"],
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

    # Copy decrypted EBOOT.ELF into the user's PS3_GAME/USRDIR/EBOOT.elf so the runner finds it
    # automatically. NOTE: this adds one file to the user's dump (documented in README); EBOOT.BIN
    # and all other game files are left untouched.
    usrdir_eboot = os.path.join(GAME_ROOT, "PS3_GAME", "USRDIR", "EBOOT.elf")
    if not os.path.exists(usrdir_eboot) or sha256_file(usrdir_eboot) != sha256_file(ELF_PATH):
        log(f"Copying decrypted EBOOT.elf to {usrdir_eboot} ...")
        shutil.copy2(ELF_PATH, usrdir_eboot)

    # Stage executable into RecompFolder/
    dist_dir = os.path.join(ROOT, "RecompFolder")
    os.makedirs(dist_dir, exist_ok=True)
    dist_exe = os.path.join(dist_dir, "NexusRecomp.exe")
    shutil.copy2(exe_src, dist_exe)

    settings_path = os.path.join(dist_dir, "settings.txt")
    if not os.path.exists(settings_path):
        tmpl = os.path.join(ROOT, "settings.template.txt")
        if os.path.exists(tmpl):
            txt = open(tmpl, "r", encoding="utf-8", errors="ignore").read()
            lines = [f"GAME={os.path.join(GAME_ROOT, 'PS3_GAME')}" if l.startswith("GAME=") else l for l in txt.splitlines()]
            open(settings_path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    log(f"Staged ready-to-play build at: {dist_exe}")

    if os.environ.get("NEXUS_NO_SMOKE", "0") == "1":
        return

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
    try:
        stage0_verify_inputs()
    except InputError as e:
        log(f"[ERROR] {e}")
        sys.exit(2)
    stage1_prepare_sdk_and_deps()
    stage2_lift_ppu()
    stage3_lift_spu()
    write_lift_stamp()
    stage4_write_sources_and_build()
    stage5_stage_and_verify()
    log(f"=== ALL STAGES COMPLETE in {(time.time() - t0) / 60.0:.1f} minutes! ===")


if __name__ == "__main__":
    main()
