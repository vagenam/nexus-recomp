/*
 * ps3recomp - cellFs VFS (sys_fs HLE)
 *
 * Backs the game's file I/O with the real host filesystem so it can load its
 * data/config/assets. Guest PS3 paths (/dev_bdvd/..., /app_home/..., /dev_hdd0,
 * etc.) are translated to a host root (the game directory that contains
 * PS3_GAME), set by the boot harness from the EBOOT path (or $PS3_VFS_ROOT).
 *
 * Only the calls the boot actually imports are implemented:
 *   cellFsOpen/Close/Read/Write/Lseek/Stat/Fstat/Opendir/Readdir/Closedir/
 *   Mkdir/Rmdir/Unlink/Fsync.
 *
 * Context-aware HLE: guest pointers (path, buffers, out params) are read/written
 * through vm_base in big-endian.
 */
#include "ppu_recomp.h"      /* ppu_context */
#include "../../libs/filesystem/edat.h"
#include "ps3emu/nid.h"      /* ps3_compute_nid */
#include "ps3emu/guest_call.h" /* ps3_invoke_guest: AIO completion is a guest OPD */
#include "sdata_decrypt.h"   /* SDATA/EDAT (NPD) decryption for cellFsSdataOpen */
#include "../../../zlib/zlib.h"   /* PSARC manifests (DualSense routing) */
#include <string>
#include <vector>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "../platform/win32_dirent.h"   /* <dirent.h>, or a Win32 stand-in */

/* Guest-resolving host backtrace (ppu_loader.cpp). Must be declared at file scope:
 * `extern "C"` inside a function body is ill-formed in C++. */
extern "C" void ydkj_host_bt(const char* tag);
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>          /* open/close on MinGW */
#else
#include <unistd.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0       /* POSIX has no text/binary distinction */
#endif

extern "C" uint8_t* vm_base;
extern "C" void ppu_guest_caller(char* out, size_t n);
extern "C" uint32_t ppu_vm_size;
extern "C" void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
#include "../platform/win32_backtrace.h"   /* RtlCaptureStackBackTrace / GetModuleHandleA on POSIX */

/* Host root that the PS3 mount points map into (dir containing PS3_GAME). */
extern "C" const char* ppu_vfs_root = ".";

/* CELL_FS return / mode / flag constants. */
#define CELL_OK              0
#define CELL_FS_ENOENT       (-2147418106)   /* 0x80010006 -- was -2147418090, which is 0x80010016 = CELL_ENOTCONN, not ENOENT */
#define CELL_FS_EISDIR       (-2147418094)   /* 0x80010012 */
#define CELL_FS_EIO          (-2147418111)
#define CELL_FS_S_IFDIR      0x4000u
#define CELL_FS_S_IFREG      0x8000u
#define CELL_FS_O_RDONLY     0
#define CELL_FS_O_WRONLY     1
#define CELL_FS_O_RDWR       2
#define CELL_FS_O_CREAT      0x0200
#define CELL_FS_O_TRUNC      0x0400
#define CELL_FS_O_APPEND     0x0100
#define CELL_FS_SEEK_SET     0
#define CELL_FS_SEEK_CUR     1
#define CELL_FS_SEEK_END     2
#define CELL_FS_TYPE_DIR     1
#define CELL_FS_TYPE_REG     2

