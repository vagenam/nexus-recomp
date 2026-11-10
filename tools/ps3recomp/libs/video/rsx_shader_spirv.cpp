/*
 * ps3recomp - HLSL -> SPIR-V through glslang, for the Vulkan backend
 *
 * See rsx_shader_spirv.h for the binding contract. The front end is driven
 * exactly as rsx_shader_msl.cpp drives it (C++ API for setEntryPoint, Vulkan
 * rules, auto-mapped bindings and locations, then mapIO, legalized SPIR-V),
 * with three differences, all because the output goes to a Vulkan driver
 * rather than to spirv-cross:
 *
 *   - Vulkan 1.0 client, SPIR-V 1.0: the backend creates a 1.0 instance and
 *     promises to run on 1.0-level drivers. (The MSL path targets 1.3 because
 *     that is spirv-cross's floor, not a driver's.)
 *   - Texture and sampler registers are shifted apart (setShiftBinding),
 *     because Vulkan has one binding namespace per set.
 *   - The vertex stage flips Y (setInvertY): D3D clip space is +y up.
 */

#include "rsx_shader_spirv.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <string>

#if defined(PS3RECOMP_HAVE_SPIRV_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#include <glslang/SPIRV/GlslangToSpv.h>     /* installed package layout */
#else
#include <SPIRV/GlslangToSpv.h>             /* glslang source tree (Android/Windows builds) */
#endif

#include <mutex>
#include <unordered_map>
#include <vector>
#include <stdlib.h>

namespace {

void put_log(char* log, u32 log_size, const char* what, const char* detail)
{
    if (!log || log_size == 0) return;
    snprintf(log, log_size, "%s%s%s", what ? what : "",
             (what && detail && *detail) ? ": " : "", detail ? detail : "");
}

bool glslang_ready()
{
    static bool inited = false;
    if (!inited) {
        if (!glslang::InitializeProcess()) return false;
        inited = true;
    }
    return true;
}

} // namespace

extern "C" int rsx_hlsl_to_spirv_available(void) { return 1; }

static int hlsl_to_spirv_compile(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    if (out_words) *out_words = 0;
    if (!hlsl || !words || !out_words || max_words == 0) {
        put_log(log, log_size, "bad arguments", nullptr);
        return -1;
    }
    if (stage != RSX_SHADER_STAGE_VERTEX && stage != RSX_SHADER_STAGE_FRAGMENT) {
        put_log(log, log_size, "unknown shader stage", nullptr);
        return -1;
    }
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return -1;
    }

    const EShLanguage lang = (stage == RSX_SHADER_STAGE_VERTEX) ? EShLangVertex
                                                                : EShLangFragment;
    const EShMessages msgs =
        (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

    glslang::TShader shader(lang);
    const char* strings[1] = { hlsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setEntryPoint("main");
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    shader.setShiftBinding(glslang::EResTexture, RSX_SPIRV_TEXTURE_BINDING);
    shader.setShiftBinding(glslang::EResSampler, RSX_SPIRV_SAMPLER_BINDING);
    if (lang == EShLangVertex) shader.setInvertY(true);

    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        put_log(log, log_size, "HLSL parse", shader.getInfoLog());
        /* RSX_HLSL_FAIL_DUMP=<dir>: keep the source that did not parse
         * (hlsl_fail_<n>_<vs|fs>.hlsl, first 16), so a translator bug can be
         * read rather than inferred from a line number. */
        static int s_n = 0;
        const char* dir = getenv("RSX_HLSL_FAIL_DUMP");
        if (dir && *dir && s_n < 16) {
            char path[1024];
            snprintf(path, sizeof path, "%s/hlsl_fail_%02d_%s.hlsl", dir, s_n++,
                     stage == RSX_SHADER_STAGE_VERTEX ? "vs" : "fs");
            if (FILE* f = fopen(path, "wb")) { fwrite(hlsl, 1, strlen(hlsl), f); fclose(f); }
        }
        return -1;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "HLSL link", program.getInfoLog());
        return -1;
    }
    if (!program.mapIO()) {
        put_log(log, log_size, "HLSL io map", program.getInfoLog());
        return -1;
    }

    std::vector<unsigned int> spirv;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;     /* run the HLSL legalization passes */
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation", logger.getAllMessages().c_str());
        return -1;
    }
    if (spirv.size() > max_words) {
        char n[64];
        snprintf(n, sizeof n, "%zu words, room for %u", spirv.size(), max_words);
        put_log(log, log_size, "SPIR-V output too large", n);
        return -1;
    }
    memcpy(words, spirv.data(), spirv.size() * sizeof(u32));
    *out_words = (u32)spirv.size();
    return 0;
}

