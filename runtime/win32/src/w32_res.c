/* w32_res.c -- resources of the mapped image (version information, resource enumeration) and
 * small identity queries (computer and user names, the event log OpenSSL reports to).
 *
 * The exe's resources are read from guest memory, where the image loader mapped the image at
 * 0x400000: PE header -> data directory 2 -> the resource tree. Nothing is copied out of the
 * window. Version queries for other files fail with ERROR_RESOURCE_TYPE_NOT_FOUND, as for a
 * file without a version resource.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "w32_int.h"

#define ERROR_RESOURCE_DATA_NOT_FOUND 1812
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813
#define RT_VERSION 16

int w32_ver_query(uint8_t *mem, uint32_t block, const char *sub, int wide, uint32_t *ptr, uint32_t *len, uint32_t *err);

/* Resource directory of the module at `base` (only the exe has one here). */
static int rsrc_dir(const uint8_t *mem, uint32_t base, uint32_t *root, uint32_t *size) {
    if (!w32_vm_is_committed(base, 0x40) || sg_ld16(mem, base) != 0x5a4d) return 0;
    uint32_t pe = base + sg_ld32(mem, base + 0x3c);
    if (pe < base || pe > base + 0x1000 || sg_ld32(mem, pe) != 0x4550 || sg_ld16(mem, pe + 0x18) != 0x10b) return 0;
    if (sg_ld32(mem, pe + 0x74) < 3) return 0; /* NumberOfRvaAndSizes */
    uint32_t rva = sg_ld32(mem, pe + 0x78 + 2 * 8), sz = sg_ld32(mem, pe + 0x78 + 2 * 8 + 4);
    if (!rva || !sz || !w32_vm_is_committed(base + rva, 16)) return 0;
    *root = base + rva;
    *size = sz;
    return 1;
}

/* Finds the entry with integer id `id` (or the first entry if id == ~0) in a directory. */
static int dir_find(const uint8_t *mem, uint32_t root, uint32_t dir, uint32_t id, uint32_t *child, int *is_dir) {
    uint32_t named = sg_ld16(mem, dir + 12), ids = sg_ld16(mem, dir + 14);
    for (uint32_t i = 0; i < named + ids; i++) {
        uint32_t e = dir + 16 + 8 * i, name = sg_ld32(mem, e), off = sg_ld32(mem, e + 4);
        if (id != ~0u && ((name & 0x80000000u) || name != id)) continue;
        *child = root + (off & 0x7fffffffu);
        *is_dir = (off & 0x80000000u) != 0;
        return 1;
    }
    return 0;
}

/* Data (guest address and size) of the first resource of `type` in the module. */
static int find_resource(const uint8_t *mem, uint32_t base, uint32_t type, uint32_t *data, uint32_t *size) {
    uint32_t root, rsz, d1, d2, leaf;
    int isd;
    if (!rsrc_dir(mem, base, &root, &rsz)) return 0;
    if (!dir_find(mem, root, root, type, &d1, &isd) || !isd) return 0;
    if (!dir_find(mem, root, d1, ~0u, &d2, &isd) || !isd) return 0;   /* first name */
    /* language: en-US (our locale), then neutral, then the first; the exe carries 22 languages
     * of its version block, all with the same fixed version 3.1.0.29 [obs] */
    if (!(dir_find(mem, root, d2, 0x409, &leaf, &isd) || dir_find(mem, root, d2, 0, &leaf, &isd) ||
          dir_find(mem, root, d2, ~0u, &leaf, &isd)) || isd)
        return 0;
    uint32_t rva = sg_ld32(mem, leaf), sz = sg_ld32(mem, leaf + 4);
    if (!sz || !w32_vm_is_committed(base + rva, sz)) return 0;
    *data = base + rva;
    *size = sz;
    return 1;
}

/* Which module a version query names: the exe (by its Windows path or bare name) or nothing. */
static uint32_t version_module(const uint8_t *mem, uint32_t name, int wide, uint32_t *err) {
    char raw[W32_PATH_CAP * 2], full[W32_PATH_CAP];
    if (!name) { *err = W32_ERROR_INVALID_PARAMETER; return 0; }
    if (wide) w32_wstr_get(mem, name, raw, sizeof raw); else w32_astr_get(mem, name, raw, sizeof raw);
    if (w32_path_full(raw, full, sizeof full) >= 0 && !w32_name_casecmp(full, w32_folder(W32_F_EXE))) return SG_IMAGE_BASE;
    if (!w32_name_casecmp(raw, "SporeApp.exe")) return SG_IMAGE_BASE;
    char host[W32_PATH_CAP];
    int ex = 0;
    if (w32_path_full(raw, full, sizeof full) < 0 || w32_path_to_host(full, host, sizeof host, &ex, NULL) || !ex)
        *err = W32_ERROR_FILE_NOT_FOUND;
    else
        *err = ERROR_RESOURCE_TYPE_NOT_FOUND;
    return 0;
}

