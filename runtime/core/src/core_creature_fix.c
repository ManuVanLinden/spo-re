/* SPDX-License-Identifier: GPL-3.0-only
 * Native adaptation of the CreatureStageFunction1 null-target fix documented by
 * SporeCrashFix, Copyright (C) 2021 Rosalie Wanders.
 * https://github.com/Rosalie241/SporeCrashFix (dllmain.cpp)
 *
 * The cdecl routine at guest 0x00d65be0 takes three arguments. When a creature's
 * buddy target has disappeared, its third argument can be NULL, but the routine
 * later dereferences it at 0x00d65d23. Return true, as SporeCrashFix does, before
 * touching any actor state. A present target follows the original translation.
 */
#include <stdatomic.h>
#include <stdio.h>
#include "spore_guest.h"

int sg_creature_buddy_guard(sg_ctx *ctx, uint8_t *mem) {
    if (SG_ARG(2) != 0) return 0;
    static _Atomic unsigned prevented;
    unsigned n = atomic_fetch_add_explicit(&prevented, 1, memory_order_relaxed) + 1;
    if (n <= 4)
        fprintf(stderr, "creature-fix: skipped null buddy target at 0x00d65be0 (count %u)\n", n);
    ctx->eax = 1;
    ctx->eip = sg_ld32(mem, ctx->esp);
    ctx->esp += 4; /* cdecl: the caller removes the three arguments */
    return 1;
}
