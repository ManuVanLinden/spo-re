/* d3d9msl.c -- D3D9 shader bytecode -> MSL. A line-by-line port of tools/shaders/msl.py and the
 * parser of tools/shaders/d3dbc.py; keep the two in step (tests/crosscheck.py compares their output).
 * Token format: MSDN "Shader Binary Format" and the D3DSHADER_* enums of d3d9types.h. */
#include "d3d9msl.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ tables */

typedef struct { const char *name; uint8_t has_dst; uint8_t nsrc_mask; } opinfo;  /* bit k: k sources ok */
#define N1(k) (1u << (k))
static const opinfo OPS[97] = {
    [0] = {"nop", 0, N1(0)}, [1] = {"mov", 1, N1(1)}, [2] = {"add", 1, N1(2)}, [3] = {"sub", 1, N1(2)},
    [4] = {"mad", 1, N1(3)}, [5] = {"mul", 1, N1(2)}, [6] = {"rcp", 1, N1(1)}, [7] = {"rsq", 1, N1(1)},
    [8] = {"dp3", 1, N1(2)}, [9] = {"dp4", 1, N1(2)}, [10] = {"min", 1, N1(2)}, [11] = {"max", 1, N1(2)},
    [12] = {"slt", 1, N1(2)}, [13] = {"sge", 1, N1(2)}, [14] = {"exp", 1, N1(1)}, [15] = {"log", 1, N1(1)},
    [16] = {"lit", 1, N1(1)}, [17] = {"dst", 1, N1(2)}, [18] = {"lrp", 1, N1(3)}, [19] = {"frc", 1, N1(1)},
    [20] = {"m4x4", 1, N1(2)}, [21] = {"m4x3", 1, N1(2)}, [22] = {"m3x4", 1, N1(2)}, [23] = {"m3x3", 1, N1(2)},
    [24] = {"m3x2", 1, N1(2)}, [25] = {"call", 0, N1(1)}, [26] = {"callnz", 0, N1(2)}, [27] = {"loop", 0, N1(2)},
    [28] = {"ret", 0, N1(0)}, [29] = {"endloop", 0, N1(0)}, [30] = {"label", 0, N1(1)}, [31] = {"dcl", 1, N1(0)},
    [32] = {"pow", 1, N1(2)}, [33] = {"crs", 1, N1(2)}, [34] = {"sgn", 1, N1(3)}, [35] = {"abs", 1, N1(1)},
    [36] = {"nrm", 1, N1(1)}, [37] = {"sincos", 1, N1(1) | N1(3)}, [38] = {"rep", 0, N1(1)},
    [39] = {"endrep", 0, N1(0)}, [40] = {"if", 0, N1(1)}, [41] = {"ifc", 0, N1(2)}, [42] = {"else", 0, N1(0)},
    [43] = {"endif", 0, N1(0)}, [44] = {"break", 0, N1(0)}, [45] = {"breakc", 0, N1(2)}, [46] = {"mova", 1, N1(1)},
    [47] = {"defb", 1, N1(0)}, [48] = {"defi", 1, N1(0)},
    [64] = {"texcoord", 1, N1(0) | N1(1)}, [65] = {"texkill", 1, N1(0)}, [66] = {"texld", 1, N1(0) | N1(1) | N1(2)},
    [67] = {"texbem", 1, N1(1)}, [68] = {"texbeml", 1, N1(1)}, [69] = {"texreg2ar", 1, N1(1)},
    [70] = {"texreg2gb", 1, N1(1)}, [71] = {"texm3x2pad", 1, N1(1)}, [72] = {"texm3x2tex", 1, N1(1)},
    [73] = {"texm3x3pad", 1, N1(1)}, [74] = {"texm3x3tex", 1, N1(1)}, [76] = {"texm3x3spec", 1, N1(2)},
    [77] = {"texm3x3vspec", 1, N1(1)}, [78] = {"expp", 1, N1(1)}, [79] = {"logp", 1, N1(1)}, [80] = {"cnd", 1, N1(3)},
    [81] = {"def", 1, N1(0)}, [82] = {"texreg2rgb", 1, N1(1)}, [83] = {"texdp3tex", 1, N1(1)},
    [84] = {"texm3x2depth", 1, N1(1)}, [85] = {"texdp3", 1, N1(1)}, [86] = {"texm3x3", 1, N1(1)},
    [87] = {"texdepth", 1, N1(0)}, [88] = {"cmp", 1, N1(3)}, [89] = {"bem", 1, N1(2)}, [90] = {"dp2add", 1, N1(3)},
    [91] = {"dsx", 1, N1(1)}, [92] = {"dsy", 1, N1(1)}, [93] = {"texldd", 1, N1(4)}, [94] = {"setp", 1, N1(2)},
    [95] = {"texldl", 1, N1(2)}, [96] = {"breakp", 0, N1(1)},
};
#define OP_PHASE 0xFFFDu
#define OP_COMMENT 0xFFFEu
#define OP_END 0xFFFFu

static const char *USAGE[14] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord",
                                "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample"};
static const char *CMPOP[8] = {NULL, ">", "==", ">=", "<", "!=", "<=", NULL};
static const char SW[] = "xyzw";

static const char *textype_name(unsigned t) {
    switch (t) {
    case 2: return "2d";
    case 3: return "cube";
    case 4: return "volume";
    default: return "unk";
    }
}

/* ------------------------------------------------------------------------------------------------ model */

typedef struct { uint32_t tok, rel; int has_rel; } P;
static inline int p_rt(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static inline int p_num(uint32_t t) { return (int)(t & 0x7FF); }
static inline int p_relative(uint32_t t) { return (t & 0x2000) != 0; }
static inline int p_mask(uint32_t t) { return (int)((t >> 16) & 0xF); }
static inline int p_dmod(uint32_t t) { return (int)((t >> 20) & 0xF); }
static inline int p_shift(uint32_t t) { return (int)((t >> 24) & 0xF); }
static inline int p_swz(uint32_t t) { return (int)((t >> 16) & 0xFF); }
static inline int p_smod(uint32_t t) { return (int)((t >> 24) & 0xF); }

typedef struct {
    uint32_t op, tok;
    int has_dst, has_pred, nsrc, nextra;
    P dst, pred, src[4];
    uint32_t extra[4];
} Ins;

typedef struct {
    int rt, n;
    char reg[12];
    const char *usage;   /* NULL: none */
    int index, attribute, centroid, flat;
} IO;

typedef struct {
    jmp_buf jb;
    int rc;
    char *err;
    /* shader */
    int vs, sm3, major, minor;
    char profile[12];
    Ins *ins;
    int nins;
    const char *entry;
    uint32_t opts;
    /* state */
    uint8_t def_f_has[2048], def_i_has[2048], def_b_has[2048];
    uint32_t def_f[2048][4], def_i[2048][4], def_b[2048];
    IO inputs[D3D9MSL_MAX_IO];
    int ninputs;
    IO outputs[D3D9MSL_MAX_IO];
    int noutputs;
    uint8_t samp_has[32];
    uint8_t samp_type[32];
    uint8_t temps[2048];
    uint8_t c_used[2048], i_used[2048], b_used[2048];
    int max_c, rel_c, loop_depth, ind;
    int use_vpos, use_vface, use_texkill;
    /* body lines and string arena */
    char **lines;
    int nlines, caplines;
    void **arena;
    int narena, caparena;
} E;

static void fail(E *e, int rc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->err, 256, fmt, ap);
    va_end(ap);
    e->rc = rc;
    longjmp(e->jb, 1);
}

static void *track(E *e, void *p) {
    if (!p) fail(e, D3D9MSL_ENOMEM, "out of memory");
    if (e->narena == e->caparena) {
        int nc = e->caparena ? e->caparena * 2 : 256;
        void **na = realloc(e->arena, (size_t)nc * sizeof *na);
        if (!na) { free(p); fail(e, D3D9MSL_ENOMEM, "out of memory"); }
        e->arena = na;
        e->caparena = nc;
    }
    e->arena[e->narena++] = p;
    return p;
}