/* Size reported by GetFileVersionInfoSize: the resource plus room for the ANSI copies that
 * Windows appends for the A functions (it reports roughly 3/2 of a Unicode block). */
static uint32_t version_size(uint32_t raw) { return raw + raw / 2 + 4; }

static uint32_t info_size(sg_ctx *ctx, uint8_t *mem, int wide) {
    uint32_t err = 0, data, size, h = SG_ARG(1);
    if (h) sg_st32(mem, h, 0);
    uint32_t mod = version_module(mem, SG_ARG(0), wide, &err);
    if (!mod || !find_resource(mem, mod, RT_VERSION, &data, &size)) {
        hle_set_last_error(ctx, mem, err ? err : ERROR_RESOURCE_TYPE_NOT_FOUND);
        return 0;
    }
    return version_size(size);
}
W32_IMPORT(version, GetFileVersionInfoSizeA) { SG_RETURN_STDCALL(info_size(ctx, mem, 0), 8); }

static int info_get(sg_ctx *ctx, uint8_t *mem, int wide) {
    /* (lptstrFilename, dwHandle, dwLen, lpData) */
    uint32_t err = 0, data, size, len = SG_ARG(2), out = SG_ARG(3);
    uint32_t mod = version_module(mem, SG_ARG(0), wide, &err);
    if (!mod || !find_resource(mem, mod, RT_VERSION, &data, &size)) {
        hle_set_last_error(ctx, mem, err ? err : ERROR_RESOURCE_TYPE_NOT_FOUND);
        return 0;
    }
    if (!out || !len) { hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER); return 0; }
    uint32_t n = size < len ? size : len;
    memmove(mem + out, mem + data, n);
    if (n >= 2) sg_st16(mem, out, (uint16_t)(sg_ld16(mem, out) <= n ? sg_ld16(mem, out) : n));
    return 1;
}
W32_IMPORT(version, GetFileVersionInfoA) { SG_RETURN_STDCALL(info_get(ctx, mem, 0), 16); }
W32_IMPORT(version, GetFileVersionInfoW) { SG_RETURN_STDCALL(info_get(ctx, mem, 1), 16); }

/* VS_VERSIONINFO starts with wLength, wValueLength, wType, L"VS_VERSION_INFO" (16 units with
 * the NUL), padded to 4 bytes: the VS_FIXEDFILEINFO value is at offset 40. Only the root
 * block ("\") is served; the string tables are not walked. */
static int query_value(sg_ctx *ctx, uint8_t *mem, int wide) {
    /* (pBlock, lpSubBlock, lplpBuffer, puLen) */
    char sub[256];
    uint32_t block = SG_ARG(0), pbuf = SG_ARG(2), plen = SG_ARG(3);
    if (!block || !SG_ARG(1) || !pbuf || !plen) return 0;
    if (wide) w32_wstr_get(mem, SG_ARG(1), sub, sizeof sub); else w32_astr_get(mem, SG_ARG(1), sub, sizeof sub);
    if (strcmp(sub, "\\") && strcmp(sub, "")) {
        /* string tables and VarFileInfo: w32_version.c walks the tree */
        uint32_t ptr, len, err;
        if (!w32_ver_query(mem, block, sub, wide, &ptr, &len, &err)) {
            hle_log(HLE_LOG_INFO, "VerQueryValue(\"%s\"): not found", sub);
            hle_set_last_error(ctx, mem, err);
            return 0;
        }
        sg_st32(mem, pbuf, ptr);
        sg_st32(mem, plen, len);
        return 1;
    }
    uint32_t vlen = sg_ld16(mem, block + 2);
    if (vlen < 0x34 || sg_ld32(mem, block + 40) != 0xfeef04bdu) return 0;
    sg_st32(mem, pbuf, block + 40);
    sg_st32(mem, plen, vlen);
    return 1;
}
W32_IMPORT(version, VerQueryValueA) { SG_RETURN_STDCALL(query_value(ctx, mem, 0), 16); }
W32_IMPORT(version, VerQueryValueW) { SG_RETURN_STDCALL(query_value(ctx, mem, 1), 16); }

/* EnumResourceNamesA(hModule, lpType, lpEnumFunc, lParam): integer names and types only (the
 * callback gets MAKEINTRESOURCE ids); named entries are passed as their UTF-16 offsets would
 * need a string copy, so they are skipped and logged. */
