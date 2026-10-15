/* w32_process.c -- process-level imports (design doc section 18): TerminateProcess,
 * GetProcessTimes, _searchenv/_wsearchenv, _spawnv/_wspawnv and _getch.
 *
 * Call sites [obs: disassembly / translated call sites]:
 *   0x11e1722  TerminateProcess(GetCurrentProcess(), STATUS_STACK_BUFFER_OVERRUN) in msvcr90's
 *              __report_gsfailure (static CRT code in the exe); 0x91e9b9 another fatal path;
 *   0xa5d859, 0xa5d8a4  GetProcessTimes (CPU-time profiling);
 *   0x935d70   wrapper: _wsearchenv(name, var ? var : L"PATH", buf), returns buf[0] != 0
 *              (0x935d89; the narrow twin calls _searchenv at 0x935dbf);
 *   _spawnv/_wspawnv are reached through jmp thunks (tail calls); nothing on the start-up path.
 * Nothing here starts a host process: the game never needs to run another program on macOS, and
 * running Windows binaries is out of scope.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "w32_int.h"
#include "w32_dirwatch.h"

#define PSEUDO_CURRENT_PROCESS 0xffffffffu

/* ---- TerminateProcess ------------------------------------------------------------------------- */

/* TerminateProcess(hProcess, uExitCode) [doc: TerminateProcess]: for the current process (the
 * pseudo handle -1, which GetCurrentProcess returns) it ends the process with the code at once:
 * no atexit handlers or DLL detach run. We flush the guest stdio streams and the host's own
 * stdio/log first, then call w32_exit_hook(code), the same exit path exit/_exit use (the boot
 * harness may catch it). Other process handles cannot exist here (no CreateProcess), so they
 * fail with ERROR_INVALID_HANDLE. */
W32_IMPORT(kernel32, TerminateProcess) {
    uint32_t h = SG_ARG(0), code = SG_ARG(1);
    if (h != PSEUDO_CURRENT_PROCESS) {
        hle_log(HLE_LOG_WARN, "TerminateProcess(0x%08x, 0x%x): not the current process", h, code);
        hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE);
        SG_RETURN_STDCALL(0, 8);
    }
    hle_log(HLE_LOG_ERROR, "TerminateProcess(current, 0x%08x) called from guest 0x%08x", code, sg_ld32(mem, ctx->esp));
    w32_stdio_flush_all();
    fflush(NULL);
    w32_exit_hook((int)code);
    /* a hook that returns (tests) lets the call return FALSE, which a real TerminateProcess on
     * the current process never does */
    hle_set_last_error(ctx, mem, W32_ERROR_ACCESS_DENIED);
    SG_RETURN_STDCALL(0, 8);
}

/* ---- GetProcessTimes ------------------------------------------------------------------------- */

static uint64_t process_start_filetime(void) {
    static uint64_t ft;
    if (!ft) {
        struct kinfo_proc kp;
        size_t len = sizeof kp;
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
        if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && len)
            ft = w32_filetime_from_unix(kp.kp_proc.p_starttime.tv_sec, (long)kp.kp_proc.p_starttime.tv_usec * 1000);
        else {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            ft = w32_filetime_from_unix(tv.tv_sec, (long)tv.tv_usec * 1000);
        }
    }
    return ft;
}

static void st_ft(uint8_t *mem, uint32_t a, uint64_t v) {
    if (!a) return;
    sg_st32(mem, a, (uint32_t)v);
    sg_st32(mem, a + 4, (uint32_t)(v >> 32));
}

/* GetProcessTimes(hProcess, lpCreationTime, lpExitTime, lpKernelTime, lpUserTime) [doc]:
 * creation time as a FILETIME (UTC), exit time undefined while running (we write 0), kernel
 * and user time in 100 ns units summed over all threads (getrusage(RUSAGE_SELF)). */
W32_IMPORT(kernel32, GetProcessTimes) {
    uint32_t h = SG_ARG(0);
    if (h != PSEUDO_CURRENT_PROCESS) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(0, 20); }
    if (!SG_ARG(1) || !SG_ARG(2) || !SG_ARG(3) || !SG_ARG(4)) { hle_set_last_error(ctx, mem, W32_ERROR_NOACCESS); SG_RETURN_STDCALL(0, 20); }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    st_ft(mem, SG_ARG(1), process_start_filetime());
    st_ft(mem, SG_ARG(2), 0);
    st_ft(mem, SG_ARG(3), (uint64_t)ru.ru_stime.tv_sec * 10000000u + (uint64_t)ru.ru_stime.tv_usec * 10u);
    st_ft(mem, SG_ARG(4), (uint64_t)ru.ru_utime.tv_sec * 10000000u + (uint64_t)ru.ru_utime.tv_usec * 10u);
    SG_RETURN_STDCALL(1, 20);
}

/* ---- _searchenv ---------------------------------------------------------------------------- */

