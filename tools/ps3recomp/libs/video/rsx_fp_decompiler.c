/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * See rsx_fp_decompiler.h and docs/RSX_FRAGMENT_PROGRAM.md.
 *
 * The encoding (4 words / 16 bytes per instruction; CONST sources carry an
 * inline 16-byte constant in the following slot) is taken verbatim from
 * Mesa/nouveau nvfx_shader.h. Bit positions are spelled out as named macros
 * below so this file is self-documenting.
 */

#include <stdlib.h>
#include "rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>

/* ---- DWORD 0 (OPDEST) --------------------------------------------------- */
#define FP_END              (1u << 0)
#define FP_OUT_REG_SHIFT    1
#define FP_OUT_REG_MASK     (63u << 1)
#define FP_OUT_HALF         (1u << 7)
#define FP_COND_WRITE       (1u << 8)
#define FP_OUT_MASK_SHIFT   9          /* X@9 Y@10 Z@11 W@12 */
#define FP_INPUT_SRC_SHIFT  13
#define FP_INPUT_SRC_MASK   (15u << 13)
#define FP_TEX_UNIT_SHIFT   17
#define FP_TEX_UNIT_MASK    (15u << 17)
#define FP_OPCODE_SHIFT     24
#define FP_OPCODE_MASK      (0x3Fu << 24)
#define FP_OUT_NONE         (1u << 30)
#define FP_OUT_SAT          (1u << 31)

/* ---- DWORD 1/2/3 (SRC0/1/2) --------------------------------------------- */
#define FP_REG_TYPE_SHIFT   0
#define FP_REG_TYPE_MASK    (3u << 0)
#define   FP_REG_TYPE_TEMP  0
#define   FP_REG_TYPE_INPUT 1
#define   FP_REG_TYPE_CONST 2
#define FP_SRC_REG_SHIFT    2
#define FP_SRC_REG_MASK     (63u << 2)
#define FP_SRC_HALF         (1u << 8)
#define FP_SRC_SWZ_SHIFT    9          /* X@9 Y@11 Z@13 W@15, 2 bits each */
#define FP_SRC_NEGATE       (1u << 17)
#define FP_SRC0_ABS         (1u << 29) /* in DWORD1 */
#define FP_SRC1_ABS         (1u << 18) /* in DWORD2 */
#define FP_BRANCH           (1u << 31) /* DWORD2 bit31: instruction is a branch */

/* ---- Execution-condition / condition-code fields (SRC0 = DWORD1) ---------
 * The NV40 fragment ISA predicates each instruction on a condition register
 * (CC0/CC1). Layout verified against RPCS3's RSXFragmentProgram.h SRC0 union
 * (exec_if_lt@18, exec_if_eq@19, exec_if_gr@20, cond_swizzle@21..28,
 * cond_mod_reg_index@30, cond_reg_index@31) and OPDEST.set_cond@8. All three
 * exec bits set (0b111) == unconditional (the default); none set == never.
 * Dropped entirely until now: MEASURED present in 4 of this capture's 43
 * fragment programs (scratch/a010_reports/fragment/, cond_detail_out.txt) —
 * genuine SET_CC→exec_if producer/consumer pairs (incl. conditional TEX). */
#define FP_EXEC_IF_LT       (1u << 18)
#define FP_EXEC_IF_EQ       (1u << 19)
#define FP_EXEC_IF_GR       (1u << 20)
#define FP_COND_SWZ_SHIFT   21          /* X@21 Y@23 Z@25 W@27, 2 bits each */
#define FP_COND_MOD_REG     (1u << 30)  /* set_cond WRITE target: cc0/cc1     */
#define FP_COND_REG         (1u << 31)  /* exec_if READ source:   cc0/cc1     */

/* ---- Opcodes ------------------------------------------------------------ */
enum {
    OP_NOP=0x00, OP_MOV=0x01, OP_MUL=0x02, OP_ADD=0x03, OP_MAD=0x04,
    OP_DP3=0x05, OP_DP4=0x06, OP_DST=0x07, OP_MIN=0x08, OP_MAX=0x09,
    OP_SLT=0x0A, OP_SGE=0x0B, OP_SLE=0x0C, OP_SGT=0x0D, OP_SNE=0x0E,
    OP_SEQ=0x0F, OP_FRC=0x10, OP_FLR=0x11, OP_KIL=0x12, OP_PK4B=0x13,
    OP_UP4B=0x14, OP_DDX=0x15, OP_DDY=0x16, OP_TEX=0x17, OP_TXP=0x18,
    OP_TXD=0x19, OP_RCP=0x1A, OP_RSQ=0x1B, OP_EX2=0x1C, OP_LG2=0x1D,
    OP_LIT=0x1E, OP_LRP=0x1F, OP_STR=0x20, OP_SFL=0x21, OP_COS=0x22,
    OP_SIN=0x23, OP_PK2H=0x24, OP_UP2H=0x25, OP_POW=0x26, OP_PK4UB=0x27,
    OP_UP4UB=0x28, OP_PK2US=0x29, OP_UP2US=0x2A, OP_DP2A=0x2E, OP_TXL=0x2F,
    OP_TXB=0x31, OP_RFL=0x36, OP_DP2=0x38, OP_NRM=0x39, OP_DIV=0x3A,
    OP_DIVSQ=0x3B, OP_LIF=0x3C, OP_FENCT=0x3D, OP_FENCB=0x3E
};

const char* rsx_fp_opcode_name(u32 op)
{
    switch (op) {
    case OP_NOP: return "NOP"; case OP_MOV: return "MOV"; case OP_MUL: return "MUL";
    case OP_ADD: return "ADD"; case OP_MAD: return "MAD"; case OP_DP3: return "DP3";
    case OP_DP4: return "DP4"; case OP_DST: return "DST"; case OP_MIN: return "MIN";
    case OP_MAX: return "MAX"; case OP_SLT: return "SLT"; case OP_SGE: return "SGE";
    case OP_SLE: return "SLE"; case OP_SGT: return "SGT"; case OP_SNE: return "SNE";
    case OP_SEQ: return "SEQ"; case OP_FRC: return "FRC"; case OP_FLR: return "FLR";
    case OP_KIL: return "KIL"; case OP_DDX: return "DDX"; case OP_DDY: return "DDY";
    case OP_TEX: return "TEX"; case OP_TXP: return "TXP"; case OP_TXD: return "TXD";
    case OP_RCP: return "RCP"; case OP_RSQ: return "RSQ"; case OP_EX2: return "EX2";
    case OP_LG2: return "LG2"; case OP_LIT: return "LIT"; case OP_LRP: return "LRP";
    case OP_STR: return "STR"; case OP_SFL: return "SFL"; case OP_COS: return "COS";
    case OP_SIN: return "SIN"; case OP_POW: return "POW"; case OP_DP2A: return "DP2A";
    case OP_TXL: return "TXL"; case OP_TXB: return "TXB"; case OP_RFL: return "RFL";
    case OP_DIV: return "DIV"; case OP_DP2: return "DP2"; case OP_NRM: return "NRM";
    case OP_DIVSQ: return "DIVSQ"; case OP_LIF: return "LIF";
    case OP_FENCT: return "FENCT"; case OP_FENCB: return "FENCB";
    default:     return "?";
    }
}

/* ------------------------------------------------------------------------- */

u32 rsx_fp_read_word(const u8* p)
{
    u32 be = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    return (be << 16) | (be >> 16);   /* 16-bit half-word swap */
}

u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        /* A CONST source pulls an inline 16-byte constant slot. */
        if (((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
            ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
            ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) off += 16;
        }
        if (w0 & FP_END) return off;
    }
    return 0;
}

static int fp_instruction_has_constant(u32 w1, u32 w2, u32 w3)
{
    return
        ((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST;
}

unsigned long long g_rsx_fp_nonfinite_constants = 0;

/* An inline fragment constant is never legitimately non-finite.  A NaN one
 * poisons every pixel the program touches, which on an FP16 HDR target shows
 * up as a saturated white fill rather than as an obviously broken draw.
 * Substitute zero and count it so the underlying bad read stays visible. */
u32 rsx_fp_sanitize_constant_word(u32 w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    if (f == f && f <= 3.0e38f && f >= -3.0e38f)
        return w;
    g_rsx_fp_nonfinite_constants++;
    return 0;
}

int rsx_fp_collect_constants(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out)
{
    if (!ucode || !out)
        return -1;
    /* Only the count: the block is 2048 slots (32 KB) and every consumer reads
     * just the first `count`. Clearing it all, per draw, was ~28% of the draw
     * engine's time in A Crack in Time. */
    out->count = 0;

    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                out->count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return -1;
            for (u32 component = 0; component < 4; ++component)
                out->values[out->count][component] =
                    rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + component * 4));
            out->count++;
            off += 16;
        }
        if (w0 & FP_END)
            return (int)out->count;
    }
    return -1;
}

