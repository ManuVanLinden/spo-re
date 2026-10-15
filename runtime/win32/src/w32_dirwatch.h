/* w32_dirwatch.h -- host-level entry points of w32_dirwatch.c, w32_nls.c and
 * w32_version.c, for the other runtime/win32 files and the tests. Design doc sections 15-19. */
#ifndef SPORE_W32_DIRWATCH_H
#define SPORE_W32_DIRWATCH_H

#include <stddef.h>
#include <stdint.h>

#include "spore_guest.h"

/* Kernel-object operations the watcher needs (the core's events). The default calls the runtime
 * core's shims; the unit tests install their own. event_create returns a handle or 0. */
typedef struct w32_dirwatch_ops {
    uint32_t (*event_create)(int manual_reset);
    int (*event_set)(uint32_t h);
    int (*event_reset)(uint32_t h);
    int (*handle_close)(uint32_t h);
} w32_dirwatch_ops;
extern w32_dirwatch_ops w32_dirwatch_sync;
/* FSEvents latency in milliseconds (default 30). SPORE_W32_DIRWATCH_POLL=1 forces polling. */
extern unsigned w32_dirwatch_latency_ms;
unsigned w32_dirwatch_count(void);          /* live watches */
void w32_dirwatch_cancel(uint32_t hdir);    /* CancelIo on a directory handle */
void w32_dirwatch_close(uint32_t hdir);     /* CloseHandle on a directory handle */
uint32_t w32_read_dir_changes(sg_ctx *ctx, uint8_t *mem, uint32_t hdir, uint32_t buf, uint32_t len, uint32_t subtree,
                              uint32_t filter, uint32_t pbytes, uint32_t ov, uint32_t routine);

/* NLS (w32_nls.c) */
uint32_t w32_nls_upper(uint32_t c);
uint32_t w32_nls_lower(uint32_t c);
int w32_nls_compare(uint32_t lcid, uint32_t flags, const uint16_t *a, int32_t na, const uint16_t *b, int32_t nb);
size_t w32_nls_sortkey(uint32_t lcid, uint32_t flags, const uint16_t *s, int32_t n, uint8_t *out, size_t cap);

/* VerQueryValue walk (w32_version.c): 1 with the guest pointer and length, or 0 with *err. */
int w32_ver_query(uint8_t *mem, uint32_t block, const char *sub, int wide, uint32_t *ptr, uint32_t *len, uint32_t *err);

#endif
