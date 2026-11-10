/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * The trophy set is the game's own, read from the disc the player dumped:
 * PS3_GAME/TROPDIR/<NPCOMMID>_00/TROPHY.TRP (a TRP archive: TROPCONF.SFM,
 * TROP.SFM and TROP_xx.SFM per language -- XML with each trophy's grade,
 * hidden flag, name and description -- plus ICON0.PNG and TROPnnn.PNG).
 *
 * Registration installs the set the way the console does, into the player's
 * folder on the PS3 hard drive, dev_hdd0/home/00000001/trophy/<NPCOMMID>_00/
 * (TROPCONF.SFM, TROP.SFM in the chosen language, the icons), and the
 * unlocks are kept there in trophies.txt ("<id> <unix seconds> <name>" per
 * line). dev_hdd0 is the launcher's data\dev_hdd0, so they survive new
 * builds, and the launcher's bring-over copies them with the saves.
 *
 * Until RACiT 0.9.7 this file kept them in gamedata/trophies/<commId>.json
 * beside the running exe: a new build started with none. Those files are
 * merged in at registration (the build's own, the launcher's other builds,
 * and every .json in <trophy folder>/import, where the launcher puts an
 * older install's), never removed.
 *
 * The structs the game reads are big-endian guest memory; every field is
 * written with vm_write*. Error codes are the system's: a title tells
 * "already unlocked" from a failure by them.
 */

#include "sceNpTrophy.h"
#include "../system/cellSysutil.h"
#include "../video/rsx_host_ui.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include "ps3emu/endian.h"
#include "../guest_struct.h"   /* GUEST_EA */
#include "../codec/stb_image.h"   /* declarations only: cellPngDec.c holds the implementation */

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

extern int cellfs_translate_path(const char* ps3_path, char* host_buf, size_t buf_size);

/* ---------------------------------------------------------------------------
 * The set and the player's unlocks
 * -----------------------------------------------------------------------*/
#define TR_MAX SCE_NP_TROPHY_MAX_NUM_TROPHIES

typedef struct {
    int  grade;                 /* SCE_NP_TROPHY_GRADE_* */
    int  hidden;
    int  pid;                   /* the platinum it counts towards, -1 none */
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE];     /* UTF-8, as on the disc */
    char detail[SCE_NP_TROPHY_DESC_MAX_SIZE];
} TrophyDef;

static struct {
    int  loaded;                /* 1 set read, -1 tried and failed */
    char comm[16];              /* "NPWR00941_00" */
    char title[SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE];
    char title_detail[SCE_NP_TROPHY_GAME_DESC_MAX_SIZE];
    int  n;
    TrophyDef t[TR_MAX];
    char dir[1024];             /* the player's trophy folder */
    int  installed;
    u8   unlocked[TR_MAX];
    long long when[TR_MAX];     /* unix seconds */
} s_set;

static volatile int s_lock_v;
static void tlock(void)   { while (__atomic_exchange_n(&s_lock_v, 1, __ATOMIC_ACQUIRE)) { } }
static void tunlock(void) { __atomic_store_n(&s_lock_v, 0, __ATOMIC_RELEASE); }

static void (*s_hook)(int);

static int s_trophy_initialized = 0;

typedef struct {
    int in_use, registered;
    char comm[16];
} TrophyContext;
typedef struct { int in_use; } TrophyHandle;

/* ID zero is the firmware invalid sentinel; retain all four usable slots. */
static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

static const char* grade_name(int g)
{
    return g == SCE_NP_TROPHY_GRADE_PLATINUM ? "platinum" : g == SCE_NP_TROPHY_GRADE_GOLD ? "gold"
         : g == SCE_NP_TROPHY_GRADE_SILVER ? "silver" : g == SCE_NP_TROPHY_GRADE_BRONZE ? "bronze" : "?";
}

/* ---------------------------------------------------------------------------
 * Files
 * -----------------------------------------------------------------------*/
static unsigned char* read_all(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (64L << 20)) { fclose(f); return NULL; }
    unsigned char* b = (unsigned char*)malloc((size_t)sz + 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[sz] = 0; *n = (size_t)sz; }
    return b;
}

static void make_dirs(const char* path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++)
        if (*p == '/' || *p == '\\') {
            if (p[-1] == ':') continue;   /* "C:\" */
            const char c = *p; *p = 0; mkdir_p(tmp); *p = c;
        }
    mkdir_p(tmp);
}

static int write_all(const char* path, const void* data, size_t n)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    const int ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) { remove(tmp); return -1; }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { remove(tmp); return -1; }
#else
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
#endif
    return 0;
}

static long long file_size(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long long n = ftell(f);
    fclose(f);
    return n;
}

/* ---------------------------------------------------------------------------
 * TROPHY.TRP: header 0x40 (magic DC A2 4D 00, version, file size, entry
 * count, entry size), then entries of 0x40: name[32], offset, size (u64 BE).
 * -----------------------------------------------------------------------*/
typedef struct { const unsigned char* d; size_t n; } trp_t;

