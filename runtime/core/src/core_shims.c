/* core_shims.c -- the table of import shims the runtime core provides, keyed by HLE import symbol
 * name (runtime-core.md section 7). runtime/core/src/core_hle_glue.c installs it into runtime/hle's table. */
#include "core_internal.h"

#define CORE_SHIMS(X)                                                        \
    /* TLS (core_teb.c) */                                                  \
    X(kernel32, TlsAlloc, "sg_imp_kernel32_TlsAlloc", 0)                        \
    X(kernel32, TlsFree, "sg_imp_kernel32_TlsFree", 4)                          \
    X(kernel32, TlsGetValue, "sg_imp_kernel32_TlsGetValue", 4)                  \
    X(kernel32, TlsSetValue, "sg_imp_kernel32_TlsSetValue", 8)                  \
    /* threads (core_thread.c) */                                           \
    X(kernel32, CreateThread, "sg_imp_kernel32_CreateThread", 24)                \
    X(msvcr90, _beginthreadex, "sg_imp_msvcr90__beginthreadex", 0)              \
    X(msvcr90, _endthreadex, "sg_imp_msvcr90__endthreadex", 0)                  \
    X(kernel32, ExitThread, "sg_imp_kernel32_ExitThread", 4)                    \
    X(kernel32, ResumeThread, "sg_imp_kernel32_ResumeThread", 4)                \
    X(kernel32, GetExitCodeThread, "sg_imp_kernel32_GetExitCodeThread", 8)      \
    X(kernel32, GetCurrentThread, "sg_imp_kernel32_GetCurrentThread", 0)        \
    X(kernel32, GetCurrentThreadId, "sg_imp_kernel32_GetCurrentThreadId", 0)    \
    X(kernel32, GetCurrentProcess, "sg_imp_kernel32_GetCurrentProcess", 0)      \
    X(kernel32, GetCurrentProcessId, "sg_imp_kernel32_GetCurrentProcessId", 0)  \
    X(kernel32, SetThreadPriority, "sg_imp_kernel32_SetThreadPriority", 8)      \
    X(kernel32, GetThreadPriority, "sg_imp_kernel32_GetThreadPriority", 4)      \
    X(kernel32, SetThreadAffinityMask, "sg_imp_kernel32_SetThreadAffinityMask", 8) \
    X(kernel32, SetThreadIdealProcessor, "sg_imp_kernel32_SetThreadIdealProcessor", 8) \
    X(kernel32, SetPriorityClass, "sg_imp_kernel32_SetPriorityClass", 8)        \
    X(kernel32, GetPriorityClass, "sg_imp_kernel32_GetPriorityClass", 4)        \
    X(kernel32, SetProcessAffinityMask, "sg_imp_kernel32_SetProcessAffinityMask", 8) \
    X(kernel32, GetProcessAffinityMask, "sg_imp_kernel32_GetProcessAffinityMask", 12) \
    /* synchronization (core_thread.c) */                                   \
    X(kernel32, InitializeCriticalSection, "sg_imp_kernel32_InitializeCriticalSection", 4) \
    X(kernel32, InitializeCriticalSectionAndSpinCount, "sg_imp_kernel32_InitializeCriticalSectionAndSpinCount", 8) \
    X(kernel32, DeleteCriticalSection, "sg_imp_kernel32_DeleteCriticalSection", 4) \
    X(kernel32, EnterCriticalSection, "sg_imp_kernel32_EnterCriticalSection", 4) \
    X(kernel32, LeaveCriticalSection, "sg_imp_kernel32_LeaveCriticalSection", 4) \
    X(kernel32, TryEnterCriticalSection, "sg_imp_kernel32_TryEnterCriticalSection", 4) \
    X(kernel32, CreateEventA, "sg_imp_kernel32_CreateEventA", 16)                \
    X(kernel32, SetEvent, "sg_imp_kernel32_SetEvent", 4)                        \
    X(kernel32, ResetEvent, "sg_imp_kernel32_ResetEvent", 4)                    \
    X(kernel32, CreateSemaphoreA, "sg_imp_kernel32_CreateSemaphoreA", 16)        \
    X(kernel32, ReleaseSemaphore, "sg_imp_kernel32_ReleaseSemaphore", 12)        \
    X(kernel32, CreateMutexA, "sg_imp_kernel32_CreateMutexA", 12)                \
    X(kernel32, ReleaseMutex, "sg_imp_kernel32_ReleaseMutex", 4)                \
    X(kernel32, CloseHandle, "sg_imp_kernel32_CloseHandle", 4)                  \
    X(kernel32, DuplicateHandle, "sg_imp_kernel32_DuplicateHandle", 28)          \
    X(kernel32, WaitForSingleObject, "sg_imp_kernel32_WaitForSingleObject", 8)  \
    X(kernel32, WaitForSingleObjectEx, "sg_imp_kernel32_WaitForSingleObjectEx", 12) \
    X(kernel32, WaitForMultipleObjects, "sg_imp_kernel32_WaitForMultipleObjects", 16) \
    X(kernel32, WaitForMultipleObjectsEx, "sg_imp_kernel32_WaitForMultipleObjectsEx", 20) \
    X(kernel32, Sleep, "sg_imp_kernel32_Sleep", 4)                              \
    X(kernel32, SleepEx, "sg_imp_kernel32_SleepEx", 8)                          \
    X(kernel32, SwitchToThread, "sg_imp_kernel32_SwitchToThread", 0)            \
    X(kernel32, QueueUserAPC, "sg_imp_kernel32_QueueUserAPC", 12)                \
    /* exception handling (core_eh.c) */                                    \
    X(msvcr90, __CxxFrameHandler3, "sg_imp_msvcr90___CxxFrameHandler3", 0)      \
    X(msvcr90, _CxxThrowException, "sg_imp_msvcr90__CxxThrowException", 8)      \
    X(msvcr90, _except_handler3, "sg_imp_msvcr90__except_handler3", 0)          \
    X(msvcr90, _except_handler4_common, "sg_imp_msvcr90__except_handler4_common", 0) \
    X(msvcr90, _setjmp3, "sg_imp_msvcr90__setjmp3", 0)                          \
    X(msvcr90, longjmp, "sg_imp_msvcr90_longjmp", 0)                            \
    X(msvcr90, _XcptFilter, "sg_imp_msvcr90__XcptFilter", 0)                    \
    X(msvcr90, _terminate, "sg_imp_msvcr90__terminate__YAXXZ", 0)               \
    X(kernel32, RaiseException, "sg_imp_kernel32_RaiseException", 16)            \
    X(kernel32, SetUnhandledExceptionFilter, "sg_imp_kernel32_SetUnhandledExceptionFilter", 4) \
    X(kernel32, UnhandledExceptionFilter, "sg_imp_kernel32_UnhandledExceptionFilter", 4)

#define DECL(dll, name, sym, n) SGC_SHIM(dll, name);
CORE_SHIMS(DECL)
#undef DECL

#define ROW(dll, name, sym, n) {sym, sgc_##dll##_##name, n},
const sg_core_shim sg_core_shims[] = {CORE_SHIMS(ROW)};
#undef ROW
const unsigned sg_core_shim_count = sizeof sg_core_shims / sizeof sg_core_shims[0];

sg_func *sg_core_shim_lookup(const char *symbol) {
    unsigned i;
    for (i = 0; i < sg_core_shim_count; i++)
        if (!strcmp(sg_core_shims[i].symbol, symbol)) return sg_core_shims[i].fn;
    return NULL;
}
