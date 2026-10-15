/* main.c -- build/sporenative/spore. Start-up order (docs/design/image-linker.md 4):
 *   1. reserve the 4 GiB guest window, install the fault reporter
 *   2. map the image at 0x400000 (from the exe file, or from the embedded copy), TLS index = 0
 *   3. thunk region; runtime/core and runtime/win32 start-up hooks (overrides of runtime/hle stubs)
 *   4. IAT and dynamic names through runtime/hle (hle_install_imports)
 *   5. final page protections
 *   6. --smoke: report and exit without calling game code; --selftest: walk the dispatch table;
 *      --run: hand over to sg_boot (runtime/core). */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle.h"
#include "sg_image.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void usage(void) {
    fprintf(stderr,
            "usage: spore [--smoke | --selftest ENTRIES | --run] [--source file|embed|embed-mmap] [--exe PATH]\n"
            "             [--no-verify] [--quiet] [--probe-miss HEX] [-- game args]\n"
            "  --smoke     map the image, install the IAT, report and exit before any game code (default)\n"
            "  --selftest  also walk the dispatch table against ENTRIES (tools/link output) and every import\n"
            "  --run       call sg_boot(entry 0x%08x): runs translated game code\n", SG_IMAGE_ENTRY);
}

/* ---------------------------------------------------------------------------------------------- */
/* Self-test: every manifest entry must resolve through sg_lookup.                                  */
/* ---------------------------------------------------------------------------------------------- */

static unsigned long probe_misses;
static void probe_sentinel(sg_ctx *ctx, uint8_t *mem) { (void)ctx; (void)mem; abort(); }
static sg_func *probe_hook(sg_ctx *ctx, uint8_t *mem, uint32_t g) {
    (void)ctx; (void)mem; (void)g;
    probe_misses++;
    return probe_sentinel;
}

/* Host protection of the page holding guest address g (-1 when unmapped/reserved-only). */
static int prot_at(uint8_t *mem, uint32_t g) {
    mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)(mem + g);
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return -1;
    if (addr > (mach_vm_address_t)(uintptr_t)(mem + g)) return -1; /* g is in a hole before the next region */
    return info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
}