u32 rsx_fp_texture_mask(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0xFFFFu;
    u32 mask = 0, off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (!(w2 & FP_BRANCH)) {
            const u32 opcode = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
            if (opcode == OP_TEX || opcode == OP_TXP || opcode == OP_TXD ||
                opcode == OP_TXB || opcode == OP_TXL)
                mask |= 1u << ((w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT);
        }
        if (fp_instruction_has_constant(w1, w2, w3)) off += 16;
        if (w0 & FP_END) return mask;
    }
    return 0xFFFFu;                 /* no end: say every unit */
}

static u64 fp_hash_bytes(const void* data, u32 size, u64 hash)
{
    const u8* bytes = (const u8*)data;
    u32 i = 0;
    for (; i + 8 <= size; i += 8) {      /* a cache key: word-wise is enough */
        u64 w; memcpy(&w, bytes + i, 8);
        hash ^= w;
        hash *= 1099511628211ull;
    }
    for (; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

u64 rsx_fp_structural_hash(const u8* ucode, u32 max_bytes, u64 seed)
{
    if (!ucode)
        return 0;
    static const u32 structural_tag = 0x31535046u; /* "FPS1" */
    u64 hash = fp_hash_bytes(&structural_tag, sizeof(structural_tag), seed);
    u32 off = 0;
    u32 instruction_count = 0;
    u32 constant_count = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        hash = fp_hash_bytes(ucode + off, 16, hash);
        instruction_count++;
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                constant_count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return 0;
            constant_count++;
            off += 16;
        }
        if (w0 & FP_END) {
            hash = fp_hash_bytes(
                &instruction_count, sizeof(instruction_count), hash);
            hash = fp_hash_bytes(
                &constant_count, sizeof(constant_count), hash);
            return hash;
        }
    }
    return 0;
}

u64 rsx_fp_literal_source_hash(
    const u8* ucode, u32 max_bytes, u64 seed)
{
    const u64 structural =
        rsx_fp_structural_hash(ucode, max_bytes, seed);
    if (!structural)
        return 0;
    rsx_fp_constant_block constants;
    if (rsx_fp_collect_constants(ucode, max_bytes, &constants) < 0)
        return 0;
    u64 hash = structural;
    for (u32 slot = 0; slot < constants.count; ++slot) {
        float value[4];
        for (u32 component = 0; component < 4; ++component)
            memcpy(
                &value[component],
                &constants.values[slot][component],
                sizeof(value[component]));
        char literal[128];
        const int length = snprintf(
            literal, sizeof(literal), "float4(%g,%g,%g,%g)",
            value[0], value[1], value[2], value[3]);
        if (length < 0 || (u32)length >= sizeof(literal))
            return 0;
        hash = fp_hash_bytes(literal, (u32)length, hash);
    }
    return hash;
}

int rsx_fp_constant_ring_plan(
    u32 used, u32 capacity, u32 data_bytes,
    u32* out_offset, u32* out_allocation_bytes)
{
    if (!out_offset || !out_allocation_bytes || data_bytes == 0)
        return -1;
    if (data_bytes > 0xFFFFFF00u)
        return -1;
    const u32 allocation = (data_bytes + 255u) & ~255u;
    if (allocation > capacity)
        return -1;
    if (used > capacity || allocation > capacity - used)
        return 0;
    *out_offset = used;
    *out_allocation_bytes = allocation;
    return 1;
}

int rsx_vertex_constant_ring_plan(
    u32 used, u32 capacity, u32 block_bytes, u32* out_offset)
{
    if (!out_offset || block_bytes == 0 || (block_bytes & 255u) != 0 ||
        block_bytes > capacity)
        return -1;
    if (used > capacity || block_bytes > capacity - used)
        return 0;
    *out_offset = used;
    return 1;
}

/* Bounded string appender. */
typedef struct { char* p; u32 cap; u32 len; int ok; } Out;

static void out_puts(Out* o, const char* s)
{
    if (!o->ok) return;
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->ok = 0; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

/* Decoded view of one source word. */
typedef struct {
    u32 type;       /* FP_REG_TYPE_* */
    u32 index;
    int half;
    int negate;
    int abs;
    char swz[5];    /* e.g. "xyzw" */
} Src;

static void decode_src(u32 w, int abs_bit, Src* s)
{
    static const char comp[4] = {'x','y','z','w'};
    s->type   = (w & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT;
    s->index  = (w & FP_SRC_REG_MASK)  >> FP_SRC_REG_SHIFT;
    s->half   = (w & FP_SRC_HALF) ? 1 : 0;
    s->negate = (w & FP_SRC_NEGATE) ? 1 : 0;
    s->abs    = abs_bit ? 1 : 0;
    for (int i = 0; i < 4; i++)
        s->swz[i] = comp[(w >> (FP_SRC_SWZ_SHIFT + 2 * i)) & 3];
    s->swz[4] = '\0';
}

/* HLSL expression for the interpolated input selected by OPDEST.INPUT_SRC.
 * The backend's passthrough VS forwards COL0/COL1/FOG/TEXCOORD0..7; WPOS maps
 * to SV_POSITION (screen-space — an approximation of the RSX window coord). */
/* The backend renders at this multiple of the guest's resolution
 * (RACACIT_RES_SCALE); SV_POSITION then counts the backend's pixels, and WPOS
 * is the guest's, so it is divided back. */
float rsx_fp_wpos_scale = 1.0f;
static int s_fp_buffered;   /* the program being built reads its constants from PSConstants */

static const char* input_expr(u32 input_src)
{
    static const char* tc[8] = {
        "input.tc0", "input.tc1", "input.tc2", "input.tc3",
        "input.tc4", "input.tc5", "input.tc6", "input.tc7" };
    switch (input_src) {
    case 0x0: {                         /* WPOS */
        /* A buffered program takes the factor per draw from fp_alpha.y: a pass
         * the engine draws at more than its guest size (A Crack in Time's
         * 480x352 4x-multisampled effects pass, drawn at 960x704 by
         * eng_aa_scale) must still see the guest's pixel. A zero there (a
         * caller that does not set it) means the scale baked below. */
        if (s_fp_buffered) {
            static char wposb[160];
            if (!wposb[0])
                snprintf(wposb, sizeof wposb,
                         "(input.position * float4((fp_alpha.y != 0.0 ? fp_alpha.yy : float2(%.9g, %.9g)), 1.0, 1.0))",
                         1.0 / rsx_fp_wpos_scale, 1.0 / rsx_fp_wpos_scale);
            return wposb;
        }
        if (rsx_fp_wpos_scale == 1.0f) return "input.position";
        static char wpos[96];
        if (!wpos[0])
            snprintf(wpos, sizeof wpos, "(input.position * float4(%.9g, %.9g, 1.0, 1.0))",
                     1.0 / rsx_fp_wpos_scale, 1.0 / rsx_fp_wpos_scale);
        return wpos;
    }
    case 0x1: return "input.col0";     /* COL0 */
    case 0x2: return "input.col1";     /* COL1 */
    case 0x3: return "input.fog";      /* FOGC */
    default:
        if (input_src >= 0x4 && input_src <= 0xB) return tc[input_src - 0x4]; /* TC0..7 */
        return "float4(0,0,0,0)";      /* TC8/TC9/FACING not plumbed */
    }
}

/* Build the swizzled/negated/abs'd HLSL for one source into `buf`. */
static void emit_src(const Src* s, u32 input_src, const float* k, int has_k,
                     int buffered, u32 constant_slot, char* buf, u32 bufsz)
{
    char base[96];
    if (s->type == FP_REG_TYPE_TEMP) {
        snprintf(base, sizeof(base), "%s[%u]", s->half ? "h" : "r", s->index);
    } else if (s->type == FP_REG_TYPE_INPUT) {
        snprintf(base, sizeof(base), "%s", input_expr(input_src));
    } else { /* CONST */
        if (buffered && has_k)
            snprintf(
                base, sizeof(base), "fp_constants[%u]", constant_slot);
        else if (has_k)
            snprintf(base, sizeof(base), "float4(%g,%g,%g,%g)",
                     k[0], k[1], k[2], k[3]);
        else
            snprintf(base, sizeof(base), "float4(0,0,0,0)");
    }

    char swz[160];
    snprintf(swz, sizeof(swz), "(%s).%s", base, s->swz);

    const char* pre = "";
    const char* post = "";
    if (s->abs)    { pre = "abs("; post = ")"; }
    if (s->negate) snprintf(buf, bufsz, "-(%s%s%s)", pre, swz, post);
    else           snprintf(buf, bufsz, "%s%s%s", pre, swz, post);
}

/* Dest write-mask letters from OPDEST bits 9..12 (X..W). */
static void dest_mask(u32 op0, char* m)
{
    int n = 0;
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 0))) m[n++] = 'x';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 1))) m[n++] = 'y';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 2))) m[n++] = 'z';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 3))) m[n++] = 'w';
    if (n == 0) { m[0] = 'x'; m[1] = 'y'; m[2] = 'z'; m[3] = 'w'; n = 4; }
    m[n] = '\0';
}

/* CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS: when set, fragment output is fp32
 * (color = r0); when clear, output is fp16 (color = h0). Matches RPCS3
 * FragmentProgramDecompiler::BuildCode (gcm_enums.h). */
#define FP_CTRL_32BIT_EXPORTS 0x40u

/* The four colour exports live at fixed register indices, one set per export
 * width: r0/r2/r3/r4 with 32-bit exports and h0/h4/h6/h8 with 16-bit ones
 * (rsx_commands.h, SHADER_CONTROL 0x40; RPCS3 FragmentProgramDecompiler's
 * BuildCode table). Everything else in the file is a plain temp. */
#define FP_MAX_COLOR_TARGETS 4
static const u32 fp_export_reg[2][FP_MAX_COLOR_TARGETS] = {
    { 0, 2, 3, 4 },   /* 32-bit exports: r */
    { 0, 4, 6, 8 },   /* 16-bit exports: h */
};

/* Which colour target a destination register is, or -1 for a plain temp. */
static int fp_export_target(u32 index, int half)
{
    for (int t = 0; t < FP_MAX_COLOR_TARGETS; t++)
        if (fp_export_reg[half][t] == index) return t;
    return -1;
}

int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size)
{
    return rsx_fp_decompile_ex(ucode, max_bytes, ctrl, 0u, out, out_size);
}

/* How cube units are declared when a program has any (see the texture bank
 * below). 0: each unit individually at t<unit> (D3D12 / Metal). 1: the 2D
 * array stays at t0 and cube units come from a second array, TextureCube
 * rsx_texcube[16] at t48 -- two fixed bindings, which is what a Vulkan
 * descriptor set layout shared by every pipeline can express. */
int rsx_fp_cube_array_style = 0;
u32 rsx_fp_shadow_mask = 0;
u8  rsx_fp_shadow_func[16];
/* Units whose TEXTURE_ADDRESS unsigned remap is BIASED (bits 12-15 = 1): the
 * RSX returns every channel as 2x-1, the way normal maps and G-buffers are
 * stored. A Crack in Time's shadow-light pass reads its normal buffer so and
 * dots it with the light direction straight away; read raw, N.L came out ~0
 * and each character shadow volume shaded its whole box black. */
u32 rsx_fp_biased_mask = 0;

/* TEXTURE_ADDRESS gamma, 4 bits a unit (R, G, B, A): those channels are
 * converted from sRGB to linear on read. A Crack in Time marks its colour
 * maps so and lights in linear space; read raw, every level whose materials
 * set it came out washed out and overbright (Molonoth, Tombli, Vorselon's
 * ship). rsx_gamma() is in every program's prologue. */
u64 rsx_fp_gamma_mask = 0;
/* The program's colour outputs are converted from linear to sRGB on write
 * (rsx_srgb_enc in the prologue). */
int rsx_fp_srgb_out = 0;
int rsx_fp_dynamic_tex = 0;
static int s_fp_dyn;     /* rsx_fp_dynamic_tex, for a buffered program (it needs the block) */

/* A fetch's value as the unit returns it (rsx_fp_gamma_mask, then
 * rsx_fp_biased_mask). */
static void fp_tex_remap(char* rhs, size_t n, u32 unit)
{
    if (s_fp_dyn) {
        char t[704];
        snprintf(t, sizeof t, "rsx_texmode(%s, %uu)", rhs, unit & 15u);
        snprintf(rhs, n, "%s", t);
        return;
    }
    const u32 gm = (u32)(rsx_fp_gamma_mask >> ((unit & 15u) * 4u)) & 0xFu;
    if (gm) {
        char t[704];
        snprintf(t, sizeof t, "rsx_gamma(%s, float4(%u, %u, %u, %u))", rhs,
                 gm & 1u, (gm >> 1) & 1u, (gm >> 2) & 1u, (gm >> 3) & 1u);
        snprintf(rhs, n, "%s", t);
    }
    if (!((rsx_fp_biased_mask >> (unit & 15u)) & 1u)) return;
    char t[704];
    snprintf(t, sizeof t, "((%s) * 2.0 - 1.0)", rhs);
    snprintf(rhs, n, "%s", t);
}

/* The HLSL expression naming texture unit `u` for this program. */
static void fp_tex_name(char* out, size_t n, u32 cube_mask, u32 u)
{
    const int cube = (cube_mask >> u) & 1u;
    if (rsx_fp_cube_array_style)
        snprintf(out, n, cube ? "rsx_texcube[%u]" : "rsx_tex[%u]", u);
    else if (cube_mask)
        snprintf(out, n, "rsx_tex%u", u);
    else
        snprintf(out, n, "rsx_tex[%u]", u);
}