static const unsigned char* trp_find(const trp_t* t, const char* name, size_t* len)
{
    if (t->n < 0x40 || t->d[0] != 0xDC || t->d[1] != 0xA2 || t->d[2] != 0x4D || t->d[3] != 0x00) return NULL;
    const u32 cnt = ((u32)t->d[16] << 24) | ((u32)t->d[17] << 16) | ((u32)t->d[18] << 8) | t->d[19];
    const u32 esz = ((u32)t->d[20] << 24) | ((u32)t->d[21] << 16) | ((u32)t->d[22] << 8) | t->d[23];
    if (esz < 48 || cnt > 4096) return NULL;
    for (u32 i = 0; i < cnt; i++) {
        const size_t e = 0x40 + (size_t)i * esz;
        if (e + 48 > t->n) break;
        char nm[33]; memcpy(nm, t->d + e, 32); nm[32] = 0;
        if (strcmp(nm, name) != 0) continue;
        u64 off = 0, sz = 0;
        for (int k = 0; k < 8; k++) { off = (off << 8) | t->d[e + 32 + k]; sz = (sz << 8) | t->d[e + 40 + k]; }
        if (off + sz > t->n) return NULL;
        *len = (size_t)sz;
        return t->d + off;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * SFM (XML) reading
 * -----------------------------------------------------------------------*/
static void xml_text(const char* s, const char* e, char* out, size_t n)
{
    size_t k = 0;
    while (s < e && k + 1 < n) {
        if (*s == '&') {
            const char* semi = (const char*)memchr(s, ';', (size_t)(e - s));
            if (semi && semi - s <= 10) {
                unsigned cp = 0; const char* ent = s + 1; const size_t el = (size_t)(semi - ent);
                if (el == 3 && !strncmp(ent, "amp", 3)) cp = '&';
                else if (el == 2 && !strncmp(ent, "lt", 2)) cp = '<';
                else if (el == 2 && !strncmp(ent, "gt", 2)) cp = '>';
                else if (el == 4 && !strncmp(ent, "quot", 4)) cp = '"';
                else if (el == 4 && !strncmp(ent, "apos", 4)) cp = '\'';
                else if (el > 1 && ent[0] == '#') cp = (unsigned)strtoul(ent + 1 + (ent[1] == 'x'), NULL, ent[1] == 'x' ? 16 : 10);
                if (cp) {   /* as UTF-8 */
                    char u[4]; int ul = 0;
                    if (cp < 0x80) u[ul++] = (char)cp;
                    else if (cp < 0x800) { u[ul++] = (char)(0xC0 | (cp >> 6)); u[ul++] = (char)(0x80 | (cp & 63)); }
                    else if (cp < 0x10000) { u[ul++] = (char)(0xE0 | (cp >> 12)); u[ul++] = (char)(0x80 | ((cp >> 6) & 63)); u[ul++] = (char)(0x80 | (cp & 63)); }
                    for (int q = 0; q < ul && k + 1 < n; q++) out[k++] = u[q];
                    s = semi + 1;
                    continue;
                }
            }
        }
        out[k++] = (*s == '\r' || *s == '\n' || *s == '\t') ? ' ' : *s;
        s++;
    }
    out[k] = 0;
    while (k && out[k - 1] == ' ') out[--k] = 0;
}

/* The value of attribute `a` inside the tag [s, e). */
static int xml_attr(const char* s, const char* e, const char* a, char* out, size_t n)
{
    const size_t al = strlen(a);
    for (const char* p = s; p + al + 2 < e; p++) {
        if (strncmp(p, a, al) != 0 || p[al] != '=' || (p > s && isalnum((unsigned char)p[-1]))) continue;
        const char q = p[al + 1];
        if (q != '"' && q != '\'') continue;
        const char* v = p + al + 2;
        const char* ve = (const char*)memchr(v, q, (size_t)(e - v));
        if (!ve) return 0;
        xml_text(v, ve, out, n);
        return 1;
    }
    return 0;
}

/* The text of <tag>...</tag> inside [s, e). */
static int xml_elem(const char* s, const char* e, const char* tag, char* out, size_t n)
{
    char open[40], close[40];
    snprintf(open, sizeof open, "<%s>", tag); snprintf(close, sizeof close, "</%s>", tag);
    const char* a = strstr(s, open);
    if (!a || a >= e) return 0;
    a += strlen(open);
    const char* b = strstr(a, close);
    if (!b || b > e) return 0;
    xml_text(a, b, out, n);
    return 1;
}

/* Every <trophy ...> of an SFM: grade, hidden, pid, and (TROP*.SFM) name and detail. */
static int sfm_parse(const char* x, int names)
{
    int count = 0;
    if (names) {
        const char* end = x + strlen(x);
        const char* tl = strstr(x, "<trophy ");
        if (!tl) tl = end;
        xml_elem(x, tl, "title-name", s_set.title, sizeof s_set.title);
        xml_elem(x, tl, "title-detail", s_set.title_detail, sizeof s_set.title_detail);
    }
    for (const char* p = strstr(x, "<trophy "); p; p = strstr(p + 1, "<trophy ")) {
        const char* te = strchr(p, '>');
        if (!te) break;
        char v[32];
        if (!xml_attr(p, te, "id", v, sizeof v)) continue;
        const int id = atoi(v);
        if (id < 0 || id >= TR_MAX) continue;
        TrophyDef* d = &s_set.t[id];
        if (xml_attr(p, te, "ttype", v, sizeof v))
            d->grade = v[0] == 'P' ? SCE_NP_TROPHY_GRADE_PLATINUM : v[0] == 'G' ? SCE_NP_TROPHY_GRADE_GOLD
                     : v[0] == 'S' ? SCE_NP_TROPHY_GRADE_SILVER : v[0] == 'B' ? SCE_NP_TROPHY_GRADE_BRONZE : 0;
        if (xml_attr(p, te, "hidden", v, sizeof v)) d->hidden = !strcmp(v, "yes");
        d->pid = xml_attr(p, te, "pid", v, sizeof v) ? atoi(v) : -1;
        if (names) {
            const char* next = strstr(te, "<trophy ");
            const char* close = strstr(te, "</trophy>");
            const char* lim = close && (!next || close < next) ? close : next ? next : te + strlen(te);
            char nm[SCE_NP_TROPHY_NAME_MAX_SIZE], dt[SCE_NP_TROPHY_DESC_MAX_SIZE];
            if (xml_elem(te, lim, "name", nm, sizeof nm) && nm[0]) snprintf(d->name, sizeof d->name, "%s", nm);
            if (xml_elem(te, lim, "detail", dt, sizeof dt) && dt[0]) snprintf(d->detail, sizeof d->detail, "%s", dt);
        }
        if (id + 1 > s_set.n) s_set.n = id + 1;
        count++;
    }
    return count;
}

/* The overlay's font is ASCII: accents folded, typographic marks to plain ones. */
static void fold_ascii(const char* in, char* out, size_t n)
{
    static const char* const latin1[64] = {
        "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
        "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y" };
    const unsigned char* s = (const unsigned char*)in;
    size_t k = 0;
    while (*s && k + 4 < n) {
        unsigned cp = *s; int len = 1;
        if (cp >= 0xF0 && s[1] && s[2] && s[3]) { cp = ((cp & 7) << 18) | ((s[1] & 63) << 12) | ((s[2] & 63) << 6) | (s[3] & 63); len = 4; }
        else if (cp >= 0xE0 && s[1] && s[2]) { cp = ((cp & 15) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); len = 3; }
        else if (cp >= 0xC0 && s[1]) { cp = ((cp & 31) << 6) | (s[1] & 63); len = 2; }
        s += len;
        char one[2] = { 0, 0 };
        const char* r = "?";
        if (cp >= 32 && cp < 127) { one[0] = (char)cp; r = one; }
        else if (cp < 32 || cp == 0xA0) r = " ";
        else if (cp >= 0xC0 && cp <= 0xFF) r = latin1[cp - 0xC0];
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032 || cp == 0xB4) r = "'";
        else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0xAB || cp == 0xBB) r = "\"";
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) r = "-";
        else if (cp == 0x2026) r = "...";
        else if (cp == 0x152) r = "OE";
        else if (cp == 0x153) r = "oe";
        else if (cp == 0xA1 || cp == 0xBF || cp == 0xAD) r = "";
        else if (cp == 0x2122) r = "(TM)";
        else if (cp == 0xAE) r = "(R)";
        else if (cp == 0xA9) r = "(C)";
        else if (cp == 0xBA || cp == 0xB0) r = "o";
        else if (cp == 0xAA) r = "a";
        for (; *r && k + 1 < n; r++) out[k++] = *r;
    }
    out[k] = 0;
}