#define FAIL(...) do { if (nfail++ < 20) { fprintf(stderr, "selftest: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int selftest(uint8_t *mem, const char *entries_path) {
    static sg_ctx ctx;
    sg_miss_hook_fn saved = sg_miss_hook;
    unsigned long nfail = 0, nf = 0, nl = 0, nl_func = 0, nl_resume = 0, named = 0, nimp = 0, ndyn = 0, nabsent = 0;
    uint32_t i, prev = 0, b;
    FILE *f;
    char line[256];
    double t0 = now_ms();

    ctx.mem = mem;
    ctx.esp = 0x7f0f0000u; /* never dereferenced: the probe hook does not read the stack */
    sg_miss_hook = probe_hook;

    /* 1. table shape: sorted, unique, inside .text, every bucket points at the first entry >= its start */
    for (i = 0; i < sg_dispatch_count; i++) {
        uint32_t g = sg_dispatch_entries[i].guest;
        if (i && g <= prev) FAIL("entries not sorted at %u (0x%08x after 0x%08x)", i, g, prev);
        if (g < SG_TEXT_START || g >= SG_TEXT_END) FAIL("entry 0x%08x outside .text", g);
        if (!sg_dispatch_entries[i].fn) FAIL("entry 0x%08x has no function", g);
        prev = g;
    }
    for (b = 0, i = 0; b < SG_DISPATCH_BUCKETS; b++) {
        uint32_t start = SG_TEXT_START + 16u * b;
        while (i < sg_dispatch_count && sg_dispatch_entries[i].guest < start) i++;
        if (sg_dispatch_bucket[b] != i) { FAIL("bucket %u is %u, expected %u", b, sg_dispatch_bucket[b], i); break; }
    }
    if (sg_dispatch_bucket[SG_DISPATCH_BUCKETS] != sg_dispatch_count) FAIL("sentinel bucket wrong");

    /* 2. symbol identity: the host function of each entry is sg_f_<guest> (dladdr on our own symbols) */
    for (i = 0; i < sg_dispatch_count; i++) {
        Dl_info di;
        char want[32];
        snprintf(want, sizeof want, "sg_f_%08x", sg_dispatch_entries[i].guest);
        if (dladdr((const void *)sg_dispatch_entries[i].fn, &di) && di.dli_sname) {
            if (strcmp(di.dli_sname, want) && !(di.dli_sname[0] == '_' && !strcmp(di.dli_sname + 1, want)))
                FAIL("entry 0x%08x points at %s", sg_dispatch_entries[i].guest, di.dli_sname);
            else named++;
        }
    }

    /* 3. every manifest function and label */
    f = fopen(entries_path, "r");
    if (!f) { fprintf(stderr, "selftest: cannot open %s\n", entries_path); return 2; }
    while (fgets(line, sizeof line, f)) {
        char kind[64] = "";
        unsigned va;
        char t;
        if (line[0] == '#' || sscanf(line, "%c %x %63s", &t, &va, kind) < 2) continue;
        if (t == 'F') {
            unsigned long m0 = probe_misses;
            sg_func *fn = sg_lookup(&ctx, mem, va);
            const sg_dispatch_entry *e = sg_dispatch_find(va);
            nf++;
            if (probe_misses != m0 || !e || fn != e->fn || !(e->flags & SG_DE_FUNC)) FAIL("function 0x%08x does not resolve", va);
        } else if (t == 'L') {
            unsigned long m0 = probe_misses;
            sg_func *fn = sg_lookup(&ctx, mem, va);
            nl++;
            if (probe_misses == m0) { nl_func++; (void)fn; } /* a label that is also a function entry */
            else if (sg_resume_owner(va)) nl_resume++;      /* resume point: entered only via sg_eh_resume */
            else FAIL("label 0x%08x (%s) is neither an entry nor a resume point", va, kind);
        }
    }
    fclose(f);

    /* 4. IAT: every slot leads through sg_lookup to the shim registered for it */
    for (i = 0; i < hle_import_count; i++) {
        const sg_import *imp = &hle_imports[i];
        uint32_t v = sg_ld32(mem, imp->iat);
        nimp++;
        if (imp->cc == HLE_CC_DATA) {
            if (!v || v != imp->guest) FAIL("data import %s: IAT 0x%08x", imp->name, v);
            continue;
        }
        if (v != imp->guest || v - SG_THUNK_BASE >= SG_THUNK_LIMIT - SG_THUNK_BASE) FAIL("IAT slot 0x%08x = 0x%08x", imp->iat, v);
        else if (sg_lookup(&ctx, mem, v) != imp->fn) FAIL("IAT slot 0x%08x (%s) resolves to the wrong shim", imp->iat, hle_import_info[i].symbol);
    }
    /* 5. GetProcAddress names */
    for (i = 0; i < hle_dynamic_count; i++) {
        const hle_dynamic *d = &hle_dynamics[i];
        uint32_t g = hle_resolve(d->module, d->name, 0);
        ndyn++;
        if (!d->fn) { if (g) FAIL("absent dynamic %s!%s resolved", d->module, d->name); else nabsent++; continue; }
        if (!g || sg_lookup(&ctx, mem, g) != d->fn) FAIL("dynamic %s!%s does not resolve (0x%08x)", d->module, d->name, g);
    }
    /* 6. non-entries miss: one past every entry that is not itself followed by an entry */
    {
        unsigned long m0 = probe_misses, expect = 0;
        for (i = 0; i < sg_dispatch_count; i++) {
            uint32_t g = sg_dispatch_entries[i].guest + 1u;
            if (i + 1 < sg_dispatch_count && sg_dispatch_entries[i + 1].guest == g) continue;
            expect++;
            (void)sg_lookup(&ctx, mem, g);
        }
        if (probe_misses - m0 != expect) FAIL("%lu of %lu non-entry probes resolved", expect - (probe_misses - m0), expect);
    }
    /* 7. page protections: sections as in the PE (16 KiB host pages), null guard and thunks */
    {
        static const struct { uint32_t g; int prot; const char *what; } want[] = {
            {0x00000000u, PROT_NONE, "null guard"}, {0x0000fffcu, PROT_NONE, "null guard end"},
            {SG_IMAGE_BASE, PROT_READ, "headers"}, {0x00401000u, PROT_READ, ".text"}, {0x013c8000u, PROT_READ, ".text end"},
            {SG_IAT_START, PROT_READ, "IAT"}, {0x0150b000u, PROT_READ, ".rdata end"}, {0x0150c000u, PROT_READ | PROT_WRITE, ".data"},
            {SG_IMAGE_TLS_INDEX_VA, PROT_READ | PROT_WRITE, "TLS index"}, {0x0171f000u, PROT_READ | PROT_WRITE, ".tls"},
            {0x01720000u, PROT_READ | PROT_WRITE, "CONST (shares a host page with .rsrc)"},
            {0x01724000u, PROT_READ, ".rsrc"}, {0x01c28000u, PROT_READ, ".reloc"}, {0x01d0c000u, PROT_READ, "last image page"},
            {SG_IMAGE_MAPPED_END, PROT_NONE, "after the image"}, {SG_THUNK_BASE, PROT_READ, "thunk region"},
            {SG_THUNK_LIMIT - 4u, PROT_READ, "thunk region end"}, {0x80000000u, PROT_NONE, "above 2 GiB"},
        };
        unsigned k;
        for (k = 0; k < sizeof want / sizeof want[0]; k++) {
            int p = prot_at(mem, want[k].g);
            if (p < 0) p = PROT_NONE;
            if (p != want[k].prot) FAIL("protection at 0x%08x (%s) is %d, expected %d", want[k].g, want[k].what, p, want[k].prot);
        }
        if (mem[SG_THUNK_BASE + 16u * 20u] != 0xcc) FAIL("thunk region bytes are not int3");
    }
    sg_miss_hook = saved;
    printf("selftest: %u dispatch entries (%lu named by dladdr), %lu manifest functions, %lu labels "
           "(%lu entries, %lu resume points), %lu imports, %lu dynamic names (%lu absent), %u resume points; "
           "%lu failures; %.1f ms\n",
           sg_dispatch_count, named, nf, nl, nl_func, nl_resume, nimp, ndyn, nabsent, sg_resume_point_count, nfail,
           now_ms() - t0);
    return nfail ? 1 : 0;
}

