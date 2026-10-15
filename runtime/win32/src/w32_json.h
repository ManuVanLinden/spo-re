/* w32_json.h -- the registry store's JSON reader and writer helpers. */
#ifndef SPORE_W32_JSON_H
#define SPORE_W32_JSON_H
#include <stddef.h>

enum { JV_NULL, JV_BOOL, JV_NUMBER, JV_STRING, JV_ARRAY, JV_OBJECT };
typedef struct jv {
    int type;
    double num;           /* JV_NUMBER, JV_BOOL */
    char *s;              /* JV_STRING (UTF-8) */
    struct jv **items;    /* JV_ARRAY / JV_OBJECT members, in file order */
    char **keys;          /* JV_OBJECT keys (NULL entries for arrays) */
    size_t n;
} jv;

jv *json_parse(const char *text, char *err, size_t errcap);
void json_free(jv *v);
jv *json_get(const jv *obj, const char *key);

typedef struct jbuf { char *p; size_t n, cap; } jbuf;
void jbuf_putc(jbuf *b, char c);
void jbuf_puts(jbuf *b, const char *s);
void json_put_string(jbuf *b, const char *s);
#endif
