/* core_teb.c -- TEBs, Win32 TLS slots (TlsAlloc/TlsFree/TlsGetValue/TlsSetValue) and the image's
 * __declspec(thread) block (runtime-abi 9.2, 3.2). */
#include "core_internal.h"

/* TEB pages live in [SG_TEB_REGION, SG_PEB_ADDR). Expansion-slot pages come from the same pool. */
#define TEB_PAGES ((SG_PEB_ADDR - SG_TEB_REGION) / 0x1000u)
static uint8_t teb_used[TEB_PAGES];
static pthread_mutex_t teb_lock = PTHREAD_MUTEX_INITIALIZER;

/* Layout inside our TEB page beyond the Windows fields we model. */
#define TEB_TLS_ARRAY 0xfa0u    /* ThreadLocalStoragePointer -> this array; [0] -> block below */
#define TEB_TLS_BLOCK 0xfb0u    /* this thread's copy of the image TLS template (<= 80 bytes) */
#define TEB_TLS_BLOCK_MAX (SG_TEB_SIZE - TEB_TLS_BLOCK)

static uint32_t img_tls_index, img_tls_template, img_tls_size, img_tls_zero;

static uint32_t teb_page_alloc(void) {
    unsigned i;
    pthread_mutex_lock(&teb_lock);
    for (i = 0; i < TEB_PAGES; i++)
        if (!teb_used[i]) {
            teb_used[i] = 1;
            pthread_mutex_unlock(&teb_lock);
            memset(sg_core_mem + SG_TEB_REGION + 0x1000u * i, 0, 0x1000);
            return SG_TEB_REGION + 0x1000u * i;
        }
    pthread_mutex_unlock(&teb_lock);
    return 0;
}

static void teb_page_free(uint32_t a) {
    if (a < SG_TEB_REGION || a >= SG_PEB_ADDR) return;
    pthread_mutex_lock(&teb_lock);
    teb_used[(a - SG_TEB_REGION) / 0x1000u] = 0;
    pthread_mutex_unlock(&teb_lock);
}

void sg_core_image_tls(uint32_t index_va, uint32_t template_va, uint32_t template_size, uint32_t zero_fill) {
    if (template_size + zero_fill > TEB_TLS_BLOCK_MAX) {
        fprintf(stderr, "sg_core_image_tls: TLS block of %u bytes does not fit (max %u)\n", template_size + zero_fill,
                TEB_TLS_BLOCK_MAX);
        abort();
    }
    img_tls_index = index_va;
    img_tls_template = template_va;
    img_tls_size = template_size;
    img_tls_zero = zero_fill;
    if (index_va) sg_st32(sg_core_mem, index_va, 0); /* the loader stores TLS index 0 */
}

uint32_t core_teb_alloc(sg_thread *t) {
    uint8_t *mem = sg_core_mem;
    uint32_t teb = teb_page_alloc();
    if (!teb) return 0;
    sg_st32(mem, teb + SG_TEB_EXCEPTION_LIST, SG_END_OF_CHAIN);
    sg_st32(mem, teb + SG_TEB_STACK_BASE, t->stack_hi);
    sg_st32(mem, teb + SG_TEB_STACK_LIMIT, t->stack_lo);
    sg_st32(mem, teb + SG_TEB_SELF, teb);
    sg_st32(mem, teb + SG_TEB_CLIENT_PID, 0x1000u /* matches GetCurrentProcessId */);
    sg_st32(mem, teb + SG_TEB_CLIENT_TID, t->tid);
    sg_st32(mem, teb + SG_TEB_PEB, SG_PEB_ADDR);
    sg_st32(mem, teb + SG_TEB_TLS_POINTER, teb + TEB_TLS_ARRAY);
    sg_st32(mem, teb + TEB_TLS_ARRAY, teb + TEB_TLS_BLOCK);
    t->teb = teb;
    t->tls_block = teb + TEB_TLS_BLOCK;
    core_tls_thread_init(t);
    return teb;
}

void core_tls_thread_init(sg_thread *t) {
    if (img_tls_template && img_tls_size)
        memcpy(sg_core_mem + t->tls_block, sg_core_mem + img_tls_template, img_tls_size);
    if (img_tls_zero) memset(sg_core_mem + t->tls_block + img_tls_size, 0, img_tls_zero);
}

void core_teb_free(sg_thread *t) {
    core_thread *h = core_th(t);
    if (h && h->tls_expansion) {
        teb_page_free(h->tls_expansion);
        h->tls_expansion = 0;
    }
    if (t->teb) teb_page_free(t->teb);
    t->teb = 0;
}

/* ---- live-thread registry ---------------------------------------------------------------------- */

pthread_mutex_t core_threads_lock = PTHREAD_MUTEX_INITIALIZER;
core_thread *core_threads_all;

void core_threads_register(core_thread *h) {
    pthread_mutex_lock(&core_threads_lock);
    h->all_prev = NULL;
    h->all_next = core_threads_all;
    if (core_threads_all) core_threads_all->all_prev = h;
    core_threads_all = h;
    pthread_mutex_unlock(&core_threads_lock);
}
void core_threads_unregister(core_thread *h) {
    pthread_mutex_lock(&core_threads_lock);
    if (h->all_prev) h->all_prev->all_next = h->all_next; else if (core_threads_all == h) core_threads_all = h->all_next;
    if (h->all_next) h->all_next->all_prev = h->all_prev;
    h->all_next = h->all_prev = NULL;
    pthread_mutex_unlock(&core_threads_lock);
}

