/* core_hle_glue.c -- registers the core's shims in runtime/hle's import table without
 * editing runtime/hle: hle_imports[i].fn and hle_dynamics[i].fn are replaced for every symbol
 * in sg_core_shims[]. Call sg_core_register_hle_shims() before hle_install_imports(mem), which
 * then hands out thunk slots and fills the IAT with the core functions.
 *
 * Direct calls: the translator emits SG_CALL(sg_imp_<dll>_<name>, ra) when the IAT slot is known
 * at translation time, which binds to runtime/hle's generated stub symbol, not to the table. A forwarding-stub patch to the
 * HLE generator makes each generated stub tail-call
 * hle_imports[i].fn when it has been replaced, so both paths reach the core shim. */
#include <ctype.h>

#include "core_internal.h"
#include "hle.h"

/* "sg_imp_kernel32_TlsGetValue" -> stem "kernel32", name "TlsGetValue" */
static int split_symbol(const char *sym, char *stem, size_t sc, const char **name) {
    const char *p = sym + 7, *u; /* after "sg_imp_" */
    size_t n;
    if (strncmp(sym, "sg_imp_", 7)) return 0;
    u = strchr(p, '_');
    if (!u) return 0;
    n = (size_t)(u - p);
    if (n + 1 > sc) return 0;
    memcpy(stem, p, n);
    stem[n] = 0;
    *name = u + 1;
    return 1;
}

static int module_is(const char *module, const char *stem) {
    size_t n = strlen(stem), i;
    for (i = 0; i < n; i++)
        if (!module[i] || tolower((unsigned char)module[i]) != stem[i]) return 0;
    return module[n] == 0 || module[n] == '.';
}

static const sg_core_shim *shim_row(const char *symbol) {
    unsigned k;
    for (k = 0; k < sg_core_shim_count; k++)
        if (!strcmp(sg_core_shims[k].symbol, symbol)) return &sg_core_shims[k];
    return NULL;
}

unsigned sg_core_register_hle_shims(unsigned *mismatches) {
    unsigned i, k, n = 0, bad = 0;
    for (i = 0; i < hle_import_count; i++) {
        const sg_core_shim *row;
        sg_func *f;
        uint32_t table;
        if (hle_imports[i].cc == HLE_CC_DATA) continue;
        row = shim_row(hle_import_info[i].symbol);
        if (!row) continue;
        f = row->fn;
        table = hle_imports[i].cc == SG_CC_STDCALL || hle_imports[i].cc == SG_CC_THISCALL ? hle_imports[i].argbytes : 0;
        if (table != row->cleanup) {
            sg_core_log(0, "core shim %s pops %u bytes but the import table says %u: not registered", row->symbol,
                        row->cleanup, table);
            bad++;
            continue;
        }
        if (hle_import_info[i].implemented)
            sg_core_log(1, "core shim replaces the hand-written %s", hle_import_info[i].symbol);
        hle_imports[i].fn = f;
        n++;
    }
    for (k = 0; k < sg_core_shim_count; k++) {
        char stem[32];
        const char *name;
        if (!split_symbol(sg_core_shims[k].symbol, stem, sizeof stem, &name)) continue;
        for (i = 0; i < hle_dynamic_count; i++)
            if (hle_dynamics[i].fn && !strcmp(hle_dynamics[i].name, name) && module_is(hle_dynamics[i].module, stem))
                hle_dynamics[i].fn = sg_core_shims[k].fn;
    }
    sg_core_log(2, "registered %u core shims in the import table (%u refused)", n, bad);
    if (mismatches) *mismatches = bad;
    return n;
}
