/* w32_json.c -- a small JSON reader (objects, arrays, strings, numbers, true/false/null) for
 * the registry store. Strings are returned as UTF-8 (\uXXXX escapes and surrogate pairs
 * decoded). Not a general-purpose parser: no depth limit beyond 64, no streaming. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32_json.h"
#include "w32_int.h"

typedef struct jp { const char *s; char *err; size_t errcap; int depth; } jp;

static void ws(jp *p) { while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r') p->s++; }
static jv *fail(jp *p, const char *m) {
    if (p->err && !p->err[0]) snprintf(p->err, p->errcap, "%s near '%.20s'", m, p->s);
    return NULL;
}
static jv *mk(int t) { jv *v = (jv *)calloc(1, sizeof *v); v->type = t; return v; }
static jv *value(jp *p);

static int hex4(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *string(jp *p) {
    size_t cap = 64, n = 0;
    char *o = (char *)malloc(cap);
    p->s++; /* opening quote */
    for (;;) {
        char c = *p->s++;
        uint32_t cp;
        char tmp[4];
        size_t k = 1;
        if (!c) { free(o); fail(p, "unterminated string"); return NULL; }
        if (c == '"') break;
        if (c == '\\') {
            char e = *p->s++;
            switch (e) {
            case '"': tmp[0] = '"'; break;
            case '\\': tmp[0] = '\\'; break;
            case '/': tmp[0] = '/'; break;
            case 'b': tmp[0] = '\b'; break;
            case 'f': tmp[0] = '\f'; break;
            case 'n': tmp[0] = '\n'; break;
            case 'r': tmp[0] = '\r'; break;
            case 't': tmp[0] = '\t'; break;
            case 'u':
                if (!hex4(p->s, &cp)) { free(o); fail(p, "bad \\u escape"); return NULL; }
                p->s += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && p->s[0] == '\\' && p->s[1] == 'u') {
                    uint32_t lo;
                    if (hex4(p->s + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p->s += 6;
                    }
                }
                k = w32_utf8_encode(cp, tmp);
                break;
            default: free(o); fail(p, "bad escape"); return NULL;
            }
        } else {
            tmp[0] = c;
        }
        if (n + k + 1 > cap) { cap *= 2; o = (char *)realloc(o, cap); }
        memcpy(o + n, tmp, k);
        n += k;
    }
    o[n] = 0;
    return o;
}

static jv *value(jp *p) {
    ws(p);
    if (++p->depth > 64) return fail(p, "too deep");
    jv *v = NULL;
    char c = *p->s;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        v = mk(obj ? JV_OBJECT : JV_ARRAY);
        p->s++;
        ws(p);
        if (*p->s == (obj ? '}' : ']')) { p->s++; goto done; }
        for (;;) {
            char *key = NULL;
            ws(p);
            if (obj) {
                if (*p->s != '"') { json_free(v); return fail(p, "expected key"); }
                if (!(key = string(p))) { json_free(v); return NULL; }
                ws(p);
                if (*p->s++ != ':') { free(key); json_free(v); return fail(p, "expected ':'"); }
            }
            jv *item = value(p);
            if (!item) { free(key); json_free(v); return NULL; }
            v->items = (jv **)realloc(v->items, (v->n + 1) * sizeof *v->items);
            v->keys = (char **)realloc(v->keys, (v->n + 1) * sizeof *v->keys);
            v->items[v->n] = item;
            v->keys[v->n] = key;
            v->n++;
            ws(p);
            if (*p->s == ',') { p->s++; continue; }
            if (*p->s == (obj ? '}' : ']')) { p->s++; break; }
            json_free(v);
            return fail(p, "expected ',' or end");
        }
    } else if (c == '"') {
        v = mk(JV_STRING);
        if (!(v->s = string(p))) { free(v); return NULL; }
    } else if (!strncmp(p->s, "true", 4)) { v = mk(JV_BOOL); v->num = 1; p->s += 4; }
    else if (!strncmp(p->s, "false", 5)) { v = mk(JV_BOOL); p->s += 5; }
    else if (!strncmp(p->s, "null", 4)) { v = mk(JV_NULL); p->s += 4; }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        v = mk(JV_NUMBER);
        v->num = strtod(p->s, &end);
        if (end == p->s) { free(v); return fail(p, "bad number"); }
        p->s = end;
    } else {
        return fail(p, "unexpected character");
    }
done:
    p->depth--;
    return v;
}

jv *json_parse(const char *text, char *err, size_t errcap) {
    jp p = {text, err, errcap, 0};
    if (err && errcap) err[0] = 0;
    jv *v = value(&p);
    if (!v) return NULL;
    ws(&p);
    if (*p.s) { json_free(v); return fail(&p, "trailing characters"); }
    return v;
}

void json_free(jv *v) {
    if (!v) return;
    for (size_t i = 0; i < v->n; i++) { json_free(v->items[i]); free(v->keys ? v->keys[i] : NULL); }
    free(v->items);
    free(v->keys);
    free(v->s);
    free(v);
}

jv *json_get(const jv *obj, const char *key) {
    if (!obj || obj->type != JV_OBJECT) return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

void json_put_string(jbuf *b, const char *s) {
    jbuf_putc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { jbuf_putc(b, '\\'); jbuf_putc(b, (char)c); }
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); jbuf_puts(b, t); }
        else jbuf_putc(b, (char)c);
    }
    jbuf_putc(b, '"');
}

void jbuf_putc(jbuf *b, char c) {
    if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 1024; b->p = (char *)realloc(b->p, b->cap); }
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}
void jbuf_puts(jbuf *b, const char *s) { while (*s) jbuf_putc(b, *s++); }
