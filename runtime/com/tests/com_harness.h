/* com_harness.h -- host-only test harness shared by runtime/com, runtime/d3d9null and runtime/dsound
 * tests: a guest window, a thunk table, an empty dispatch table, and a translated-code-style caller. */
#ifndef COM_HARNESS_H
#define COM_HARNESS_H
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sg_com.h"

extern sg_ctx T;
extern uint8_t *M;
extern int t_fail, t_pass;
#define G_STACK_TOP 0x30100000u

void harness_init(void);
/* Calls method `slot` of guest object `obj` the way translated code does: pushes the arguments
 * right to left and `this`, loads the vtable from [obj], `call [vtbl+4*slot]` through sg_lookup
 * with a return-address check. Checks that the callee popped exactly 4+4*n bytes. */
uint32_t gcall(uint32_t obj, unsigned slot, unsigned n, ...);
uint32_t gcallv(uint32_t obj, unsigned slot, unsigned n, const uint32_t *args);
/* Guest scratch memory for out parameters and structs (bump, never freed). */
uint32_t gscratch(uint32_t size);
uint32_t gld(uint32_t a);
void gst(uint32_t a, uint32_t v);

#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b); \
    if (a_ == b_) t_pass++; else { t_fail++; fprintf(stderr, "%s:%d: CHECK_EQ failed: %s = 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
int harness_report(const char *name);
#endif