unsigned sg_core_thread_count(void) {
    unsigned n = 0;
    core_thread *h;
    pthread_mutex_lock(&core_threads_lock);
    for (h = core_threads_all; h; h = h->all_next) n++;
    pthread_mutex_unlock(&core_threads_lock);
    return n;
}

/* Live guest threads as "name(entry 0x..)[, waiting]" separated by "; " for reports (the
 * SPORE_RUN_SECONDS summary). Returns the number of threads. */
unsigned sg_core_thread_list(char *buf, size_t cap) {
    unsigned n = 0;
    size_t used = 0;
    core_thread *h;
    if (cap) buf[0] = 0;
    pthread_mutex_lock(&core_threads_lock);
    for (h = core_threads_all; h; h = h->all_next) {
        n++;
        if (used + 1 < cap) {
            int k = snprintf(buf + used, cap - used, "%s%s (entry 0x%08x%s)", n > 1 ? "; " : "", h->name[0] ? h->name : "unnamed",
                             h->entry, h->waiting ? ", waiting" : "");
            if (k > 0) used += (size_t)k < cap - used ? (size_t)k : cap - used - 1;
        }
    }
    pthread_mutex_unlock(&core_threads_lock);
    return n;
}

/* ---- TLS slots ------------------------------------------------------------------------------------ */

static uint8_t tls_used[SG_TLS_SLOTS_TOTAL];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;

static void clear_slot_everywhere(uint32_t idx) {
    core_thread *h;
    uint8_t *mem = sg_core_mem;
    pthread_mutex_lock(&core_threads_lock);
    for (h = core_threads_all; h; h = h->all_next) {
        sg_thread *t = h->self;
        if (!t->teb) continue;
        if (idx < SG_TLS_SLOTS_INLINE) sg_st32(mem, t->teb + SG_TEB_TLS_SLOTS + 4u * idx, 0);
        else if (h->tls_expansion) sg_st32(mem, h->tls_expansion + 4u * (idx - SG_TLS_SLOTS_INLINE), 0);
    }
    pthread_mutex_unlock(&core_threads_lock);
}

uint32_t sg_core_tls_alloc(void) {
    uint32_t i;
    pthread_mutex_lock(&tls_lock);
    for (i = 0; i < SG_TLS_SLOTS_TOTAL; i++)
        if (!tls_used[i]) { tls_used[i] = 1; break; }
    pthread_mutex_unlock(&tls_lock);
    if (i == SG_TLS_SLOTS_TOTAL) return 0xffffffffu;
    clear_slot_everywhere(i);
    return i;
}

int sg_core_tls_free(uint32_t idx) {
    int ok;
    if (idx >= SG_TLS_SLOTS_TOTAL) return 0;
    pthread_mutex_lock(&tls_lock);
    ok = tls_used[idx];
    tls_used[idx] = 0;
    pthread_mutex_unlock(&tls_lock);
    if (ok) clear_slot_everywhere(idx);
    return ok;
}

int sg_core_tls_set(sg_ctx *ctx, uint32_t idx, uint32_t v) {
    uint8_t *mem = ctx->mem;
    if (idx < SG_TLS_SLOTS_INLINE) {
        sg_st32(mem, ctx->fs_base + SG_TEB_TLS_SLOTS + 4u * idx, v);
        return 1;
    }
    if (idx >= SG_TLS_SLOTS_TOTAL) return 0;
    {
        core_thread *h = core_cur(ctx);
        uint32_t x = sg_ld32(mem, ctx->fs_base + SG_TEB_TLS_EXPANSION);
        if (!x) {
            x = teb_page_alloc();
            if (!x) return 0;
            if (h) h->tls_expansion = x;
            sg_st32(mem, ctx->fs_base + SG_TEB_TLS_EXPANSION, x);
        }
        sg_st32(mem, x + 4u * (idx - SG_TLS_SLOTS_INLINE), v);
    }
    return 1;
}

/* ---- shims ----------------------------------------------------------------------------------------- */

SGC_SHIM(kernel32, TlsAlloc) { SG_RETURN_STDCALL(sg_core_tls_alloc(), 0); }

SGC_SHIM(kernel32, TlsFree) {
    uint32_t ok = (uint32_t)sg_core_tls_free(SG_ARG(0));
    if (!ok) core_set_last_error(ctx, WERR_INVALID_PARAMETER);
    SG_RETURN_STDCALL(ok, 4);
}

/* The hottest import (1,973 uses): a bounds test and one or two loads. Windows clears the last
 * error on success, which callers rely on to tell a stored 0 from a failure. */
SGC_SHIM(kernel32, TlsGetValue) {
    uint32_t idx = SG_ARG(0);
    if (SG_UNLIKELY(idx >= SG_TLS_SLOTS_TOTAL)) {
        core_set_last_error(ctx, WERR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 4);
    }
    sg_st32(mem, ctx->fs_base + SG_TEB_LAST_ERROR, 0);
    SG_RETURN_STDCALL(sg_tls_get(ctx, mem, idx), 4);
}

SGC_SHIM(kernel32, TlsSetValue) {
    int ok = sg_core_tls_set(ctx, SG_ARG(0), SG_ARG(1));
    if (!ok) core_set_last_error(ctx, WERR_INVALID_PARAMETER);
    SG_RETURN_STDCALL(ok, 8);
}