static int rsx_fp_decompile_internal(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    int buffered, char* out, u32 out_size, u32* out_constant_count)
{
    if (!ucode || !out || out_size == 0) return -1;

    u32 buffered_constant_count = 0;
    s_fp_dyn = rsx_fp_dynamic_tex && buffered;
    s_fp_buffered = buffered;
    if (buffered) {
        rsx_fp_constant_block constants;
        const int result =
            rsx_fp_collect_constants(ucode, max_bytes, &constants);
        if (result < 0)
            return -1;
        buffered_constant_count = constants.count;
    }
    if (out_constant_count)
        *out_constant_count = buffered_constant_count;

    /* The BODY is emitted first and the preamble is inserted in front of it
     * afterwards. main's signature names the return type, and the return type
     * is a struct as soon as the program turns out to write a second colour
     * export -- which is only known once every instruction has been decoded.
     * Emitting the preamble here would mean guessing it. */
    Out o = { out, out_size, 0, 1 };
    out[0] = '\0';
    out_puts(&o,
        "    float4 r[48]; float4 h[48];\n"
        /* Fully initialise both register files: RSX programs routinely read a
         * register lane before writing it (the hardware reads undefined), but
         * HLSL rejects use-before-init (error X4000). Zero everything. */
        "    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }\n"
        /* Condition-code registers for exec_if predication / set_cond. Reset
         * to 0 (NV40 CC power-up state). Dead in non-predicated programs. */
        "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n");

    /* Which colour export each register file received a write to, indexed
     * [half][target]. The whole file is tracked, not just target 0, because
     * the highest one written decides how many SV_TARGETs come out. */
    int wrote[2][FP_MAX_COLOR_TARGETS];
    memset(wrote, 0, sizeof wrote);
    int count = 0;
    u32 off = 0;
    u32 constant_slot = 0;

    /* Open IFE/LOOP/REP blocks: the byte offset each one's else branch starts
     * at (0 = none) and where it ends. See the branch case below. */
    enum { FP_MAX_BLOCKS = 32 };
    u32 blk_else[FP_MAX_BLOCKS], blk_end[FP_MAX_BLOCKS];
    int blk_loop[FP_MAX_BLOCKS];
    int n_blk = 0, n_loop = 0, loops_open = 0;

    while (off + 16 <= max_bytes) {
        /* Close blocks ending here, innermost first, then open an else. Ends
         * before elses, as RPCS3's FragmentProgramDecompiler::Decompile does. */
        while (n_blk > 0 && blk_end[n_blk - 1] == off) {
            out_puts(&o, "    }\n");
            loops_open -= blk_loop[--n_blk];
        }
        if (n_blk > 0 && blk_else[n_blk - 1] == off) {
            out_puts(&o, "    } else {\n");
            blk_else[n_blk - 1] = 0;
        }
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        count++;

        u32 opcode    = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        u32 tex_unit  = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
        int is_branch = (w2 & FP_BRANCH) ? 1 : 0;
        /* Texture lookups this decompiler does not compare or does not do at
         * all: TXD anywhere, TXB/TXL or a cube map on a depth-compare unit
         * (those sample the raw depth). Logged once each, to see whether a
         * title uses them. */
        if (!is_branch && (opcode == OP_TXD || opcode == OP_TXB || opcode == OP_TXL || opcode == OP_TEX || opcode == OP_TXP)) {
            const int shadow = (rsx_fp_shadow_mask >> tex_unit) & 1u, cube = (tex_cube_mask >> tex_unit) & 1u;
            const int kind = opcode == OP_TXD ? 0 : (shadow && cube) ? 1 : (shadow && (opcode == OP_TXB || opcode == OP_TXL)) ? 2 : -1;
            static u8 said[3];
            if (kind >= 0 && !said[kind]) { said[kind] = 1;
                fprintf(stderr, "[rsx fp] %s on texture unit %u%s: %s%c", rsx_fp_opcode_name(opcode), tex_unit,
                        shadow ? " (depth compare)" : "", cube ? "cube map" : "2D",
                        10); }
        }

        /* Execution condition (exec_if) + condition-code write (set_cond).
         * Faithful to RPCS3 FragmentProgramDecompiler {GetRawCond, AddCodeCond,
         * SetDst}: all-set == unconditional, none-set == never, otherwise a
         * per-component select of the result vs the destination, gated by a
         * sign test of the cond-swizzled CC register. */
        int exec_lt   = (w1 & FP_EXEC_IF_LT) ? 1 : 0;
        int exec_eq   = (w1 & FP_EXEC_IF_EQ) ? 1 : 0;
        int exec_gr   = (w1 & FP_EXEC_IF_GR) ? 1 : 0;
        int is_uncond = (exec_lt && exec_eq && exec_gr);
        int is_never  = (!exec_lt && !exec_eq && !exec_gr);
        int set_cond  = (w0 & FP_COND_WRITE) ? 1 : 0;
        u32 cc_read   = (w1 & FP_COND_REG)     ? 1u : 0u;
        u32 cc_write  = (w1 & FP_COND_MOD_REG) ? 1u : 0u;
        static const char comp_c[4] = {'x','y','z','w'};
        char cswz[5];
        for (int ci = 0; ci < 4; ci++)
            cswz[ci] = comp_c[(w1 >> (FP_COND_SWZ_SHIFT + 2 * ci)) & 3];
        cswz[4] = '\0';
        /* HLSL comparison operator for the exec_if sign mask (matches the
         * oracle's SGE/SLE/SNE/SGT/SLT/SEQ selection in GetRawCond). */
        const char* cmp_op = "==";
        if      (exec_gr && exec_eq) cmp_op = ">=";
        else if (exec_lt && exec_eq) cmp_op = "<=";
        else if (exec_gr && exec_lt) cmp_op = "!=";
        else if (exec_gr)            cmp_op = ">";
        else if (exec_lt)            cmp_op = "<";
        else                         cmp_op = "==";  /* exec_eq only */

        Src s0, s1, s2;
        decode_src(w1, w1 & FP_SRC0_ABS, &s0);
        decode_src(w2, w2 & FP_SRC1_ABS, &s1);
        decode_src(w3, 0,                &s2);

        /* Inline constant: any CONST source pulls the next 16 bytes as a
         * float4 literal and advances past it. */
        float k[4] = {0,0,0,0};
        int has_k = 0;
        if (s0.type == FP_REG_TYPE_CONST || s1.type == FP_REG_TYPE_CONST ||
            s2.type == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + i * 4));
                    memcpy(&k[i], &cw, 4);
                }
                off += 16;
            }
            has_k = 1;
        }

        char a[200], b[200], c[200];
        emit_src(
            &s0, input_src, k, has_k, buffered, constant_slot,
            a, sizeof(a));
        emit_src(
            &s1, input_src, k, has_k, buffered, constant_slot,
            b, sizeof(b));
        emit_src(
            &s2, input_src, k, has_k, buffered, constant_slot,
            c, sizeof(c));
        if (has_k)
            constant_slot++;

        /* Flow control: SRC1.branch set, and OPDEST's opcode field selects
         * BRK/CAL/IFE/LOOP/REP/RET (RPCS3 RSXFragmentProgram.h, 0x40..0x45).
         * IFE carries the else and end addresses in SRC1/SRC2 [0:30] as word
         * offsets; LOOP/REP carry end/init/increment counters in SRC1 [2:9],
         * [10:17], [19:26] and the end address in SRC2. The condition is the
         * exec_if test on the first component of the condition swizzle.
         * Skipping these ran both sides of every if/else: Crazy Taxi's uber
         * material shader then overwrote its lit colour with a COLOR0 the
         * vertex program never writes, and the whole city drew black. */
        if (is_branch) {
            char cond[64], line[160];
            if (is_uncond)
                snprintf(cond, sizeof cond, "true");
            else if (is_never)
                snprintf(cond, sizeof cond, "false");
            else
                snprintf(cond, sizeof cond, "cc%u.%c %s 0.0", cc_read, cswz[0], cmp_op);
            const u32 end_at = (w3 & 0x7FFFFFFFu) << 2;
            switch (opcode) {
            case 0x02: /* IFE */
            case 0x03: /* LOOP */
            case 0x04: /* REP */
                if (n_blk == FP_MAX_BLOCKS || end_at <= off) {
                    out_puts(&o, "    /* TODO: malformed flow-control op skipped */\n");
                    break;
                }
                if (opcode == 0x02) {
                    const u32 else_at = (w2 & 0x7FFFFFFFu) << 2;
                    blk_else[n_blk] = else_at != end_at ? else_at : 0;
                    blk_loop[n_blk] = 0;
                    snprintf(line, sizeof line, "    if (%s) {\n", cond);
                } else {
                    const u32 end_n = (w2 >> 2) & 0xFFu, init = (w2 >> 10) & 0xFFu;
                    const u32 inc = (w2 >> 19) & 0xFFu;
                    blk_else[n_blk] = 0;
                    blk_loop[n_blk] = 1;
                    snprintf(line, sizeof line,
                             "    if (%s) [loop] for (int _l%d = %u; _l%d < %u; _l%d += %u) {\n",
                             cond, n_loop, init, n_loop, end_n, n_loop, inc ? inc : 1u);
                    n_loop++;
                    loops_open++;
                }
                blk_end[n_blk++] = end_at;
                out_puts(&o, line);
                break;
            case 0x00: /* BRK */
                snprintf(line, sizeof line, "    if (%s) break;\n", cond);
                if (loops_open) out_puts(&o, line);
                break;
            default:   /* CAL / RET: no subroutine support yet */
                out_puts(&o, "    /* TODO: CAL/RET flow-control op skipped */\n");
                break;
            }
            if (w0 & FP_END) break;
            continue;
        }

        /* Build the RHS expression for this opcode. */
        char rhs[640];
        int handled = 1;
        switch (opcode) {
        case OP_NOP: rhs[0] = '\0'; handled = 0; break;
        case OP_MOV: snprintf(rhs, sizeof(rhs), "%s", a); break;
        case OP_MUL: snprintf(rhs, sizeof(rhs), "(%s) * (%s)", a, b); break;
        case OP_ADD: snprintf(rhs, sizeof(rhs), "(%s) + (%s)", a, b); break;
        case OP_MAD: snprintf(rhs, sizeof(rhs), "(%s) * (%s) + (%s)", a, b, c); break;
        case OP_DP3: snprintf(rhs, sizeof(rhs), "dot((%s).xyz, (%s).xyz)", a, b); break;
        case OP_DP4: snprintf(rhs, sizeof(rhs), "dot((%s), (%s))", a, b); break;
        case OP_MIN: snprintf(rhs, sizeof(rhs), "min((%s), (%s))", a, b); break;
        case OP_MAX: snprintf(rhs, sizeof(rhs), "max((%s), (%s))", a, b); break;
        case OP_FRC: snprintf(rhs, sizeof(rhs), "frac(%s)", a); break;
        case OP_FLR: snprintf(rhs, sizeof(rhs), "floor(%s)", a); break;
        case OP_RCP: snprintf(rhs, sizeof(rhs), "(1.0 / (%s).x)", a); break;
        /* Real RSX RSQ ignores the input's sign — 1/sqrt(|x|), not 1/sqrt(x)
         * (oracle: RPCS3 FragmentProgramDecompiler.cpp, "RSQ ignores the sign
         * of the inputs (Metro Last Light, GTA4)"). The strict form NaN'd the
         * normal-map Z-reconstruction (1-x²-y² measurably negative for most
         * pixels, median -0.0078) and poisoned the whole lighting chain — the
         * s25-s27 black/blue character class (scratch/s26_fp_bisect.md s27
         * part 4; two independent shader pairs confirmed). */
        case OP_RSQ: snprintf(rhs, sizeof(rhs), "rsqrt(abs((%s).x))", a); break;
        case OP_EX2: snprintf(rhs, sizeof(rhs), "exp2((%s).x)", a); break;
        case OP_LG2: snprintf(rhs, sizeof(rhs), "log2((%s).x)", a); break;
        case OP_COS: snprintf(rhs, sizeof(rhs), "cos((%s).x)", a); break;
        case OP_SIN: snprintf(rhs, sizeof(rhs), "sin((%s).x)", a); break;
        case OP_POW: snprintf(rhs, sizeof(rhs), "pow((%s).x, (%s).x)", a, b); break;
        case OP_DIV: snprintf(rhs, sizeof(rhs), "(%s) / (%s).x", a, b); break;
        /* Real RSX DIVSQ shares RSQ's sign-ignoring sqrt on the denominator
         * (oracle: RPCS3 FragmentProgramDecompiler.cpp's _builtin_sqrt macro,
         * reused by _builtin_divsq) AND forces the result to exactly 0,
         * component-wise, wherever the NUMERATOR component is 0 -- even if
         * the denominator is also 0 (oracle: same file, "DIVSQ is not
         * compliant. Result is 0 if numerator is 0 regardless of
         * denominator", FragmentProgramDecompiler.cpp:1076). Using a true
         * per-component select (HLSL vector `?:`, not lerp/mix) avoids NaN
         * contamination from the 0/0 branch, matching the oracle's own note
         * on why it can't use a blend here. */
        case OP_DIVSQ:
            snprintf(rhs, sizeof(rhs),
                     "(abs(%s) > 0.0 ? (%s) / sqrt(abs((%s).x)) : (float4)0.0)",
                     a, a, b);
            break;
        case OP_DP2: snprintf(rhs, sizeof(rhs), "dot((%s).xy, (%s).xy)", a, b); break;
        case OP_NRM: snprintf(rhs, sizeof(rhs), "float4(normalize((%s).xyz), 1.0)", a); break;
        case OP_LRP: snprintf(rhs, sizeof(rhs), "lerp((%s), (%s), (%s))", c, b, a); break;
        /* Lighting coefficients: (1, y, y > 0 ? 2^w : 0, 1) (RPCS3
         * FragmentProgramDecompiler, RSX_FP_OPCODE_LIF). */
        case OP_LIF:
            snprintf(rhs, sizeof(rhs),
                     "float4(1.0, (%s).y, (%s).y > 0.0 ? exp2((%s).w) : 0.0, 1.0)", a, a, a);
            break;
        /* Texture/branch fences: ordering hints with no result -- no-op. */
        case OP_FENCT: case OP_FENCB: rhs[0] = '\0'; handled = 0; break;
        case OP_SLT: snprintf(rhs, sizeof(rhs), "(float4)((%s) <  (%s))", a, b); break;
        case OP_SGE: snprintf(rhs, sizeof(rhs), "(float4)((%s) >= (%s))", a, b); break;
        case OP_SLE: snprintf(rhs, sizeof(rhs), "(float4)((%s) <= (%s))", a, b); break;
        case OP_SGT: snprintf(rhs, sizeof(rhs), "(float4)((%s) >  (%s))", a, b); break;
        case OP_SNE: snprintf(rhs, sizeof(rhs), "(float4)((%s) != (%s))", a, b); break;
        case OP_SEQ: snprintf(rhs, sizeof(rhs), "(float4)((%s) == (%s))", a, b); break;
        case OP_TEX: {
            char tn[32];
            fp_tex_name(tn, sizeof tn, tex_cube_mask, tex_unit);
            if (((rsx_fp_shadow_mask >> tex_unit) & 1u) && !((tex_cube_mask >> tex_unit) & 1u)) {
                snprintf(rhs, sizeof(rhs), "rsx_shadow(%s, rsx_samp[%u], (%s).xyz, %u)", tn, tex_unit, a,
                         (unsigned)rsx_fp_shadow_func[tex_unit & 15u]);
                break;
            }
            /* Cubemap: sample with the full 3-component direction vector. */
            snprintf(rhs, sizeof(rhs), "%s.Sample(rsx_samp[%u], (%s).%s)", tn, tex_unit, a,
                     ((tex_cube_mask >> tex_unit) & 1u) ? "xyz" : "xy");
            fp_tex_remap(rhs, sizeof rhs, tex_unit);
            break;
        }
        case OP_TXP: {
            char tn[32];
            fp_tex_name(tn, sizeof tn, tex_cube_mask, tex_unit);
            if (((rsx_fp_shadow_mask >> tex_unit) & 1u) && !((tex_cube_mask >> tex_unit) & 1u)) {
                snprintf(rhs, sizeof(rhs), "rsx_shadow(%s, rsx_samp[%u], (%s).xyz / (%s).w, %u)", tn, tex_unit,
                         a, a, (unsigned)rsx_fp_shadow_func[tex_unit & 15u]);
                break;
            }
            if ((tex_cube_mask >> tex_unit) & 1u)
                /* Projective divide is meaningless for a cube lookup; sample
                 * the direction directly. */
                snprintf(rhs, sizeof(rhs), "%s.Sample(rsx_samp[%u], (%s).xyz)", tn, tex_unit, a);
            else
                snprintf(rhs, sizeof(rhs), "%s.Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tn, tex_unit, a, a);
            fp_tex_remap(rhs, sizeof rhs, tex_unit);
            break;
        }
        case OP_TXB: case OP_TXL: {
            /* Biased / explicit-LOD sample; bias or LOD is src1.x (RPCS3
             * TEXTURE_SAMPLE2D_BIAS / _LOD). GH3's fret buttons and fret lines
             * are TXB: unhandled, they sampled nothing and output alpha 0. */
            const int cube = (tex_cube_mask >> tex_unit) & 1u;
            char tn[32];
            fp_tex_name(tn, sizeof tn, tex_cube_mask, tex_unit);
            snprintf(rhs, sizeof(rhs), "%s.%s(rsx_samp[%u], (%s).%s, (%s).x)", tn,
                     opcode == OP_TXB ? "SampleBias" : "SampleLevel", tex_unit, a,
                     cube ? "xyz" : "xy", b);
            fp_tex_remap(rhs, sizeof rhs, tex_unit);
            break;
        }
        case OP_KIL:
            /* Fragment kill. Predicated by the same exec_if condition as any
             * other instruction (RPCS3 FragmentProgramDecompiler case
             * RSX_FP_OPCODE_KIL: AddFlowOp("discard") under GetCond());
             * unconditional encodings discard outright. Dropping this (the
             * old TODO) rendered every alpha-cutout material as a solid
             * quad. */
            if (is_never) {
                rhs[0] = '\0';
            } else if (is_uncond) {
                out_puts(&o, "    discard;\n");
            } else {
                char kil[128];
                snprintf(kil, sizeof(kil),
                         "    if (any(cc%u.%s %s (float4)0)) discard;\n",
                         cc_read, cswz, cmp_op);
                out_puts(&o, kil);
            }
            handled = 0;   /* no destination write */
            break;
        default:
            out_puts(&o, "    /* TODO: unhandled FP opcode ");
            out_puts(&o, rsx_fp_opcode_name(opcode));
            out_puts(&o, " */\n");
            handled = 0;
            break;
        }

        int has_dest = !(w0 & FP_OUT_NONE);
        if (handled && (has_dest || set_cond)) {
            u32 dst_idx = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            int dst_half = (w0 & FP_OUT_HALF) ? 1 : 0;
            const char* reg = dst_half ? "h" : "r";
            char m[5];
            dest_mask(w0, m);

            const char* sat = (w0 & FP_OUT_SAT) ? " _v = saturate(_v);" : "";
            /* NV40 per-instruction result-scale modifier (SRC1 word bits
             * 28-30): 1/2/3 = *2/*4/*8, 5/6/7 = /2//4//8; applied to the
             * result BEFORE saturate (RPCS3 FragmentProgramDecompiler.cpp
             * SetDst, RSXFragmentProgram.h SRC1.scale:3). Dropped entirely
             * until s32: draw 1625's two-tap box filter (÷2 on the final
             * ADD) summed unaveraged -- the flat 2x composite gain behind
             * the whiteout/overbright class (scratch/s32_gain_hunt.md). */
            u32 res_scale = (w2 >> 28) & 0x7u;
            static const char* scale_txt[8] = { "", " * 2.0", " * 4.0", " * 8.0",
                                                "", " / 2.0", " / 4.0", " / 8.0" };

            /* Broadcast the result to float4 first so the write-mask picks the
             * CORRESPONDING components: `dst.w = rhs` would HLSL-truncate a
             * vector rhs to its .x, but NV40 writes rhs.w to dst.w. Scalar
             * results (DPx/RCP/...) replicate, which is also the hardware
             * behavior. */
            if (has_dest && is_uncond && !set_cond) {
                /* Common unconditional path — kept byte-identical so the 39
                 * non-predicated programs in this capture emit unchanged. */
                char line[800];
                snprintf(line, sizeof(line),
                         "    { float4 _v = (float4)(%s)%s;%s %s[%u].%s = _v.%s; }\n",
                         rhs, scale_txt[res_scale], sat, reg, dst_idx, m, m);
                out_puts(&o, line);
            } else {
                /* Predicated and/or CC-writing instruction. */
                char buf[1200];
                snprintf(buf, sizeof(buf),
                         "    { float4 _v = (float4)(%s)%s;%s\n",
                         rhs, scale_txt[res_scale], sat);
                out_puts(&o, buf);
                if (has_dest) {
                    if (is_uncond) {
                        snprintf(buf, sizeof(buf),
                                 "      %s[%u].%s = _v.%s;\n", reg, dst_idx, m, m);
                    } else if (is_never) {
                        snprintf(buf, sizeof(buf),
                                 "      /* exec_if=none: write suppressed */\n");
                    } else {
                        /* Per-component select gated by the CC sign test.
                         * lerp(dst, _v, mask) == mask ? _v : dst since the
                         * mask is exactly 0.0/1.0 from the bool->float cast. */
                        snprintf(buf, sizeof(buf),
                                 "      float4 _p = (float4)((cc%u).%s %s (float4)0.0);\n"
                                 "      %s[%u].%s = lerp(%s[%u].%s, _v.%s, _p.%s);\n",
                                 cc_read, cswz, cmp_op,
                                 reg, dst_idx, m, reg, dst_idx, m, m, m);
                    }
                    out_puts(&o, buf);
                }
                if (set_cond) {
                    /* CC receives the destination value (post-select), masked;
                     * for no-dest set_cond it receives the raw result. */
                    if (has_dest)
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = %s[%u].%s;\n", cc_write, m, reg, dst_idx, m);
                    else
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = _v.%s;\n", cc_write, m, m);
                    out_puts(&o, buf);
                }
                out_puts(&o, "    }\n");
            }

            if (has_dest) {
                const int target = fp_export_target(dst_idx, dst_half);
                if (target >= 0) wrote[dst_half][target] = 1;
            }
        }

        if (w0 & FP_END) break;
    }
    /* A block whose end address is at or past END never saw it: close it. */
    while (n_blk-- > 0)
        out_puts(&o, "    }\n");

    /* Fragment color output register selection. The NV40 hardware picks the
     * output from the SHADER_CONTROL word's 32_BITS_EXPORTS bit, not from
     * whichever temp happened to be written: fp32-export programs output r0,
     * fp16-export programs (the default) output h0 (RPCS3
     * FragmentProgramDecompiler::BuildCode). Many real material/lighting
     * shaders accumulate into fp32 temps (r2/r3/r4) but write their FINAL
     * color to h0 — the old "wrote_r0 => return r0" heuristic returned a stale
     * intermediate, producing flat/constant surfaces. */
    int half_exports;
    if (ctrl == RSX_FP_CTRL_AUTO) {
        /* No control word (standalone tests): legacy heuristic. */
        half_exports = (wrote[0][0] || !wrote[1][0]) ? 0 : 1;
    } else {
        half_exports = (ctrl & FP_CTRL_32BIT_EXPORTS) ? 0 : 1;
    }
    const char* export_file = half_exports ? "h" : "r";
    /* CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT (0xE): the program replaces the
     * fragment's depth with r1.z (h2.z with 16-bit exports) -- RPCS3
     * FragmentProgramDecompiler. A Crack in Time resolves its multisampled
     * scene depth into 0x1380000 with such a pass; ignoring the export left
     * that buffer at the quad's own depth, and everything that reads scene
     * depth back (decals, blob shadows, soft particles) went wrong. */
    const int depth_export = ctrl != RSX_FP_CTRL_AUTO && (ctrl & 0xEu) != 0;

    /* How many colour targets the program feeds: one past the highest export
     * register of the selected file it wrote. A program that writes only the
     * first keeps the single-value return it has always had, byte for byte;
     * only a program that reaches the second, third or fourth grows a struct.
     * Writes to the OTHER file's export registers do not count -- r2/r3/r4 are
     * ordinary temps under 16-bit exports, and accumulating into them while
     * the colour goes to h0 is what most material shaders do. */
    u32 targets = 1;
    for (u32 t = FP_MAX_COLOR_TARGETS; t-- > 1; )
        if (wrote[half_exports][t]) { targets = t + 1; break; }

    if (targets == 1 && !depth_export) {
        char tail[512];
        /* RSX_FP_SHADOW_DEBUG=<s>: a program with a depth compare returns its
         * last compare's inputs instead -- (x*s+.5, y*s+.5, ref, stored). */
        static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_FP_SHADOW_DEBUG") ? 1 : 0;
        /* RSX_FP_SHADOW_OUT=<HLSL float4 expression>: a program with a depth
         * compare returns that instead (any r[], h[], cc0 of the program),
         * unguarded -- for reading its intermediate values with a probe. */
        static const char* oexp = (const char*)-1;
        if (oexp == (const char*)-1) oexp = getenv("RSX_FP_SHADOW_OUT");
        if (oexp && *oexp && rsx_fp_shadow_mask) {
            out_puts(&o, "    return ");
            out_puts(&o, oexp);
            out_puts(&o, ";\n}\n");
        } else {
        snprintf(tail, sizeof(tail),
                 rsx_fp_srgb_out
                     ? "    float4 _o = rsx_srgb_enc(%s);\n    return (_o == _o) ? _o : (float4)0;\n}\n"
                     : "    float4 _o = %s;\n    return (_o == _o) ? _o : (float4)0;\n}\n",
                 (dbg && rsx_fp_shadow_mask) ? "rsx_dbg" : (export_file[0] == 'h' ? "h[0]" : "r[0]"));
        out_puts(&o, tail);
        }
    } else {
        /* Multiple render targets. The NaN guard is per target: a poisoned
         * G-buffer plane is as fatal as a poisoned colour one, and the planes
         * are computed independently. */
        out_puts(&o, "    PSOutput _out;\n");
        for (u32 t = 0; t < targets; t++) {
            char line[512];
            snprintf(line, sizeof(line),
                     rsx_fp_srgb_out
                         ? "    float4 _o%u = rsx_srgb_enc(%s[%u]);\n    _out.t%u = (_o%u == _o%u) ? _o%u : (float4)0;\n"
                         : "    float4 _o%u = %s[%u];\n    _out.t%u = (_o%u == _o%u) ? _o%u : (float4)0;\n",
                     t, export_file, fp_export_reg[half_exports][t],
                     t, t, t, t);
            out_puts(&o, line);
        }
        /* On the console the exported depth lands in a 24-bit buffer, and so
         * does every later fragment's depth before it is compared. Here both
         * stay 32-bit floats, so a copy of the scene's depth (A Crack in Time
         * rebuilds it from 24-bit bytes into 0x01380000 and tests every effect
         * against it, LEQUAL) sat up to half a step below the surfaces it came
         * from: an effect lying on one lost about half its pixels, the console
         * none. Stored one 24-bit step up instead (a float has no room for the
         * half step near 1.0), every fragment the console puts in that step
         * passes. The far plane stays 1.0. */
        if (depth_export)
            out_puts(&o, half_exports
                ? "    _out.d = min((round(saturate(h[2].z) * 16777215.0) + 1.0) / 16777215.0, 1.0);\n"
                : "    _out.d = min((round(saturate(r[1].z) * 16777215.0) + 1.0) / 16777215.0, 1.0);\n");
        out_puts(&o, "    return _out;\n}\n");
    }

    /* Preamble: PSInput matches the backend's placeholder layout; temp/half
     * register files; texture+sampler banks for TEX. Built here, in front of
     * the body, now that the return type is known. */
    char preamble[8192];
    Out p = { preamble, sizeof(preamble), 0, 1 };
    preamble[0] = '\0';
    out_puts(&p,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float4 fog : FOG;\n"
        "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
        "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
        "};\n");
    /* Texture bank. With no cube units (the default) emit the exact legacy
     * array declaration so 2D-only programs are byte-identical. When any unit
     * is a cubemap, declare each unit individually at its t-register so the
     * cube units can be TextureCube while the rest stay Texture2D. */
    if (tex_cube_mask == 0 || rsx_fp_cube_array_style) {
        out_puts(&p, "Texture2D    rsx_tex[16] : register(t0);\n");
        if (tex_cube_mask)
            out_puts(&p, "TextureCube  rsx_texcube[16] : register(t48);\n");
    } else {
        char decl[64];
        for (u32 u = 0; u < 16; u++) {
            snprintf(decl, sizeof(decl), "%s rsx_tex%u : register(t%u);\n",
                     ((tex_cube_mask >> u) & 1u) ? "TextureCube" : "Texture2D  ", u, u);
            out_puts(&p, decl);
        }
    }
    out_puts(&p,
        "SamplerState rsx_samp[16] : register(s0);\n"
    );
    if (rsx_fp_srgb_out)
        out_puts(&p,
            "float4 rsx_srgb_enc(float4 c) {\n"
            "    float3 v = saturate(c.rgb);\n"
            "    float3 e = lerp(1.055 * pow(v, 1.0 / 2.4) - 0.055, v * 12.92, step(v, 0.0031308));\n"
            "    return float4(e, c.a);\n"
            "}\n");
    /* sRGB to linear for the channels m selects (rsx_fp_gamma_mask). */
    if (rsx_fp_gamma_mask)
        out_puts(&p,
            "float4 rsx_gamma(float4 c, float4 m) {\n"
            "    float4 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);\n"
            "    float4 l = lerp(hi, c / 12.92, step(c, 0.04045));\n"
            "    return lerp(c, l, m);\n"
            "}\n");
    /* Depth compare with the hardware's bilinear PCF: four taps, each
     * (depth OP ref) -- the RSX puts the stored texel on the left, the
     * reverse of GL's (ref OP texel); RPCS3 reverses its shadow samplers'
     * compare direction for the same reason. A Crack in Time's sun shadows
     * use GEQUAL with 1 = lit: read as (ref >= depth), every receiver in
     * front of the map came out 0 and each light volume a solid dark slab
     * under Ratchet. Weighted by the sample position's fraction. */
    if (rsx_fp_shadow_mask)
        out_puts(&p,
            "float rsx_zcmp(float r, float d, int f) {\n"
            "    return f == 1 ? (float)(d <  r) : f == 2 ? (float)(d == r) : f == 3 ? (float)(d <= r) :\n"
            "           f == 4 ? (float)(d >  r) : f == 5 ? (float)(d != r) : f == 6 ? (float)(d >= r) :\n"
            "           f == 7 ? 1.0 : 0.0;\n"
            "}\n"
            "static float4 rsx_dbg = (float4)0;\n");
    if (rsx_fp_shadow_mask) {
        /* RSX_FP_SHADOW_DEBUG=<mode>: 1 coords/ref/depth, 2 the four gathered
         * depths, 3 the reference at four scales, 4 a plain sample + size. */
        static int mode = -1;
        if (mode < 0) { const char* e = getenv("RSX_FP_SHADOW_DEBUG"); mode = e ? atoi(e) : 0; }
        char dm[64];
        snprintf(dm, sizeof dm, "static const int rsx_dbg_mode = %d;\n", mode);
        out_puts(&p, dm);
    }
    /* The reference is clamped to [0,1] before the compare, as for every
     * fixed-point depth format (GL and D3D do the same). A Crack in Time fits
     * each character's shadow frustum tightly around the character, so the
     * ground beside Ratchet lies past its far plane (ref ~1.5): unclamped,
     * that ground compared as shadowed and his shadow was a solid slab. */
    /* RSX_SHADOW_FILTER=smooth (the default): a 3x3 PCF of bilinear taps
     * instead of the RSX's single bilinear PCF -- four gathers over the 4x4
     * texels around the lookup, weighted (1-f, 1, 1, f)/3 each way. A
     * magnified shadow map's edge then ramps over ~1.5 texels instead of
     * stepping texel by texel: the blocky "boxy" edges on the moons and in
     * the Great Clock, which the console's own filter draws too. =console is
     * the hardware's filter exactly; so is every RSX_FP_SHADOW_DEBUG mode. */
    static int s_smooth = -1;
    if (s_smooth < 0) {
        const char* e = getenv("RSX_SHADOW_FILTER");
        s_smooth = (e && !strcmp(e, "console")) || getenv("RSX_FP_SHADOW_DEBUG") ? 0 : 1;
    }
    if (rsx_fp_shadow_mask && s_smooth)
        out_puts(&p,
            "float4 rsx_zcmp4(float r, float4 d, int f) {\n"
            "    return float4(rsx_zcmp(r, d.x, f), rsx_zcmp(r, d.y, f), rsx_zcmp(r, d.z, f), rsx_zcmp(r, d.w, f));\n"
            "}\n"
            "float4 rsx_shadow(Texture2D t, SamplerState s, float3 c, int f) {\n"
            "    float2 sz; t.GetDimensions(sz.x, sz.y);\n"
            "    float2 p = c.xy * sz - 0.5; float2 fr = frac(p); float2 b = floor(p);\n"
            "    float r = saturate(c.z);\n"
            "    float4 q00 = rsx_zcmp4(r, t.GatherRed(s, b / sz), f);\n"
            "    float4 q10 = rsx_zcmp4(r, t.GatherRed(s, (b + float2(2.0, 0.0)) / sz), f);\n"
            "    float4 q01 = rsx_zcmp4(r, t.GatherRed(s, (b + float2(0.0, 2.0)) / sz), f);\n"
            "    float4 q11 = rsx_zcmp4(r, t.GatherRed(s, (b + 2.0) / sz), f);\n"
            "    float4 wx = float4(1.0 - fr.x, 1.0, 1.0, fr.x), wy = float4(1.0 - fr.y, 1.0, 1.0, fr.y);\n"
            "    float4 rows = float4(dot(wx, float4(q00.w, q00.z, q10.w, q10.z)),\n"
            "                         dot(wx, float4(q00.x, q00.y, q10.x, q10.y)),\n"
            "                         dot(wx, float4(q01.w, q01.z, q11.w, q11.z)),\n"
            "                         dot(wx, float4(q01.x, q01.y, q11.x, q11.y)));\n"
            "    return (float4)(dot(wy, rows) / 9.0);\n"
            "}\n");
    if (rsx_fp_shadow_mask && !s_smooth) {
        out_puts(&p,
            "float4 rsx_shadow(Texture2D t, SamplerState s, float3 c, int f) {\n"
            "    float2 sz; t.GetDimensions(sz.x, sz.y);\n"
            "    float2 p = c.xy * sz - 0.5; float2 fr = frac(p);\n"
            "    float4 d = t.GatherRed(s, (floor(p) + 1.0) / sz);\n");
        if (getenv("RSX_FP_SHADOW_DEBUG"))
            out_puts(&p,
            "    rsx_dbg = rsx_dbg_mode == 2 ? d : rsx_dbg_mode == 3 ? float4(c.z, c.z / 4.0, c.z / 16.0, c.z / 64.0) :\n"
            "              rsx_dbg_mode == 4 ? float4(t.Sample(s, c.xy).r, sz.x / 1024.0, sz.y / 1024.0, 1.0) :\n"
            "              float4(c.x * 0.015625 + 0.5, c.y * 0.015625 + 0.5, c.z, d.x);\n");
        out_puts(&p,
            "    float r = saturate(c.z);\n"
            "    float a = rsx_zcmp(r, d.w, f), b = rsx_zcmp(r, d.z, f);\n"
            "    float e = rsx_zcmp(r, d.x, f), g = rsx_zcmp(r, d.y, f);\n");
        if (getenv("RSX_FP_SHADOW_DEBUG"))
            out_puts(&p,
            "    if (rsx_dbg_mode == 5) rsx_dbg = float4(a, b, e, g);\n"
            "    if (rsx_dbg_mode == 6) rsx_dbg = float4(r, d.x, fr.x, fr.y);\n");
        out_puts(&p,
            "    return (float4)lerp(lerp(a, b, fr.x), lerp(e, g, fr.x), fr.y);\n"
            "}\n");
    }
    if (buffered) {
        char constants_decl[256];
        snprintf(
            constants_decl, sizeof(constants_decl),
            "cbuffer PSConstants : register(b1) {\n"
            "    float4 fp_constants[%u];\n"
            "    float4 fp_alpha;\n"
            "%s"
            "};\n",
            buffered_constant_count ? buffered_constant_count : 1u,
            s_fp_dyn ? "    float4 fp_texp[16];\n" : "");
        out_puts(&p, constants_decl);
    }
    if (s_fp_dyn)
        out_puts(&p,
            "float4 rsx_texmode(float4 c, uint u) {\n"
            "    float4 tp = fp_texp[u];\n"
            "    if (tp.z >= 16.0) {\n"                     /* RGBA8 bytes read as Y16_X16 */
            "        uint q = (uint)tp.z;\n"
            "        float4 b = round(saturate(c) * 255.0);\n"
            "        float k[6] = { (b.a * 256.0 + b.r) / 65535.0, (b.g * 256.0 + b.b) / 65535.0, 0.0, 1.0, 0.0, 1.0 };\n"
            "        return float4(k[(q >> 8) & 7u], k[(q >> 11) & 7u], k[(q >> 14) & 7u], k[(q >> 5) & 7u]);\n"
            "    }\n"
            "    if (tp.z != 0.0) {\n"
            "        uint g = (uint)tp.z;\n"
            "        float4 m = float4(g & 1u, (g >> 1) & 1u, (g >> 2) & 1u, (g >> 3) & 1u);\n"
            "        float4 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);\n"
            "        c = lerp(c, lerp(hi, c / 12.92, step(c, 0.04045)), m);\n"
            "    }\n"
            "    if (tp.w != 0.0) c = c * 2.0 - 1.0;\n"
            "    return c;\n"
            "}\n");
    if (targets == 1 && !depth_export) {
        out_puts(&p, "float4 main(PSInput input) : SV_TARGET {\n");
    } else {
        out_puts(&p, "struct PSOutput {\n");
        for (u32 t = 0; t < targets; t++) {
            char member[64];
            snprintf(member, sizeof(member),
                     "    float4 t%u : SV_TARGET%u;\n", t, t);
            out_puts(&p, member);
        }
        if (depth_export) out_puts(&p, "    float d : SV_Depth;\n");
        out_puts(&p, "};\n"
                     "PSOutput main(PSInput input) {\n");
    }

    if (!o.ok || !p.ok) return -1;
    if (o.len + p.len + 1 > out_size) return -1;
    memmove(out + p.len, out, o.len + 1);
    memcpy(out, preamble, p.len);
    return count;
}

