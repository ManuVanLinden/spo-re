/*
 * sg_core.h -- runtime core: guest window, thunk region, TEB/TLS, guest threads, kernel
 * objects and waits, critical sections, and the MSVC exception-handling runtime.
 *
 * Spec: docs/design/runtime-core.md (this package) and docs/design/runtime-abi.md sections 3, 7, 9
 * and 10. The ABI header spore_guest.h declares the [runtime] entry
 * points generated code calls; this header is what OTHER RUNTIME PACKAGES call.
 *
 * Ownership (which [runtime] symbols of spore_guest.h this library defines):
 *   sg_thunk_fn[], sg_thunk_register, sg_call_guest, sg_self, sg_thread_create, sg_eh_resume,
 *   sg_seh_raise, sg_bad_resume, sg_return_mismatch, sg_misaligned_atomic, sg_unhandled_insn,
 *   sg_int3, sg_divide_error, sg_cpuid, sg_rdtsc, sg_ldf80, sg_stf80, sg_set_mxcsr, sg_fxsave.
 * NOT defined here (the image linker generates them): sg_dispatch_bucket[],
 *   sg_dispatch_entries[], sg_miss_hook, sg_resume_points[], sg_resume_point_count.
 * NOT defined here (guest heap, runtime/win32): hle_guest_alloc.
 *
 * No game code. Plain C11, clang, arm64 macOS (x86-64 Linux for host-only tests).
 */
#ifndef SG_CORE_H
#define SG_CORE_H

#include <stdarg.h>
#include <stdint.h>

#include "spore_guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------ */
/* 1. Window and guest pages                                                                   */
/* ------------------------------------------------------------------------------------------ */

/* Reserves the 4 GiB + 64 KiB window (PROT_NONE). Returns the host base, or NULL. Use this only if
 * nobody else (the image loader) reserved one; sg_core_init accepts any reservation. */
uint8_t *sg_window_reserve(void);
/* Commits [a, a+n) (page-rounded) with read/write (prot: 1 = R, 3 = RW, 0 = none). 0 on success. */
int sg_window_protect(uint32_t a, uint32_t n, int prot);

/* Guest page allocator used for thread stacks and TLS expansion blocks. The default allocates
 * top-down from [SG_CORE_STACK_REGION_LO, SG_CORE_STACK_REGION_HI); the guest heap (runtime/win32) may
 * replace both hooks with VirtualAlloc/VirtualFree before the first thread is created.
 * alloc returns a 64 KiB-aligned guest address of `size` committed RW bytes, or 0. */
#define SG_CORE_STACK_REGION_LO 0x7c000000u
#define SG_CORE_STACK_REGION_HI 0x7fe00000u
typedef uint32_t (*sg_core_vm_alloc_fn)(uint32_t size);
typedef void (*sg_core_vm_free_fn)(uint32_t addr, uint32_t size);
extern sg_core_vm_alloc_fn sg_core_vm_alloc;
extern sg_core_vm_free_fn sg_core_vm_free;

/* ------------------------------------------------------------------------------------------ */
/* 2. Initialisation                                                                           */
/* ------------------------------------------------------------------------------------------ */

/* Global init. `mem` is the window base (already reserved). Commits the thunk region (filled with
 * 0xCC, then read-only), the PEB page, and registers the runtime thunk slots 0..2. Idempotent. */
void sg_core_init(uint8_t *mem);
extern uint8_t *sg_core_mem;

/* Image TLS (runtime-abi 3.2): writes 0 into the image's TLS index variable and records the
 * template (normally SG_IMAGE_TLS_INDEX_VA, SG_IMAGE_TLS_TEMPLATE, SG_IMAGE_TLS_TEMPLATE_SIZE).
 * Call after the loader mapped .tls and before the first thread (attach_main) is created.
 * Threads created before this get no __declspec(thread) block. size <= 96 bytes. */
void sg_core_image_tls(uint32_t index_va, uint32_t template_va, uint32_t template_size, uint32_t zero_fill);

/* Makes the calling host thread the guest main thread: allocates its guest stack (0 = 1 MiB),
 * TEB and TLS block, sets sg_self and returns it. ctx is reset (fpu cw 0x27f, mxcsr 0x1f80,
 * esp = stack top, fs_base = TEB). Also used by tests for each host thread they drive. */
sg_thread *sg_core_attach_main(uint32_t stack_bytes);
/* Releases what attach_main allocated (tests). */
void sg_core_detach_thread(sg_thread *t);

/* ------------------------------------------------------------------------------------------ */
/* 3. Thunks and calls                                                                         */
/* ------------------------------------------------------------------------------------------ */

