/* w32_version.c -- VerQueryValue over the whole VS_VERSIONINFO tree (design doc section 18).
 *
 * w32_res.c finds the exe's RT_VERSION resource and serves GetFileVersionInfo*, and its
 * VerQueryValueA/W shims answer the root block ("\") themselves. This file walks the children
 * for every other sub-block: "\VarFileInfo\Translation" and
 * "\StringFileInfo\<lang><codepage>\<name>". The shims in w32_res.c call w32_ver_query() for
 * anything that is not the root.
 *
 * Layout [doc: VS_VERSIONINFO, StringFileInfo, StringTable, String, VarFileInfo, Var]: every node
 * is { WORD wLength; WORD wValueLength; WORD wType; WCHAR szKey[]; padding to 32 bits; Value;
 * padding to 32 bits; Children[] }, wLength covering the node and its children. wType 1 is text
 * (wValueLength in WCHARs), 0 binary (in bytes). The exe's block (en-US copy, 1,300 bytes) has
 * StringFileInfo\040904b0 with 11 strings and VarFileInfo\Translation = 0409 04b0 [obs: static
 * read of the exe's resource, test_version.c with the real file].
 *
 * VerQueryValue semantics [doc: VerQueryValueW]:
 *   - keys compare case-insensitively; empty path segments are skipped;
 *   - a String value: *lplpBuffer points at the text, *puLen is its length in characters (the
 *     stored wValueLength, which counts the NUL in the exe's resource);
 *   - a binary value (Translation): pointer to the bytes, *puLen in bytes;
 *   - a node without a value (StringFileInfo, a StringTable): TRUE with *puLen 0;
 *   - not found: FALSE (ERROR_RESOURCE_TYPE_NOT_FOUND, as Windows 7 reports [inf]).
 * VerQueryValueA returns code page 1252 text. Like Windows, which keeps ANSI copies after the
 * Unicode block (why GetFileVersionInfoSize reports about 3/2 of it), the ANSI copy is written
 * into the spare area after the block: at block + align4(wLength) + offset/2. w32_res.c's size
 * (raw + raw/2 + 4) leaves exactly that room.
 */
#include <string.h>

#include "w32_int.h"
#include "w32_dirwatch.h"

#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813

typedef struct vnode {
    uint32_t at, len, vlen, type, key, value, children;
} vnode;

/* Parses the node at `at` within [at, limit). Returns 0 if malformed. */
static int node_at(const uint8_t *mem, uint32_t at, uint32_t limit, vnode *n) {
    if (at + 6 > limit) return 0;
    n->at = at;
    n->len = sg_ld16(mem, at);
    n->vlen = sg_ld16(mem, at + 2);
    n->type = sg_ld16(mem, at + 4);
    if (n->len < 6 || at + n->len > limit) return 0;
    n->key = at + 6;
    uint32_t k = n->key;
    while (k + 2 <= at + n->len && sg_ld16(mem, k)) k += 2;
    if (k + 2 > at + n->len) return 0;
    n->value = (k + 2 + 3) & ~3u;
    uint32_t vbytes = n->type == 1 ? 2 * n->vlen : n->vlen;
    if (n->value + vbytes > at + n->len) {
        /* some linkers write wValueLength in bytes for text; clamp rather than fail */
        if (n->value > at + n->len) return 0;
        vbytes = at + n->len - n->value;
        n->vlen = n->type == 1 ? vbytes / 2 : vbytes;
    }
    n->children = (n->value + vbytes + 3) & ~3u;
    return 1;
}

/* Compares a node's UTF-16 key with an ASCII/UTF-8 path segment, case-insensitively. */
static int key_eq(const uint8_t *mem, uint32_t key, const char *seg, size_t n) {
    char buf[256];
    w32_wstr_get(mem, key, buf, sizeof buf);
    if (strlen(buf) != n) return 0;
    char tmp[256];
    memcpy(tmp, seg, n);
    tmp[n] = 0;
    return !w32_name_casecmp(buf, tmp);
}

/* Resolves `sub` in the version block at `block`. On success *node is the node found. */
static int ver_find(const uint8_t *mem, uint32_t block, const char *sub, vnode *node) {
    uint32_t total = sg_ld16(mem, block);
    if (!node_at(mem, block, block + total, node)) return 0;
    const char *p = sub;
    for (;;) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) return 1;
        const char *e = p;
        while (*e && *e != '\\' && *e != '/') e++;
        size_t n = (size_t)(e - p);
        uint32_t c = node->children, end = node->at + node->len;
        int found = 0;
        vnode ch;
        while (c + 6 <= end) {
            if (!node_at(mem, c, end, &ch)) break;
            if (key_eq(mem, ch.key, p, n)) { found = 1; break; }
            c = (c + ch.len + 3) & ~3u;
        }
        if (!found) return 0;
        *node = ch;
        p = e;
    }
}

/* VerQueryValue for any sub-block. Writes the guest pointer and length; returns 1, or 0 with
 * *err set. The root ("\" or "") gives the VS_FIXEDFILEINFO as w32_res.c does. */
int w32_ver_query(uint8_t *mem, uint32_t block, const char *sub, int wide, uint32_t *ptr, uint32_t *len, uint32_t *err) {
    vnode n;
    *err = ERROR_RESOURCE_TYPE_NOT_FOUND;
    if (!block || !w32_vm_is_committed(block, 6)) { *err = W32_ERROR_INVALID_PARAMETER; return 0; }
    uint32_t total = sg_ld16(mem, block);
    if (total < 6 || !w32_vm_is_committed(block, total)) { *err = W32_ERROR_INVALID_PARAMETER; return 0; }
    if (!ver_find(mem, block, sub, &n)) return 0;
    if (n.at == block) { /* root: VS_FIXEDFILEINFO */
        if (n.vlen < 0x34 || sg_ld32(mem, n.value) != 0xfeef04bdu) return 0;
        *ptr = n.value;
        *len = n.vlen;
        return 1;
    }
    if (!n.vlen) { *ptr = n.value; *len = 0; return 1; }
    if (n.type != 1 || wide) { *ptr = n.value; *len = n.vlen; return 1; }
    /* VerQueryValueA on text: the 1252 copy in the spare area after the block */
    uint32_t dst = ((block + total + 3) & ~3u) + (n.value - block) / 2;
    if (!w32_vm_is_committed(dst, n.vlen + 1)) { *err = W32_ERROR_NOT_ENOUGH_MEMORY; return 0; }
    for (uint32_t i = 0; i < n.vlen; i++) {
        int b = w32_unicode_to_cp1252(sg_ld16(mem, n.value + 2 * i));
        mem[dst + i] = (uint8_t)b;
    }
    if (n.vlen == 0 || mem[dst + n.vlen - 1]) mem[dst + n.vlen] = 0;
    *ptr = dst;
    *len = n.vlen;
    return 1;
}