/* TROP_xx.SFM by RACACIT_LANGUAGE (the game's language setting): the system
 * language numbers, CELL_SYSUTIL_LANG_*. English when unset or not on the disc. */
static int chosen_language(void)
{
    const char* e = getenv("RACACIT_LANGUAGE");
    if (!e || !*e) return 1;
    static const struct { const char* code; const char* name; int id; } k[] = {
        { "en", "english", 1 }, { "fr", "french", 2 }, { "es", "spanish", 3 }, { "de", "german", 4 },
        { "it", "italian", 5 }, { "nl", "dutch", 6 } };
    char l[16] = { 0 };
    for (int i = 0; e[i] && i < 15; i++) l[i] = (char)tolower((unsigned char)e[i]);
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(l, k[i].code) || !strcmp(l, k[i].name) || atoi(l) == k[i].id) return k[i].id;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Load the set (once per communication id)
 * -----------------------------------------------------------------------*/
static void ui_push(void);
static void install_and_icons(const trp_t* trp, const char* lang_sfm, size_t lang_len);

static int set_load(const char* comm)
{
    if (s_set.loaded && !strcmp(s_set.comm, comm)) return s_set.loaded > 0 ? 0 : -1;
    memset(&s_set, 0, sizeof s_set);
    snprintf(s_set.comm, sizeof s_set.comm, "%s", comm);
    s_set.loaded = -1;

    char ps3[256], trp_path[1200] = "";
    snprintf(ps3, sizeof ps3, "/dev_bdvd/PS3_GAME/TROPDIR/%s/TROPHY.TRP", comm);
    if (cellfs_translate_path(ps3, trp_path, sizeof trp_path) != 0 || file_size(trp_path) <= 0) {
        const char* root = getenv("PS3_VFS_ROOT");
        snprintf(trp_path, sizeof trp_path, "%s/PS3_GAME/TROPDIR/%s/TROPHY.TRP", root && *root ? root : ".", comm);
    }
    size_t n = 0;
    unsigned char* d = read_all(trp_path, &n);
    if (!d) { printf("[trophy] no trophy set: %s not found (trophies are still kept, without names)\n", trp_path); }
    trp_t trp = { d, n };
    size_t cl = 0, ll = 0;
    const unsigned char* conf = d ? trp_find(&trp, "TROPCONF.SFM", &cl) : NULL;
    char lname[32];
    snprintf(lname, sizeof lname, "TROP_%02d.SFM", chosen_language());
    const unsigned char* lang = d ? trp_find(&trp, lname, &ll) : NULL;
    size_t dl = 0;
    const unsigned char* deflt = d ? trp_find(&trp, "TROP.SFM", &dl) : NULL;
    if (!lang) { lang = deflt; ll = dl; snprintf(lname, sizeof lname, "TROP.SFM"); }

    for (int i = 0; i < TR_MAX; i++) s_set.t[i].pid = -1;
    if (conf) {
        char* x = (char*)malloc(cl + 1); memcpy(x, conf, cl); x[cl] = 0;
        sfm_parse(x, 0); free(x);
    }
    if (deflt) {   /* English names first: a language file missing one keeps it */
        char* x = (char*)malloc(dl + 1); memcpy(x, deflt, dl); x[dl] = 0;
        sfm_parse(x, 1); free(x);
    }
    if (lang && lang != deflt) {
        char* x = (char*)malloc(ll + 1); memcpy(x, lang, ll); x[ll] = 0;
        sfm_parse(x, 1); free(x);
    }
    if (s_set.n > 0) {
        s_set.loaded = 1;
        int g[5] = { 0 };
        for (int i = 0; i < s_set.n; i++) g[s_set.t[i].grade >= 1 && s_set.t[i].grade <= 4 ? s_set.t[i].grade : 0]++;
        printf("[trophy] %s: \"%s\", %d trophies (%d platinum, %d gold, %d silver, %d bronze), names from %s\n",
               comm, s_set.title, s_set.n, g[1], g[2], g[3], g[4], lname);
    }

    /* The player's folder on the PS3 hard drive. */
    char hp[256];
    snprintf(hp, sizeof hp, "/dev_hdd0/home/00000001/trophy/%s", comm);
    if (cellfs_translate_path(hp, s_set.dir, sizeof s_set.dir) != 0) {
        const char* h = getenv("PS3_HDD0_ROOT");
        snprintf(s_set.dir, sizeof s_set.dir, "%s%chome%c00000001%ctrophy%c%s", h && *h ? h : "dev_hdd0",
                 PATH_SEP, PATH_SEP, PATH_SEP, PATH_SEP, comm);
    }
    make_dirs(s_set.dir);
    if (d) install_and_icons(&trp, lang ? (const char*)lang : NULL, ll);
    free(d);
    return s_set.loaded > 0 ? 0 : -1;
}

