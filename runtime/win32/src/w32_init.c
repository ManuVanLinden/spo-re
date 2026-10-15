/* w32_init.c -- start-up of the whole layer, and shim registration through runtime/hle's tables. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "w32_int.h"
#include "w32_shims.h"

extern const int w32_overrides_linked;

int w32_init(uint8_t *mem, const w32_config *cfg_in) {
    w32_config dflt;
    const w32_config *cfg = cfg_in;
    if (!cfg) { memset(&dflt, 0, sizeof dflt); cfg = &dflt; }
    w32_mem = mem;
    hle_log(HLE_LOG_INFO, "w32: sg_imp_* overrides %s", w32_overrides_linked ? "linked (weak HLE stubs)" : "off (table swap only)");
    w32_vm_init();
    w32_heap_init();
    w32_crt_init();
    if (w32_paths_init(cfg)) return -1;
    w32_env_init(cfg);
    w32_files_init(cfg);
    w32_stdio_init();
    char regpath[W32_PATH_CAP];
    const char *rp = cfg->registry_file ? cfg->registry_file : getenv("SPORE_REGISTRY");
    if (!rp) {
        const char *sup = cfg->support_dir ? cfg->support_dir : getenv("SPORE_SUPPORT_DIR");
        const char *home = getenv("HOME");
        if (sup) snprintf(regpath, sizeof regpath, "%s/registry.json", sup);
        else snprintf(regpath, sizeof regpath, "%s/Library/Application Support/spore-native/registry.json", home ? home : "/tmp");
        rp = regpath;
    }
    w32_reg_load(rp, cfg->persist_registry); /* a damaged store falls back to the seed (logged) */
    return 0;
}

void w32_after_install(uint8_t *mem) { w32_crt_after_install(mem); }

static void stem(const char *dll, char *out, size_t cap) {
    size_t n = strlen(dll), i;
    if (n > 4 && !strcasecmp(dll + n - 4, ".dll")) n -= 4;
    for (i = 0; i < n && i + 1 < cap; i++) out[i] = (char)(dll[i] >= 'A' && dll[i] <= 'Z' ? dll[i] + 32 : dll[i]);
    out[i] = 0;
}

const w32_shim w32_shims[] = {
#define X(dllstem, dll, export_name, sym, cleanup) {dll, export_name, w32_imp_##dllstem##_##sym, cleanup},
    W32_SHIM_LIST(X)
#undef X
};
const unsigned w32_shim_count = sizeof w32_shims / sizeof w32_shims[0];

unsigned w32_register_shims(unsigned *mismatches) {
    unsigned replaced = 0, bad = 0;
    for (unsigned s = 0; s < w32_shim_count; s++) {
        const w32_shim *w = &w32_shims[s];
        char ws[32], hs[32];
        stem(w->dll, ws, sizeof ws);
        for (unsigned i = 0; i < hle_import_count; i++) {
            sg_import *imp = &hle_imports[i];
            if (!imp->name || strcmp(imp->name, w->name)) continue;
            stem(imp->dll, hs, sizeof hs);
            if (strcmp(ws, hs)) continue;
            unsigned table = imp->cc == SG_CC_STDCALL ? imp->argbytes : 0;
            if (table != w->cleanup) {
                hle_log(HLE_LOG_ERROR, "w32: %s!%s pops %u bytes but the import table says %u", w->dll, w->name,
                        w->cleanup, table);
                bad++;
                continue; /* keep the generated stub rather than corrupt esp */
            }
            sg_func *old = imp->fn;
            imp->fn = w->fn;
            for (unsigned d = 0; d < hle_dynamic_count; d++)
                if (hle_dynamics[d].fn == old) hle_dynamics[d].fn = w->fn;
            replaced++;
        }
    }
    if (mismatches) *mismatches = bad;
    hle_log(HLE_LOG_INFO, "w32: %u import slots now use runtime/win32 shims (%u mismatches)", replaced, bad);
    return replaced;
}
