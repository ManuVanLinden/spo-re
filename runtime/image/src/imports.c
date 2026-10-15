/* imports.c -- import overrides and IAT installation through runtime/hle's table (runtime-abi 7.2). */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hle.h"
#include "sg_image.h"

/* "KERNEL32.dll", "kernel32", "C:\\x\\Kernel32.DLL" -> "kernel32" (same rule as runtime/hle). */
static void stem(const char *m, char *out, size_t cap) {
    size_t i, n;
    const char *s = strrchr(m, '\\');
    if (s) m = s + 1;
    n = strlen(m);
    if (n > 4 && !strcasecmp(m + n - 4, ".dll")) n -= 4;
    for (i = 0; i < n && i + 1 < cap; i++) out[i] = (char)(m[i] >= 'A' && m[i] <= 'Z' ? m[i] + 32 : m[i]);
    out[i] = 0;
}

static unsigned replace(int by_symbol, const char *dll, const char *name, sg_func *fn) {
    char want[64], have[64];
    unsigned i, k, n = 0;
    if (!by_symbol) stem(dll, want, sizeof want);
    for (i = 0; i < hle_import_count; i++) {
        sg_import *imp = &hle_imports[i];
        int hit;
        if (imp->cc == HLE_CC_DATA) continue;
        if (by_symbol) {
            hit = !strcmp(hle_import_info[i].symbol, name);
        } else {
            stem(imp->dll, have, sizeof have);
            hit = imp->name && !strcmp(want, have) && !strcmp(name, imp->name);
        }
        if (!hit) continue;
        if (imp->guest) {
            fprintf(stderr, "image: override of %s after installation ignored\n", hle_import_info[i].symbol);
            continue;
        }
        for (k = 0; k < hle_dynamic_count; k++) /* GetProcAddress names that share this shim */
            if (hle_dynamics[k].fn == imp->fn && !hle_dynamics[k].guest) { hle_dynamics[k].fn = fn; n++; }
        imp->fn = fn;
        n++;
    }
    if (!by_symbol)
        for (k = 0; k < hle_dynamic_count; k++) {
            hle_dynamic *d = &hle_dynamics[k];
            stem(d->module, have, sizeof have);
            if (!d->guest && d->fn != fn && !strcmp(want, have) && !strcmp(name, d->name)) { d->fn = fn; n++; }
        }
    return n;
}

unsigned sg_import_override(const char *dll, const char *name, sg_func *fn) { return replace(0, dll, name, fn); }
unsigned sg_import_override_symbol(const char *symbol, sg_func *fn) { return replace(1, NULL, symbol, fn); }

unsigned sg_image_install_imports(uint8_t *mem) {
    unsigned n = hle_install_imports(mem), k;
    /* Then every dynamic name, in table order, so GetProcAddress results are the same every run
     * (deliberately absent names, fn == NULL, stay unregistered and resolve to NULL). */
    for (k = 0; k < hle_dynamic_count; k++)
        if (hle_dynamics[k].fn && !hle_dynamics[k].guest) (void)hle_resolve(hle_dynamics[k].module, hle_dynamics[k].name, 0);
    return n;
}