/* The console installs the set into the player's folder on registration; so
 * do we (once: the launcher reads it from there), and the icons go to the
 * overlay. */
static void install_and_icons(const trp_t* trp, const char* lang_sfm, size_t lang_len)
{
    char p[1200];
    size_t cl = 0;
    const unsigned char* conf = trp_find(trp, "TROPCONF.SFM", &cl);
    snprintf(p, sizeof p, "%s%cTROPCONF.SFM", s_set.dir, PATH_SEP);
    const int fresh = conf && file_size(p) != (long long)cl;
    if (fresh) write_all(p, conf, cl);
    snprintf(p, sizeof p, "%s%cTROP.SFM", s_set.dir, PATH_SEP);
    if (lang_sfm && file_size(p) != (long long)lang_len) write_all(p, lang_sfm, lang_len);
    for (int i = -1; i < s_set.n; i++) {
        char nm[32];
        if (i < 0) snprintf(nm, sizeof nm, "ICON0.PNG"); else snprintf(nm, sizeof nm, "TROP%03d.PNG", i);
        size_t il = 0;
        const unsigned char* png = trp_find(trp, nm, &il);
        if (!png) continue;
        snprintf(p, sizeof p, "%s%c%s", s_set.dir, PATH_SEP, nm);
        if (file_size(p) != (long long)il) write_all(p, png, il);
        if (i < 0) continue;
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load_from_memory(png, (int)il, &w, &h, &c, 4);
        if (px) { rsx_host_ui_trophy_icon(i, px, w, h); stbi_image_free(px); }
    }
    s_set.installed = 1;
    if (fresh) printf("[trophy] installed the trophy set into %s\n", s_set.dir);
}

/* ---------------------------------------------------------------------------
 * trophies.txt and the older JSON files
 * -----------------------------------------------------------------------*/