int rsx_fp_decompile_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                        u32 tex_cube_mask, char* out, u32 out_size)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, out, out_size, NULL);
}

int rsx_fp_decompile_buffered_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 1, out, out_size,
        out_constant_count);
}

static float fp_half_to_float(u16 h)
{
    const u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exponent = (h >> 10) & 0x1Fu;
    u32 mantissa = h & 0x03FFu;
    u32 bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x03FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

float rsx_fp_alpha_ref(u32 raw, u32 surface_color_format)
{
    /* NV4097 surface color formats: 14=W16Z16Y16X16,
     * 15=W32Z32Y32X32, 16=X32. */
    if (surface_color_format == 14u)
        return fp_half_to_float((u16)(raw & 0xFFFFu));
    if (surface_color_format == 15u || surface_color_format == 16u) {
        float value;
        memcpy(&value, &raw, sizeof(value));
        return value;
    }
    return (float)(raw & 0xFFu) / 255.0f;
}

/* The comparison the alpha test has to pass. The alpha it tests is colour
 * target 0's, which for a single-target program is the returned value itself
 * and for an MRT one is the returned struct's first member -- the NV40 alpha
 * test has one input however many targets the program feeds. `cmp` is the
 * CELL_GCM function minus 0x200, with NEVER (0) spelled out and ALWAYS (7)
 * already handled by the caller. */
static void fp_alpha_pass_expr(u32 cmp, const char* alpha, char* out, u32 out_size)
{
    static const char* op[8] = { NULL, "<", "==", "<=", ">", "!=", ">=", NULL };
    if (cmp == 0 || cmp >= 7 || !op[cmp])
        snprintf(out, out_size, "false");
    else
        snprintf(out, out_size, "%s.a %s _rsx_alpha_ref", alpha, op[cmp]);
}

/* Where that alpha lives, and what type the value being returned has. */
static int fp_returns_struct(const char* hlsl)
{
    return strstr(hlsl, "struct PSOutput") != NULL;
}

int rsx_fp_apply_alpha_test(char* hlsl, u32 out_size, u32 func, float ref)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp = func >= 0x0200u && func <= 0x0207u ? func - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    const int mrt = fp_returns_struct(hlsl);
    char pass[96];
    fp_alpha_pass_expr(cmp, mrt ? "_rsx_out.t0" : "_rsx_out", pass, sizeof(pass));
    char suffix[512];
    const int n = snprintf(suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = %.9g;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, (double)ref, pass);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

int rsx_fp_apply_alpha_test_buffered(
    char* hlsl, u32 out_size, u32 func)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp =
        func >= 0x0200u && func <= 0x0207u ? func - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    const int mrt = fp_returns_struct(hlsl);
    char pass[96];
    fp_alpha_pass_expr(cmp, mrt ? "_rsx_out.t0" : "_rsx_out", pass, sizeof(pass));
    char suffix[512];
    const int n = snprintf(
        suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = fp_alpha.x;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, pass);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

/* --- kept from this tree when adopting the newer decompiler ---------------
 * rsx_d3d12_backend.c calls both of these: extract_consts feeds the per-draw
 * fp_k[] slots, code_hash keys the PSO cache on the ucode rather than the
 * constants. The live-draw engine does not need them, so they are absent
 * upstream in caner's fork. */

/* Walk a program the same way the decompiler does and copy out its inline
 * constants in the SAME order the decompiler assigns fp_k[] indices. The backend
 * calls this per draw, so re-patched constants take effect without recompiling. */
int rsx_fp_extract_consts(const u8* ucode, u32 max_bytes, float* out, int max_out)
{
    if (!ucode || !out) return 0;
    u32 off = 0; int n = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        int is_branch = (w2 & FP_BRANCH) != 0;
        int has_k = !is_branch &&
            ((((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST));
        if (has_k) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + i * 4));
                    if (n < max_out) memcpy(&out[n * 4 + i], &cw, 4);
                }
                off += 16;
            }
            if (n < max_out) n++;
        }
        if (w0 & FP_END) break;
    }
    { static int dbg = -1;
      if (dbg < 0) { const char* e = getenv("FP_KDBG"); dbg = e ? atoi(e) : 0; }
      if (dbg) { static int m = 0; if ((m < 3 && (m++, 1))) {
        fprintf(stderr, "[FPK] %d consts%c", n, 10);
        for (int _q = 0; _q < n; _q++)
            fprintf(stderr, "   k[%2d] = (%g %g %g %g)%c", _q, out[_q*4+0],
                    out[_q*4+1], out[_q*4+2], out[_q*4+3], 10); } }
    }
    return n;
}