static char *S(E *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static char *S(E *e, const char *fmt, ...) {
    va_list ap;
    char *p = NULL;
    va_start(ap, fmt);
    int n = vasprintf(&p, fmt, ap);
    va_end(ap);
    if (n < 0) p = NULL;
    return track(e, p);
}

static void W(E *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void W(E *e, const char *fmt, ...) {
    va_list ap;
    char *p = NULL;
    va_start(ap, fmt);
    int n = vasprintf(&p, fmt, ap);
    va_end(ap);
    if (n < 0) p = NULL;
    track(e, p);
    char *line = S(e, "%*s%s", e->ind * 4, "", p);
    if (e->nlines == e->caplines) {
        int nc = e->caplines ? e->caplines * 2 : 128;
        char **nl = realloc(e->lines, (size_t)nc * sizeof *nl);
        if (!nl) fail(e, D3D9MSL_ENOMEM, "out of memory");
        e->lines = nl;
        e->caplines = nc;
    }
    e->lines[e->nlines++] = line;
}

/* shortest %g text that reads back as the same float32; msl.py's _f32 uses the same rule */
static void f32s(char *out, size_t cap, uint32_t bits) {
    float v;
    memcpy(&v, &bits, 4);
    if (v != v) { snprintf(out, cap, "NAN"); return; }
    if (isinf(v)) { snprintf(out, cap, v > 0 ? "INFINITY" : "-INFINITY"); return; }
    for (int p = 1; p < 10; p++) {
        snprintf(out, cap, "%.*g", p, (double)v);
        if (strtof(out, NULL) == v) break;
    }
    if (!strchr(out, 'e') && !strchr(out, '.')) strncat(out, ".0", cap - strlen(out) - 1);
    strncat(out, "f", cap - strlen(out) - 1);
}

static char *f4(E *e, const uint32_t *v) {
    char a[4][40];
    for (int k = 0; k < 4; k++) f32s(a[k], sizeof a[k], v[k]);
    return S(e, "%s, %s, %s, %s", a[0], a[1], a[2], a[3]);
}

/* ------------------------------------------------------------------------------------------------ parse */

static int parse(E *e, const uint32_t *t, size_t n, char *err) {
    if (!n) { snprintf(err, 256, "empty"); return D3D9MSL_EPARSE; }
    uint32_t v = t[0];
    int kind = (v >> 16) == 0xFFFE ? 0 : (v >> 16) == 0xFFFF ? 1 : -1;
    e->major = (v >> 8) & 0xFF;
    e->minor = v & 0xFF;
    if (kind < 0 || e->major < 1 || e->major > 3) {
        snprintf(err, 256, "bad version token %#x", v);
        return D3D9MSL_EPARSE;
    }
    e->vs = kind == 0;
    if (e->major < 2) {
        snprintf(err, 256, "shader model 1.x not supported by the prototype");
        return D3D9MSL_EUNSUPPORTED;
    }
    {
        char mn[8];
        if (e->minor == 0xFF) snprintf(mn, sizeof mn, "sw");
        else if (!e->vs && e->major == 2 && e->minor == 1) snprintf(mn, sizeof mn, "x");
        else snprintf(mn, sizeof mn, "%d", e->minor);
        snprintf(e->profile, sizeof e->profile, "%s_%d_%s", e->vs ? "vs" : "ps", e->major, mn);
    }
    e->ins = calloc(n, sizeof(Ins));
    if (!e->ins) { snprintf(err, 256, "out of memory"); return D3D9MSL_ENOMEM; }
    size_t i = 1;
    for (;;) {
        if (i >= n) { snprintf(err, 256, "no end token"); return D3D9MSL_EPARSE; }
        uint32_t tk = t[i], op = tk & 0xFFFF;
        Ins *in = &e->ins[e->nins];
        memset(in, 0, sizeof *in);
        in->op = op;
        in->tok = tk;
        if (op == OP_END) {
            if (tk != 0x0000FFFFu) { snprintf(err, 256, "bad end token %#x", tk); return D3D9MSL_EPARSE; }
            e->nins++;
            i++;
            break;
        }
        if (op == OP_COMMENT) {
            size_t ln = (tk >> 16) & 0x7FFF;
            if ((tk & 0x80000000u) || i + 1 + ln > n) { snprintf(err, 256, "bad comment at %zu", i); return D3D9MSL_EPARSE; }
            e->nins++;
            i += 1 + ln;
            continue;
        }
        if (!(op < 97 && OPS[op].name) && op != OP_PHASE) {
            snprintf(err, 256, "unknown opcode %#x at token %zu", op, i);
            return D3D9MSL_EPARSE;
        }
        if (tk & 0x80000000u) { snprintf(err, 256, "bit 31 set in opcode token at %zu", i); return D3D9MSL_EPARSE; }
        size_t length = (tk >> 24) & 0xF, j = i + 1;
        const char *name = op == OP_PHASE ? "phase" : OPS[op].name;
#define TAKE(dst) do { if (j >= n) { snprintf(err, 256, "truncated"); return D3D9MSL_EPARSE; } (dst) = t[j++]; } while (0)
#define PARAM(pp, is_dst) do { uint32_t v_; TAKE(v_); \
            if (!(v_ & 0x80000000u)) { snprintf(err, 256, "param token without bit 31 at %zu (%s)", j - 1, name); return D3D9MSL_EPARSE; } \
            (pp).tok = v_; (pp).has_rel = 0; \
            if (p_relative(v_) && (!(is_dst) || (e->vs && e->major == 3))) { TAKE((pp).rel); (pp).has_rel = 1; } } while (0)
        if (op == 31) {
            TAKE(in->extra[0]);
            in->nextra = 1;
            PARAM(in->dst, 1);
            in->has_dst = 1;
        } else if (op == 81 || op == 48) {
            PARAM(in->dst, 1);
            in->has_dst = 1;
            for (int k = 0; k < 4; k++) TAKE(in->extra[k]);
            in->nextra = 4;
        } else if (op == 47) {
            PARAM(in->dst, 1);
            in->has_dst = 1;
            TAKE(in->extra[0]);
            in->nextra = 1;
        } else if (op == OP_PHASE) {
        } else {
            if (OPS[op].has_dst) { PARAM(in->dst, 1); in->has_dst = 1; }
            if (tk & 0x10000000u) { PARAM(in->pred, 0); in->has_pred = 1; }
            size_t end = i + 1 + length;
            while (j < end) {
                if (in->nsrc == 4) { snprintf(err, 256, "%s: too many sources at token %zu", name, i); return D3D9MSL_EPARSE; }
                PARAM(in->src[in->nsrc], 0);
                in->nsrc++;
            }
            if (!(OPS[op].nsrc_mask >> in->nsrc & 1)) {
                snprintf(err, 256, "%s: %d sources at token %zu", name, in->nsrc, i);
                return D3D9MSL_EPARSE;
            }
        }
#undef PARAM
#undef TAKE
        if (j - i - 1 != length) {
            snprintf(err, 256, "%s: length field %zu but %zu tokens at %zu", name, length, j - i - 1, i);
            return D3D9MSL_EPARSE;
        }
        e->nins++;
        i = j;
    }
    if (i != n) { snprintf(err, 256, "%zu trailing tokens after end", n - i); return D3D9MSL_EPARSE; }
    return 0;
}

size_t d3d9msl_length(const uint32_t *t, size_t max) {
    if (!t || !max) return 0;
    size_t i = 1;
    while (i < max) {
        uint32_t tk = t[i];
        if (tk == 0x0000FFFFu) return i + 1;
        if ((tk & 0xFFFF) == OP_COMMENT) i += 1 + ((tk >> 16) & 0x7FFF);
        else i++;
    }
    return 0;
}

uint64_t d3d9msl_hash(const uint32_t *t, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t *b = (const uint8_t *)t;
    for (size_t i = 0; i < n * 4; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* ------------------------------------------------------------------------------------------------ helpers */

static IO *io_find(IO *list, int cnt, int rt, int n) {
    for (int k = 0; k < cnt; k++)
        if (list[k].rt == rt && list[k].n == n) return &list[k];
    return NULL;
}
/* setdefault (overwrite = 0) or assignment (overwrite = 1); returns the entry */
static IO *io_set(E *e, IO *list, int *cnt, int rt, int n, int overwrite, const char *reg, const char *usage,
                  int index) {
    IO *o = io_find(list, *cnt, rt, n);
    if (o && !overwrite) return o;
    if (!o) {
        if (*cnt == D3D9MSL_MAX_IO) fail(e, D3D9MSL_EUNSUPPORTED, "more than %d inputs or outputs", D3D9MSL_MAX_IO);
        o = &list[(*cnt)++];
    }
    memset(o, 0, sizeof *o);
    o->rt = rt;
    o->n = n;
    snprintf(o->reg, sizeof o->reg, "%s", reg);
    o->usage = usage;
    o->index = index;
    return o;
}
static IO *out_set(E *e, int rt, int n, int overwrite, const char *reg, const char *usage, int index) {
    return io_set(e, e->outputs, &e->noutputs, rt, n, overwrite, reg, usage, index);
}

static char *reladdr(E *e, const P *p) {
    if (!p->has_rel) fail(e, D3D9MSL_EUNSUPPORTED, "relative address register");
    uint32_t r = p->rel;
    char comp = SW[p_swz(r) & 3];
    if (p_rt(r) == 15) return S(e, "aL");
    if (p_rt(r) == 3) return S(e, "a0.%c", comp);
    fail(e, D3D9MSL_EUNSUPPORTED, "relative address register");
    return NULL;
}

/* msl.py _Emit.reg */
static char *reg(E *e, const P *p) {
    int rt = p_rt(p->tok), n = p_num(p->tok);
    switch (rt) {
    case 0:
        e->temps[n] = 1;
        return S(e, "r%d", n);
    case 1:
        if (e->vs || e->sm3) return S(e, "in.v%d", n);
        return S(e, "in.d%d", n);
    case 2:
        if (p_relative(p->tok)) {
            e->rel_c = 1;
            return S(e, "sg_ldc_%s(c, %s + %d)", e->entry, reladdr(e, p), n);
        }
        if (n > e->max_c) e->max_c = n;
        if (e->def_f_has[n]) return S(e, "kc%d", n);
        e->c_used[n] = 1;
        return S(e, "c[%d]", n);
    case 3:
        if (e->vs) return S(e, "a0");
        return S(e, "in.t%d", n);
    case 4: {
        static const char *nm[3] = {"oPos", "oFog", "oPts"};
        if (n > 2) fail(e, D3D9MSL_EUNSUPPORTED, "rasteriser output %d", n);
        out_set(e, 4, n, 0, nm[n], NULL, 0);
        return S(e, "%s", nm[n]);
    }
    case 5: {
        char *r = S(e, "oD%d", n);
        out_set(e, 5, n, 0, r, "color", n);
        return r;
    }
    case 6:
        if (e->sm3) {
            if (!io_find(e->outputs, e->noutputs, 6, n)) fail(e, D3D9MSL_EUNSUPPORTED, "o%d written without dcl", n);
            return S(e, "o%d", n);
        } else {
            char *r = S(e, "oT%d", n);
            out_set(e, 6, n, 0, r, "texcoord", n);
            return r;
        }
    case 7:
        if (e->def_i_has[n]) return S(e, "ki%d", n);
        e->i_used[n] = 1;
        return S(e, "ci[%d]", n);
    case 8: {
        char *r = S(e, "oC%d", n);
        out_set(e, 8, n, 0, r, NULL, 0);
        return r;
    }
    case 9:
        out_set(e, 9, 0, 0, "oDepth", NULL, 0);
        return S(e, "oDepth");
    case 14:
        if (e->def_b_has[n]) return S(e, "kb%d", n);
        e->b_used[n] = 1;
        return S(e, "(cb[%d] != 0u)", n);
    case 15:
        return S(e, "aL");
    case 17:
        if (n == 0) {
            e->use_vpos = 1;
            return S(e, "float4(sg_pos.xy - 0.5f, 0.0f, 0.0f)");
        }
        e->use_vface = 1;
        return S(e, "float4(sg_front ? 1.0f : -1.0f)");
    }
    fail(e, D3D9MSL_EUNSUPPORTED, "register type %d", rt);
    return NULL;
}

/* msl.py _Emit.src: comps NULL or a string like "xyz" */
static char *src(E *e, const P *p, const char *comps) {
    char *base = reg(e, p);
    char sw[5], s2[5];
    for (int k = 0; k < 4; k++) sw[k] = SW[(p_swz(p->tok) >> (2 * k)) & 3];
    sw[4] = 0;
    int rt = p_rt(p->tok);
    if (rt == 7 || rt == 14 || rt == 15) fail(e, D3D9MSL_EUNSUPPORTED, "integer/bool register as float source");
    if (comps) {
        size_t k = 0;
        for (; comps[k]; k++) s2[k] = sw[strchr(SW, comps[k]) - SW];
        s2[k] = 0;
        memcpy(sw, s2, sizeof sw);
    }
    char *ex = !strcmp(sw, "xyzw") ? base : S(e, "%s.%s", base, sw);
    switch (p_smod(p->tok)) {
    case 0: return ex;
    case 1: return S(e, "(-%s)", ex);
    case 11: return S(e, "abs(%s)", ex);
    case 12: return S(e, "(-abs(%s))", ex);
    case 2: return S(e, "(%s - 0.5f)", ex);
    case 3: return S(e, "(0.5f - %s)", ex);
    case 4: return S(e, "(%s * 2.0f - 1.0f)", ex);
    case 5: return S(e, "(1.0f - %s * 2.0f)", ex);
    case 6: return S(e, "(1.0f - %s)", ex);
    case 7: return S(e, "(%s * 2.0f)", ex);
    case 8: return S(e, "(-%s * 2.0f)", ex);
    }
    fail(e, D3D9MSL_EUNSUPPORTED, "source modifier %d", p_smod(p->tok));
    return NULL;
}
static char *scalar(E *e, const P *p) { return src(e, p, "x"); }

static void maskstr(int m, char *out) {
    int k = 0;
    for (int c = 0; c < 4; c++)
        if (m >> c & 1) out[k++] = SW[c];
    out[k] = 0;
}

/* msl.py _Emit.store */
static void store(E *e, const Ins *in, const char *expr, int is_scalar) {
    const P *d = &in->dst;
    if (p_shift(d->tok)) fail(e, D3D9MSL_EUNSUPPORTED, "result shift");
    if (p_relative(d->tok)) fail(e, D3D9MSL_EUNSUPPORTED, "relative-addressed destination");
    char *lhs = reg(e, d);
    char mask[5];
    maskstr(p_mask(d->tok), mask);
    int sat = p_dmod(d->tok) & 1, ml = (int)strlen(mask);
    char *val;
    if (is_scalar) {
        char *ex = sat ? S(e, "saturate(%s)", expr) : (char *)expr;
        val = ml == 1 ? ex : S(e, "float%d(%s)", ml, ex);
    } else {
        char *ex = S(e, "(%s)", expr);
        if (sat) ex = S(e, "saturate%s", ex);
        val = !strcmp(mask, "xyzw") ? ex : S(e, "%s.%s", ex, mask);
    }
    int scalar_out = !strcmp(lhs, "oFog") || !strcmp(lhs, "oPts") || !strcmp(lhs, "oDepth");
    for (int k = 0; k < e->noutputs && !scalar_out; k++)
        if (!strcmp(e->outputs[k].reg, lhs) && e->outputs[k].usage && !strcmp(e->outputs[k].usage, "psize"))
            scalar_out = 1;
    if (scalar_out) {
        char *expr1;
        if (!is_scalar) {
            if (!ml) fail(e, D3D9MSL_EUNSUPPORTED, "empty write mask on a scalar output");
            char *ex = sat ? S(e, "saturate(%s)", expr) : S(e, "(%s)", expr);
            expr1 = S(e, "(%s).%c", ex, mask[0]);
        } else {
            expr1 = sat ? S(e, "saturate(%s)", expr) : (char *)expr;
        }
        W(e, "%s = %s;", lhs, expr1);
        return;
    }
    if (p_rt(d->tok) == 3 && e->vs) {
        if (ml > 1) val = S(e, "int%d(rint(%s))", ml, val);
        else val = S(e, "int(rint(%s))", val);
    }
    if (!strcmp(mask, "xyzw")) W(e, "%s = %s;", lhs, val);
    else W(e, "%s.%s = %s;", lhs, mask, val);
}

/* ------------------------------------------------------------------------------------------------ scan */

static void scan(E *e) {
    for (int k = 0; k < e->nins; k++) {
        const Ins *in = &e->ins[k];
        int n = p_num(in->dst.tok);
        if (in->op == 81) { e->def_f_has[n] = 1; memcpy(e->def_f[n], in->extra, 16); }
        else if (in->op == 48) { e->def_i_has[n] = 1; memcpy(e->def_i[n], in->extra, 16); }
        else if (in->op == 47) { e->def_b_has[n] = 1; e->def_b[n] = in->extra[0]; }
        else if (in->op == 31) {
            uint32_t u = in->extra[0];
            int rt = p_rt(in->dst.tok);
            if (rt == 10) {
                if (n >= 32) fail(e, D3D9MSL_EUNSUPPORTED, "sampler s%d", n);
                e->samp_has[n] = 1;
                unsigned tt = (u >> 27) & 0xF;
                e->samp_type[n] = (uint8_t)(tt == 2 || tt == 3 || tt == 4 ? tt : 0);
                continue;
            }
            unsigned usage = u & 0x1F, idx = (u >> 16) & 0xF;
            if (usage > 13) fail(e, D3D9MSL_EUNSUPPORTED, "dcl usage %u", usage);
            int cen = (p_dmod(in->dst.tok) & 4) != 0;
            int flat = (e->opts & D3D9MSL_OPT_FLAT) && !e->vs &&
                       (e->sm3 ? !strcmp(USAGE[usage], "color") : rt == 1);
            IO *io;
            char r[12];
            if (e->vs && rt == 1) {
                snprintf(r, sizeof r, "v%d", n);
                io = io_set(e, e->inputs, &e->ninputs, 1, n, 1, r, USAGE[usage], (int)idx);
                io->attribute = n;
            } else if (e->vs && rt == 6) {
                snprintf(r, sizeof r, "o%d", n);
                out_set(e, 6, n, 1, r, USAGE[usage], (int)idx);
            } else if (!e->vs && e->sm3 && rt == 1) {
                snprintf(r, sizeof r, "v%d", n);
                io = io_set(e, e->inputs, &e->ninputs, 1, n, 1, r, USAGE[usage], (int)idx);
                io->centroid = cen;
                io->flat = flat;
            } else if (!e->vs && !e->sm3 && rt == 3) {
                snprintf(r, sizeof r, "t%d", n);
                io = io_set(e, e->inputs, &e->ninputs, 3, n, 1, r, "texcoord", n);
                io->centroid = cen;
                io->flat = flat;
            } else if (!e->vs && !e->sm3 && rt == 1) {
                snprintf(r, sizeof r, "d%d", n);
                io = io_set(e, e->inputs, &e->ninputs, 1, n, 1, r, "color", n);
                io->centroid = cen;
                io->flat = flat;
            } else if (rt == 17) {
            } else {
                fail(e, D3D9MSL_EUNSUPPORTED, "dcl of register type %d", rt);
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ body */

static char *sample(E *e, const Ins *in, const P *cp, const P *sp, char mode) {
    int n = p_num(sp->tok);
    unsigned ty = n < 32 && e->samp_has[n] ? e->samp_type[n] : 2;
    const char *fn = ty == 2 ? "sg_tex2d" : ty == 3 ? "sg_texcube" : ty == 4 ? "sg_tex3d" : NULL;
    if (!fn) fail(e, D3D9MSL_EUNSUPPORTED, "sampler type %s", textype_name(ty));
    char *c4 = src(e, cp, NULL);
    char *ex;
    if (mode == 'p') ex = S(e, "%s_b(t%d, s%d, sg_proj(%s), 0.0f, aux.samp[%d])", fn, n, n, c4, n);
    else if (mode == 'b') ex = S(e, "%s_b(t%d, s%d, %s, (%s).w, aux.samp[%d])", fn, n, n, c4, c4, n);
    else if (mode == 'l') ex = S(e, "%s_l(t%d, s%d, %s, (%s).w, aux.samp[%d])", fn, n, n, c4, c4, n);
    else if (mode == 'd') {
        char *dx = src(e, &in->src[2], NULL);
        char *dy = src(e, &in->src[3], NULL);
        ex = S(e, "%s_d(t%d, s%d, %s, %s, %s, aux.samp[%d])", fn, n, n, c4, dx, dy, n);
    } else if (e->vs) ex = S(e, "%s_l(t%d, s%d, %s, 0.0f, aux.samp[%d])", fn, n, n, c4, n);
    else ex = S(e, "%s_b(t%d, s%d, %s, 0.0f, aux.samp[%d])", fn, n, n, c4, n);
    char sw[5];
    for (int k = 0; k < 4; k++) sw[k] = SW[(p_swz(sp->tok) >> (2 * k)) & 3];
    sw[4] = 0;
    if (strcmp(sw, "xyzw")) ex = S(e, "(%s).%s", ex, sw);
    return ex;
}

static const char *cmpop(E *e, const Ins *in) {
    const char *o = CMPOP[(in->tok >> 16) & 7];
    if (!o) fail(e, D3D9MSL_EUNSUPPORTED, "comparison %u", (in->tok >> 16) & 7);
    return o;
}

static void body(E *e) {
    for (int k = 0; k < e->nins; k++) {
        const Ins *in = &e->ins[k];
        uint32_t op = in->op;
        if (op == OP_COMMENT || op == 31 || op == 81 || op == 48 || op == 47 || op == 0) continue;
        if (op == OP_END) break;
        if (in->tok & 0x10000000u) fail(e, D3D9MSL_EUNSUPPORTED, "predication");
        if (op == OP_PHASE) continue;
        const char *nm = OPS[op].name;
        const P *s = in->src;
#define IS(x) (!strcmp(nm, x))
        if (IS("mov") || IS("mova")) store(e, in, src(e, &s[0], NULL), 0);
        else if (IS("add") || IS("sub") || IS("mul")) {
            const char *o = IS("add") ? "+" : IS("sub") ? "-" : "*";
            char *a = src(e, &s[0], NULL);
            store(e, in, S(e, "%s %s %s", a, o, src(e, &s[1], NULL)), 0);
        } else if (IS("mad")) {
            char *a = src(e, &s[0], NULL), *b = src(e, &s[1], NULL);
            store(e, in, S(e, "%s * %s + %s", a, b, src(e, &s[2], NULL)), 0);
        } else if (IS("rcp")) store(e, in, S(e, "1.0f / %s", scalar(e, &s[0])), 1);
        else if (IS("rsq")) store(e, in, S(e, "rsqrt(abs(%s))", scalar(e, &s[0])), 1);
        else if (IS("exp") || IS("expp")) store(e, in, S(e, "exp2(%s)", scalar(e, &s[0])), 1);
        else if (IS("log") || IS("logp")) store(e, in, S(e, "log2(abs(%s))", scalar(e, &s[0])), 1);
        else if (IS("pow")) {
            char *a = scalar(e, &s[0]);
            store(e, in, S(e, "pow(abs(%s), %s)", a, scalar(e, &s[1])), 1);
        } else if (IS("frc")) store(e, in, S(e, "fract(%s)", src(e, &s[0], NULL)), 0);
        else if (IS("min") || IS("max")) {
            char *a = src(e, &s[0], NULL);
            store(e, in, S(e, "%s(%s, %s)", nm, a, src(e, &s[1], NULL)), 0);
        } else if (IS("slt") || IS("sge")) {
            char *a = src(e, &s[0], NULL);
            store(e, in, S(e, "select(float4(0.0f), float4(1.0f), %s %s %s)", a, IS("slt") ? "<" : ">=",
                           src(e, &s[1], NULL)), 0);
        } else if (IS("dp3") || IS("dp4")) {
            const char *cs = IS("dp3") ? "xyz" : "xyzw";
            char *a = src(e, &s[0], cs);
            store(e, in, S(e, "dot(%s, %s)", a, src(e, &s[1], cs)), 1);
        } else if (IS("dp2add")) {
            char *a = src(e, &s[0], "xy"), *b = src(e, &s[1], "xy");
            store(e, in, S(e, "dot(%s, %s) + %s", a, b, scalar(e, &s[2])), 1);
        } else if (IS("lrp")) {
            char *c2 = src(e, &s[2], NULL), *c1 = src(e, &s[1], NULL);
            store(e, in, S(e, "mix(%s, %s, %s)", c2, c1, src(e, &s[0], NULL)), 0);
        } else if (IS("cmp")) {
            char *c2 = src(e, &s[2], NULL), *c1 = src(e, &s[1], NULL);
            store(e, in, S(e, "select(%s, %s, %s >= 0.0f)", c2, c1, src(e, &s[0], NULL)), 0);
        } else if (IS("abs")) store(e, in, S(e, "abs(%s)", src(e, &s[0], NULL)), 0);
        else if (IS("sgn")) store(e, in, S(e, "sign(%s)", src(e, &s[0], NULL)), 0);
        else if (IS("nrm")) {
            char *a = src(e, &s[0], NULL);
            store(e, in, S(e, "%s * rsqrt(dot((%s).xyz, (%s).xyz))", a, a, a), 0);
        } else if (IS("crs")) {
            char *a = src(e, &s[0], "xyz");
            store(e, in, S(e, "float4(cross(%s, %s), 0.0f)", a, src(e, &s[1], "xyz")), 0);
        } else if (IS("sincos")) {
            char *x = scalar(e, &s[0]);
            store(e, in, S(e, "float4(cos(%s), sin(%s), 0.0f, 0.0f)", x, x), 0);
        } else if (IS("lit")) store(e, in, S(e, "sg_lit(%s)", src(e, &s[0], NULL)), 0);
        else if (IS("dst")) {
            char *a = src(e, &s[0], NULL), *b = src(e, &s[1], NULL);
            char *a2 = src(e, &s[0], NULL), *b2 = src(e, &s[1], NULL);
            store(e, in, S(e, "float4(1.0f, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a2, b2), 0);
        } else if (IS("m4x4") || IS("m4x3") || IS("m3x4") || IS("m3x3") || IS("m3x2")) {
            int kk = nm[1] - '0', rows = nm[3] - '0';
            char cs[5];
            memcpy(cs, SW, 4);
            cs[kk] = 0;
            char *v = src(e, &s[0], cs);
            char *parts[4];
            for (int j = 0; j < 4; j++) {
                if (j < rows) {
                    P q = s[1];
                    q.tok = s[1].tok + (uint32_t)j;
                    parts[j] = S(e, "dot(%s, %s)", v, src(e, &q, cs));
                } else parts[j] = "0.0f";
            }
            store(e, in, S(e, "float4(%s, %s, %s, %s)", parts[0], parts[1], parts[2], parts[3]), 0);
        } else if (IS("texld")) {
            unsigned c = (in->tok >> 16) & 3;
            if (c == 3) fail(e, D3D9MSL_EUNSUPPORTED, "texld control 3");
            if (in->nsrc < 2) fail(e, D3D9MSL_EUNSUPPORTED, "texld without a sampler");
            store(e, in, sample(e, in, &s[0], &s[1], c == 0 ? 0 : c == 1 ? 'p' : 'b'), 0);
        } else if (IS("texldl")) store(e, in, sample(e, in, &s[0], &s[1], 'l'), 0);
        else if (IS("texldd")) store(e, in, sample(e, in, &s[0], &s[1], 'd'), 0);
        else if (IS("texkill")) {
            char mask[5];
            maskstr(p_mask(in->dst.tok), mask);
            e->use_texkill = 1;
            W(e, "if (any((%s).%s < 0.0f)) discard_fragment();", reg(e, &in->dst), mask);
        } else if (IS("dsx") || IS("dsy")) store(e, in, S(e, "%s(%s)", IS("dsx") ? "dfdx" : "dfdy", src(e, &s[0], NULL)), 0);
        else if (IS("if")) {
            W(e, "if (%s) {", reg(e, &s[0]));
            e->ind++;
        } else if (IS("ifc")) {
            char *a = scalar(e, &s[0]);
            const char *o = cmpop(e, in);
            W(e, "if (%s %s %s) {", a, o, scalar(e, &s[1]));
            e->ind++;
        } else if (IS("else")) {
            e->ind--;
            W(e, "} else {");
            e->ind++;
        } else if (IS("endif") || IS("endloop") || IS("endrep")) {
            e->ind--;
            W(e, "}");
            if (!IS("endif")) e->loop_depth--;
        } else if (IS("loop")) {
            e->loop_depth++;
            W(e, "for (int4 _L%d = %s, _n = int4(0); _n.x < _L%d.x; ++_n.x) {", e->loop_depth, reg(e, &s[1]), e->loop_depth);
            e->ind++;
            W(e, "int aL = _L%d.y + _n.x * _L%d.z;", e->loop_depth, e->loop_depth);
        } else if (IS("rep")) {
            e->loop_depth++;
            W(e, "for (int _L%d = 0; _L%d < %s.x; ++_L%d) {", e->loop_depth, e->loop_depth, reg(e, &s[0]), e->loop_depth);
            e->ind++;
        } else if (IS("break")) W(e, "break;");
        else if (IS("breakc")) {
            char *a = scalar(e, &s[0]);
            const char *o = cmpop(e, in);
            W(e, "if (%s %s %s) break;", a, o, scalar(e, &s[1]));
        } else fail(e, D3D9MSL_EUNSUPPORTED, "opcode %s", nm);
#undef IS
        if (e->ind < 1) fail(e, D3D9MSL_EUNSUPPORTED, "unbalanced flow control");
    }
}

/* ------------------------------------------------------------------------------------------------ build */

typedef struct { char *p; size_t n, cap; } SB;
static void sb_put(E *e, SB *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->n + l + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) fail(e, D3D9MSL_ENOMEM, "out of memory");
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

typedef struct { const char *nm, *ty, *attr, *dflt; int kind, rt, n; char sem[20]; } OUTD;

static int cmp_io(const void *a, const void *b) {
    const IO *x = a, *y = b;
    if (x->rt != y->rt) return x->rt - y->rt;
    return x->n - y->n;
}

static const char PRELUDE[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct SgSampler {\n"
    "    float4 border;           // D3DSAMP_BORDERCOLOR as RGBA 0..1, used where an SG_SAMP_BORDER_* bit is set\n"
    "    float mipBias;           // D3DSAMP_MIPMAPLODBIAS (a Metal sampler has no LOD bias)\n"
    "    uint flags;              // SG_SAMP_* below\n"
    "    float _pad1[2];\n"
    "};\n"
    "struct SgAux {\n"
    "    float alphaRef;          // D3DRS_ALPHAREF / 255\n"
    "    float _pad0[3];\n"
    "    float4 fogColor;         // D3DRS_FOGCOLOR\n"
    "    float4 fogParams;        // start, end, density, mode\n"
    "    float4 rtSize;           // width, height, 1/width, 1/height\n"
    "    float4 posFixup;         // VS: added to oPos.xy per unit of oPos.w (half-pixel offset), or zero\n"
    "    SgSampler samp[16];      // PS: sampler s0..s15; VS: vertex texture sampler 0..3\n"
    "};\n"
    "constant uint SG_SAMP_SRGB = 1u;       // D3DSAMP_SRGBTEXTURE: decode sRGB after the fetch\n"
    "constant uint SG_SAMP_BORDER_U = 2u;   // D3DTADDRESS_BORDER on U with a colour the Metal sampler cannot hold\n"
    "constant uint SG_SAMP_BORDER_V = 4u;\n"
    "constant uint SG_SAMP_BORDER_W = 8u;\n"
    "constant uint SG_SAMP_DEPTH_CMP = 16u; // depth texture: return (ref <= depth) like D3D9 shadow-map hardware\n"
    "constant uint sg_alpha_func [[function_constant(0)]];\n"
    "constant bool sg_alpha_func_set = is_function_constant_defined(sg_alpha_func);\n"
    "static float4 sg_fin(float4 r, float4 c, constant SgSampler& st, uint dims) {\n"
    "    uint f = st.flags;\n"
    "    if (f == 0u) return r;\n"
    "    if ((f & SG_SAMP_SRGB) != 0u)\n"
    "        r.rgb = select(pow((r.rgb + 0.055f) / 1.055f, float3(2.4f)), r.rgb / 12.92f, r.rgb <= 0.04045f);\n"
    "    uint b = f & dims;\n"
    "    if (b != 0u) {\n"
    "        bool ox = (b & SG_SAMP_BORDER_U) != 0u && (c.x < 0.0f || c.x > 1.0f);\n"
    "        bool oy = (b & SG_SAMP_BORDER_V) != 0u && (c.y < 0.0f || c.y > 1.0f);\n"
    "        bool oz = (b & SG_SAMP_BORDER_W) != 0u && (c.z < 0.0f || c.z > 1.0f);\n"
    "        if (ox || oy || oz) r = st.border;\n"
    "    }\n"
    "    if ((f & SG_SAMP_DEPTH_CMP) != 0u) r = float4(c.z <= r.x ? 1.0f : 0.0f);\n"
    "    return r;\n"
    "}\n"
    "static float4 sg_proj(float4 c) { return float4(c.xyz / c.w, c.w); }\n"
    "static float4 sg_tex2d_b(texture2d<float> t, sampler s, float4 c, float b, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xy, bias(b + st.mipBias)), c, st, 6u);\n"
    "}\n"
    "static float4 sg_tex2d_l(texture2d<float> t, sampler s, float4 c, float l, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xy, level(l + st.mipBias)), c, st, 6u);\n"
    "}\n"
    "static float4 sg_tex2d_d(texture2d<float> t, sampler s, float4 c, float4 dx, float4 dy, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xy, gradient2d(dx.xy, dy.xy)), c, st, 6u);\n"
    "}\n"
    "static float4 sg_texcube_b(texturecube<float> t, sampler s, float4 c, float b, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xyz, bias(b + st.mipBias)), c, st, 0u);\n"
    "}\n"
    "static float4 sg_texcube_l(texturecube<float> t, sampler s, float4 c, float l, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xyz, level(l + st.mipBias)), c, st, 0u);\n"
    "}\n"
    "static float4 sg_texcube_d(texturecube<float> t, sampler s, float4 c, float4 dx, float4 dy, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xyz, gradientcube(dx.xyz, dy.xyz)), c, st, 0u);\n"
    "}\n"
    "static float4 sg_tex3d_b(texture3d<float> t, sampler s, float4 c, float b, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xyz, bias(b + st.mipBias)), c, st, 14u);\n"
    "}\n"
    "static float4 sg_tex3d_l(texture3d<float> t, sampler s, float4 c, float l, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xyz, level(l + st.mipBias)), c, st, 14u);\n"
    "}\n"
    "static float4 sg_tex3d_d(texture3d<float> t, sampler s, float4 c, float4 dx, float4 dy, constant SgSampler& st) {\n"
    "    return sg_fin(t.sample(s, c.xyz, gradient3d(dx.xyz, dy.xyz)), c, st, 14u);\n"
    "}\n"
    "static bool sg_alpha_test(float a, float ref) {\n"
    "    uint f = sg_alpha_func_set ? sg_alpha_func : 8u;\n"
    "    // D3D9 compares at render-target precision (8 bits for the formats Spore uses)\n"
    "    float qa = rint(saturate(a) * 255.0f), qr = rint(saturate(ref) * 255.0f);\n"
    "    switch (f) {\n"
    "        case 1u: return false;\n"
    "        case 2u: return qa <  qr;\n"
    "        case 3u: return qa == qr;\n"
    "        case 4u: return qa <= qr;\n"
    "        case 5u: return qa >  qr;\n"
    "        case 6u: return qa != qr;\n"
    "        case 7u: return qa >= qr;\n"
    "        default: return true;\n"
    "    }\n"
    "}\n"
    "static float4 sg_lit(float4 s) {\n"
    "    float4 r = float4(1.0f, 0.0f, 0.0f, 1.0f);\n"
    "    float p = clamp(s.w, -127.9961f, 127.9961f);\n"
    "    if (s.x > 0.0f) {\n"
    "        r.y = s.x;\n"
    "        if (s.y > 0.0f) r.z = pow(s.y, p);\n"
    "    }\n"
    "    return r;\n"
    "}\n";

const char *d3d9msl_prelude(void) { return PRELUDE; }

static void build(E *e, SB *b, d3d9msl_reflection *R) {
    scan(e);
    body(e);
    const char *en = e->entry;
    int fog = (e->opts & D3D9MSL_OPT_FOG_VERTEX) != 0, plain = (e->opts & D3D9MSL_OPT_PLAIN) != 0;
    int nconst = e->vs ? 256 : 224;
    if (!(e->opts & D3D9MSL_OPT_NO_PRELUDE)) sb_put(e, b, PRELUDE);
    if (e->rel_c) {
        sb_put(e, b, S(e, "static float4 sg_ldc_%s(constant float4* c, int i) {\n", en));
        for (int n = 0; n < 2048; n++)
            if (e->def_f_has[n]) sb_put(e, b, S(e, "    if (i == %d) return float4(%s);\n", n, f4(e, e->def_f[n])));
        sb_put(e, b, S(e, "    return (i >= 0 && i < %d) ? c[i] : float4(0.0f);\n}\n", nconst));
    }
    sb_put(e, b, S(e, "// %s -> MSL (spore-native d3d9msl %s)\n", e->profile, D3D9MSL_VERSION));
    qsort(e->inputs, (size_t)e->ninputs, sizeof(IO), cmp_io);
    qsort(e->outputs, (size_t)e->noutputs, sizeof(IO), cmp_io);
    /* input struct */
    int has_in = e->ninputs > 0 || (!e->vs && fog);
    if (has_in) {
        sb_put(e, b, S(e, "struct %s_in {\n", en));
        for (int k = 0; k < e->ninputs; k++) {
            IO *d = &e->inputs[k];
            if (e->vs) sb_put(e, b, S(e, "    float4 v%d [[attribute(%d)]];\n", d->n, d->n));
            else
                sb_put(e, b, S(e, "    float4 %s [[user(%s%d)%s%s]];\n", d->reg, d->usage, d->index,
                               d->centroid ? ", centroid_perspective" : "", d->flat ? ", flat" : ""));
        }
        if (!e->vs && fog) sb_put(e, b, "    float sg_fog [[user(fog0)]];\n");
        sb_put(e, b, "};\n");
    }
    /* output struct */
    OUTD outs[D3D9MSL_MAX_IO];
    int nouts = 0;
    for (int k = 0; k < e->noutputs; k++) {
        IO *d = &e->outputs[k];
        OUTD *o = &outs[nouts];
        memset(o, 0, sizeof *o);
        o->rt = d->rt;
        o->n = d->n;
        if (e->vs) {
            if (d->rt == 4) {
                if (d->n == 0) { o->nm = "oPos"; o->ty = "float4"; o->attr = "[[position]]"; o->dflt = "float4(0.0f)"; o->kind = D3D9MSL_OUT_POSITION; }
                else if (d->n == 1) { o->nm = "oFog"; o->ty = "float"; o->attr = "[[user(fog0)]]"; o->dflt = "1.0f"; o->kind = D3D9MSL_OUT_USER; snprintf(o->sem, sizeof o->sem, "fog0"); }
                else { o->nm = "oPts"; o->ty = "float"; o->attr = "[[point_size]]"; o->dflt = "1.0f"; o->kind = D3D9MSL_OUT_POINT_SIZE; }
                nouts++;
                continue;
            }
            const char *u = d->usage;
            int i = d->index;
            if (!u) fail(e, D3D9MSL_EUNSUPPORTED, "vertex output %s without a usage", d->reg);
            if (!strcmp(u, "position") && i == 0) { o->attr = "[[position]]"; o->kind = D3D9MSL_OUT_POSITION; }
            else if (!strcmp(u, "psize")) { o->attr = "[[point_size]]"; o->kind = D3D9MSL_OUT_POINT_SIZE; }
            else {
                o->attr = S(e, "[[user(%s%d)]]", u, i);
                o->kind = D3D9MSL_OUT_USER;
                snprintf(o->sem, sizeof o->sem, "%s%d", u, i);
            }
            o->ty = o->kind == D3D9MSL_OUT_POINT_SIZE ? "float" : "float4";
            if (!strcmp(u, "color")) o->dflt = i == 0 ? "float4(1.0f)" : "float4(0.0f, 0.0f, 0.0f, 1.0f)";
            else if (!strcmp(u, "fog")) o->dflt = "float4(1.0f)";
            else o->dflt = !strcmp(o->ty, "float4") ? "float4(0.0f)" : "1.0f";
            o->nm = d->reg;
            nouts++;
        } else {
            if (d->rt == 8) {
                o->nm = d->reg; o->ty = "float4"; o->attr = S(e, "[[color(%d)]]", d->n); o->dflt = "float4(0.0f)";
                o->kind = D3D9MSL_OUT_COLOR;
                nouts++;
            } else if (d->rt == 9) {
                o->nm = "oDepth"; o->ty = "float"; o->attr = "[[depth(any)]]"; o->dflt = "0.0f"; o->kind = D3D9MSL_OUT_DEPTH;
                nouts++;
            }
        }
    }
    sb_put(e, b, S(e, "struct %s_out {\n", en));
    for (int k = 0; k < nouts; k++) sb_put(e, b, S(e, "    %s %s %s;\n", outs[k].ty, outs[k].nm, outs[k].attr));
    sb_put(e, b, "};\n");
    /* signature */
    if (plain && (!e->vs || memchr(e->samp_has, 1, sizeof e->samp_has)))
        fail(e, D3D9MSL_EUNSUPPORTED, "plain mode is for ALU-only vertex shaders");
#define ATTR(x) (plain ? "" : (x))
    const char *stage = plain ? "static" : e->vs ? "vertex" : "fragment";
    sb_put(e, b, S(e, "%s %s_out %s(", stage, en, en));
    int first = 1;
#define ARG(...) do { if (!first) sb_put(e, b, ",\n        "); first = 0; sb_put(e, b, S(e, __VA_ARGS__)); } while (0)
    if (has_in) {
        if (plain) ARG("thread const %s_in& in", en);
        else ARG("%s_in in [[stage_in]]", en);
    }
    ARG("constant float4* c%s", ATTR(" [[buffer(0)]]"));
    ARG("constant int4* ci%s", ATTR(" [[buffer(1)]]"));
    ARG("constant uint* cb%s", ATTR(" [[buffer(2)]]"));
    ARG("constant SgAux& aux%s", ATTR(" [[buffer(3)]]"));
    for (int n = 0; n < 32; n++) {
        if (!e->samp_has[n]) continue;
        unsigned ty = e->samp_type[n];
        const char *tt = ty == 2 ? "texture2d<float>" : ty == 3 ? "texturecube<float>" : ty == 4 ? "texture3d<float>" : NULL;
        if (!tt) fail(e, D3D9MSL_EUNSUPPORTED, "sampler type unk");
        ARG("%s t%d [[texture(%d)]]", tt, n, n);
        ARG("sampler s%d [[sampler(%d)]]", n, n);
    }
    if (e->use_vpos) ARG("float4 sg_pos%s", ATTR(" [[position]]"));
    if (e->use_vface) ARG("bool sg_front%s", ATTR(" [[front_facing]]"));
#undef ARG
#undef ATTR
    sb_put(e, b, ") {\n");
    for (int n = 0; n < 2048; n++)
        if (e->def_f_has[n]) sb_put(e, b, S(e, "    const float4 kc%d = float4(%s);\n", n, f4(e, e->def_f[n])));
    for (int n = 0; n < 2048; n++)
        if (e->def_i_has[n]) {
            const int32_t *v = (const int32_t *)e->def_i[n];
            sb_put(e, b, S(e, "    const int4 ki%d = int4(%d, %d, %d, %d);\n", n, v[0], v[1], v[2], v[3]));
        }
    for (int n = 0; n < 2048; n++)
        if (e->def_b_has[n]) sb_put(e, b, S(e, "    const bool kb%d = %s;\n", n, e->def_b[n] ? "true" : "false"));
    for (int n = 0; n < 2048; n++)
        if (e->temps[n]) sb_put(e, b, S(e, "    float4 r%d = float4(0.0f);\n", n));
    if (e->vs) sb_put(e, b, "    int4 a0 = int4(0);\n");
    for (int k = 0; k < nouts; k++) sb_put(e, b, S(e, "    %s %s = %s;\n", outs[k].ty, outs[k].nm, outs[k].dflt));
    for (int k = 0; k < e->nlines; k++) {
        sb_put(e, b, e->lines[k]);
        sb_put(e, b, "\n");
    }
    const char *pos = NULL;
    for (int k = 0; k < nouts && !pos; k++)
        if (!strcmp(outs[k].attr, "[[position]]")) pos = outs[k].nm;
    if (e->vs && pos) sb_put(e, b, S(e, "    %s.xy += aux.posFixup.xy * %s.w;\n", pos, pos));
    int has_oc0 = io_find(e->outputs, e->noutputs, 8, 0) != NULL;
    if (!e->vs && has_oc0) {
        if (fog && !e->sm3) sb_put(e, b, "    oC0.rgb = mix(aux.fogColor.rgb, oC0.rgb, saturate(in.sg_fog));\n");
        sb_put(e, b, "    if (!sg_alpha_test(oC0.a, aux.alphaRef)) discard_fragment();\n");
    }
    sb_put(e, b, S(e, "    %s_out out;\n", en));
    for (int k = 0; k < nouts; k++) sb_put(e, b, S(e, "    out.%s = %s;\n", outs[k].nm, outs[k].nm));
    sb_put(e, b, "    return out;\n}\n");

    /* reflection */
    memset(R, 0, sizeof *R);
    R->stage = e->vs ? 0 : 1;
    R->major = (uint8_t)e->major;
    R->minor = (uint8_t)e->minor;
    snprintf(R->profile, sizeof R->profile, "%s", e->profile);
    R->c_max = e->max_c;
    R->relative_c = (uint8_t)e->rel_c;
    for (int n = 0; n < 2048; n++) {
        if (e->c_used[n]) R->c_used[n >> 5] |= 1u << (n & 31);
        if (e->i_used[n]) R->i_used[n >> 5] |= 1u << (n & 31);
        if (e->b_used[n]) R->b_used[n >> 5] |= 1u << (n & 31);
        R->defs = (uint16_t)(R->defs + e->def_f_has[n] + e->def_i_has[n] + e->def_b_has[n]);
    }
    R->ninputs = (uint8_t)e->ninputs;
    for (int k = 0; k < e->ninputs; k++) {
        IO *d = &e->inputs[k];
        d3d9msl_input *ri = &R->inputs[k];
        snprintf(ri->reg, sizeof ri->reg, "%s", d->reg);
        ri->regtype = (uint8_t)d->rt;
        ri->regnum = (uint8_t)d->n;
        for (int u = 0; u < 14; u++)
            if (!strcmp(USAGE[u], d->usage)) ri->usage = (uint8_t)u;
        ri->usage_index = (uint8_t)d->index;
        ri->attribute = (uint8_t)d->attribute;
        ri->centroid = (uint8_t)d->centroid;
        ri->flat = (uint8_t)d->flat;
    }
    R->noutputs = (uint8_t)nouts;
    for (int k = 0; k < nouts; k++) {
        d3d9msl_output *ro = &R->outputs[k];
        snprintf(ro->name, sizeof ro->name, "%s", outs[k].nm);
        snprintf(ro->binding, sizeof ro->binding, "%s", outs[k].attr);
        snprintf(ro->semantic, sizeof ro->semantic, "%s", outs[k].sem);
        ro->kind = (uint8_t)outs[k].kind;
        ro->regtype = (uint8_t)outs[k].rt;
        ro->regnum = (uint8_t)outs[k].n;
        ro->scalar = !strcmp(outs[k].ty, "float");
    }
    for (int n = 0; n < 32; n++)
        if (e->samp_has[n]) {
            R->sampler_mask |= 1u << n;
            R->sampler_type[n] = e->samp_type[n];
        }
    R->uses_vpos = (uint8_t)e->use_vpos;
    R->uses_vface = (uint8_t)e->use_vface;
    R->uses_texkill = (uint8_t)e->use_texkill;
    R->position_fixup = (uint8_t)(e->vs && pos);
    R->alpha_test = (uint8_t)(!e->vs && has_oc0);
    R->fog_input = (uint8_t)(!e->vs && fog);
    R->options = e->opts;
}

/* ------------------------------------------------------------------------------------------------ API */

static int valid_entry(const char *s) {
    if (!s || !*s || strlen(s) >= 64) return 0;
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_')) return 0;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}

int d3d9msl_translate_opts(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts, d3d9msl_result *out) {
    if (!out) return D3D9MSL_EARG;
    memset(out, 0, sizeof *out);
    const char *entry = opts && opts->entry ? opts->entry : "sg_main";
    if (!tokens || !valid_entry(entry)) {
        snprintf(out->error, sizeof out->error, "bad arguments");
        return D3D9MSL_EARG;
    }
    snprintf(out->entry, sizeof out->entry, "%s", entry);
    E *e = calloc(1, sizeof *e);
    if (!e) { snprintf(out->error, sizeof out->error, "out of memory"); return D3D9MSL_ENOMEM; }
    e->err = out->error;
    e->entry = out->entry;
    e->opts = opts ? opts->flags : 0;
    e->max_c = -1;
    e->ind = 1;
    SB b = {0};
    int rc = parse(e, tokens, ntokens, out->error);
    if (!rc) {
        e->sm3 = e->major >= 3;
        if (setjmp(e->jb) == 0) {
            build(e, &b, &out->refl);
            out->msl = b.p;
            out->msl_len = b.n;
            b.p = NULL;
        } else {
            rc = e->rc;
        }
    }
    free(b.p);
    for (int k = 0; k < e->narena; k++) free(e->arena[k]);
    free(e->arena);
    free(e->lines);
    free(e->ins);
    free(e);
    if (rc) {
        out->msl = NULL;
        out->msl_len = 0;
    }
    return rc;
}

int d3d9msl_translate(const uint32_t *tokens, size_t ntokens, d3d9msl_result *out) {
    return d3d9msl_translate_opts(tokens, ntokens, NULL, out);
}

void d3d9msl_result_free(d3d9msl_result *r) {
    if (!r) return;
    free(r->msl);
    r->msl = NULL;
    r->msl_len = 0;
}

/* ------------------------------------------------------------------------------------------------ JSON */

static const char *usage_name(unsigned u) { return u < 14 ? USAGE[u] : "?"; }

static void js_bits(FILE *f, const uint32_t *bits) {
    int first = 1;
    fputc('[', f);
    for (int n = 0; n < 2048; n++)
        if (bits[n >> 5] >> (n & 31) & 1) { fprintf(f, first ? "%d" : ", %d", n); first = 0; }
    fputc(']', f);
}

char *d3d9msl_reflection_json(const d3d9msl_reflection *r, const char *entry) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return NULL;
    fprintf(f, "{\"kind\": \"%s\", \"profile\": \"%s\", \"entry\": \"%s\", \"inputs\": [", r->stage ? "ps" : "vs",
            r->profile, entry ? entry : "sg_main");
    for (int k = 0; k < r->ninputs; k++) {
        const d3d9msl_input *i = &r->inputs[k];
        fprintf(f, "%s{\"reg\": \"%s\", \"usage\": \"%s\", \"index\": %d", k ? ", " : "", i->reg, usage_name(i->usage),
                i->usage_index);
        if (r->stage == 0) fprintf(f, ", \"attribute\": %d}", i->attribute);
        else fprintf(f, ", \"centroid\": %s, \"flat\": %s}", i->centroid ? "true" : "false", i->flat ? "true" : "false");
    }
    fprintf(f, "], \"outputs\": [");
    for (int k = 0; k < r->noutputs; k++) {
        const d3d9msl_output *o = &r->outputs[k];
        fprintf(f, "%s{\"reg\": \"%s\", \"type\": \"%s\", \"binding\": \"%s\"}", k ? ", " : "", o->name,
                o->scalar ? "float" : "float4", o->binding);
    }
    fprintf(f, "], \"samplers\": {");
    int first = 1;
    for (int n = 0; n < 32; n++)
        if (r->sampler_mask >> n & 1) {
            fprintf(f, "%s\"%d\": \"%s\"", first ? "" : ", ", n, textype_name(r->sampler_type[n]));
            first = 0;
        }
    fprintf(f, "}, \"float_constants_max_index\": %d, \"relative_constants\": %s, \"float_constants_used\": ",
            (int)r->c_max, r->relative_c ? "true" : "false");
    js_bits(f, r->c_used);
    fprintf(f, ", \"int_constants_used\": ");
    js_bits(f, r->i_used);
    fprintf(f, ", \"bool_constants_used\": ");
    js_bits(f, r->b_used);
    fprintf(f, ", \"uses\": [");
    first = 1;
    if (r->uses_texkill) { fprintf(f, "\"texkill\""); first = 0; }
    if (r->uses_vface) { fprintf(f, "%s\"vFace\"", first ? "" : ", "); first = 0; }
    if (r->uses_vpos) { fprintf(f, "%s\"vPos\"", first ? "" : ", "); first = 0; }
    fprintf(f, "], \"position_fixup\": %s, \"alpha_test\": %s, \"fog_input\": %s, \"defs\": %d}",
            r->position_fixup ? "true" : "false", r->alpha_test ? "true" : "false", r->fog_input ? "true" : "false",
            r->defs);
    if (fclose(f)) { free(buf); return NULL; }
    return buf;
}