/* Start-up entry points of runtime/core and runtime/win32, looked up in our own
 * executable so this file needs neither header: absent packages leave them NULL. */
static struct {
    void (*core_init)(uint8_t *mem);
    void (*core_image_tls)(uint32_t index_va, uint32_t template_va, uint32_t size, uint32_t zero_fill);
    int (*w32_init)(uint8_t *mem, const void *cfg);
    unsigned (*w32_register)(unsigned *mismatches);
    void (*w32_mark_image)(uint32_t addr, uint32_t size, uint32_t protect);
    void (*w32_after_install)(uint8_t *mem);
} opt;

static void find_optional(void) {
    void *h = dlopen(NULL, RTLD_NOW);
    *(void **)&opt.core_init = dlsym(h, "sg_core_init");
    *(void **)&opt.core_image_tls = dlsym(h, "sg_core_image_tls");
    *(void **)&opt.w32_init = dlsym(h, "w32_init");
    *(void **)&opt.w32_register = dlsym(h, "w32_register_shims");
    *(void **)&opt.w32_mark_image = dlsym(h, "w32_vm_mark_image");
    *(void **)&opt.w32_after_install = dlsym(h, "w32_after_install");
}

static unsigned thunks_in_use(void) {
    unsigned k, n = 0;
    for (k = 0; k < (SG_THUNK_LIMIT - SG_THUNK_BASE) / SG_THUNK_SLOT; k++) n += sg_thunk_fn[k] != NULL;
    return n;
}