/* ---- guest memory string helpers ---- */
static void guest_strcpy(char* dst, uint32_t gaddr, size_t cap)
{
    size_t i = 0;
    for (; i < cap - 1; i++) {
        if (ppu_vm_size && gaddr + i >= ppu_vm_size) break;
        char c = (char)vm_base[gaddr + i];
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = 0;
}

/* Defined in runtime/syscalls/sys_fs.c -- shared by all three path
 * translators. (extern "C" is ill-formed at block scope.) */
extern "C" void ps3_vfs_ps3game_fallback(char* path, size_t cap);

/* Translate a guest path to a host path under ppu_vfs_root. Known PS3 mount
 * prefixes are stripped; the rest is appended to the root. */
static void host_path(char* out, size_t cap, const char* guest)
{
    const char* rel = guest;
    /* /dev_hdd0 overlays the installed game-update dir (a disc title patched to
     * e.g. v1.30 runs the update's EBOOT and reads its patchN.farc from
     * /dev_hdd0/game/<title>/), mirroring the PS3/RPCS3 layout: base data on
     * /dev_bdvd (disc), update data on /dev_hdd0. Set PS3_HDD0_ROOT to the host
     * dir that /dev_hdd0 maps into (the one containing game/<title>/). */
    static const char* hdd0_root = nullptr; static int hdd0_init = 0;
    if (!hdd0_init) { hdd0_root = getenv("PS3_HDD0_ROOT"); hdd0_init = 1; }
    if (hdd0_root && strncmp(guest, "/dev_hdd0/", 10) == 0) {
        snprintf(out, cap, "%s/%s", hdd0_root, guest + 10);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    /* /dev_flash is FIRMWARE, not game data — mapping it into the game root (the
     * old behaviour) makes every firmware lookup miss. YDKJ's FMOD asks for
     * /dev_flash/sys/external/flashMP3.pic and, on a miss, prints "mp3 failed to
     * load MP3 codec (are you using the correct flash?)" and then crashes.
     * Serve it from a real dev_flash tree instead ($PS3_DEV_FLASH, else RPCS3's). */
    if (strncmp(guest, "/dev_flash/", 11) == 0) {
        const char* fw = getenv("PS3_DEV_FLASH");
        if (!fw || !*fw) fw = "D:/recomp/tools/rpcs3/dev_flash";
        snprintf(out, cap, "%s/%s", fw, guest + 11);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    static const char* mounts[] = {
        "/dev_bdvd/", "/app_home/", "/dev_hdd0/", "/dev_hdd1/",
        "/dev_flash/", "/host_root/", "/dev_usb000/", "/dev_usb/"
    };
    for (size_t i = 0; i < sizeof(mounts)/sizeof(mounts[0]); i++) {
        size_t n = strlen(mounts[i]);
        if (strncmp(guest, mounts[i], n) == 0) { rel = guest + n; break; }
        /* Every mount above ends in '/', so a BARE device root never matched
         * its own mount: "/dev_bdvd" is not a prefix-match for "/dev_bdvd/".
         * It fell through to the strip-leading-slash case and resolved to
         * <root>/dev_bdvd, which does not exist -- so a title that opendirs or
         * stats the device to check the medium is present reads it as "no
         * disc". YDKJ does exactly that, 27 times a boot, and consequently
         * loaded none of its Scaleform UI or FMOD banks: no logos, no legal
         * screen, just a clear colour.
         *
         * libs/filesystem/cellFs.c already carries this fix (Tokyo Jungle hit
         * the same wall and retried forever); ppu_fs is the other half of the
         * split filesystem and never got it. Map the bare root to the mount
         * directory itself. */
        if (n > 1 && strncmp(guest, mounts[i], n - 1) == 0 && guest[n - 1] == '\0') {
            rel = guest + n - 1;   /* points at the NUL -> <root>/ */
            break;
        }
    }
    if (rel == guest && guest[0] == '/') rel = guest + 1;   /* strip leading '/' */
    snprintf(out, cap, "%s/%s", ppu_vfs_root, rel);
    for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
    ps3_vfs_ps3game_fallback(out, cap);
    /* Mods (the Android launcher's mods folder, and the Windows launcher's):
     * RACACIT_MODS lists the enabled mods' files/ folders, ';'-separated,
     * first = highest priority; each mirrors the game folder
     * (PS3_GAME/USRDIR/...). A file a mod has replaces the game's -- whole
     * files, so a mod ships e.g. a patched .psarc. Unset: nothing changes. */
    {
        static char mods[16][512]; static int nmods = -1;
        static char root[512]; static size_t rn;
        if (nmods < 0) {
            nmods = 0;
            const char* e = getenv("RACACIT_MODS");
            while (e && *e && nmods < 16) {
                const char* sep = strchr(e, ';');
                const size_t n = sep ? (size_t)(sep - e) : strlen(e);
                if (n && n < sizeof mods[0]) { memcpy(mods[nmods], e, n); mods[nmods][n] = 0; nmods++; }
                e = sep ? sep + 1 : NULL;
            }
            for (int i = 0; i < nmods; i++) {
                for (char* p = mods[i]; *p; p++) if (*p == '\\') *p = '/';
                fprintf(stderr, "[mods] %s%c", mods[i], 10);
            }
            /* `out` has forward slashes; the root may be a Windows path */
            snprintf(root, sizeof root, "%s", ppu_vfs_root);
            for (char* p = root; *p; p++) if (*p == '\\') *p = '/';
            rn = strlen(root);
            while (rn && root[rn - 1] == '/') root[--rn] = 0;
        }
        if (nmods > 0 && rn && strncmp(out, root, rn) == 0 && out[rn] == '/') {
            char cand[1024];
            for (int i = 0; i < nmods; i++) {
                snprintf(cand, sizeof cand, "%s/%s", mods[i], out + rn + 1);
                struct stat st;
                if (stat(cand, &st) == 0) {
                    static int said; if (said++ < 64) fprintf(stderr, "[mods] %s -> %s%c", guest, cand, 10);
                    snprintf(out, cap, "%s", cand);
                    break;
                }
            }
        }
    }
}

/* ---- fd / dir handle tables ---- */
#define FS_MAX 256
static FILE* g_files[FS_MAX];
static uint8_t g_fd_usm[FS_MAX];
static char    g_fd_path[FS_MAX][192];   /* guest path per fd (diagnostics) */
/* racacit: DualSense speaker routing needs to know which file a read serves,
 * and A Crack in Time streams its level dialogue and streamed sound effects
 * out of PSARC archives (level_uncached.psarc: streaming_dialogue.us.dat,
 * streaming_sound.dat). On open, a .psarc's index is read and the file-offset
 * spans of those entries kept: kind 2 = dialogue, 4 = streamed sounds.
 * `key` names the entry's dialogue line table (fs_dlg_key), `space` is 1 for
 * the space sectors' files (levels/space_sector_N), 0 other levels, -1 global;
 * `raw` = stored uncompressed, so a file position maps to an entry offset. */
struct FsPsarcSpan { uint64_t lo, hi; int kind; int space; int raw; char key[64]; };
static FsPsarcSpan* g_fd_spans[FS_MAX];
static int          g_fd_nspans[FS_MAX];
static void fs_psarc_index(int fd, FILE* f);   /* 1 if this fd is an open .usm movie (read tracker) */
static DIR*  g_dirs[FS_MAX];
static char  g_dir_path[FS_MAX][1024];   /* host path per open dir (for readdir stat) */

static int fd_alloc_file(FILE* f)
{
    for (int i = 3; i < FS_MAX; i++) if (!g_files[i] && !g_dirs[i]) { g_files[i] = f; return i; }
    return -1;
}
static int fd_alloc_dir(DIR* d)
{
    for (int i = 3; i < FS_MAX; i++) if (!g_files[i] && !g_dirs[i]) { g_dirs[i] = d; return i; }
    return -1;
}

/* ---- handlers ---- */
static void cellFsOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t flags  = (uint32_t)ctx->gpr[4];
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* NPDRM: see the note in sys_fs.c -- an EDAT is decrypted once into a cache
     * file and that is opened in its place. */
    { char dec_path[1200];
      const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);
      if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }

    /* fopen() mode strings can't express the PS3/POSIX open semantics (e.g.
     * O_WRONLY without create+truncate, or O_CREAT without O_TRUNC), so build
     * real open() flags from the access mode (low 2 bits) + the modifiers, then
     * wrap the fd in a FILE* with fdopen() (which neither creates nor truncates
     * -- that was already decided by open()). */
    int acc = flags & 0x3;
    int oflags = (acc == CELL_FS_O_RDWR)   ? O_RDWR
               : (acc == CELL_FS_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & CELL_FS_O_CREAT)  oflags |= O_CREAT;
    if (flags & CELL_FS_O_TRUNC)  oflags |= O_TRUNC;
    if (flags & CELL_FS_O_APPEND) oflags |= O_APPEND;

    /* A bare device root ("/dev_bdvd") resolves to a DIRECTORY, and open() on a
     * directory fails on Windows. Real cellFsOpen answers EISDIR there, and a
     * title reads that as "the medium is mounted" -- YDKJ probes the disc this
     * way before loading its Scaleform UI and FMOD banks, so the ENOENT it got
     * instead read as "no disc" and it drew nothing but a clear colour.
     * S_IFMT/S_IFDIR rather than S_ISDIR: MSVC's CRT does not define the macro. */
    {
        struct stat _dst;
        if (stat(hpath, &_dst) == 0 && (_dst.st_mode & S_IFMT) == S_IFDIR) {
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EISDIR;
            return;
        }
    }
    /* racacit: RACACIT_SKIP_MOVIES=1 -- see sys_fs_open: a Bink movie reads
     * as absent, so the title skips it. Some movies are probed through here. */
    { static int skip = -1;
      if (skip < 0) { const char* e = getenv("RACACIT_SKIP_MOVIES"); skip = (e && *e && *e != '0') ? 1 : 0; }
      size_t n = strlen(gpath);
      if (skip && n > 4 && gpath[n-4] == '.' && (gpath[n-3] | 32) == 'b' &&
          (gpath[n-2] | 32) == 'i' && (gpath[n-1] | 32) == 'k') {
          fprintf(stderr, "[movies] skipping %s (RACACIT_SKIP_MOVIES)\n", gpath);
          ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
      } }
    int hfd = open(hpath, oflags | O_BINARY, 0666);
    if (hfd < 0) {
        fprintf(stderr, "[fs] open FAIL '%s' -> '%s'\n", gpath, hpath);
        { const char* t = getenv("PS3_FS_TRAP"); if (t && *t && strstr(gpath, t)) { fprintf(stderr, "[fs] PS3_FS_TRAP hit\n"); __builtin_trap(); } }
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    const char* fmode = (acc == CELL_FS_O_RDWR)
                        ? ((flags & CELL_FS_O_APPEND) ? "ab+" : "rb+")
                        : (acc == CELL_FS_O_WRONLY)
                          ? ((flags & CELL_FS_O_APPEND) ? "ab" : "wb")
                          : "rb";
    FILE* f = fdopen(hfd, fmode);
    if (!f) {
        close(hfd);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
    }
    int fd = fd_alloc_file(f);
    if (fd < 0) { fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    g_fd_usm[fd] = (strstr(gpath, ".usm") != nullptr) ? 1 : 0;
    /* FS_CALLER=1: name the guest function that opened each file. A title whose
     * streaming layer opens a file and never reads it (You Don't Know Jack's
     * movies) is diagnosed from the opener: its sibling read path is the one
     * that is not running. */
    if (fd >= 0 && fd < FS_MAX) { strncpy(g_fd_path[fd], gpath, sizeof g_fd_path[fd]-1); g_fd_path[fd][sizeof g_fd_path[fd]-1]=0; }
    if (fd >= 0 && fd < FS_MAX) { const size_t L = strlen(gpath);
        if (L > 6 && !strcmp(gpath + L - 6, ".psarc")) fs_psarc_index(fd, f);
        else { free(g_fd_spans[fd]); g_fd_spans[fd] = nullptr; g_fd_nspans[fd] = 0; } }
    { char who[64] = "?";
      if (getenv("FS_CALLER")) { ppu_guest_caller(who, sizeof who);
          fprintf(stderr, "[fs] open '%s' -> fd %d  (opened by %s)\n", gpath, fd, who);
          /* FS_STACK=<substr>: every open funnels through one guest wrapper, so
           * one caller level says nothing about WHICH subsystem wanted the file.
           * Dump the guest call chain for the opens that matter. */
          { const char* want = getenv("FS_STACK");
            if (want && *want && strstr(gpath, want)) ydkj_host_bt("fs-open");
          } }
      else fprintf(stderr, "[fs] open '%s' -> fd %d\n", gpath, fd); }
    if (getenv("PS3_FSLOG_BT") && strstr(gpath, ".usm")) {
        /* Resolve to GUEST functions (raw host RVAs are useless here): this tells us
         * which criMv/criFs function opened the movie, so the reader-attach path
         * (stream+0x10, never wired => movie opened but never read) can be found. */
        fprintf(stderr,"[USMBT] open '%s' -> fd %d\n", gpath, fd); fflush(stderr);
        ydkj_host_bt("usm-open");
    }
    ctx->gpr[3] = CELL_OK;
}

/* cellFsSdataOpen(path, flags, fd*, arg*, size): opens a PS3 SDATA/EDAT
 * encrypted-data file and returns an fd whose reads yield the DECRYPTED
 * plaintext. LBP 1.30 stores its SPU job code modules inside patch.sdat (an
 * FSHb container, zlib-compressed C0DEC0DE modules) and opens it via this call;
 * leaving it faked (default CELL_OK, no handle) meant the container was never
 * read -> zero job modules loaded -> every SPU physics job dispatched into an
 * empty code buffer -> the loading freeze.
 *
 * Our run env's PS3_HDD0_ROOT (RPCS3 dev_hdd0) already holds patch.sdat in
 * DECRYPTED form (magic "FSHb", not "NPD"), so we just open it read-only --
 * no EDAT decryption needed. (A title supplying a still-encrypted NPD .sdat
 * would need the SDATA block cipher; not required here.) The license `arg`
 * (gpr[6]) and its size (gpr[7]) are ignored. */
static void cellFsSdataOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* Same NPDRM step cellFsOpen does: an encrypted container is decrypted once
     * into a cache file and that is opened in its place. This used to be a
     * second, weaker decryptor local to this function (sdata_decrypt below),
     * which handled the self-keyed SDATA form only and refused every real EDAT
     * -- so a title whose data file was EDAT rather than SDATA got "success
     * without a handle" here while the very same file opened fine through
     * cellFsOpen. One decryptor, reached from both doors. */
    { char dec_path[1200];
      const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);
      if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }

    int hfd = open(hpath, O_RDONLY | O_BINARY, 0666);
    if (hfd < 0) {
        fprintf(stderr, "[fs] SdataOpen FAIL '%s' -> '%s'\n", gpath, hpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    FILE* f = fdopen(hfd, "rb");
    if (!f) { close(hfd); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    /* If the file IS still NPD-encrypted, we can't serve it plaintext here --
     * warn loudly rather than hand back ciphertext the title will choke on.
     * Use portable fread/fseek (POSIX read()/ssize_t aren't available under the
     * exe's compiler). */
    unsigned char magic[4] = {0};
    size_t got = fread(magic, 1, 4, f);
    fseek(f, 0, SEEK_SET);
    if (got == 4 && magic[0]=='N' && magic[1]=='P' && magic[2]=='D') {
        /* NPD-encrypted SDATA: decrypt it in full and serve the plaintext from
         * an anonymous tmpfile so the title's reads see the real container. */
        fseek(f, 0, SEEK_END); long enc_sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t* enc = (enc_sz > 0) ? (uint8_t*)malloc((size_t)enc_sz) : nullptr;
        size_t rd = enc ? fread(enc, 1, (size_t)enc_sz, f) : 0;
        fclose(f);
        size_t dec_sz = 0;
        uint8_t* dec = (enc && rd == (size_t)enc_sz) ? sdata_decrypt(enc, rd, &dec_sz) : nullptr;
        free(enc);
        if (!dec) {
            fprintf(stderr, "[fs] SdataOpen '%s' NPD decrypt FAILED (unsupported "
                    "EDAT/needs license); returning success without a handle\n", gpath);
            ctx->gpr[3] = CELL_OK; return;   /* don't feed the title ciphertext */
        }
        char dmagic[4] = {0};
        if (dec_sz >= 4) memcpy(dmagic, dec, 4);
        FILE* tf = tmpfile();
        if (!tf || fwrite(dec, 1, dec_sz, tf) != dec_sz) {
            if (tf) fclose(tf); free(dec);
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
        }
        free(dec);
        rewind(tf);
        int fd = fd_alloc_file(tf);
        if (fd < 0) { fclose(tf); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
        if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
        fprintf(stderr, "[fs] SdataOpen '%s' -> fd %d (NPD decrypted, 0x%zX bytes, magic '%c%c%c%c')\n",
                gpath, fd, dec_sz,
                dmagic[0]?dmagic[0]:'?', dmagic[1]?dmagic[1]:'?',
                dmagic[2]?dmagic[2]:'?', dmagic[3]?dmagic[3]:'?');
        ctx->gpr[3] = CELL_OK;
        return;
    }
    int fd = fd_alloc_file(f);
    if (fd < 0) { fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    fprintf(stderr, "[fs] SdataOpen '%s' -> fd %d (magic %c%c%c%c)\n", gpath, fd,
            magic[0]?magic[0]:'?', magic[1]?magic[1]:'?', magic[2]?magic[2]:'?', magic[3]?magic[3]:'?');
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClose(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] close fd=%d  (by %s)\n", fd, w); }
    if (fd >= 0 && fd < FS_MAX && g_files[fd]) {
        if (g_fd_usm[fd] && getenv("PS3_FSLOG_READS")) fprintf(stderr, "[USMRD] CLOSE usm fd=%d\n", fd);
        fclose(g_files[fd]); g_files[fd] = nullptr; g_fd_usm[fd] = 0;
    }
    ctx->gpr[3] = CELL_OK;
}

/* Pre-fault a guest range so the kernel can write into it.
 *
 * The flat VM is MEM_RESERVEd and each page is committed on FIRST ACCESS by a
 * vectored exception handler. That covers CPU access from lifted code, but
 * fread/fwrite move data through the kernel, and a kernel write to a reserved
 * page does not raise a user-mode exception -- the I/O just fails, returning 0
 * with ferror set. Twisted Metal hit this on every read whose destination the
 * guest had not touched yet: read 1 of a file succeeded, read 2 into a fresh
 * page came back n=0 eof=0 err=1, and the title logged its own
 * "Short read ... Possible reasons include disc eject" and gave up.
 *
 * Touch each page read-then-write so the fault happens here, in user mode,
 * where the handler can commit it. Read-then-write rather than a plain store so
 * nothing already in the buffer is disturbed. */
static inline void fs_prefault(uint32_t buf, uint64_t len)
{
    if (!vm_base || !len) return;
    volatile uint8_t* p = (volatile uint8_t*)(vm_base + buf);
    for (uint64_t o = 0; o < len; o += 0x1000) p[o] = p[o];
    p[len - 1] = p[len - 1];
}

/* Each thread's last cellFsLseek: a read on that fd right after it must
 * start where it put the position. When it does not, another thread moved
 * the shared position in between -- the read returns some other part of the
 * file (logged as [fs] POSITION RACE). */
static thread_local int  t_seek_fd = -1;
static thread_local long t_seek_pos;

static int fs_psarc_kind(const char* name)
{
    if (strstr(name, "streaming_dialogue")) return 2;
    if (strstr(name, "streaming_sound")) return 4;
    return 0;
}

/* Dialogue lines, for the DualSense speaker. Every level's
 * resident_dialogue.<lang>.dat (in level_cached.psarc; the global one,
 * /sound/global/, in global_cached.psarc) lists its lines and where each
 * starts in the matching streaming_dialogue.<lang>.dat. IGHW sections:
 * 0x21200 = 64-byte names ("ss1_qwar_tap_water_r.us": level, speaker, ...),
 * 0x21300 = name -> line (u32 per name from +0x10, low half), 0x21100 =
 * line count at +4 and at +0x80 a pointer to the lines' sorted u32 offsets.
 * A dialogue read is then a known line, and its speaker decides: in the space
 * sectors everyone but Ratchet (speakers "ratc", "ratchet") talks through the
 * controller -- the radio. Mr. Zurkon ("zurk") talks through it anywhere: he
 * is a weapon. Not speech: "aph_*" (the Aphelion's boarding sounds) and
 * "mus_*" stored with the lines -- but for mus_ryno_1812 (global table),
 * the RYNO V's 1812 Overture: the game plays it as a line while the gun
 * fires, a 5 s piece read again and again. It is the weapon's sound, so it
 * goes to the controller anywhere, above the RYNO's rockets
 * (fs_ds_line_level, game/stubs.cpp racacit_ds_level), and stays on the TV
 * too (owner, 0.9.7: "the most iconic part of the weapon. It should be
 * prominent and the center piece of the weapon sounds"). RACACIT_DS_DIALOGUE:
 * space (default), all (on foot too), off. */
struct FsDlgTable { std::string key; std::vector<uint32_t> off; std::vector<std::string> name; };
static std::vector<FsDlgTable> g_dlg_tables;
static volatile long g_dlg_mu;
static volatile int g_dlg_in_space;   /* the level whose streams were read last is a space sector */
/* "<dir>|<lang>" for .../<dir>/{resident,streaming}_dialogue.<lang>.dat, <dir>
 * from "/built/" or "/sound/" on, so an installed copy's path matches the
 * PSARC entry's. Empty if the name is neither. */
static std::string fs_dlg_key(const char* path)
{
    const char* d = strstr(path, "/built/"); if (!d) d = strstr(path, "/sound/");
    const char* f = strrchr(path, '/');
    if (!d || !f || f < d) return std::string();
    const char* b = strstr(f, "_dialogue.");
    if (!b || (strncmp(f + 1, "resident_", 9) && strncmp(f + 1, "streaming_", 10))) return std::string();
    b += 10;
    const char* e = strchr(b, '.');
    if (!e) return std::string();
    return std::string(d, f - d) + "|" + std::string(b, e - b);
}
static void fs_dlg_parse(const std::string& key, const std::vector<unsigned char>& d)
{
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3] : 0; };
    if (d.size() < 0x40 || memcmp(d.data(), "IGHW", 4)) return;
    size_t names = 0, nsize = 0, map = 0, lines = 0;
    for (uint32_t i = 0, n = u32(8); i < n && i < 16; i++) {
        const size_t s = 0x20 + i * 16;
        const uint32_t id = u32(s), off = u32(s + 4), size = u32(s + 12);
        if (id == 0x21200u) { names = off; nsize = size; } else if (id == 0x21300u) map = off; else if (id == 0x21100u) lines = off;
    }
    if (!names || !map || !lines) return;
    const uint32_t cnt = u32(lines + 4), p = u32(lines + 0x80), nn = (uint32_t)(nsize / 64);
    if (!cnt || cnt > 100000u || (size_t)p + 4ull * cnt > d.size() || u32(map) != nn || names + nsize > d.size()) return;
    FsDlgTable t; t.key = key; t.off.resize(cnt); t.name.resize(cnt);
    for (uint32_t k = 0; k < cnt; k++) t.off[k] = u32(p + 4 * k);
    for (uint32_t i = 0; i < nn; i++) {
        const uint32_t m = u32(map + 0x10 + 4 * i);
        if ((m >> 16) != 1 || (m & 0xFFFF) >= cnt) continue;
        const char* s = (const char*)d.data() + names + (size_t)i * 64;
        t.name[m & 0xFFFF] = std::string(s, strnlen(s, 64));
    }
    while (__sync_lock_test_and_set(&g_dlg_mu, 1)) { }
    size_t k = 0; while (k < g_dlg_tables.size() && g_dlg_tables[k].key != key) k++;
    if (k < g_dlg_tables.size()) g_dlg_tables[k] = std::move(t); else g_dlg_tables.push_back(std::move(t));
    __sync_lock_release(&g_dlg_mu);
    fprintf(stderr, "[fs] dialogue lines %s: %u%c", key.c_str(), cnt, 10);
}
/* The dialogue line at file offset `at`: who speaks, and whether the
 * controller plays it (2) or not (0); *next = where the next line starts
 * (~0 if unknown). `space`: the file's own level (-1 = global: the level read
 * last decides). Logs each line once, when a read reaches its start. */
static thread_local char t_dlg_line[40];   /* the line fs_dlg_category found */
extern "C" volatile unsigned long long g_racacit_wav8_blocks;   /* cellAudio.c AUDIO_WAV8 */
static int fs_dlg_category(const char* key, uint64_t at, int space, uint64_t* next)
{
    static int mode = -1;
    if (mode < 0) { const char* e = getenv("RACACIT_DS_DIALOGUE");
                    mode = !e || !*e || *e == 's' || *e == 'S' ? 1 : (*e == 'a' || *e == 'A') ? 2 : 0; }   /* 1 space, 2 all, 0 off */
    const int in_space = space >= 0 ? space : g_dlg_in_space;
    std::string name; int start = 0;
    *next = ~0ull;
    while (__sync_lock_test_and_set(&g_dlg_mu, 1)) { }
    for (const FsDlgTable& t : g_dlg_tables)
        if (t.key == key) {
            size_t lo = 0, hi = t.off.size();   /* the last line starting at or before `at` */
            while (hi - lo > 1) { const size_t m = (lo + hi) / 2; if (t.off[m] <= at) lo = m; else hi = m; }
            if (!t.off.empty() && t.off[lo] <= at) { name = t.name[lo]; start = at - t.off[lo] <= 0x30;
                                                     if (lo + 1 < t.off.size()) *next = t.off[lo + 1]; }
            else if (!t.off.empty()) *next = t.off[0];
            break;
        }
    __sync_lock_release(&g_dlg_mu);
    const char* why;
    int who = 1;                                   /* 1 someone else, 0 Ratchet / not speech, 2 a weapon */
    if (name.empty()) why = "unknown line";
    else {
        const size_t u1 = name.find('_'), u2 = u1 == std::string::npos ? u1 : name.find('_', u1 + 1);
        const std::string t0 = name.substr(0, u1), t1 = u2 == std::string::npos ? std::string() : name.substr(u1 + 1, u2 - u1 - 1);
        if (!name.compare(0, 9, "mus_ryno_")) { who = 2; why = "the RYNO V's music, a weapon"; }
        else if (t0 == "aph" || t0 == "mus") { who = 0; why = "not speech"; }
        else if (t1 == "ratc" || t1 == "ratchet") { who = 0; why = "Ratchet"; }
        else if (t1 == "zurk") { who = 2; why = "Mr. Zurkon, a weapon"; }   /* deployed, he talks like a gun: anywhere */
        else if (t1 == "zoni") { who = 2; why = "a Zoni"; }   /* their calls when caught (gen_zoni_*): anywhere (owner, v54) */
        else if (t1 == "aphl") { who = 2; why = "the Aphelion"; }   /* the ship's radio: at landings too (tom_aphl_plat_intro_*, owner, v69) */
        else {
            /* Only those who call Ratchet over the radio, and the radio itself
             * (RACACIT_DS_RADIO, speaker codes = the 2nd token of a line name):
             * the story's contacts and the Aphelion, Kip's news, the stations'
             * hosts and shows. v49-v53 sent everyone but Ratchet, and the
             * depot/station NPCs (junk dealers ss1_junk2_*, the smuggler, ...)
             * talked through the controller (owner, v53). */
            static std::vector<std::string> radio;
            static volatile long rmu;
            while (__sync_lock_test_and_set(&rmu, 1)) { }
            if (radio.empty()) {
                const char* e = getenv("RACACIT_DS_RADIO");
                std::string l = e && *e ? e : "qwar,mqwar,azi,vors,nef,aphl,clnk,sig,kip,slag,maxz,dbot,mguy,pete,key,cari,libr";
                for (size_t a = 0; a <= l.size(); ) { size_t c = l.find(',', a); if (c == std::string::npos) c = l.size();
                                                      if (c > a) radio.push_back(l.substr(a, c - a)); a = c + 1; }
            }
            int ok = 0; for (const std::string& r : radio) if (r == t1) { ok = 1; break; }
            __sync_lock_release(&rmu);
            /* The game's own radio marks (v59): the ship radio's stations play
             * "spc_*" lines (DJs dbot/maxz/slag, ads mac, announcer anno, news
             * kip, plyx -- space_radio_data.lua; mac/anno/plyx were missing
             * from the list, so the radio reached the controller in blips),
             * and a line recorded through the radio filter ends in "_r"
             * (ss2_azi_family_r, the Valkyries' valk1_*_r, the smuggler's,
             * the junk dealers' and Terachnoids' calls, ...). */
            const size_t dot = name.find('.');
            const std::string stem = name.substr(0, dot);
            if (!ok && t0 == "spc") { ok = 1; why = "radio station"; }
            else if (!ok && stem.size() > 2 && !stem.compare(stem.size() - 2, 2, "_r")) { ok = 1; why = "radio call (_r)"; }
            else if (ok) why = "radio";
            else { who = 0; why = "not a radio contact"; }
        }
    }
    /* RACACIT_DS_LINE_SKIP: lines kept on the TV whatever the rules above
     * say (comma list of name prefixes; default none). gen_zoni_misc001 was
     * kept here from v67 ("a very very loud zoniii ... very jarring" at
     * every spawn, owner v66 -- three stacked copies, deduplicated since);
     * v77 brings it back, quieter (fs_ds_line_level): the owner misses the
     * Zoni's "weee wee wee weee ahhhh" on the controller (v76). */
    if (who && !name.empty()) {
        static std::vector<std::string> skip; static int skip_init; static volatile long smu;
        while (__sync_lock_test_and_set(&smu, 1)) { }
        if (!skip_init) {
            skip_init = 1;
            const char* e = getenv("RACACIT_DS_LINE_SKIP");
            std::string l = e ? e : "";
            for (size_t a = 0; a <= l.size(); ) { size_t c = l.find(',', a); if (c == std::string::npos) c = l.size();
                                                  if (c > a) skip.push_back(l.substr(a, c - a)); a = c + 1; }
        }
        for (const std::string& s : skip) if (!name.compare(0, s.size(), s)) { who = 0; why = "kept on the TV (RACACIT_DS_LINE_SKIP)"; break; }
        __sync_lock_release(&smu);
    }
    const int cat = who == 2 && mode ? 2 : (mode == 2 || (mode == 1 && in_space)) && who ? 2 : 0;
    snprintf(t_dlg_line, sizeof t_dlg_line, "%s", name.c_str());
    if (start && !name.empty()) {
        static std::vector<std::string> said; static volatile long mu;
        while (__sync_lock_test_and_set(&mu, 1)) { }
        int seen = 0; for (const std::string& s : said) if (s == name) { seen = 1; break; }
        if (!seen && said.size() < 400) said.push_back(name);
        __sync_lock_release(&mu);
        if (!seen && said.size() < 400)
            fprintf(stderr, "[ds-line] %s (%s, %s) -> %s (cap %.2f s)%c", name.c_str(), in_space ? "space" : "on foot", why,
                    cat ? "controller speaker" : "TV", g_racacit_wav8_blocks * 256.0 / 48000.0, 10);
    }
    return cat;
}
static void fs_psarc_index(int fd, FILE* f)
{
    free(g_fd_spans[fd]); g_fd_spans[fd] = nullptr; g_fd_nspans[fd] = 0;
    const long here = ftell(f);
    unsigned char h[32];
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 32, f) != 32 || memcmp(h, "PSAR", 4)) { fseek(f, here, SEEK_SET); return; }
    auto be32 = [](const unsigned char* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; };
    const uint32_t toc_len = be32(h + 12), ent = be32(h + 16), n = be32(h + 20), block = be32(h + 24);
    if (toc_len < 32 || toc_len > (64u << 20) || ent < 30 || !n || !block) { fseek(f, here, SEEK_SET); return; }
    unsigned char* toc = (unsigned char*)malloc(toc_len - 32);
    if (!toc || fread(toc, 1, toc_len - 32, f) != toc_len - 32) { free(toc); fseek(f, here, SEEK_SET); return; }
    const unsigned nb = block <= 0x10000u ? 2 : block <= 0x1000000u ? 3 : 4;
    const unsigned char* zt = toc + (size_t)n * ent;
    const size_t nz = ((toc_len - 32) - (size_t)n * ent) / nb;
    auto zsz = [&](uint32_t i) -> uint32_t { if (i >= nz) return block; uint32_t v = 0; for (unsigned k = 0; k < nb; k++) v = (v << 8) | zt[(size_t)i * nb + k]; return v ? v : block; };
    auto e_idx = [&](uint32_t i) { return be32(toc + (size_t)i * ent + 16); };
    auto e_len = [&](uint32_t i) { const unsigned char* q = toc + (size_t)i * ent + 20; uint64_t v = 0; for (int k = 0; k < 5; k++) v = (v << 8) | q[k]; return v; };
    auto e_off = [&](uint32_t i) { const unsigned char* q = toc + (size_t)i * ent + 25; uint64_t v = 0; for (int k = 0; k < 5; k++) v = (v << 8) | q[k]; return v; };
    /* the manifest (entry 0): the names of entries 1.., one per line */
    const uint64_t mlen = e_len(0);
    std::string man;
    if (mlen && mlen < (8u << 20)) {
        fseek(f, (long)e_off(0), SEEK_SET);
        std::vector<unsigned char> raw;
        for (uint32_t b = 0; man.size() < mlen; b++) {
            const uint32_t c = zsz(e_idx(0) + b), want = (uint32_t)((mlen - man.size()) < block ? (mlen - man.size()) : block);
            raw.resize(c);
            if (fread(raw.data(), 1, c, f) != c) break;
            if (c == want) man.append((const char*)raw.data(), c);
            else { std::vector<unsigned char> out(block); uLongf ol = block;
                   if (uncompress(out.data(), &ol, raw.data(), c) != Z_OK) break;
                   man.append((const char*)out.data(), ol); }
        }
    }
    std::vector<FsPsarcSpan> spans;
    size_t pos = 0;
    for (uint32_t i = 1; i < n && pos <= man.size(); i++) {
        size_t e = man.find((char)10, pos); if (e == std::string::npos) e = man.size();
        const std::string name = man.substr(pos, e - pos); pos = e + 1;
        const uint64_t len = e_len(i), off = e_off(i);
        if (strstr(name.c_str(), "/resident_dialogue.") && len && len < (4u << 20)) {   /* a dialogue line table */
            const std::string key = fs_dlg_key(name.c_str());
            std::vector<unsigned char> data, raw;
            fseek(f, (long)off, SEEK_SET);
            for (uint32_t b = 0; data.size() < len; b++) {
                const uint32_t c = zsz(e_idx(i) + b), want = (uint32_t)((len - data.size()) < block ? (len - data.size()) : block);
                raw.resize(c);
                if (fread(raw.data(), 1, c, f) != c) break;
                if (c == want) data.insert(data.end(), raw.begin(), raw.end());
                else { std::vector<unsigned char> out(block); uLongf ol = block;
                       if (uncompress(out.data(), &ol, raw.data(), c) != Z_OK) break;
                       data.insert(data.end(), out.begin(), out.begin() + ol); }
            }
            if (!key.empty() && data.size() == len) fs_dlg_parse(key, data);
            continue;
        }
        const int kind = fs_psarc_kind(name.c_str());
        if (!kind) continue;
        uint64_t ext = 0; const uint32_t nbk = (uint32_t)((len + block - 1) / block);
        for (uint32_t b = 0; b < nbk; b++) ext += zsz(e_idx(i) + b);
        FsPsarcSpan s; memset(&s, 0, sizeof s);
        s.lo = off; s.hi = off + ext; s.kind = kind; s.raw = ext == len;
        s.space = strstr(name.c_str(), "/levels/space_sector_") ? 1 : strstr(name.c_str(), "/levels/") ? 0 : -1;
        snprintf(s.key, sizeof s.key, "%s", fs_dlg_key(name.c_str()).c_str());
        spans.push_back(s);
        fprintf(stderr, "[fs] %s: %s at 0x%llX (%llu bytes) -> DualSense routing kind %d%c", g_fd_path[fd], name.c_str(),
                (unsigned long long)off, (unsigned long long)ext, kind, 10);
    }
    free(toc);
    if (!spans.empty()) {
        g_fd_spans[fd] = (FsPsarcSpan*)malloc(spans.size() * sizeof(FsPsarcSpan));
        if (g_fd_spans[fd]) { memcpy(g_fd_spans[fd], spans.data(), spans.size() * sizeof(FsPsarcSpan)); g_fd_nspans[fd] = (int)spans.size(); }
    }
    fseek(f, here, SEEK_SET);
}
static int fs_read_kind(int fd, long pos)
{
    const int k = fs_psarc_kind(g_fd_path[fd]);
    if (k) return k;
    for (int i = 0; i < g_fd_nspans[fd]; i++)
        if ((uint64_t)pos >= g_fd_spans[fd][i].lo && (uint64_t)pos < g_fd_spans[fd][i].hi) return g_fd_spans[fd][i].kind;
    return 0;
}

/* The DualSense speaker (tools/ps3recomp/libs/audio/ds_player.c) plays the
 * dialogue lines fs_dlg_category gives the controller, and the routed sounds
 * that stream (game/stubs.cpp racacit_ds_stream_expect), from the file. */
extern "C" int  ds_player_enabled(void);
extern "C" void ds_player_adpcm(const uint8_t* data, uint32_t bytes, uint32_t rate, float spk, float felt,
                                uint32_t owner, uint32_t delay_ms, const char* name);
extern "C" uint32_t ds_player_line(const uint8_t* data, uint32_t bytes, uint32_t rate, float spk, uint32_t delay_ms, const char* name);
extern "C" void ds_player_line_read(uint32_t token, uint32_t bytes);
extern "C" unsigned long long ps3_qpc_us(void);
/* The controller line whose data a descriptor is reading (file range): its
 * reads are zeroed in guest memory, so the TV plays silence where the
 * controller plays the line -- moved, not doubled (owner, v59).
 * RACACIT_DS_LINES_ON_TV=1 keeps them on both. */
static struct { uint64_t lo, hi; uint32_t token; int keep; } g_fd_mute[FS_MAX];   /* keep: followed, not muted */
static struct { uint32_t owner; float level; unsigned long long t; char name[48]; } g_ds_stream;
static volatile long g_ds_stream_mu;
extern "C" void racacit_ds_stream_expect(uint32_t owner, float level, const char* name)
{
    while (__sync_lock_test_and_set(&g_ds_stream_mu, 1)) { }
    g_ds_stream.owner = owner; g_ds_stream.level = level; g_ds_stream.t = ps3_qpc_us();
    snprintf(g_ds_stream.name, sizeof g_ds_stream.name, "%s", name ? name : "");
    __sync_lock_release(&g_ds_stream_mu);
}
/* A VAG header just read at file position `pos` on `fd`: play the `bytes` of
 * ADPCM that follow it, read from the file here. 1 if it played. */
static uint32_t g_ds_recent_token[8];   /* the line in fs_ds_play's recent[slot] */
static void fs_ds_recent_token(int slot, uint32_t token) { if (slot >= 0 && slot < 8) g_ds_recent_token[slot] = token; }
static int fs_ds_play(int fd, long pos, const uint8_t* h, float level, uint32_t owner, const char* name, uint32_t* token)
{
    const uint32_t bytes = ((uint32_t)h[12] << 24) | ((uint32_t)h[13] << 16) | ((uint32_t)h[14] << 8) | h[15];
    const uint32_t rate  = ((uint32_t)h[16] << 24) | ((uint32_t)h[17] << 16) | ((uint32_t)h[18] << 8) | h[19];
    if (bytes < 16 || bytes > (16u << 20) || rate < 4000 || rate > 96000) return 0;
    { static int dbg = -1; if (dbg < 0) { const char* e = getenv("RACACIT_DS_DEBUG"); dbg = e && *e == '1'; }
      if (dbg) { static int nl; if (nl++ < 60) { char hx[100]; int o = 0;
          for (int i = 0; i < 48; i++) o += snprintf(hx + o, sizeof hx - o, "%02X", h[i]);
          fprintf(stderr, "[ds-dbg] VAG header of %s: %s%c", name, hx, 10); } } }
    int recent_slot = -1;
    /* The same line again within a second (the game reads a line's header
     * once per emitter or retry -- three reads for gen_zoni_misc001 at a
     * spawn): one copy on the controller, not three stacked at full level.
     * Its TV reads are still muted. */
    {
        static struct { char name[48]; unsigned long long t; } recent[8]; static int next_slot; static volatile long rmu;
        const unsigned long long now = ps3_qpc_us();
        int dup = 0;
        while (__sync_lock_test_and_set(&rmu, 1)) { }
        for (int i = 0; i < 8; i++)
            if (recent[i].t && now - recent[i].t < 1000000ull && !strncmp(recent[i].name, name, sizeof recent[i].name - 1)) { dup = 1; if (token) *token = g_ds_recent_token[i]; break; }
        if (!dup) { snprintf(recent[next_slot].name, sizeof recent[next_slot].name, "%s", name); recent[next_slot].t = now; g_ds_recent_token[next_slot] = 0; }
        const int slot = next_slot;
        if (!dup) next_slot = (next_slot + 1) % 8;
        __sync_lock_release(&rmu);
        if (dup) return 1;
        recent_slot = slot;
    }
    std::vector<uint8_t> d(bytes);
    FILE* f = g_files[fd];
    const long here = ftell(f);
    const int ok = fseek(f, pos + 48, SEEK_SET) == 0 && fread(d.data(), 1, bytes, f) == bytes;
    fseek(f, here, SEEK_SET);
    if (!ok) return 0;
    /* The game starts the line ~70 ms after it reads it (its stream buffers
     * first; a capture matched our copy to the TV's at +69..75 ms):
     * RACACIT_DS_LINE_DELAY_MS, default 70. */
    static int delay = -1;
    if (delay < 0) { const char* e = getenv("RACACIT_DS_LINE_DELAY_MS"); delay = e ? atoi(e) : 70; if (delay < 0) delay = 0; }
    if (token) {   /* a line: the copy that follows the game's reads of it (ds_player.c) */
        *token = ds_player_line(d.data(), bytes, rate, level, (uint32_t)delay, name);
        fs_ds_recent_token(recent_slot, *token);
    } else ds_player_adpcm(d.data(), bytes, rate, level, 0.0f, owner, (uint32_t)delay, name);
    return 1;
}
/* The controller lines' level: RACACIT_DS_LINE_LEVEL (default 0.63, the
 * game's own dialogue gain on the TV centre), the ship radio's stations
 * ("spc_*": DJs, ads, news) RACACIT_DS_RADIO_LEVEL of that (default 0.35:
 * "just too loud ... maybe 30-40 percent", owner, v59). Mr. Zurkon's lines
 * ("_zurk_") 1.15x that since v69: he is the one weapon above the others'
 * baseline (owner, v68; game/stubs.cpp racacit_ds_level). */
static float fs_ds_line_level(const char* line)
{
    static float lv = -1.0f, radio = -1.0f;
    if (lv < 0.0f) { const char* e = getenv("RACACIT_DS_LINE_LEVEL"); lv = e ? (float)atof(e) : 0.63f;
                     e = getenv("RACACIT_DS_RADIO_LEVEL"); radio = e ? (float)atof(e) : 0.35f; }
    if (strstr(line, "_zurk_")) return lv * 1.15f;
    if (!strncmp(line, "mus_ryno_", 9)) return lv * 1.45f;   /* the RYNO V's overture: over its rockets (0.30) */
    if (!strncmp(line, "gen_zoni_misc001", 16)) return lv * 0.55f;   /* the spawn/collect "weee" (v77): present, not jarring */
    return !strncmp(line, "spc_", 4) ? lv * radio : lv;
}

static void cellFsRead(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    uint64_t raw_nbytes = ctx->gpr[5];
    long fpos_before = ftell(g_files[fd]);
    if (t_seek_fd == fd) {
        if (fpos_before != t_seek_pos) {
            static int races; if (races < 64) { races++;
                fprintf(stderr, "[fs] POSITION RACE fd=%d: this thread seeked to %ld, the read starts at %ld (%d so far)%c",
                        fd, t_seek_pos, fpos_before, races, 10); }
        }
        t_seek_fd = -1;
    }
    if (ppu_vm_size && (uint64_t)buf + nbytes > ppu_vm_size) nbytes = ppu_vm_size - buf;
    fs_prefault(buf, nbytes);
    size_t n = fread(vm_base + buf, 1, (size_t)nbytes, g_files[fd]);   /* raw bytes, no swap */
    /* racacit: the DualSense speaker. A dialogue line's playback starts with
     * a read of its 48-byte VAG header ("VAGp", data size at +12, sample rate
     * at +16) right before its data; a line fs_dlg_category gives the
     * controller is played from the file by the controller's own player and
     * its reads here are zeroed. The game also reads the whole file in 256 KB
     * staging chunks -- those are left alone (no header read precedes them).
     * Streamed sounds (kind 4) the same way, right after a routed _stream /
     * Groovitron track starts (racacit_ds_stream_expect). */
    if (n && ds_player_enabled()) {
        const int kind = fs_read_kind(fd, fpos_before);
        const FsPsarcSpan* sp = nullptr;
        for (int i = 0; i < g_fd_nspans[fd]; i++)
            if ((uint64_t)fpos_before >= g_fd_spans[fd][i].lo && (uint64_t)fpos_before < g_fd_spans[fd][i].hi) { sp = &g_fd_spans[fd][i]; break; }
        const int space = sp ? sp->space : strstr(g_fd_path[fd], "/levels/space_sector_") ? 1 : strstr(g_fd_path[fd], "/levels/") ? 0 : -1;
        if (kind && space >= 0) g_dlg_in_space = space;
        const int header = n == 48 && !memcmp(vm_base + buf, "VAGp", 4);
        if (kind == 2 && header) {
            const std::string key = sp ? std::string(sp->key) : fs_dlg_key(g_fd_path[fd]);
            const int known = sp ? sp->raw : 1;
            const uint64_t at = sp ? (uint64_t)fpos_before - sp->lo : (uint64_t)fpos_before;
            uint64_t nx;
            t_dlg_line[0] = 0;
            const int cat = fs_dlg_category(known ? key.c_str() : "", at, space, &nx);
            g_fd_mute[fd].lo = g_fd_mute[fd].hi = 0; g_fd_mute[fd].token = 0; g_fd_mute[fd].keep = 0;
            uint32_t line_token = 0;
            if (cat && t_dlg_line[0] && fs_ds_play(fd, fpos_before, vm_base + buf, fs_ds_line_level(t_dlg_line), 0, t_dlg_line, &line_token)) {
                static int keep = -1; if (keep < 0) { const char* e = getenv("RACACIT_DS_LINES_ON_TV"); keep = e && *e == '1'; }
                /* Music (the RYNO V's overture) plays on both: its reads are
                 * followed for the controller's copy, not zeroed. */
                const int music = !strncmp(t_dlg_line, "mus_", 4);
                if (!keep || music) {
                    const uint8_t* h = vm_base + buf;
                    g_fd_mute[fd].lo = (uint64_t)fpos_before + 48u;
                    g_fd_mute[fd].hi = g_fd_mute[fd].lo + (((uint32_t)h[12] << 24) | ((uint32_t)h[13] << 16) | ((uint32_t)h[14] << 8) | h[15]);
                    g_fd_mute[fd].token = line_token;
                    g_fd_mute[fd].keep = music;
                }
            }
        } else if (g_fd_mute[fd].hi > g_fd_mute[fd].lo) {
            const uint64_t a = (uint64_t)fpos_before, b = a + n;
            const uint64_t lo = a > g_fd_mute[fd].lo ? a : g_fd_mute[fd].lo, hi = b < g_fd_mute[fd].hi ? b : g_fd_mute[fd].hi;
            if (lo < hi) {
                if (!g_fd_mute[fd].keep) memset(vm_base + buf + (lo - a), 0, (size_t)(hi - lo));
                ds_player_line_read(g_fd_mute[fd].token, (uint32_t)(hi - g_fd_mute[fd].lo));
            }
        }
        if (kind == 4 && header && g_ds_stream.t) {
            while (__sync_lock_test_and_set(&g_ds_stream_mu, 1)) { }
            const int fresh = ps3_qpc_us() - g_ds_stream.t < 2000000ull;
            const uint32_t owner = g_ds_stream.owner; const float level = g_ds_stream.level;
            char name[48]; snprintf(name, sizeof name, "%s", g_ds_stream.name);
            g_ds_stream.t = 0;
            __sync_lock_release(&g_ds_stream_mu);
            if (fresh) fs_ds_play(fd, fpos_before, vm_base + buf, level, owner, name, nullptr);
        }
    }
    /* PS3_FSLOG_EOF=<fd>,<bytes>: report end-of-file past <bytes> on one descriptor.
     * The intro cinematic is 28 seconds and this port renders it at a few frames
     * a second, so it cannot be watched to its end -- truncating the stream makes
     * the demuxer see EOF and the movie finish, which is what advances the title
     * to whatever follows the intro. Deliberately a testing knob, not a fix. */
    { static int efd = -2; static long elim = 0;
      if (efd == -2) { const char* e = getenv("PS3_FSLOG_EOF");
          if (e) { efd = atoi(e); const char* c = strchr(e, 44); elim = c ? atol(c + 1) : 0; }
          else efd = -1; }
      if (efd >= 0 && fd == efd && elim > 0 && fpos_before >= elim) {
          static int once = 0;
          if (!once++) fprintf(stderr, "[fs] PS3_FSLOG_EOF: fd=%d truncated at %ld bytes\n", fd, elim);
          n = 0;
      } }
    if (g_fd_usm[fd] && getenv("PS3_FSLOG_READS")) fprintf(stderr, "[USMRD] READ usm fd=%d nbytes=%llu -> %zu magic=%02X%02X%02X%02X pos=%ld lr=0x%08X\n", fd, (unsigned long long)nbytes, n, vm_base[buf], vm_base[buf+1], vm_base[buf+2], vm_base[buf+3], fpos_before, (uint32_t)ctx->lr);
    /* PS3_FSLOG_READS=<fd>: log every read on one descriptor. PS3_FSLOG caps at 20
     * lines and they are all spent before a movie ever opens, so it cannot
     * answer "is the streamer reading the .avi". */
    { static int wfd = -2;
      if (wfd == -2) { const char* e = getenv("PS3_FSLOG_READS"); wfd = e ? atoi(e) : -1; }
      if (wfd >= 0 && fd == wfd)
          fprintf(stderr, "[fsread] fd=%d want=%llu got=%zu pos=%ld\n",
                  fd, (unsigned long long)nbytes, n, fpos_before); }
    if (getenv("PS3_FSLOG")) { static int _fd=0; if((_fd < 20 && (_fd++, 1))) fprintf(stderr,"[FSDBG] fd=%d raw_nbytes=0x%llX clamped=0x%llX buf=0x%08X fpos_before=%ld n=%zu eof=%d err=%d\n", fd,(unsigned long long)raw_nbytes,(unsigned long long)nbytes,buf,fpos_before,n,feof(g_files[fd]),ferror(g_files[fd])); }
#ifdef _WIN32
    if (getenv("PS3_FSLOG") && buf==0 && raw_nbytes>0x10000) { static int _b=0; if((_b < 2 && (_b++, 1))){ void* fr[30]; unsigned short nn=RtlCaptureStackBackTrace(0,30,fr,0); uintptr_t mb=(uintptr_t)GetModuleHandleA(0); fprintf(stderr,"[FSBT] null-buf read caller rvas:"); for(unsigned short i=0;i<nn&&i<16;i++) fprintf(stderr," %llX",(unsigned long long)((uintptr_t)fr[i]-mb)); fprintf(stderr,"\n"); } }
#endif
    /* Per-fd totals, not just the first 50 lines. The flat cap made "this file is
     * opened and never read" unfalsifiable: reads on a later-opened fd fall off
     * the end of the log and look identical to reads that never happen.
     * FS_READ_ALL=1 logs every read; otherwise a per-fd first-read line plus a
     * periodic summary is enough to tell the two apart. */
    { static uint64_t tot=0; static int _n=0;
      static uint64_t per_fd[64]; static uint32_t cnt_fd[64];
      tot+=n;
      if (fd>=0 && fd<64) { per_fd[fd]+=n; cnt_fd[fd]++; }
      int first_for_fd = (fd>=0 && fd<64 && cnt_fd[fd]==1);
      if(_n++<50 || first_for_fd || getenv("FS_READ_ALL"))
        fprintf(stderr,"[fs] read fd=%d nbytes=%llu -> %zu (magic=%02X%02X%02X%02X, total=%llu)%s\n",
                fd,(unsigned long long)nbytes,n,vm_base[buf],vm_base[buf+1],vm_base[buf+2],vm_base[buf+3],
                (unsigned long long)tot, first_for_fd?"  <= FIRST READ ON THIS FD":"");
      if (getenv("FS_READ_PATH") && fd>=0 && fd<64 && g_fd_path[fd][0])
          fprintf(stderr, "        from %s\n", g_fd_path[fd]);
      if ((_n % 2000)==0) { fprintf(stderr,"[fs] read summary after %d reads:",_n);
          for (int i=0;i<64;i++) if (cnt_fd[i]) fprintf(stderr," fd%d=%ux/%lluB",i,cnt_fd[i],(unsigned long long)per_fd[i]);
          fprintf(stderr,"\n"); } }
    if (getenv("PS3_FSLOG_TOC") && nbytes >= 50000) {  /* data.toc read -> who parses it? */
        fprintf(stderr, "[TOC] data.toc read into buf=0x%08X n=%zu; lr=0x%08llX; guest-stack RAs:\n", buf, n, (unsigned long long)ctx->lr);
        uint32_t sp = (uint32_t)ctx->gpr[1];
        for (uint32_t i = 0; i < 128 && sp + i*4 + 4 <= ppu_vm_size; i++) {
            uint32_t a = sp + i*4; uint32_t w = (vm_base[a]<<24)|(vm_base[a+1]<<16)|(vm_base[a+2]<<8)|vm_base[a+3];
            if (w >= 0x10000u && w < 0x600000u) fprintf(stderr, "[TOC]   ra 0x%08X (@sp+0x%X)\n", w, i*4);
        }
    }
    if (nread_ptr) vm_write64(nread_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsWrite(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nwr_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    fs_prefault(buf, nbytes);   /* kernel READS the buffer; same reserved-page trap */
    size_t n = fwrite(vm_base + buf, 1, (size_t)nbytes, g_files[fd]);
    /* DIAGNOSTIC (PS3_FSLOG_BT=1): dump the guest back-chain when the game logs the
     * render-config failure, to locate setScreenRenderTargetInternal & the config obj. */
    if (getenv("PS3_FSLOG_BT") && buf && nbytes > 0 && nbytes < 4096 && vm_base) {
        char tmp[256]; uint32_t nn = (uint32_t)(nbytes < 255 ? nbytes : 255);
        memcpy(tmp, vm_base + buf, nn); tmp[nn] = 0;
        if (strstr(tmp,"config") || strstr(tmp,"Config") || strstr(tmp,"Mystery") ||
            strstr(tmp,"downsample") || strstr(tmp,"RenderTarget")) {
            uint32_t sp = (uint32_t)ctx->gpr[1];
            fprintf(stderr, "[cfgbt] write \"%.60s\" lr=0x%08X sp=0x%08X\n", tmp, (uint32_t)ctx->lr, sp);
            for (int i = 0; i < 24 && sp && sp < 0x10000000u; i++) {
                uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                if (nsp <= sp || nsp >= 0x10000000u) break;
                uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                fprintf(stderr, "[cfgbt]   #%d lr=0x%08X\n", i, lr);
                sp = nsp;
            }
            fflush(stderr);
        }
    }
    if (nwr_ptr) vm_write64(nwr_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

/* cellFsFGetBlockSize(int fd, u64* sector_size, u64* block_size)
 * cellFsGetBlockSize(const char* path, u64* sector_size, u64* block_size)
 *
 * Titles ask the filesystem what granularity it wants and then size and ALIGN
 * their reads to the answer. Left unregistered, the call fell through to the
 * unresolved-NID path: the out-params were never written, so the title read
 * whatever was already in them.
 *
 * The Orange Box's chooser does exactly this. With no answer it re-seeks to
 * offset 0 and re-reads the same 64 KB of APP_CHOOSER.GRP forever -- 241,997
 * reads and 15.86 GB of I/O against a 4 MB file in one run -- because the
 * offset arithmetic it derives from the block size collapses to zero.
 *
 * Both out-params are u64 and big-endian, so they go through vm_write64.
 * PS3_FS_BLOCK_SIZE overrides the value for a title that wants a different
 * granularity. */
static void fs_report_block_size(ppu_context* ctx, uint32_t sec_ptr, uint32_t blk_ptr)
{
    static uint64_t bs = 0;
    if (!bs) {
        const char* e = getenv("PS3_FS_BLOCK_SIZE");
        bs = (e && *e) ? (uint64_t)strtoull(e, nullptr, 0) : 4096ull;
        if (!bs) bs = 4096ull;
    }
    if (sec_ptr) vm_write64(sec_ptr, bs);
    if (blk_ptr) vm_write64(blk_ptr, bs);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFGetBlockSize(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO;
        return;
    }
    fs_report_block_size(ctx, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

static void cellFsGetBlockSize(ppu_context* ctx)
{
    /* Path variant: the answer does not depend on the file, only the device,
     * and every mount we serve is backed by the same host filesystem. */
    fs_report_block_size(ctx, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

static void cellFsLseek(ppu_context* ctx)
{
    int fd        = (int)(uint32_t)ctx->gpr[3];
    int64_t off   = (int64_t)ctx->gpr[4];
    uint32_t wh   = (uint32_t)ctx->gpr[5];
    uint32_t pos_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] lseek fd=%d off=%lld whence=%u  (by %s)\n", fd, (long long)off, wh, w); }
    int worigin = (wh == CELL_FS_SEEK_END) ? SEEK_END : (wh == CELL_FS_SEEK_CUR) ? SEEK_CUR : SEEK_SET;
    fseek(g_files[fd], (long)off, worigin);
    long p = ftell(g_files[fd]);
    t_seek_fd = fd; t_seek_pos = p;
    if (pos_ptr) vm_write64(pos_ptr, (uint64_t)p);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsStat is 0x34 (52) bytes, 4-byte aligned -- the s64/u64 members are
 * be_t<...,4> so there is NO 4-byte pad after gid (verified vs RPCS3:
 * CHECK_SIZE_ALIGN(CellFsStat, 52, 4)). Laying it out 8-byte-aligned (0x38,
 * pad@0x0C) shifts size/blksize +4 and overruns the struct by 4 bytes -- games
 * that embed a CellFsStat inside a larger object (e.g. Dantelion's
 * DLFileDeviceStream, stat@obj+0xD8) then have the trailing blksize clobber the
 * field right after the stat (the fd at obj+0x10c), which later fails lseek.
 * Layout: mode@0 uid@4 gid@8 atime@0x0C mtime@0x14 ctime@0x1C size@0x24 blksize@0x2C. */
static void write_stat(uint32_t sb, uint32_t mode, uint64_t size)
{
    vm_write32(sb + 0x00, mode);
    vm_write32(sb + 0x04, 0);            /* uid */
    vm_write32(sb + 0x08, 0);            /* gid */
    vm_write64(sb + 0x0C, 0);            /* atime */
    vm_write64(sb + 0x14, 0);            /* mtime */
    vm_write64(sb + 0x1C, 0);            /* ctime */
    vm_write64(sb + 0x24, size);         /* size */
    vm_write64(sb + 0x2C, 0x200);        /* blksize */
}

static void cellFsStat(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t sb = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    struct stat st;
    if (stat(hpath, &st) != 0) {
        if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] stat '%s' -> ENOENT\n", gpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] stat '%s' -> OK (size=%lld)\n", gpath, (long long)st.st_size);
    uint32_t mode = (st.st_mode & S_IFDIR) ? (CELL_FS_S_IFDIR | 0x1FF)
                                           : (CELL_FS_S_IFREG | 0x1B6);
    if (sb) write_stat(sb, mode, (uint64_t)st.st_size);
    if (getenv("PS3_FSLOG") && strstr(gpath,".toc")) fprintf(stderr,"[FSDBG] cellFsStat('%s') -> size=0x%llX\n",gpath,(unsigned long long)st.st_size);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFstat(ppu_context* ctx)
{
    int fd      = (int)(uint32_t)ctx->gpr[3];
    uint32_t sb = (uint32_t)ctx->gpr[4];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] fstat fd=%d  (by %s)\n", fd, w); }
    long cur = ftell(g_files[fd]);
    fseek(g_files[fd], 0, SEEK_END);
    long sz = ftell(g_files[fd]);
    fseek(g_files[fd], cur, SEEK_SET);
    /* FS_FSTAT_CAP=<bytes>: DIAGNOSTIC. Report a smaller size than the file
     * has. You Don't Know Jack reads a 100 KB archive whole but only probes
     * (open/fstat/close) its 500 KB and 2.4 MB ones; if that branch is driven
     * by the size it just asked for, capping it makes the title take the read
     * path. Answers whether the split is size-based -- nothing more. */
    { const char* cap = getenv("FS_FSTAT_CAP");
      const char* only = getenv("FS_FSTAT_CAP_PATH");
      if (cap && only && *only && !(g_fd_path[fd][0] && strstr(g_fd_path[fd], only))) cap = 0;
      if (cap) { long c = strtol(cap, 0, 0);
          if (c > 0 && sz > c) {
              fprintf(stderr, "[fs] fstat fd=%d size %ld -> capped %ld (FS_FSTAT_CAP)\n", fd, sz, c);
              sz = c; } } }
    if (sb) write_stat(sb, CELL_FS_S_IFREG | 0x1B6, (uint64_t)sz);
    if (getenv("PS3_FSLOG")) { static int _n=0; if((_n < 12 && (_n++, 1))) fprintf(stderr,"[FSDBG] cellFsFstat(fd=%d) -> size=0x%lX\n",fd,sz); }
    ctx->gpr[3] = CELL_OK;
}

static void cellFsOpendir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    DIR* d = opendir(hpath);
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] opendir '%s' -> %s\n", gpath, d ? "OK" : "ENOENT");
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    int fd = fd_alloc_dir(d);
    if (fd < 0) { closedir(d); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    strncpy(g_dir_path[fd], hpath, sizeof g_dir_path[fd] - 1);
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsDirent: d_type(1) d_namlen(1) d_name[256]; total 0x102. */
static void cellFsReaddir(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t dirent = (uint32_t)ctx->gpr[4];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[5];
    if (fd < 0 || fd >= FS_MAX || !g_dirs[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    struct dirent* e = readdir(g_dirs[fd]);
    if (!e) { if (nread_ptr) vm_write64(nread_ptr, 0); ctx->gpr[3] = CELL_OK; return; }
    char full[1300]; struct stat st;
    snprintf(full, sizeof full, "%s/%s", g_dir_path[fd], e->d_name);
    uint8_t type = (stat(full, &st) == 0 && (st.st_mode & S_IFDIR))
                   ? CELL_FS_TYPE_DIR : CELL_FS_TYPE_REG;
    size_t nl = strlen(e->d_name); if (nl > 255) nl = 255;
    vm_base[dirent + 0] = type;
    vm_base[dirent + 1] = (uint8_t)nl;
    for (size_t i = 0; i < nl; i++) vm_base[dirent + 2 + i] = (uint8_t)e->d_name[i];
    vm_base[dirent + 2 + nl] = 0;
    if (nread_ptr) vm_write64(nread_ptr, 0x102);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClosedir(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd >= 0 && fd < FS_MAX && g_dirs[fd]) { closedir(g_dirs[fd]); g_dirs[fd] = nullptr; }
    ctx->gpr[3] = CELL_OK;
}

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

/* Create `path` and any missing parent dirs on the host. Returns 0 on
 * success or if the directory already exists. */
static int host_mkdir_p(const char* path)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len-1] == '/' || tmp[len-1] == '\\')) tmp[--len] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; HOST_MKDIR(tmp); *p = c; }
    }
    int r = HOST_MKDIR(tmp);
    if (r != 0) { struct stat st; if (stat(tmp, &st) == 0 && (st.st_mode & S_IFDIR)) r = 0; }
    return r;
}

/* cellFsMkdir(path, mode): create the guest directory on the host. A no-op stub
 * here silently breaks games that create then poll a dir (e.g. LBP's boot waits
 * for /dev_hdd0/game/<title>/USRDIR to appear). Create parents too. */
static void cellFsMkdir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    host_path(hpath, sizeof hpath, gpath);
    int r = host_mkdir_p(hpath);
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] mkdir '%s' -> '%s' (%s)\n", gpath, hpath, r == 0 ? "OK" : "FAIL");
    ctx->gpr[3] = CELL_OK;   /* existing dir is benign for boot */
}
static void cellFsRmdir(ppu_context* ctx)  { ctx->gpr[3] = CELL_OK; }
static void cellFsUnlink(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
static void cellFsFsync(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd >= 0 && fd < FS_MAX && g_files[fd]) fflush(g_files[fd]);
    ctx->gpr[3] = CELL_OK;
}
/* File-area preallocation (LBP cache warm-up spams this): host filesystems
 * grow files on write, so accepting is correct -- no preallocation needed. */
static void cellFsAllocateFileAreaWithoutZeroFill(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

/* ---- cellFs AIO -----------------------------------------------------------
 *
 * Scott Pilgrim loads everything through cellFsAioRead: it opens gamedata.fat
 * with cellFsOpen and then never calls cellFsRead again. With the AIO NIDs
 * unresolved, its loader thread submitted requests that nothing ever completed
 * and the whole boot parked -- one open, no reads, and the main thread spinning.
 *
 * CellFsAio, big-endian, 0x28 bytes:
 *   0x00 u32 fd    0x08 u64 offset    0x10 u32 buf    0x18 u64 size    0x20 u64 user_data
 *
 * The completion callback is (CellFsAio* aio, s32 error, s32 id, u64 size).
 *
 * ponytail: the read runs synchronously and the callback fires before the
 * submit call returns. A title that submits, THEN arms the thing the callback
 * signals, would miss it -- move completion to a worker thread if one shows up.
 */
#ifdef _WIN32
#  define HOST_FSEEK64(f,off) _fseeki64((f), (__int64)(off), SEEK_SET)
#else
#  define HOST_FSEEK64(f,off) fseeko((f), (off_t)(off), SEEK_SET)
#endif

static uint32_t guest_be32(uint32_t a)
{
    if (ppu_vm_size && (uint64_t)a + 4 > ppu_vm_size) return 0;
    return ((uint32_t)vm_base[a] << 24) | ((uint32_t)vm_base[a+1] << 16) |
           ((uint32_t)vm_base[a+2] << 8) | (uint32_t)vm_base[a+3];
}
static uint64_t guest_be64(uint32_t a)
{
    return ((uint64_t)guest_be32(a) << 32) | guest_be32(a + 4);
}

static void cellFsAioInit(ppu_context* ctx)   { ctx->gpr[3] = CELL_OK; }
static void cellFsAioFinish(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
/* Nothing is ever outstanding, so a cancel has nothing to find. Real cellFs
 * answers CELL_FS_ENOENT for an unknown id and callers treat that as "already
 * done", which is exactly true here. */
static void cellFsAioCancel(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

static void cellFsAioRead(ppu_context* ctx)
{
    uint32_t aio    = (uint32_t)ctx->gpr[3];
    uint32_t id_ptr = (uint32_t)ctx->gpr[4];
    uint32_t cb_opd = (uint32_t)ctx->gpr[5];

    int      fd     = (int)guest_be32(aio + 0x00);
    uint64_t offset = guest_be64(aio + 0x08);
    uint32_t buf    = guest_be32(aio + 0x10);
    uint64_t size   = guest_be64(aio + 0x18);

    static int32_t s_next_id = 1;
    int32_t id = s_next_id++;
    if (id_ptr) vm_write32(id_ptr, (uint32_t)id);

    size_t n = 0;
    int32_t err = CELL_FS_ENOENT;
    if (fd >= 0 && fd < FS_MAX && g_files[fd]) {
        if (ppu_vm_size && (uint64_t)buf + size > ppu_vm_size) size = ppu_vm_size - buf;
        fs_prefault(buf, size);
        /* AIO reads are absolute -- they do not disturb the fd's own file
         * position, and the guest interleaves them freely across threads. */
        if (HOST_FSEEK64(g_files[fd], offset) == 0)
            n = fread(vm_base + buf, 1, (size_t)size, g_files[fd]);
        err = CELL_OK;
    }
    if (getenv("PS3_FSLOG"))
        fprintf(stderr, "[fs] aio read id=%d fd=%d off=%llu size=%llu -> %zu\n",
                id, fd, (unsigned long long)offset, (unsigned long long)size, n);

    ctx->gpr[3] = CELL_OK;
    if (cb_opd) ps3_invoke_guest(cb_opd, aio, (uint64_t)(int64_t)err,
                                 (uint64_t)(int64_t)id, (uint64_t)n, 0, 0, 0, 0);
}

extern "C" void ppu_fs_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpen"),     "cellFsOpen",     cellFsOpen);
    /* Register by the literal import NID: LBP imports 0xB1840B53 for
     * cellFsSdataOpen, which ps3_compute_nid("cellFsSdataOpen") does NOT match
     * (the real exported symbol name differs from the friendly name). */
    ps3_hle_register_ctx(0xB1840B53u, "cellFsSdataOpen", cellFsSdataOpen);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClose"),    "cellFsClose",    cellFsClose);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRead"),     "cellFsRead",     cellFsRead);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsWrite"),    "cellFsWrite",    cellFsWrite);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsLseek"),    "cellFsLseek",    cellFsLseek);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFGetBlockSize"), "cellFsFGetBlockSize", cellFsFGetBlockSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsGetBlockSize"),  "cellFsGetBlockSize",  cellFsGetBlockSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsStat"),     "cellFsStat",     cellFsStat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFstat"),    "cellFsFstat",    cellFsFstat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpendir"),  "cellFsOpendir",  cellFsOpendir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsReaddir"),  "cellFsReaddir",  cellFsReaddir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClosedir"), "cellFsClosedir", cellFsClosedir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsMkdir"),    "cellFsMkdir",    cellFsMkdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRmdir"),    "cellFsRmdir",    cellFsRmdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsUnlink"),   "cellFsUnlink",   cellFsUnlink);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFsync"),    "cellFsFsync",    cellFsFsync);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsAllocateFileAreaWithoutZeroFill"),
                         "cellFsAllocateFileAreaWithoutZeroFill", cellFsAllocateFileAreaWithoutZeroFill);
    /* AIO by literal import NID -- ps3_compute_nid() of the friendly name does
     * not match the exported symbols. */
    ps3_hle_register_ctx(0xDB869F20u, "cellFsAioInit",   cellFsAioInit);
    ps3_hle_register_ctx(0x9F951810u, "cellFsAioFinish", cellFsAioFinish);
    ps3_hle_register_ctx(0xC1C507E7u, "cellFsAioRead",   cellFsAioRead);
    ps3_hle_register_ctx(0x7F13FC8Cu, "cellFsAioCancel", cellFsAioCancel);
}
