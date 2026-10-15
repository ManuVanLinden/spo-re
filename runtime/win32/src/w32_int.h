/* w32_int.h -- private to runtime/win32. */
#ifndef SPORE_W32_INT_H
#define SPORE_W32_INT_H

#include <stdarg.h>

#include "hle.h"
#include "w32.h"

/* Win32 error codes (winerror.h). */
enum {
    W32_ERROR_SUCCESS = 0, W32_ERROR_INVALID_FUNCTION = 1, W32_ERROR_FILE_NOT_FOUND = 2,
    W32_ERROR_PATH_NOT_FOUND = 3, W32_ERROR_TOO_MANY_OPEN_FILES = 4, W32_ERROR_ACCESS_DENIED = 5,
    W32_ERROR_INVALID_HANDLE = 6, W32_ERROR_NOT_ENOUGH_MEMORY = 8, W32_ERROR_INVALID_DATA = 13,
    W32_ERROR_OUTOFMEMORY = 14, W32_ERROR_INVALID_DRIVE = 15, W32_ERROR_NOT_SAME_DEVICE = 17,
    W32_ERROR_NO_MORE_FILES = 18, W32_ERROR_WRITE_PROTECT = 19, W32_ERROR_SHARING_VIOLATION = 32,
    W32_ERROR_HANDLE_EOF = 38, W32_ERROR_NOT_SUPPORTED = 50, W32_ERROR_FILE_EXISTS = 80,
    W32_ERROR_INVALID_PARAMETER = 87, W32_ERROR_BROKEN_PIPE = 109, W32_ERROR_INSUFFICIENT_BUFFER = 122,
    W32_ERROR_INVALID_NAME = 123, W32_ERROR_MOD_NOT_FOUND = 126, W32_ERROR_PROC_NOT_FOUND = 127,
    W32_ERROR_NEGATIVE_SEEK = 131, W32_ERROR_DIR_NOT_EMPTY = 145, W32_ERROR_BAD_PATHNAME = 161,
    W32_ERROR_ALREADY_EXISTS = 183, W32_ERROR_ENVVAR_NOT_FOUND = 203, W32_ERROR_MORE_DATA = 234,
    W32_ERROR_NO_MORE_ITEMS = 259, W32_ERROR_INVALID_ADDRESS = 487, W32_ERROR_NOACCESS = 998,
    W32_ERROR_BAD_LENGTH = 24, W32_ERROR_CANTOPEN = 1011, W32_ERROR_BADKEY = 1010,
    W32_ERROR_CALL_NOT_IMPLEMENTED = 120, W32_ERROR_FILENAME_EXCED_RANGE = 206, W32_ERROR_DISK_FULL = 112, W32_ERROR_DIRECTORY = 267,
};
#define W32_INVALID_FILE_ATTRIBUTES 0xffffffffu
#define W32_INVALID_SET_FILE_POINTER 0xffffffffu

void w32_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* Module initialisers called by w32_init. */
void w32_vm_init(void);
void w32_heap_init(void);
int w32_paths_init(const w32_config *cfg);
void w32_env_init(const w32_config *cfg);
void w32_files_init(const w32_config *cfg);
void w32_stdio_init(void);
void w32_crt_after_install(uint8_t *mem);
void w32_crt_init(void);
/* __getmainargs' work: argv/envp blocks in guest memory from the current command line. */
int w32_getmainargs(uint8_t *mem, uint32_t *argc, uint32_t *argv, uint32_t *envp);
uint32_t w32_onexit_add(uint32_t fn);
unsigned w32_onexit_count(void);
uint32_t w32_crt_fmode(void);
void w32_stdio_flush_all(void);
/* Environment block as NAME=VALUE\0...; returns the bytes needed (without the final extra NUL). */
size_t w32_env_block(char *out, size_t cap, unsigned *count);

int w32_vm_is_committed(uint32_t addr, uint32_t len);

/* host errno -> Win32 error / MSVC errno */
uint32_t w32_err_from_errno(int e);
int w32_msvc_errno_from_errno(int e);

/* UTF-8 helpers */
uint32_t w32_cp1252_to_unicode(uint8_t b);
int w32_unicode_to_cp1252(uint32_t cp);          /* '?' when unmappable */
size_t w32_utf8_encode(uint32_t cp, char *out);              /* 1..4 bytes */
uint32_t w32_utf8_decode(const char **s);                     /* next code point; 0 at the end */
/* Writes the UTF-8 string into a guest buffer (UTF-16 or 1252), at most `cap` units including
 * the NUL; returns the units written without the NUL, truncating. */
uint32_t w32_put_str_trunc(uint8_t *mem, uint32_t dst, uint32_t cap, const char *s, int wide);

/* Win32 FILETIME (100 ns since 1601) from host time. */
uint64_t w32_filetime_from_unix(int64_t sec, long nsec);

/* Handle table for the file layer (w32_file.c). */
typedef struct w32_hobj w32_hobj;
uint32_t w32_handle_new(enum w32_htype type, void *obj);
void *w32_handle_get(uint32_t h, enum w32_htype type);
void *w32_handle_take(uint32_t h, enum w32_htype *type); /* removes it */

/* Paths: guest string argument (A or W) -> full Windows path (UTF-8). Returns 0 or a Win32 error. */
uint32_t w32_arg_path(const uint8_t *mem, uint32_t a, int wide, char *full, size_t cap);

/* SPORE_SKIP_INTRO (w32_skipintro.c): the offset in a package at which ReadFile serves 0 for the
 * intro-movie property, or -1 (switch off, not a package, or no such property). */
int w32_skip_intro_enabled(void);
int64_t w32_skip_intro_offset(int fd, const char *host);
int64_t w32_skip_intro_scan(int fd, const char *host);
void w32_skip_intro_reset(void); /* forgets the per-file cache (tests) */

#endif