/* SPIR-V cache. A translation costs several milliseconds of the render
 * thread, and a level brings hundreds of programs the moment new scenery
 * comes into view: a stall of seconds. Results are kept in memory per run
 * and, with RSX_SHADER_CACHE=<file>, on disk across runs, keyed by the HLSL
 * text and stage (two 64-bit hashes) plus the options above (the salt: bump
 * it whenever hlsl_to_spirv_compile drives glslang differently).
 *
 * File: "RSXSPV01", then records { u64 k1, u64 k2, u32 words, u32 check,
 * u32 spirv[words] }. A torn tail (the process killed mid-append) is cut off
 * by rewriting the file from the records before it.
 *
 * racacit: only an INDEX (key -> file offset, ~24 bytes a program) stays in
 * memory; a hit reads its words from the file (microseconds from the OS
 * cache, against milliseconds for a translation). The whole cache used to be
 * loaded into RAM and kept -- 98 MB in a dev run/ folder -- and the file only
 * ever grew. Over RSX_SHADER_CACHE_MB (default 192: a full playthrough must
 * fit, or every start would drop and re-translate a level's programs) at load, the file is
 * rewritten with its newest records up to half that; anything dropped is
 * translated again once if a level still needs it. Without a cache file the
 * words are kept in memory as before. */
namespace {

const char kSalt[] = "hlsl->spv glslang vk1.0 spv1.0 invertY shifted legalized v1";
const char kMagic[8] = { 'R', 'S', 'X', 'S', 'P', 'V', '0', '1' };

struct Key { u64 a, b; bool operator==(const Key& o) const { return a == o.a && b == o.b; } };
struct KeyHash { size_t operator()(const Key& k) const { return (size_t)(k.a ^ (k.b >> 7)); } };

struct Rec { long long off; u32 n; };   /* words at file offset off (+24 header) */
std::mutex s_mu;
std::unordered_map<Key, std::vector<u32>, KeyHash> s_cache;   /* no file: words in memory */
std::unordered_map<Key, Rec, KeyHash> s_index;                /* file: where they are */
FILE* s_file;                 /* "a+b": appends at the end, reads anywhere */
bool s_loaded;
unsigned long s_hits, s_misses;

u64 fnv(const void* p, size_t n, u64 h)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

Key make_key(const char* hlsl, int stage)
{
    const size_t n = strlen(hlsl);
    Key k;
    k.a = fnv(kSalt, sizeof kSalt, 1469598103934665603ull);
    k.a = fnv(&stage, sizeof stage, k.a);
    k.a = fnv(hlsl, n, k.a);
    /* Second hash: other basis, and the text walked with its length mixed
     * in, so a collision needs both to agree. */
    k.b = fnv(&n, sizeof n, 0x84222325CBF29CE4ull ^ (u64)stage);
    k.b = fnv(hlsl, n, k.b);
    k.b = fnv(kSalt, sizeof kSalt, k.b);
    return k;
}

void put_record(FILE* f, const Key& k, const u32* w, u32 n)
{
    const u32 check = n ^ 0x5350565Au;
    fwrite(&k.a, 8, 1, f); fwrite(&k.b, 8, 1, f);
    fwrite(&n, 4, 1, f); fwrite(&check, 4, 1, f);
    fwrite(w, 4, n, f);
}

void cache_load()
{
    s_loaded = true;
    const char* path = getenv("RSX_SHADER_CACHE");
    if (!path || !*path) return;
    const char* mb = getenv("RSX_SHADER_CACHE_MB");
    const long long cap = (long long)(mb && atoi(mb) > 0 ? atoi(mb) : 192) << 20;
    /* Pass 1: index the valid records (a torn tail ends the file). */
    bool torn = false, fresh = true;
    long long end = 8;
    std::vector<std::pair<Key, Rec>> recs;
    if (FILE* f = fopen(path, "rb")) {
        char m[8];
        if (fread(m, 1, 8, f) == 8 && !memcmp(m, kMagic, 8)) {
            fresh = false;
            for (;;) {
                Key k; u32 n, check;
                size_t got = fread(&k.a, 8, 1, f) + fread(&k.b, 8, 1, f) +
                             fread(&n, 4, 1, f) + fread(&check, 4, 1, f);
                if (got == 0 && feof(f)) break;
                if (got != 4 || check != (n ^ 0x5350565Au) || n == 0 || n > (64u << 20) / 4u) { torn = true; break; }
                if (fseek(f, (long)n * 4L, SEEK_CUR) != 0) { torn = true; break; }
                recs.push_back({ k, Rec{ end, n } });
                end += 24 + (long long)n * 4;
            }
            if (!torn && ftell(f) != (long)end) torn = true;
        }
        fclose(f);
    }
    const bool over = end > cap;
    if (!fresh && (torn || over)) {
        /* Rewrite: the valid records, or only the newest ones (the file is in
         * append order) up to half the cap, through a temporary file. */
        size_t first = 0;
        if (over) {
            long long keep = 0;
            first = recs.size();
            while (first > 0 && keep + 24 + (long long)recs[first - 1].second.n * 4 <= cap / 2) {
                keep += 24 + (long long)recs[first - 1].second.n * 4; first--;
            }
        }
        std::string tmp = std::string(path) + ".tmp";
        FILE* in = fopen(path, "rb");
        FILE* out = in ? fopen(tmp.c_str(), "wb") : nullptr;
        bool ok = out != nullptr;
        if (ok) {
            fwrite(kMagic, 1, 8, out);
            std::vector<u32> w;
            for (size_t i = first; ok && i < recs.size(); i++) {
                w.resize(recs[i].second.n);
                ok = fseek(in, (long)(recs[i].second.off + 24), SEEK_SET) == 0 &&
                     fread(w.data(), 4, w.size(), in) == w.size();
                if (ok) put_record(out, recs[i].first, w.data(), (u32)w.size());
            }
            ok = fclose(out) == 0 && ok;
        }
        if (in) fclose(in);
        if (ok && (remove(path) == 0 || errno == ENOENT) && rename(tmp.c_str(), path) == 0) {
            fprintf(stderr, "[spirv-cache] %s: rewrote %s, kept %zu of %zu programs%c", path,
                    torn ? "(torn tail)" : "(over the cap)", recs.size() - first, recs.size(), 10);
            recs.erase(recs.begin(), recs.begin() + (long)first);
            long long off = 8;
            for (auto& r : recs) { r.second.off = off; off += 24 + (long long)r.second.n * 4; }
        } else {
            remove(tmp.c_str());
            recs.clear(); fresh = true;   /* could not rewrite: start a new file */
        }
    }
    if (fresh) {
        s_file = fopen(path, "w+b");
        if (s_file) { fwrite(kMagic, 1, 8, s_file); fflush(s_file); }
        recs.clear();
    } else {
        s_file = fopen(path, "a+b");
    }
    if (s_file) for (const auto& r : recs) s_index.emplace(r.first, r.second);
    fprintf(stderr, "[spirv-cache] %s: %zu programs indexed%s%c", path, s_index.size(),
            s_file ? "" : " (cannot open: in-memory only)", 10);
}

/* The words of an indexed program; false when the file read fails. */
bool cache_read(const Rec& r, u32* words)
{
    if (!s_file) return false;
    fflush(s_file);   /* switching from appending to reading */
    if (fseek(s_file, (long)(r.off + 24), SEEK_SET) != 0) return false;
    return fread(words, 4, r.n, s_file) == r.n;
}

} // namespace