static void state_path(char* out, size_t n) { snprintf(out, n, "%s%ctrophies.txt", s_set.dir, PATH_SEP); }

static int state_load(void)
{
    char p[1200]; state_path(p, sizeof p);
    size_t n = 0;
    char* d = (char*)read_all(p, &n);
    if (!d) return 0;
    int got = 0;
    for (char* line = d; line && *line; ) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        int id = -1; long long t = 0;
        if (isdigit((unsigned char)line[0]) && sscanf(line, "%d %lld", &id, &t) >= 1 && id >= 0 && id < TR_MAX) {
            if (!s_set.unlocked[id]) got++;
            s_set.unlocked[id] = 1;
            s_set.when[id] = t > 0 ? t : 1;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(d);
    return got;
}

static void state_save(void)
{
    char* b = (char*)malloc(64 * 1024);
    if (!b) return;
    size_t k = (size_t)snprintf(b, 64 * 1024,
        "# RACiT trophies: %s (%s)\n# <trophy id> <unlocked at, unix seconds UTC> <name>\n", s_set.title, s_set.comm);
    for (int i = 0; i < TR_MAX && k < 60 * 1024; i++)
        if (s_set.unlocked[i]) {
            char nm[128]; fold_ascii(s_set.t[i].name, nm, sizeof nm);
            k += (size_t)snprintf(b + k, 64 * 1024 - k, "%d %lld %s\n", i, s_set.when[i], nm);
        }
    char p[1200]; state_path(p, sizeof p);
    if (write_all(p, b, k) != 0) printf("[trophy] WARNING: could not write %s\n", p);
    free(b);
}

/* {"trophies": [ {"id": N, "timestamp": T}, ... ]} (this file's own format until 0.9.7). */
static int legacy_merge(const char* path)
{
    size_t n = 0;
    char* d = (char*)read_all(path, &n);
    if (!d) return 0;
    int got = 0;
    for (const char* p = strstr(d, "\"id\""); p; p = strstr(p + 4, "\"id\"")) {
        const char* c = strchr(p + 4, ':');
        if (!c) break;
        const int id = atoi(c + 1);
        const char* ts = strstr(c, "\"timestamp\"");
        const char* nx = strstr(c, "\"id\"");
        long long t = 0;
        if (ts && (!nx || ts < nx)) { const char* tc = strchr(ts + 11, ':'); if (tc) t = atoll(tc + 1); }
        if (id < 0 || id >= TR_MAX || (s_set.n && id >= s_set.n)) continue;
        if (!s_set.unlocked[id] || (t > 0 && t < s_set.when[id])) {
            if (!s_set.unlocked[id]) got++;
            s_set.unlocked[id] = 1;
            s_set.when[id] = t > 0 ? t : 1;
        }
    }
    free(d);
    if (got) printf("[trophy] brought over %d unlock(s) from %s\n", got, path);
    return got;
}

static int legacy_scan_dir(const char* dir, const char* only)   /* *.json in dir (or just `only`) */
{
    int got = 0;
    char p[1400];
    if (only) { snprintf(p, sizeof p, "%s%c%s", dir, PATH_SEP, only); return legacy_merge(p); }
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    snprintf(p, sizeof p, "%s\\*.json", dir);
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        got += legacy_merge(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dd = opendir(dir);
    if (!dd) return 0;
    struct dirent* de;
    while ((de = readdir(dd))) {
        const size_t l = strlen(de->d_name);
        if (l < 6 || strcmp(de->d_name + l - 5, ".json")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        got += legacy_merge(p);
    }
    closedir(dd);
#endif
    return got;
}

static int legacy_import(const char* comm9)
{
    char file[64];
    snprintf(file, sizeof file, "%s.json", comm9);
    int got = legacy_scan_dir("gamedata/trophies", file);   /* the old default, beside the exe */
    char p[1200];
    snprintf(p, sizeof p, "%s%cimport", s_set.dir, PATH_SEP);   /* the launcher's bring-over */
    got += legacy_scan_dir(p, NULL);
#ifdef _WIN32
    /* The launcher runs builds\vNN\RaCACiT.exe: its other builds kept their own. */
    char exe[MAX_PATH * 2];
    const DWORD el = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    char* slash = el ? strrchr(exe, '\\') : NULL;
    if (slash) {
        *slash = 0;                                  /* builds\vNN */
        char* up = strrchr(exe, '\\');
        char parent[MAX_PATH * 2] = "";
        if (up) { snprintf(parent, sizeof parent, "%.*s", (int)(up - exe), exe); }
        const char* pn = strrchr(parent, '\\');
        if (parent[0] && pn && !_stricmp(pn + 1, "builds")) {   /* every build of this install */
            WIN32_FIND_DATAA fd;
            snprintf(p, sizeof p, "%s\\*", parent);
            HANDLE h = FindFirstFileA(p, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
                    char d2[1200];
                    snprintf(d2, sizeof d2, "%s\\%s\\gamedata\\trophies", parent, fd.cFileName);
                    got += legacy_scan_dir(d2, file);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        } else {
            char d2[1200];
            snprintf(d2, sizeof d2, "%s\\gamedata\\trophies", exe);
            got += legacy_scan_dir(d2, file);
        }
    }
#endif
    return got;
}

/* ---------------------------------------------------------------------------
 * The platinum
 * -----------------------------------------------------------------------*/
static int platinum_id(void)
{
    for (int i = 0; i < s_set.n; i++) if (s_set.t[i].grade == SCE_NP_TROPHY_GRADE_PLATINUM) return i;
    return -1;
}

/* All the trophies that count towards it unlocked, it not: its id, else -1. */
static int platinum_due(void)
{
    const int p = platinum_id();
    if (p < 0 || s_set.unlocked[p]) return -1;
    /* Those whose pid names it (every other one, in A Crack in Time); a set
     * with no pids: every other one. */
    int linked = 0;
    for (int i = 0; i < s_set.n; i++) linked += i != p && s_set.t[i].pid == p;
    int any = 0;
    for (int i = 0; i < s_set.n; i++) {
        if (i == p || s_set.t[i].grade == 0) continue;
        if (linked && s_set.t[i].pid != p) continue;
        any = 1;
        if (!s_set.unlocked[i]) return -1;
    }
    return any ? p : -1;
}

static int unlocked_count(void)
{
    int c = 0;
    for (int i = 0; i < (s_set.n ? s_set.n : TR_MAX); i++) c += s_set.unlocked[i];
    return c;
}

/* ---------------------------------------------------------------------------
 * The overlay
 * -----------------------------------------------------------------------*/
static void ui_push(void)
{
    static rsx_trophy t[RSX_UI_TROPHY_MAX];
    const int n = s_set.n < RSX_UI_TROPHY_MAX ? s_set.n : RSX_UI_TROPHY_MAX;
    for (int i = 0; i < n; i++) {
        memset(&t[i], 0, sizeof t[i]);
        t[i].id = i; t[i].grade = s_set.t[i].grade; t[i].hidden = s_set.t[i].hidden;
        t[i].when = s_set.unlocked[i] ? s_set.when[i] : 0;
        if (s_set.t[i].name[0]) fold_ascii(s_set.t[i].name, t[i].name, sizeof t[i].name);
        else snprintf(t[i].name, sizeof t[i].name, "Trophy %d", i);
        fold_ascii(s_set.t[i].detail, t[i].detail, sizeof t[i].detail);
    }
    char title[128];
    fold_ascii(s_set.title, title, sizeof title);
    rsx_host_ui_trophy_set(title, t, n);
}

/* ---------------------------------------------------------------------------
 * Unlocking (the game's and the host's)
 * -----------------------------------------------------------------------*/
/* Under the lock. Returns 0 unlocked now, 1 already, -1 no such trophy;
 * *plat = the platinum that came with it, or -1. */
static int unlock_locked(int id, int* plat)
{
    *plat = -1;
    if (id < 0 || id >= TR_MAX || (s_set.n && id >= s_set.n)) return -1;
    if (s_set.unlocked[id]) return 1;
    const long long now = (long long)time(NULL);
    s_set.unlocked[id] = 1; s_set.when[id] = now;
    const int p = platinum_due();
    if (p >= 0) { s_set.unlocked[p] = 1; s_set.when[p] = now; *plat = p; }
    state_save();
    return 0;
}

static void announce(int id, const char* why)
{
    char nm[128];
    fold_ascii(s_set.t[id].name[0] ? s_set.t[id].name : "?", nm, sizeof nm);
    printf("[trophy] unlocked %d \"%s\" (%s)%s%s -- %d of %d\n", id, nm, grade_name(s_set.t[id].grade),
           why ? ": " : "", why ? why : "", unlocked_count(), s_set.n);
    rsx_host_ui_trophy_unlocked(id, s_set.when[id], 1);
}

int sceNpTrophyHostUnlock(int trophyId, const char* why)
{
    if (s_set.loaded <= 0 || !s_set.dir[0]) return -1;
    int plat;
    tlock();
    const int r = unlock_locked(trophyId, &plat);
    tunlock();
    if (r != 0) return r;
    announce(trophyId, why);
    if (plat >= 0) announce(plat, "every other trophy");
    if (s_hook) s_hook(trophyId);
    return 0;
}

void sceNpTrophySetUnlockHook(void (*hook)(int trophyId)) { s_hook = hook; }

/* ---------------------------------------------------------------------------
 * Guest memory
 * -----------------------------------------------------------------------*/
static void put_str(u32 ea, const char* s, size_t cap)
{
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) vm_write8(ea + (u32)i, (u8)s[i]);
    for (; i < cap; i++) vm_write8(ea + (u32)i, 0);
}
static void put_zero(u32 ea, size_t n) { for (size_t i = 0; i < n; i++) vm_write8(ea + (u32)i, 0); }

/* CellRtcTick: microseconds since 0001-01-01 00:00 UTC. */
static u64 rtc_tick(long long unix_s) { return (u64)unix_s * 1000000ull + 62135596800000000ull; }

static int ctx_ok(SceNpTrophyContext c) { return c > 0 && c <= SCE_NP_TROPHY_MAX_CONTEXTS && s_contexts[c].in_use; }
static int handle_ok(SceNpTrophyHandle h) { return h > 0 && h <= SCE_NP_TROPHY_MAX_HANDLES && s_handles[h].in_use; }

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

void sceNpTrophySetStoragePath(const char* path)
{
    (void)path;   /* the PS3 hard drive's trophy folder; see the top of this file */
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    (void)poolPtr; (void)poolSize; (void)containerId; (void)options;

    printf("[sceNpTrophy] Init()\n");

    if (s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 1;
    return CELL_OK;
}

s32 sceNpTrophyTerm(void)
{
    printf("[sceNpTrophy] Term()\n");

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* Every unlock is saved when it happens; the set stays loaded for the next Init. */
    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 0;
    return CELL_OK;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    (void)commSign; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!context || !commId)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    const u32 cea = GUEST_EA(commId);
    char id9[10];
    for (int i = 0; i < 9; i++) id9[i] = (char)vm_read8(cea + (u32)i);
    id9[9] = 0;
    for (int i = 0; i < 9; i++) if (!isalnum((unsigned char)id9[i])) { id9[i] = 0; break; }
    if (!id9[0]) return SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;
    const u8 num = vm_read8(cea + 10);

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(TrophyContext));
            s_contexts[i].in_use = 1;
            snprintf(s_contexts[i].comm, sizeof s_contexts[i].comm, "%s_%02u", id9, (unsigned)num);
            tlock();
            set_load(s_contexts[i].comm);
            tunlock();
            vm_write32(GUEST_EA(context), (u32)i);
            printf("[sceNpTrophy] CreateContext(commId=\"%s\") -> ctx=%d\n", s_contexts[i].comm, i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!ctx_ok(context))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    s_contexts[context].in_use = 0;
    printf("[sceNpTrophy] DestroyContext(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!handle)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].in_use = 1;
            vm_write32(GUEST_EA(handle), (u32)i);
            printf("[sceNpTrophy] CreateHandle() -> handle=%d\n", i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!handle_ok(handle))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;

    s_handles[handle].in_use = 0;
    printf("[sceNpTrophy] DestroyHandle(handle=%d)\n", handle);
    return CELL_OK;
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options)
{
    (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!ctx_ok(context))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    if (!handle_ok(handle))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;

    if (!statusCb) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!g_ps3_guest_caller) return SCE_NP_TROPHY_ERROR_ABORT;

    /* The set in the player's folder, the unlocks so far, the older files. */
    int plat = -1, got = 0, merged = 0;
    char comm9[16];
    snprintf(comm9, sizeof comm9, "%s", s_contexts[context].comm);
    { char* us = strrchr(comm9, '_'); if (us) *us = 0; }
    tlock();
    set_load(s_contexts[context].comm);
    if (s_set.dir[0]) {
        got = state_load();
        merged = legacy_import(comm9);
        plat = platinum_due();
        if (plat >= 0) { s_set.unlocked[plat] = 1; s_set.when[plat] = (long long)time(NULL); }
        if (merged || plat >= 0) state_save();
    }
    tunlock();
    s_contexts[context].registered = 1;
    if (s_set.dir[0]) {
        printf("[trophy] %d of %d unlocked (%d from %s%ctrophies.txt%s)\n", unlocked_count(), s_set.n, got,
               s_set.dir, PATH_SEP, merged ? ", the rest from older builds' files" : "");
        ui_push();
        if (plat >= 0) announce(plat, "every other trophy");
        /* The game's rules (game/stubs.cpp: a higher bolt trophy brings the
         * lower ones) for what was unlocked before this start too: the
         * owner's 0.9.7 had only the 1,000,000-bolt one, kept by an older
         * build, and the game, finding it unlocked, never asks for it again,
         * so the rule would never run for it. */
        if (s_hook) {
            static unsigned char had[TR_MAX];
            const int n = s_set.n ? s_set.n : TR_MAX;
            tlock(); for (int i = 0; i < n; i++) had[i] = (unsigned char)s_set.unlocked[i]; tunlock();
            for (int i = 0; i < n; i++) if (had[i]) s_hook(i);
        }
    }

    printf("[sceNpTrophy] RegisterContext(ctx=%d, handle=%d, statusCb=0x%08X, arg=0x%08X)\n",
           context, handle, statusCb, arg);

    /* Use the runner's OPD bridge, not the optional code/TOC scaffold entry
     * point. Registration status is synchronous; its terminal notification is
     * delivered through sysutil polling, after the HLE storage load completes.
     * Intermediate firmware installation progress is not simulated here. */
    g_ps3_guest_caller(statusCb, (u32)context, SCE_NP_TROPHY_STATUS_INSTALLED,
                       0, 0, arg, 0, 0, 0);
    if (!s_trophy_initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!s_contexts[context].in_use) return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    if (!s_handles[handle].in_use) return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
    const u64 completion[8] = {(u32)context, SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE,
                               0, 0, arg, 0, 0, 0};
    if (cellSysutilQueueGuestCallbackArgs(statusCb, completion) != CELL_OK) {
        s_contexts[context].registered = 0;
        return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
    }

    return CELL_OK;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    (void)handle; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!ctx_ok(context))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    if (!reqSpace)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    /* Big-endian: LBP feeds this straight into its free-space sums (a host-LE
     * 1 MB read back as 2^44 failed its GameData check). */
    vm_write64(GUEST_EA(reqSpace), (u64)(1024 * 1024));
    printf("[sceNpTrophy] GetRequiredDiskSpace(ctx=%d) -> 1MB\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!ctx_ok(context))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    int all[5] = { 0 }, got[5] = { 0 };
    tlock();
    for (int i = 0; i < s_set.n; i++) {
        const int g = s_set.t[i].grade >= 1 && s_set.t[i].grade <= 4 ? s_set.t[i].grade : 0;
        all[g]++;
        if (s_set.unlocked[i]) got[g]++;
    }
    tunlock();
    /* No set on the disc: as before, the game may use any id (unnamed). */
    const int n = s_set.n ? s_set.n : TR_MAX;
    const int un = s_set.n ? got[0] + got[1] + got[2] + got[3] + got[4] : unlocked_count();

    if (details) {
        const u32 ea = GUEST_EA(details);
        vm_write32(ea + 0, (u32)n);
        vm_write32(ea + 4, (u32)all[1]);
        vm_write32(ea + 8, (u32)all[2]);
        vm_write32(ea + 12, (u32)all[3]);
        vm_write32(ea + 16, (u32)all[4]);
        put_str(ea + 20, s_set.title, SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE);
        put_str(ea + 148, s_set.title_detail, SCE_NP_TROPHY_GAME_DESC_MAX_SIZE);
        put_zero(ea + 1172, 4);
    }

    if (data) {
        const u32 ea = GUEST_EA(data);
        vm_write32(ea + 0, (u32)un);
        vm_write32(ea + 4, (u32)got[1]);
        vm_write32(ea + 8, (u32)got[2]);
        vm_write32(ea + 12, (u32)got[3]);
        vm_write32(ea + 16, (u32)got[4]);
    }

    printf("[sceNpTrophy] GetGameInfo(ctx=%d) -> %d trophies, %d unlocked\n", context, n, un);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!ctx_ok(context))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || trophyId >= TR_MAX || (s_set.n && trophyId >= s_set.n))
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    const TrophyDef* t = &s_set.t[trophyId];
    if (details) {
        const u32 ea = GUEST_EA(details);
        vm_write32(ea + 0, (u32)trophyId);
        vm_write32(ea + 4, (u32)t->grade);
        put_str(ea + 8, t->name, SCE_NP_TROPHY_NAME_MAX_SIZE);
        put_str(ea + 136, t->detail, SCE_NP_TROPHY_DESC_MAX_SIZE);
        vm_write8(ea + 1160, (u8)(t->hidden ? 1 : 0));
        put_zero(ea + 1161, 3);
    }

    if (data) {
        const u32 ea = GUEST_EA(data);
        const int u = s_set.unlocked[trophyId];
        vm_write64(ea + 0, u ? rtc_tick(s_set.when[trophyId]) : 0);
        vm_write32(ea + 8, (u32)trophyId);
        vm_write8(ea + 12, (u8)(u ? 1 : 0));
        put_zero(ea + 13, 3);
    }

    return CELL_OK;
}

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!ctx_ok(context))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    if (!handle_ok(handle))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (platinumId) vm_write32(GUEST_EA(platinumId), (u32)SCE_NP_TROPHY_INVALID_TROPHY_ID);

    if (trophyId < 0 || trophyId >= TR_MAX || (s_set.n && trophyId >= s_set.n))
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (s_set.t[trophyId].grade == SCE_NP_TROPHY_GRADE_PLATINUM)
        return SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM;

    int plat;
    tlock();
    const int r = s_set.dir[0] ? unlock_locked(trophyId, &plat) : -1;
    tunlock();
    if (r == 1) return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;
    if (r < 0) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    announce(trophyId, NULL);
    if (plat >= 0) {
        if (platinumId) vm_write32(GUEST_EA(platinumId), (u32)plat);
        announce(plat, "every other trophy");
    }
    if (s_hook) s_hook(trophyId);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!ctx_ok(context))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!flags || !count)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    const u32 fea = GUEST_EA(flags);
    const int n = s_set.n ? s_set.n : TR_MAX;
    u32 w[TR_MAX / 32] = { 0 };
    for (int i = 0; i < n; i++)
        if (s_set.unlocked[i]) w[i / 32] |= 1u << (i % 32);
    for (int k = 0; k < TR_MAX / 32; k++) vm_write32(fea + (u32)k * 4u, w[k]);

    vm_write32(GUEST_EA(count), (u32)n);
    return CELL_OK;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!ctx_ok(context))
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!percentage)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    const int total = s_set.n ? s_set.n : TR_MAX, unlocked = unlocked_count();
    const s32 pct = total > 0 ? (s32)((unlocked * 100) / total) : 0;
    vm_write32(GUEST_EA(percentage), (u32)pct);

    printf("[sceNpTrophy] GetGameProgress(ctx=%d) -> %d%%\n", context, pct);
    return CELL_OK;
}