/* ---------------------------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    enum { SMOKE, SELFTEST, RUN, PROBE } mode = SMOKE;
    uint32_t probe = 0;
    enum sg_image_source src = SG_IMAGE_FROM_FILE;
    const char *exe = NULL, *entries = NULL;
    int verify = 1, quiet = 0, i, gargc = 0;
    char **gargv = NULL;
    uint8_t *mem;
    unsigned n;
    double t0 = now_ms(), t_win, t_load, t_thunks, t_imp, t_prot;

    if (sg_image_embedded()) src = SG_IMAGE_EMBED_MMAP;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--smoke")) mode = SMOKE;
        else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) { mode = SELFTEST; entries = argv[++i]; }
        else if (!strcmp(argv[i], "--run")) mode = RUN;
        else if (!strcmp(argv[i], "--probe-miss") && i + 1 < argc) { mode = PROBE; probe = (uint32_t)strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--no-verify")) verify = 0;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--source") && i + 1 < argc) {
            const char *s = argv[++i];
            if (!strcmp(s, "file")) src = SG_IMAGE_FROM_FILE;
            else if (!strcmp(s, "embed")) src = SG_IMAGE_EMBED_COPY;
            else if (!strcmp(s, "embed-mmap")) src = SG_IMAGE_EMBED_MMAP;
            else { usage(); return 2; }
        } else if (!strcmp(argv[i], "--")) { gargc = argc - i - 1; gargv = argv + i + 1; break; }
        else { usage(); return 2; }
    }

    mem = sg_image_window_reserve();
    if (!mem) return 1;
    sg_image_install_fault_handler();
    t_win = now_ms();
    /* Optional runtime packages, found by name so this file links with or without them. */
    find_optional();
    if (opt.core_init) opt.core_init(mem);                        /* thunk region, PEB, slots 0..2 */
    if (opt.w32_init) {
        static uint64_t w32_cfg[64];                              /* a zeroed w32_config: all defaults */
        if (opt.w32_init(mem, w32_cfg)) { fprintf(stderr, "spore: w32_init failed\n"); return 1; }
    }
    if (sg_image_load(mem, src, exe, verify)) return 1;
    t_load = now_ms();
    if (opt.core_image_tls) opt.core_image_tls(SG_IMAGE_TLS_INDEX_VA, SG_IMAGE_TLS_TEMPLATE, SG_IMAGE_TLS_TEMPLATE_SIZE, 0);
    if (opt.w32_mark_image)
        for (i = 0; i < (int)sg_image.nsections; i++)
            opt.w32_mark_image(sg_image.sec[i].va, sg_image.sec[i].vsize, (sg_image.sec[i].prot & 2) ? 0x04u : 0x02u);
    sg_thunks_init(mem);
    sg_core_startup(mem);
    sg_win32_startup(mem);
    if (opt.w32_register) {
        unsigned mism = 0, k = opt.w32_register(&mism);
        if (!quiet) printf("spore: runtime/win32 replaced %u import shims (%u clean-up mismatches)\n", k, mism);
    }
    t_thunks = now_ms();
    n = sg_image_install_imports(mem);
    if (opt.w32_after_install) opt.w32_after_install(mem);
    t_imp = now_ms();
    if (sg_image_protect(mem)) return 1;
    t_prot = now_ms();

    if (!quiet) {
        static const char *srcname[] = {"file", "embed", "embed-mmap"};
        printf("spore: window at %p; image %s from %s\n", (void *)mem, srcname[sg_image.source], sg_image.path);
        printf("spore: %u sections, entry 0x%08x, SHA-256 %s\n", sg_image.nsections, sg_image.entry,
               sg_image.sha_ok == 1 ? "verified" : sg_image.sha_ok == 0 ? "MISMATCH" : "not checked");
        for (i = 0; i < (int)sg_image.nsections; i++)
            printf("  %-8s 0x%08x-0x%08x %c%c\n", sg_image.sec[i].name, sg_image.sec[i].va,
                   sg_image.sec[i].va + sg_image.sec[i].vsize, (sg_image.sec[i].prot & 1) ? 'r' : '-',
                   (sg_image.sec[i].prot & 2) ? 'w' : '-');
        printf("spore: %u IAT slots written, %u thunk slots in use, %u dispatch entries (%s), %u resume points\n", n,
               thunks_in_use(), sg_dispatch_count, sg_dispatch_source, sg_resume_point_count);
        printf("spore: runtime/core %s, runtime/win32 %s\n", opt.core_init ? "linked (sg_core_init ran)" : "absent (weak defaults)",
               opt.w32_init ? "linked (w32_init ran)" : "absent (weak defaults)");
        printf("spore: timings ms: window %.3f, load %.2f (copy/map %.2f, sha %.2f), thunks+hooks %.3f, imports %.3f, "
               "protect %.3f, total %.2f\n",
               t_win - t0, t_load - t_win, sg_image.load_ms, sg_image.hash_ms, t_thunks - t_load, t_imp - t_thunks,
               t_prot - t_imp, t_prot - t0);
    }
    if (mode == SELFTEST) return selftest(mem, entries);
    if (mode == RUN) return sg_boot(mem, SG_IMAGE_ENTRY, gargc, gargv);
    if (mode == PROBE) {
        /* Exercises the default miss hook as an indirect call from a made-up caller would: no game code
         * runs (the lookup misses before anything is called). A small guest stack comes from the bump
         * allocator; the pushed return address pretends to be 0x00401005. */
        static sg_ctx ctx;
        uint32_t stk = hle_guest_alloc(0x1000, 16);
        sg_func *fn;
        ctx.mem = mem;
        ctx.esp = stk + 0x1000u - 4u;
        sg_st32(mem, ctx.esp, 0x00401005u);
        fn = sg_lookup(&ctx, mem, probe);
        printf("spore: 0x%08x resolves to %p without a miss\n", probe, (void *)fn);
        return 0;
    }
    if (!quiet) printf("spore: smoke run complete; no game code was called (use --run to boot)\n");
    return 0;
}