W32_IMPORT(kernel32, EnumResourceNamesA) {
    uint32_t mod = SG_ARG(0) ? SG_ARG(0) : SG_IMAGE_BASE, type = SG_ARG(1), fn = SG_ARG(2), lp = SG_ARG(3);
    uint32_t root, rsz, d1;
    int isd, any = 0;
    if (type >= 0x10000) { hle_log(HLE_LOG_WARN, "EnumResourceNamesA: named types unsupported"); type = ~0u; }
    if (mod != SG_IMAGE_BASE || !rsrc_dir(mem, mod, &root, &rsz) || type == ~0u ||
        !dir_find(mem, root, root, type, &d1, &isd) || !isd) {
        hle_set_last_error(ctx, mem, ERROR_RESOURCE_TYPE_NOT_FOUND);
        SG_RETURN_STDCALL(0, 16);
    }
    uint32_t named = sg_ld16(mem, d1 + 12), ids = sg_ld16(mem, d1 + 14);
    for (uint32_t i = 0; i < named + ids; i++) {
        uint32_t name = sg_ld32(mem, d1 + 16 + 8 * i);
        if (name & 0x80000000u) continue;
        any = 1;
        uint32_t args[4] = {mod, type, name, lp};
        if (!sg_call_guest(ctx, fn, SG_CC_STDCALL, 4, args)) break;
    }
    if (!any) hle_set_last_error(ctx, mem, ERROR_RESOURCE_DATA_NOT_FOUND);
    SG_RETURN_STDCALL(any, 16);
}

/* ---- identity ---------------------------------------------------------------------------- */

#define COMPUTER_NAME "SPORE-MAC"
#define USER_NAME "Player"

/* (lpBuffer, lpnSize): on success *lpnSize = length without NUL; too small -> needed size
 * including the NUL and ERROR_MORE_DATA (computer) / ERROR_INSUFFICIENT_BUFFER (user). */
static int name_query(sg_ctx *ctx, uint8_t *mem, uint32_t buf, uint32_t psize, const char *v, int wide,
                      uint32_t short_err, int count_nul_on_success) {
    if (!psize) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); return 0; }
    uint32_t cap = sg_ld32(mem, psize), need = w32_utf8_units(v, wide);
    if (!buf || cap < need + 1) {
        sg_st32(mem, psize, need + 1);
        hle_set_last_error(ctx, mem, short_err);
        return 0;
    }
    w32_put_str_trunc(mem, buf, cap, v, wide);
    sg_st32(mem, psize, count_nul_on_success ? need + 1 : need);
    return 1;
}
W32_IMPORT(kernel32, GetComputerNameA) {
    SG_RETURN_STDCALL(name_query(ctx, mem, SG_ARG(0), SG_ARG(1), COMPUTER_NAME, 0, 111 /* BUFFER_OVERFLOW */, 0), 8);
}
W32_IMPORT(kernel32, GetComputerNameExA) {
    /* (NameType, lpBuffer, lpnSize); every name type gets the NetBIOS-style name, the DNS
     * domain types an empty string */
    uint32_t type = SG_ARG(0);
    const char *v = (type == 2 || type == 6) ? "" : COMPUTER_NAME; /* ComputerNameDnsDomain, PhysicalDnsDomain */
    SG_RETURN_STDCALL(name_query(ctx, mem, SG_ARG(1), SG_ARG(2), v, 0, W32_ERROR_MORE_DATA, 0), 12);
}
/* GetUserName counts the NUL in *pcbBuffer on success */
W32_IMPORT(advapi32, GetUserNameA) {
    SG_RETURN_STDCALL(name_query(ctx, mem, SG_ARG(0), SG_ARG(1), USER_NAME, 0, W32_ERROR_INSUFFICIENT_BUFFER, 1), 8);
}
W32_IMPORT(advapi32, GetUserNameW) {
    SG_RETURN_STDCALL(name_query(ctx, mem, SG_ARG(0), SG_ARG(1), USER_NAME, 1, W32_ERROR_INSUFFICIENT_BUFFER, 1), 8);
}

/* OpenSSL's fatal-error reporter (0x11814bc..0x11814e7): the event log becomes the HLE log. */
#define EVENT_SOURCE_HANDLE 0x00c0ffeeu
W32_IMPORT(advapi32, RegisterEventSourceA) { SG_RETURN_STDCALL(EVENT_SOURCE_HANDLE, 8); }
W32_IMPORT(advapi32, DeregisterEventSource) { SG_RETURN_STDCALL(SG_ARG(0) == EVENT_SOURCE_HANDLE, 4); }
W32_IMPORT(advapi32, ReportEventA) {
    /* (hEventLog, wType, wCategory, dwEventID, lpUserSid, wNumStrings, dwDataSize, lpStrings, lpRawData) */
    uint32_t n = SG_ARG(5) & 0xffff, strs = SG_ARG(7);
    for (uint32_t i = 0; i < n && strs; i++) {
        char s[1024];
        uint32_t p = sg_ld32(mem, strs + 4 * i);
        if (p) { w32_astr_get(mem, p, s, sizeof s); hle_log(HLE_LOG_WARN, "[guest event log] %s", s); }
    }
    SG_RETURN_STDCALL(1, 36);
}