/* msvcr90 _searchenv(filename, varname, pathname) [doc: _searchenv, _wsearchenv]: the file in
 * the current directory first, then in each ';'-separated directory of the environment
 * variable; pathname (at least _MAX_PATH = 260 units) gets the full path, or "" with errno =
 * ENOENT when not found. A path longer than _MAX_PATH gives "" and ERANGE. Quotes around a
 * directory are removed, as msvcr90 does. */
static void searchenv(uint8_t *mem, uint32_t fname, uint32_t var, uint32_t out, int wide) {
    char name[W32_PATH_CAP], varname[256], full[W32_PATH_CAP], host[W32_PATH_CAP];
    int fits, ex = 0;
    if (!out) { hle_set_errno(mem, MSVC_EINVAL); return; }
    if (wide) sg_st16(mem, out, 0); else mem[out] = 0;
    if (!fname || !var) { hle_set_errno(mem, MSVC_EINVAL); return; }
    if (wide) { w32_wstr_get(mem, fname, name, sizeof name); w32_wstr_get(mem, var, varname, sizeof varname); }
    else { w32_astr_get(mem, fname, name, sizeof name); w32_astr_get(mem, var, varname, sizeof varname); }
    if (!*name) { hle_set_errno(mem, MSVC_ENOENT); return; }
    /* 1: current directory (and absolute names) */
    if (w32_path_full(name, full, sizeof full) >= 0 && !w32_path_to_host(full, host, sizeof host, &ex, NULL) && ex) goto found;
    const char *list = w32_env_get(varname);
    hle_log(HLE_LOG_INFO, "_searchenv(\"%s\", \"%s\"): %s", name, varname, list ? list : "(variable unset)");
    if (list) {
        const char *p = list;
        while (*p) {
            char dir[W32_PATH_CAP];
            size_t k = 0;
            while (*p && *p != ';') { if (*p != '"' && k + 1 < sizeof dir) dir[k++] = *p; p++; }
            if (*p == ';') p++;
            dir[k] = 0;
            if (!k) continue;
            char cand[W32_PATH_CAP * 2];
            snprintf(cand, sizeof cand, "%s%s%s", dir, (dir[k - 1] == '\\' || dir[k - 1] == '/') ? "" : "\\", name);
            ex = 0;
            if (w32_path_full(cand, full, sizeof full) >= 0 && !w32_path_to_host(full, host, sizeof host, &ex, NULL) && ex)
                goto found;
        }
    }
    hle_set_errno(mem, MSVC_ENOENT);
    return;
found:
    if (w32_utf8_units(full, wide) + 1 > W32_MAX_PATH) { hle_set_errno(mem, MSVC_ERANGE); return; }
    w32_put_str(mem, out, W32_MAX_PATH, full, wide, &fits);
}
W32_IMPORT(msvcr90, _searchenv) { searchenv(mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), 0); SG_RET(0); }
W32_IMPORT(msvcr90, _wsearchenv) { searchenv(mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), 1); SG_RET(0); }

/* ---- _spawnv ------------------------------------------------------------------------------- */

/* _spawnv(mode, cmdname, argv) / _wspawnv [doc: _spawn functions]: we never start programs
 * (they would be Windows binaries). Returns -1 with errno ENOENT, which callers handle as
 * "file not found", and logs the command line. */
static uint32_t spawnv(uint8_t *mem, uint32_t mode, uint32_t cmd, uint32_t argv, int wide) {
    char line[2048], a[W32_PATH_CAP];
    size_t n = 0;
    line[0] = 0;
    if (cmd) {
        if (wide) w32_wstr_get(mem, cmd, a, sizeof a); else w32_astr_get(mem, cmd, a, sizeof a);
        n += (size_t)snprintf(line, sizeof line, "%s", a);
    }
    for (uint32_t i = 1; argv && i < 32 && n < sizeof line - 1; i++) {
        uint32_t p = sg_ld32(mem, argv + 4 * i);
        if (!p) break;
        if (wide) w32_wstr_get(mem, p, a, sizeof a); else w32_astr_get(mem, p, a, sizeof a);
        n += (size_t)snprintf(line + n, sizeof line - n, " %s", a);
    }
    hle_log(HLE_LOG_WARN, "_%sspawnv(mode %u, \"%s\"): starting programs is not supported; returning -1/ENOENT",
            wide ? "w" : "", mode, line);
    hle_set_errno(mem, MSVC_ENOENT);
    return 0xffffffffu;
}
W32_IMPORT(msvcr90, _spawnv) { SG_RETURN_CDECL(spawnv(mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), 0)); }
W32_IMPORT(msvcr90, _wspawnv) { SG_RETURN_CDECL(spawnv(mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), 1)); }

/* ---- _getch -------------------------------------------------------------------------------- */

/* _getch() [doc: _getch]: reads a key from the console without echo. This GUI process has no
 * console (msvcr90 then fails to open CONIN$ and returns EOF, -1), so it returns EOF at once
 * instead of blocking. */
W32_IMPORT(msvcr90, _getch) {
    static int logged;
    if (!logged++) hle_log(HLE_LOG_INFO, "_getch(): no console; EOF");
    SG_RETURN_CDECL(0xffffffffu);
}