/* Debug name of a thunk address (or NULL). */
const char *sg_thunk_name(uint32_t guest);

/* sg_call_guest with explicit checking control (the EH runtime calls funclets with
 * SG_CORE_CALL_FUNCLET, which skips the callee-saved register check because funclets clobber
 * ebx/esi/edi, as msvcr90's _CallSettingFrame allows). */
enum { SG_CORE_CALL_CHECKED = 0, SG_CORE_CALL_FUNCLET = 1 };
uint32_t sg_core_call(sg_ctx *ctx, uint32_t target, enum sg_cc cc, unsigned nargs, const uint32_t *args,
                      int mode);
/* Developer checks (callee-saved registers and stack clean-up in sg_call_guest). Default 1. */
extern int sg_core_checks;

/* ------------------------------------------------------------------------------------------ */
/* 4. Fatal errors, logging                                                                     */
/* ------------------------------------------------------------------------------------------ */

/* Prints `msg`, the thread, the guest registers and a guest ebp backtrace, then calls
 * sg_core_fatal_hook (if set; a test may longjmp out of it) and aborts. */
SG_NORETURN void sg_core_fatal(sg_ctx *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern void (*sg_core_fatal_hook)(sg_ctx *ctx, const char *msg);
/* Logging (stderr by default). Levels as hle: 0 error, 1 warn, 2 info, 3 trace. Env SPORE_CORE_LOG. */
extern int sg_core_log_level;
void sg_core_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern void (*sg_core_log_sink)(int level, const char *line);
/* Installs SIGBUS/SIGSEGV/SIGILL/SIGFPE handlers that report the guest address and abort
 * (faults are not turned into guest exceptions). */
void sg_core_install_fault_handlers(void);
/* Writes "guest backtrace" lines for ctx (ebp chain, bounded by the thread's stack) via log. */
void sg_core_backtrace(sg_ctx *ctx, int level);

/* ------------------------------------------------------------------------------------------ */
/* 5. Threads, handles, kernel objects                                                         */
/* ------------------------------------------------------------------------------------------ */

/* Handle table shared by every runtime package (kernel objects here; files, find handles,
 * registry keys in runtime/win32). Values are Windows-like: multiples of 4 from 0x100. */
enum sg_handle_type {
    SG_H_NONE = 0, SG_H_THREAD, SG_H_EVENT, SG_H_SEMAPHORE, SG_H_MUTEX,
    SG_H_EXTERNAL = 16 /* + any tag >= 16: owned by another package */
};
#define SG_PSEUDO_CURRENT_PROCESS 0xffffffffu
#define SG_PSEUDO_CURRENT_THREAD 0xfffffffeu
/* External objects: `close` runs when the last handle is closed. Returns the handle or 0. */
uint32_t sg_handle_alloc_external(int tag, void *obj, void (*close)(void *obj));
/* Returns the object of an external handle with this tag, or NULL. */
void *sg_handle_external(uint32_t h, int tag);
/* Type of a handle (SG_H_NONE if invalid). */
int sg_handle_type(uint32_t h);
/* CloseHandle back end: 1 on success, 0 (and ERROR_INVALID_HANDLE) otherwise. */
int sg_handle_close(uint32_t h);

/* Handle for a guest thread (one reference to its thread object). */
uint32_t sg_core_thread_handle(sg_thread *t);
/* Starts a thread created suspended (ResumeThread back end); returns the previous count. */
int sg_core_resume_thread(sg_thread *t);
/* Waits for the thread to exit (host-side join; tests, shutdown). Returns its exit code. */
uint32_t sg_core_join(sg_thread *t);
/* ExitThread back end: never returns. Runs host cleanup records, signals the thread object. */
SG_NORETURN void sg_core_exit_thread(sg_ctx *ctx, uint32_t code);
/* Thread name for logs ("main", "Job Thread 2", ...); set by SetThreadName (0x406D1388). */
const char *sg_core_thread_name(const sg_thread *t);
/* Number of live guest threads. */
unsigned sg_core_thread_count(void);
/* Live guest threads, "name (entry 0x..[, waiting])" joined by "; ", into buf; returns the count. */
unsigned sg_core_thread_list(char *buf, size_t cap);

/* WaitForMultipleObjectsEx back end: WAIT_OBJECT_0 + i, WAIT_TIMEOUT (0x102),
 * WAIT_IO_COMPLETION (0xC0) or WAIT_FAILED (0xFFFFFFFF). Full barrier. */
uint32_t sg_core_wait(sg_ctx *ctx, unsigned n, const uint32_t *handles, int wait_all, uint32_t timeout_ms,
                      int alertable);
/* Sleep/SleepEx back end. Returns 0, or 0xC0 when an APC ran (alertable). Full barrier. */
uint32_t sg_core_sleep(sg_ctx *ctx, uint32_t ms, int alertable);

/* Event API for other runtime packages: the directory
 * watcher, the audio path and the window layer signal guest-visible events from host threads that
 * are not guest threads (FSEvents queue, CoreAudio render thread, AppKit) without building a
 * scratch guest context. The handles are ordinary SG_H_EVENT handles in the shared table, so the
 * guest waits on them with WaitForSingleObject/WaitForMultipleObjectsEx and closes them with
 * CloseHandle (sg_handle_close). Every call is a full barrier and is callable from any thread.
 *   create: 0 on failure.  set/reset: 1, or 0 if h is not an event.
 *   wait: WAIT_OBJECT_0 (0), WAIT_TIMEOUT (0x102) or WAIT_FAILED (0xFFFFFFFF); timeout in ms,
 *         0xFFFFFFFF = infinite. On a guest thread it is sg_core_wait (not alertable); on any other
 *         host thread it blocks on a private condition variable woken by every set. An auto-reset
 *         event is reset by the waiter that takes it. */
uint32_t sg_core_event_create(int manual_reset, int initial_state);
int sg_core_event_set(uint32_t h);
int sg_core_event_reset(uint32_t h);
uint32_t sg_core_event_wait(uint32_t h, uint32_t timeout_ms);

/* Main-thread stall watchdog (core_stall.c). A back end calls beat() once per frame from the
 * thread that runs the game loop; the first call names that thread and starts a monitor thread.
 *   fault(what): a fatal device state (e.g. the Vulkan device was lost; callable from any thread).
 *       The monitor aborts within the grace period (SPORE_STALL_FAULT_SECONDS, default 3 s), whether
 *       or not the main thread still beats, because nothing after a device loss can render again.
 *   A stall without a fault (no beat for SPORE_STALL_SECONDS, default 0 = off) aborts too, once
 *   SPORE_STALL_MIN_FRAMES (default 600) frames have been beaten (loading screens stall early).
 * The abort prints "core: stall watchdog: ..." with the time since the last beat, the fault, the
 * reporter's lines (set_reporter: e.g. the last D3D9 calls and device statistics), then sends
 * SIGABRT to the main thread so its guest call stack is printed by the boot signal reporter (core_boot_diag.c).
 * config(): overrides the two limits (seconds; negative leaves a value unchanged) for tests. */
void sg_core_stall_beat(void);
void sg_core_stall_fault(const char *what);
void sg_core_stall_set_reporter(void (*fn)(int fd));
void sg_core_stall_config(double stall_seconds, double fault_grace_seconds);
void sg_core_stall_suspend(int on); /* 1 while the window is inactive: the stall check pauses; 0 restarts its clock */

/* Full barrier used by every synchronization shim (runtime-core.md 6.4). */
static inline void sg_core_full_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* ------------------------------------------------------------------------------------------ */
/* 6. TLS                                                                                       */
/* ------------------------------------------------------------------------------------------ */

uint32_t sg_core_tls_alloc(void);                          /* TLS_OUT_OF_INDEXES = 0xFFFFFFFF */
int sg_core_tls_free(uint32_t idx);
int sg_core_tls_set(sg_ctx *ctx, uint32_t idx, uint32_t v); /* 0 = invalid index */

/* ------------------------------------------------------------------------------------------ */
/* 7. Exception handling                                                                        */
/* ------------------------------------------------------------------------------------------ */

#define SG_EXC_CXX 0xe06d7363u               /* 'msc' | 0xE0000000 */
#define SG_EXC_CXX_MAGIC1 0x19930520u        /* ExceptionInformation[0] */
#define SG_EXC_SET_THREAD_NAME 0x406d1388u
#define SG_STATUS_LONGJUMP 0x80000026u
#define SG_STATUS_UNWIND 0xc0000027u
#define SG_STATUS_NONCONTINUABLE 0xc0000025u
#define SG_EXC_NONCONTINUABLE 0x1u
#define SG_EXC_UNWINDING 0x2u
#define SG_EXC_EXIT_UNWIND 0x4u
#define SG_EXC_STACK_INVALID 0x8u
#define SG_EXC_NESTED_CALL 0x10u
#define SG_END_OF_CHAIN 0xffffffffu

/* RtlUnwind / _global_unwind2: calls every handler above `target_rn` with the unwinding flag and
 * pops each one from fs:[0]. `rec` is a guest EXCEPTION_RECORD, or 0 for a fresh STATUS_UNWIND
 * record. target_rn == SG_END_OF_CHAIN unwinds everything (EXIT_UNWIND). */
void sg_seh_unwind(sg_ctx *ctx, uint32_t target_rn, uint32_t rec);

/* Host cleanup records (runtime-abi 10.4): a shim that calls guest code while holding host
 * resources links one; sg_eh_resume and ExitThread run every record that lives below the target
 * landing on the host stack. The record must be a local of the shim's host frame. */
typedef struct sg_cleanup {
    struct sg_cleanup *prev;
    void (*fn)(struct sg_cleanup *self);
    int barrier; /* 1: unwinding through it is fatal (runtime-abi 10.4 "barrier") */
    const char *what;
} sg_cleanup;
void sg_cleanup_push(sg_ctx *ctx, sg_cleanup *c);
void sg_cleanup_pop(sg_ctx *ctx, sg_cleanup *c);

/* Called when an exception reaches the end of the fs:[0] chain, after logging. Default: abort.
 * Tests may longjmp out. */
extern void (*sg_core_unhandled_hook)(sg_ctx *ctx, uint32_t rec);

/* Resume-point lookup used by sg_eh_resume: the landing owner of `target`, or 0. Uses the
 * generated sg_resume_points[] unless a test installs its own table. */
uint32_t sg_core_resume_owner(uint32_t target);
extern const sg_resume_point *sg_core_resume_table;  /* NULL = use sg_resume_points */
extern uint32_t sg_core_resume_table_count;

/* ------------------------------------------------------------------------------------------ */
/* 8. Shims and their registration through runtime/hle's import table                                   */
/* ------------------------------------------------------------------------------------------ */

/* Shims defined by the core use the prefix sgc_ (so they never clash with runtime/hle's generated
 * sg_imp_ stubs). The table maps the HLE import symbol name to the core function. */
#define SGC_SHIM(dll, name) void sgc_##dll##_##name(sg_ctx *SG_RESTRICT ctx, uint8_t *mem)
typedef struct sg_core_shim {
    const char *symbol; /* HLE import symbol, e.g. "sg_imp_kernel32_TlsGetValue" */
    sg_func *fn;
    uint32_t cleanup;   /* bytes the shim pops (stdcall/thiscall), 0 for cdecl; checked at registration */
} sg_core_shim;
extern const sg_core_shim sg_core_shims[];
extern const unsigned sg_core_shim_count;
/* Looks up a core shim by HLE import symbol name (NULL if none). */
sg_func *sg_core_shim_lookup(const char *symbol);

/* Registration glue (runtime/core/src/core_hle_glue.c, links against runtime/hle): replaces
 * hle_imports[i].fn and hle_dynamics[i].fn for every symbol in sg_core_shims whose clean-up
 * bytes agree with runtime/hle's table (a disagreement is logged and the entry left alone). Call it
 * BEFORE hle_install_imports(mem). Returns the number of imports overridden; *mismatches (may be
 * NULL) receives the number refused. See runtime-core.md 3.4 for the generator patch that makes
 * direct SG_CALL(sg_imp_...) calls honour the override too. */
unsigned sg_core_register_hle_shims(unsigned *mismatches);

/* CloseHandle is shared with runtime/win32, whose file handles live in its own table
 * (0x00A00004...). Wire both directions, whichever CloseHandle shim is registered last:
 *     w32_close_foreign = sg_core_close_foreign;   (win32's CloseHandle -> core objects)
 *     sg_core_close_hook = <win32's closer>;       (core's CloseHandle -> win32 handles)
 * sg_core_close_foreign returns 1, or 0 with ERROR_INVALID_HANDLE. */
int sg_core_close_foreign(sg_ctx *ctx, uint32_t h);
extern int (*sg_core_close_hook)(sg_ctx *ctx, uint32_t h);

/* ------------------------------------------------------------------------------------------ */
/* 9. Hooks called by the image linker's main (runtime/image)                             */
/* ------------------------------------------------------------------------------------------ */

/* After the thunk region exists, before hle_install_imports: registers the core's shims and
 * wires runtime/win32's w32_close_foreign (if linked) to sg_core_close_foreign. */
void sg_core_startup(uint8_t *mem);
/* --run: attaches the guest main thread (1 MiB stack) and calls the PE entry (cdecl, no args). */
int sg_boot(uint8_t *mem, uint32_t entry, int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif /* SG_CORE_H */