extern "C" void rsx_hlsl_to_spirv_cache_stats(unsigned long* hits, unsigned long* misses)
{
    std::lock_guard<std::mutex> g(s_mu);
    if (hits) *hits = s_hits;
    if (misses) *misses = s_misses;
}

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    if (out_words) *out_words = 0;
    if (!hlsl || !words || !out_words || max_words == 0)
        return hlsl_to_spirv_compile(hlsl, stage, words, max_words, out_words, log, log_size);
    const Key k = make_key(hlsl, stage);
    {
        std::lock_guard<std::mutex> g(s_mu);
        if (!s_loaded) cache_load();
        auto ix = s_index.find(k);
        if (ix != s_index.end() && ix->second.n <= max_words && cache_read(ix->second, words)) {
            *out_words = ix->second.n;
            s_hits++;
            return 0;
        }
        auto it = s_cache.find(k);
        if (it != s_cache.end() && it->second.size() <= max_words) {
            memcpy(words, it->second.data(), it->second.size() * 4u);
            *out_words = (u32)it->second.size();
            s_hits++;
            return 0;
        }
    }
    const int r = hlsl_to_spirv_compile(hlsl, stage, words, max_words, out_words, log, log_size);
    if (r == 0 && *out_words) {
        std::lock_guard<std::mutex> g(s_mu);
        s_misses++;
        if (s_file) {
            if (s_index.find(k) == s_index.end()) {
                fflush(s_file);
                fseek(s_file, 0, SEEK_END);           /* "a+" appends anyway; this gives the offset */
                const long long off = (long long)ftell(s_file);
                put_record(s_file, k, words, *out_words); fflush(s_file);
                if (off >= 8) s_index.emplace(k, Rec{ off, *out_words });
            }
        } else {
            s_cache.emplace(k, std::vector<u32>(words, words + *out_words));
        }
    }
    return r;
}

#else /* !PS3RECOMP_HAVE_SPIRV_TRANSLATION */

extern "C" int rsx_hlsl_to_spirv_available(void) { return 0; }
extern "C" void rsx_hlsl_to_spirv_cache_stats(unsigned long* hits, unsigned long* misses)
{
    if (hits) *hits = 0;
    if (misses) *misses = 0;
}

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    (void)hlsl; (void)stage; (void)words; (void)max_words;
    if (out_words) *out_words = 0;
    if (log && log_size)
        snprintf(log, log_size, "HLSL -> SPIR-V translation not built (glslang not found)");
    return -1;
}

#endif