/* FNV-1a over a program's INSTRUCTION words only, skipping the inline constant
 * slots. With constants hoisted into fp_k[] the compiled shader does not depend
 * on their values, so including them in the pipeline key made it change every
 * time the title re-patched a constant -- which is exactly what thrashed the
 * cache. */
u32 rsx_fp_code_hash(const u8* ucode, u32 max_bytes)
{
    u32 h = 2166136261u;
    if (!ucode) return h;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        for (u32 i = 0; i < 16; i++) { h ^= ucode[off + i]; h *= 16777619u; }
        off += 16;
        int is_branch = (w2 & FP_BRANCH) != 0;
        int has_k = !is_branch &&
            ((((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST));
        if (has_k) off += 16;            /* skip the constant, do not hash it */
        if (w0 & FP_END) break;
    }
    return h;
}

/* Scale texel-space coordinates to the 0..1 range HLSL sampling expects.
 *
 * An RSX texture whose format carries RSX_TEX_FMT_UNNORM (0x40) is addressed in
 * TEXELS, not normalised units. ps1_netemu composites the PS1 framebuffer with
 * exactly that: format 0xE2 (A1R5G5B5 | UNNORM | LINEAR) over a 1024x512
 * texture, and a quad whose texture coordinates run 1..638 by 2..474 --
 *
 *     [uvdbg] unit=0 ps1fb 1024x512 pitch=2048 fmt=0xE2
 *     [uvdbg]   v0 pos(-1.0 -1.0) tc(1.0000 2.0000)
 *     [uvdbg]   v2 pos(0.8 0.8)   tc(638.0000 474.0000)
 *
 * The bit was only ever masked OUT of the base format so the decoder could
 * recognise A1R5G5B5, and never honoured for addressing. So the sampler was
 * handed coordinates hundreds of times out of range, which under clamp lands on
 * one edge texel for the whole quad -- a flat, uniform result. That is the
 * black screen: the PS1 renders, the composite pass runs, and it samples one
 * corner texel.
 *
 * D3D12 has no unnormalised addressing mode, so the division has to happen in
 * the shader. Doing it as a post-pass over the generated HLSL rather than
 * inside the emitter keeps it beside rsx_fp_apply_alpha_test, which already
 * works this way, and leaves every non-UNNORM program byte-identical.
 *
 * Cube units are skipped: their coordinate is a float3 direction, and
 * RSX_TEX_FMT_UNNORM has no meaning for one.
 */
int rsx_fp_apply_unnorm_scale(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                              const u32 dim[][2], u32 cube_mask)
{
    if (!hlsl || !unnorm_mask) return 0;
    int patched = 0;

    for (u32 u = 0; u < 16; u++) {
        if (!((unnorm_mask >> u) & 1u)) continue;
        if ((cube_mask >> u) & 1u) continue;
        const u32 w = dim ? dim[u][0] : 1u, h = dim ? dim[u][1] : 1u;
        if (!w || !h) continue;

        /* Both spellings the emitter produces for a 2D unit. */
        char needles[2][40];
        snprintf(needles[0], sizeof needles[0], "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
        snprintf(needles[1], sizeof needles[1], "rsx_tex%u.Sample(rsx_samp[%u],", u, u);

        for (int nidx = 0; nidx < 2; nidx++) {
            const char* needle = needles[nidx];
            const u32 nlen = (u32)strlen(needle);
            for (;;) {
                char* at = strstr(hlsl, needle);
                if (!at) break;
                /* The '(' of Sample(, then its matching ')'. */
                char* open = at + nlen - 1;   /* the ',' ... step back to '(' */
                while (open > at && *open != '(') open--;
                if (*open != '(') break;
                int depth = 0;
                char* p = open;
                for (; *p; p++) {
                    if (*p == '(') depth++;
                    else if (*p == ')') { depth--; if (!depth) break; }
                }
                if (*p != ')') break;

                char scale[80];
                if (dim)
                    snprintf(scale, sizeof scale, " * float2(%.9g, %.9g)",
                             1.0 / (double)w, 1.0 / (double)h);
                else
                    snprintf(scale, sizeof scale, " * fp_texp[%u].xy", u);
                const u32 slen = (u32)strlen(scale);
                const u32 used = (u32)strlen(hlsl);
                if (used + slen + 1 > hlsl_size) return -1;

                memmove(p + slen, p, used - (u32)(p - hlsl) + 1);
                memcpy(p, scale, slen);
                patched++;

                /* Mark this call site done so the next strstr moves past it:
                 * the inserted text is inside the call, so an unmarked scan
                 * would find the same site forever. */
                memcpy(at, "rsx_TEX", 7);
            }
        }
        /* Restore the marker. */
        for (char* q = hlsl; (q = strstr(q, "rsx_TEX")) != NULL; )
            memcpy(q, "rsx_tex", 7);
    }
    return patched;
}
