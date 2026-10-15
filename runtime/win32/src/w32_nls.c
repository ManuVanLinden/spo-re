/* w32_nls.c -- national language support: the default LCID, GetLocaleInfo, LCMapString and
 * CompareString (design doc section 17).
 *
 * The game's call sites [obs: disassembly of the exe]:
 *   0x87dba0  GetSystemDefaultLCID(), stored with its language and sort ids (locale set-up 0x87db80);
 *   0x847f11, 0x849c24  GetLocaleInfoW(langid, LOCALE_IDEFAULTANSICODEPAGE, buf, 0x80), then
 *             wcstoul(buf, NULL, 10): the ANSI code page of a language id;
 *   0x6b8e3b  CompareStringW(lcid, flags, s1, n1, s2, n2): lcid 0x400 (LOCALE_USER_DEFAULT), or 0x7f
 *             (LOCALE_INVARIANT) when the caller's bit 0x20 is set, or a configured LCID; flags built
 *             from caller bits: NORM_IGNORECASE, NORM_IGNOREKANATYPE, NORM_IGNORENONSPACE,
 *             NORM_IGNORESYMBOLS, NORM_IGNOREWIDTH, SORT_STRINGSORT;
 *   0x6b8f55, 0x6b8fb3  LCMapStringW(lcid, LCMAP_LOWERCASE, src, n, NULL, 0) for the size, then into
 *             a buffer.
 *
 * The process is en-US. Linguistic comparison and sort keys use ICU from the system
 * (/usr/lib/libicucore.A.dylib, loaded with dlopen so no link flag is needed): the en_US
 * collator, which follows the Unicode Collation Algorithm like Windows' NLS sorting does. Case
 * mapping is ICU's simple (1:1) mapping, as Windows' LCMapString uses one-to-one tables. Without
 * ICU everything falls back to ordinal comparison after simple upper-casing (logged once).
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "w32_int.h"
#include "w32_dirwatch.h"

#define ERROR_INVALID_FLAGS 1004
#define LCID_EN_US 0x0409u
#define LOCALE_INVARIANT 0x007fu
#define LOCALE_USER_DEFAULT 0x0400u
#define LOCALE_SYSTEM_DEFAULT 0x0800u

enum {
    NORM_IGNORECASE = 0x1, NORM_IGNORENONSPACE = 0x2, NORM_IGNORESYMBOLS = 0x4,
    LINGUISTIC_IGNORECASE = 0x10, LINGUISTIC_IGNOREDIACRITIC = 0x20, SORT_STRINGSORT = 0x1000,
    NORM_IGNOREKANATYPE = 0x10000, NORM_IGNOREWIDTH = 0x20000, NORM_LINGUISTIC_CASING = 0x08000000,
    SORT_DIGITSASNUMBERS = 0x8,
    LCMAP_LOWERCASE = 0x100, LCMAP_UPPERCASE = 0x200, LCMAP_SORTKEY = 0x400, LCMAP_BYTEREV = 0x800,
    LCMAP_LINGUISTIC_CASING = 0x01000000,
    LOCALE_NOUSEROVERRIDE = 0x80000000u, LOCALE_USE_CP_ACP = 0x40000000u, LOCALE_RETURN_NUMBER = 0x20000000u,
};
#define CMP_FLAGS (NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS | LINGUISTIC_IGNORECASE | \
                   LINGUISTIC_IGNOREDIACRITIC | SORT_STRINGSORT | NORM_IGNOREKANATYPE | NORM_IGNOREWIDTH | \
                   NORM_LINGUISTIC_CASING | SORT_DIGITSASNUMBERS)

/* ---- ICU, loaded at first use --------------------------------------------------------------- */

typedef void *(*ucol_open_t)(const char *, int *);
typedef int (*ucol_strcoll_t)(void *, const uint16_t *, int32_t, const uint16_t *, int32_t);
typedef int32_t (*ucol_sortkey_t)(void *, const uint16_t *, int32_t, uint8_t *, int32_t);
typedef void (*ucol_setattr_t)(void *, int, int, int *);
typedef int32_t (*u_case_t)(int32_t);
typedef int8_t (*u_chartype_t)(int32_t);

static struct {
    int ok;
    ucol_strcoll_t strcoll;
    ucol_sortkey_t sortkey;
    u_case_t lower, upper;
    u_chartype_t chartype;
    void *coll[2][4]; /* [invariant?][strength mode] */
} icu;
static pthread_once_t icu_once = PTHREAD_ONCE_INIT;

/* ICU constants (ucol.h, uchar.h of ICU 4+, stable ABI values) */
enum { UCOL_CASE_LEVEL = 3, UCOL_STRENGTH = 5, UCOL_NUMERIC_COLLATION = 7, UCOL_PRIMARY = 0, UCOL_SECONDARY = 1,
       UCOL_TERTIARY = 2, UCOL_OFF = 16, UCOL_ON = 17 };
/* strength modes: 0 tertiary, 1 ignore case, 2 ignore diacritics (case kept), 3 ignore both */

static void icu_load(void) {
    void *h = dlopen("/usr/lib/libicucore.A.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!h) { hle_log(HLE_LOG_WARN, "NLS: libicucore not loadable; ordinal comparison only"); return; }
    ucol_open_t op = (ucol_open_t)dlsym(h, "ucol_open");
    ucol_setattr_t sa = (ucol_setattr_t)dlsym(h, "ucol_setAttribute");
    icu.strcoll = (ucol_strcoll_t)dlsym(h, "ucol_strcoll");
    icu.sortkey = (ucol_sortkey_t)dlsym(h, "ucol_getSortKey");
    icu.lower = (u_case_t)dlsym(h, "u_tolower");
    icu.upper = (u_case_t)dlsym(h, "u_toupper");
    icu.chartype = (u_chartype_t)dlsym(h, "u_charType");
    if (!op || !sa || !icu.strcoll || !icu.sortkey || !icu.lower || !icu.upper || !icu.chartype) {
        hle_log(HLE_LOG_WARN, "NLS: ICU symbols missing; ordinal comparison only");
        return;
    }
    for (int inv = 0; inv < 2; inv++)
        for (int m = 0; m < 4; m++) {
            int err = 0;
            void *c = op(inv ? "" : "en_US", &err);
            if (!c || err > 0) { hle_log(HLE_LOG_WARN, "NLS: ucol_open failed (%d)", err); return; }
            static const int strength[4] = {UCOL_TERTIARY, UCOL_SECONDARY, UCOL_PRIMARY, UCOL_PRIMARY};
            sa(c, UCOL_STRENGTH, strength[m], &err);
            if (m == 2) sa(c, UCOL_CASE_LEVEL, UCOL_ON, &err);
            icu.coll[inv][m] = c;
        }
    icu.ok = 1;
}
static int icu_ready(void) {
    pthread_once(&icu_once, icu_load);
    return icu.ok;
}

uint32_t w32_nls_upper(uint32_t c) {
    if (icu_ready()) return (uint32_t)icu.upper((int32_t)c);
    return (uint32_t)towupper((wint_t)c);
}
uint32_t w32_nls_lower(uint32_t c) {
    if (icu_ready()) return (uint32_t)icu.lower((int32_t)c);
    return (uint32_t)towlower((wint_t)c);
}

/* ---- LCIDs -------------------------------------------------------------------------------- */

/* LOCALE_USER_DEFAULT / SYSTEM_DEFAULT / 0 / LANG_NEUTRAL sublanguages resolve to the process
 * locale, en-US [doc: "Locale Identifiers", LCID constants]. Returns 0 for an unusable LCID. */
static uint32_t resolve_lcid(uint32_t lcid) {
    if (lcid == 0 || lcid == LOCALE_USER_DEFAULT || lcid == LOCALE_SYSTEM_DEFAULT ||
        (lcid & 0xffff) == 0x0400 || (lcid & 0xffff) == 0x0800 || (lcid & 0xffff) == 0)
        return LCID_EN_US;
    if ((lcid & 0x3ff) == 0) return 0;
    return lcid & 0xfffff;
}

W32_IMPORT(kernel32, GetSystemDefaultLCID) { SG_RETURN_STDCALL(LCID_EN_US, 0); }
W32_IMPORT(kernel32, GetUserDefaultLCID) { SG_RETURN_STDCALL(LCID_EN_US, 0); }
W32_IMPORT(kernel32, GetThreadLocale) { SG_RETURN_STDCALL(LCID_EN_US, 0); }
W32_IMPORT(kernel32, GetSystemDefaultLangID) { SG_RETURN_STDCALL(LCID_EN_US, 0); }
W32_IMPORT(kernel32, GetUserDefaultLangID) { SG_RETURN_STDCALL(LCID_EN_US, 0); }

/* ---- GetLocaleInfo ------------------------------------------------------------------------- */

/* ANSI and OEM code pages by primary language (Windows' NLS tables; [doc: "Code Page
 * Identifiers"] and the National Language Support reference). Western languages: 1252/850. */
static void code_pages(uint32_t lcid, unsigned *ansi, unsigned *oem) {
    unsigned prim = lcid & 0x3ff, sub = (lcid >> 10) & 0x3f;
    *ansi = 1252;
    *oem = 850;
    switch (prim) {
    case 0x09: if (sub == 1) *oem = 437; break;                                   /* en-US */
    case 0x05: case 0x0e: case 0x15: case 0x18: case 0x1a: case 0x1b: case 0x24: case 0x1c:
        *ansi = 1250; *oem = 852; break;                                          /* cs hu pl ro hr sk sl sq */
    case 0x02: case 0x19: case 0x22: case 0x23: case 0x2f: *ansi = 1251; *oem = 866; break; /* bg ru uk be mk */
    case 0x08: *ansi = 1253; *oem = 737; break;                                   /* el */
    case 0x1f: *ansi = 1254; *oem = 857; break;                                   /* tr */
    case 0x0d: *ansi = 1255; *oem = 862; break;                                   /* he */
    case 0x01: *ansi = 1256; *oem = 720; break;                                   /* ar */
    case 0x25: case 0x26: case 0x27: *ansi = 1257; *oem = 775; break;             /* et lv lt */
    case 0x2a: *ansi = 1258; *oem = 1258; break;                                  /* vi */
    case 0x1e: *ansi = 874; *oem = 874; break;                                    /* th */
    case 0x11: *ansi = 932; *oem = 932; break;                                    /* ja */
    case 0x12: *ansi = 949; *oem = 949; break;                                    /* ko */
    case 0x04: if (sub == 2 || sub == 4) { *ansi = 936; *oem = 936; } else { *ansi = 950; *oem = 950; } break; /* zh-CN/SG; TW/HK/MO */
    default: break;
    }
}

typedef struct lang_info { const char *iso639, *iso3166, *eng_lang, *eng_ctry, *abbrev; uint32_t lcid; } lang_info;
static const lang_info langs[] = {
    {"en", "US", "English", "United States", "ENU", 0x0409}, {"en", "GB", "English", "United Kingdom", "ENG", 0x0809},
    {"fr", "FR", "French", "France", "FRA", 0x040c},        {"de", "DE", "German", "Germany", "DEU", 0x0407},
    {"it", "IT", "Italian", "Italy", "ITA", 0x0410},         {"es", "ES", "Spanish", "Spain", "ESN", 0x0c0a},
    {"nl", "NL", "Dutch", "Netherlands", "NLD", 0x0413},     {"sv", "SE", "Swedish", "Sweden", "SVE", 0x041d},
    {"da", "DK", "Danish", "Denmark", "DAN", 0x0406},        {"nb", "NO", "Norwegian", "Norway", "NOR", 0x0414},
    {"fi", "FI", "Finnish", "Finland", "FIN", 0x040b},       {"pl", "PL", "Polish", "Poland", "PLK", 0x0415},
    {"cs", "CZ", "Czech", "Czech Republic", "CSY", 0x0405},  {"hu", "HU", "Hungarian", "Hungary", "HUN", 0x040e},
    {"ru", "RU", "Russian", "Russia", "RUS", 0x0419},        {"pt", "BR", "Portuguese", "Brazil", "PTB", 0x0416},
    {"pt", "PT", "Portuguese", "Portugal", "PTG", 0x0816},   {"el", "GR", "Greek", "Greece", "ELL", 0x0408},
    {"ja", "JP", "Japanese", "Japan", "JPN", 0x0411},        {"ko", "KR", "Korean", "Korea", "KOR", 0x0412},
    {"zh", "TW", "Chinese", "Taiwan", "CHT", 0x0404},        {"zh", "CN", "Chinese", "China", "CHS", 0x0804},
    {"th", "TH", "Thai", "Thailand", "THA", 0x041e},         {"tr", "TR", "Turkish", "Turkey", "TRK", 0x041f},
};

static const char *const days[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
static const char *const months[12] = {"January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December"};

/* Value of an LCTYPE (without flags) for `lcid` as UTF-8, or NULL if the type is unknown. The
 * values are Windows 7's for en-US [doc: "Locale Information Constants"; LCTYPE pages]. Text
 * types for other languages keep the en-US values except names, codes and code pages
 * (deviation, logged once). */
static const char *locale_value(uint32_t lcid, uint32_t type, char *tmp, size_t cap) {
    unsigned ansi, oem;
    code_pages(lcid, &ansi, &oem);
    const lang_info *L = &langs[0];
    int known = 0;
    for (size_t i = 0; i < sizeof langs / sizeof langs[0]; i++)
        if (langs[i].lcid == (lcid & 0xffff)) { L = &langs[i]; known = 1; break; }
    if (!known)
        for (size_t i = 0; i < sizeof langs / sizeof langs[0]; i++)
            if ((langs[i].lcid & 0x3ff) == (lcid & 0x3ff)) { L = &langs[i]; break; }
    switch (type) {
    case 0x0001: case 0x0009: snprintf(tmp, cap, "%04x", (unsigned)(lcid & 0xffff)); return tmp; /* ILANGUAGE, IDEFAULTLANGUAGE */
    case 0x0002: case 0x0072: snprintf(tmp, cap, "%s (%s)", L->eng_lang, L->eng_ctry); return tmp; /* SLANGUAGE, SENGLISHDISPLAYNAME */
    case 0x1001: case 0x0004: return L->eng_lang;          /* SENGLANGUAGE, SNATIVELANGNAME (en) */
    case 0x0003: return L->abbrev;                         /* SABBREVLANGNAME */
    case 0x0005: case 0x000a: return "1";                  /* ICOUNTRY, IDEFAULTCOUNTRY */
    case 0x0006: case 0x1002: case 0x0008: return L->eng_ctry; /* SCOUNTRY, SENGCOUNTRY, SNATIVECTRYNAME */
    case 0x0007: return !strcmp(L->iso3166, "US") ? "USA" : L->iso3166; /* SABBREVCTRYNAME */
    case 0x000b: snprintf(tmp, cap, "%u", oem); return tmp;   /* IDEFAULTCODEPAGE (OEM) */
    case 0x1004: snprintf(tmp, cap, "%u", ansi); return tmp;  /* IDEFAULTANSICODEPAGE */
    case 0x1011: return "10000";                           /* IDEFAULTMACCODEPAGE */
    case 0x1012: return "37";                              /* IDEFAULTEBCDICCODEPAGE */
    case 0x000c: return ",";                               /* SLIST */
    case 0x000d: return "1";                               /* IMEASURE: U.S. */
    case 0x000e: case 0x0016: return ".";                  /* SDECIMAL, SMONDECIMALSEP */
    case 0x000f: case 0x0017: return ",";                  /* STHOUSAND, SMONTHOUSANDSEP */
    case 0x0010: case 0x0018: return "3;0";                /* SGROUPING, SMONGROUPING */
    case 0x0011: case 0x0019: case 0x001a: return "2";     /* IDIGITS, ICURRDIGITS, IINTLCURRDIGITS */
    case 0x0012: return "1";                               /* ILZERO */
    case 0x1010: return "1";                               /* INEGNUMBER */
    case 0x0013: return "0123456789";                      /* SNATIVEDIGITS */
    case 0x0014: return "$";                               /* SCURRENCY */
    case 0x0015: return "USD";                             /* SINTLSYMBOL */
    case 0x001b: return "0";                               /* ICURRENCY */
    case 0x001c: return "0";                               /* INEGCURR */
    case 0x001d: return "/";                               /* SDATE */
    case 0x001e: return ":";                               /* STIME */
    case 0x001f: return "M/d/yyyy";                        /* SSHORTDATE */
    case 0x0020: return "dddd, MMMM d, yyyy";              /* SLONGDATE */
    case 0x1003: return "h:mm:ss tt";                      /* STIMEFORMAT */
    case 0x1006: return "MMMM yyyy";                       /* SYEARMONTH */
    case 0x0021: case 0x0022: case 0x0023: return "0";     /* IDATE, ILDATE, ITIME */
    case 0x1005: return "0";                               /* ITIMEMARKPOSN */
    case 0x0024: return "1";                               /* ICENTURY */
    case 0x0025: case 0x0026: case 0x0027: return "0";     /* ITLZERO, IDAYLZERO, IMONLZERO */
    case 0x0028: return "AM";                              /* S1159 */
    case 0x0029: return "PM";                              /* S2359 */
    case 0x1009: return "1";                               /* ICALENDARTYPE: Gregorian */
    case 0x100b: return "0";                               /* IOPTIONALCALENDAR */
    case 0x100c: return "6";                               /* IFIRSTDAYOFWEEK: Sunday */
    case 0x100d: return "0";                               /* IFIRSTWEEKOFYEAR */
    case 0x0050: return "";                                /* SPOSITIVESIGN */
    case 0x0051: return "-";                               /* SNEGATIVESIGN */
    case 0x0052: case 0x0053: case 0x0054: case 0x0055: return "0"; /* IPOS/NEG SIGNPOSN, SYMPRECEDES */
    case 0x0056: case 0x0057: return "1";
    case 0x0059: return L->iso639;                         /* SISO639LANGNAME */
    case 0x005a: return L->iso3166;                        /* SISO3166CTRYNAME */
    case 0x005c: snprintf(tmp, cap, "%s-%s", L->iso639, L->iso3166); return tmp; /* SNAME */
    case 0x1014: return "0";                               /* IDIGITSUBSTITUTION: context */
    }
    if (type >= 0x2a && type <= 0x30) return days[type - 0x2a];                       /* SDAYNAME1..7 */
    if (type >= 0x31 && type <= 0x37) { snprintf(tmp, cap, "%.3s", days[type - 0x31]); return tmp; }
    if (type >= 0x38 && type <= 0x43) return months[type - 0x38];                     /* SMONTHNAME1..12 */
    if (type >= 0x44 && type <= 0x4f) { snprintf(tmp, cap, "%.3s", months[type - 0x44]); return tmp; }
    return NULL;
}

/* GetLocaleInfoA/W(Locale, LCType, lpLCData, cchData) [doc: GetLocaleInfoW]: returns the
 * characters written including the NUL, or with cchData == 0 the size needed; 0 with
 * ERROR_INSUFFICIENT_BUFFER, ERROR_INVALID_FLAGS or ERROR_INVALID_PARAMETER. LOCALE_RETURN_NUMBER
 * writes a DWORD and returns 2 (W) or 4 (A) [doc: LOCALE_RETURN_NUMBER]. */
uint32_t w32_get_locale_info(sg_ctx *ctx, uint8_t *mem, uint32_t lcid_in, uint32_t type, uint32_t out,
                             uint32_t cch, int wide) {
    char tmp[128];
    uint32_t lcid = resolve_lcid(lcid_in), base = type & ~(LOCALE_NOUSEROVERRIDE | LOCALE_USE_CP_ACP | LOCALE_RETURN_NUMBER);
    if (lcid_in == LOCALE_INVARIANT) lcid = LCID_EN_US; /* invariant: English names, Gregorian, 1252 */
    if (!lcid || (int32_t)cch < 0 || (cch && !out)) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); return 0; }
    const char *v = locale_value(lcid, base, tmp, sizeof tmp);
    if (!v) {
        hle_log(HLE_LOG_WARN, "GetLocaleInfo(0x%x, 0x%x): LCTYPE not supported", lcid_in, type);
        hle_set_last_error(ctx, mem, ERROR_INVALID_FLAGS);
        return 0;
    }
    if ((lcid & 0x3ff) != 0x09 && base != 0x1004 && base != 0x000b && base != 0x0001 && base != 0x0009) {
        static int logged;
        if (!logged++) hle_log(HLE_LOG_INFO, "GetLocaleInfo(0x%x, 0x%x): en-US formats served for a non-English locale", lcid_in, type);
    }
    if (type & LOCALE_RETURN_NUMBER) {
        uint32_t units = wide ? 2 : 4;
        if (!cch) return units;
        if (cch < units) { hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER); return 0; }
        char *end;
        unsigned long n = strtoul(v, &end, base == 0x0001 || base == 0x0009 ? 16 : 10);
        if (end == v || *end) { hle_set_last_error(ctx, mem, ERROR_INVALID_FLAGS); return 0; }
        sg_st32(mem, out, (uint32_t)n);
        return units;
    }
    uint32_t need = w32_utf8_units(v, wide) + 1;
    if (!cch) return need;
    if (cch < need) { hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER); return 0; }
    int fits;
    w32_put_str(mem, out, cch, v, wide, &fits);
    return need;
}
W32_IMPORT(kernel32, GetLocaleInfoW) {
    SG_RETURN_STDCALL(w32_get_locale_info(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), SG_ARG(3), 1), 16);
}
W32_IMPORT(kernel32, GetLocaleInfoA) {
    SG_RETURN_STDCALL(w32_get_locale_info(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), SG_ARG(3), 0), 16);
}

/* ---- comparison ------------------------------------------------------------------------------ */

/* Counted UTF-16 string from guest memory: n == -1 means NUL-terminated (without the NUL). */
static uint16_t *wstr_dup(const uint8_t *mem, uint32_t a, int32_t n, int32_t *len) {
    if (n < 0) { n = 0; while (sg_ld16(mem, a + 2u * (uint32_t)n)) n++; }
    uint16_t *s = (uint16_t *)malloc(2u * (size_t)n + 2);
    for (int32_t i = 0; i < n; i++) s[i] = sg_ld16(mem, a + 2u * (uint32_t)i);
    s[n] = 0;
    *len = n;
    return s;
}

/* U_GENERAL_CATEGORY values 12..29 that NORM_IGNORESYMBOLS drops: separators (Zs Zl Zp), control,
 * format, punctuation (Pd Ps Pe Pc Po Pi Pf) and symbols (Sm Sc Sk So) [doc: CompareStringEx:
 * "Ignore symbols and punctuation"]. */
static int is_symbol(uint16_t c) {
    if (icu_ready()) {
        int t = icu.chartype(c);
        return (t >= 12 && t <= 16) || (t >= 19 && t <= 29);
    }
    return c < 0x80 && !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

/* Windows' default "word sort" treats '-' and '\'' specially: they are ignored first and only
 * break ties [doc: CompareStringW remarks; SORT_STRINGSORT]. */
static int32_t strip(const uint16_t *s, int32_t n, uint16_t *out, int symbols, int word) {
    int32_t k = 0;
    for (int32_t i = 0; i < n; i++) {
        uint16_t c = s[i];
        if (symbols && is_symbol(c)) continue;
        if (word && (c == '-' || c == '\'' || c == 0x2010 || c == 0x2011 || c == 0x2019)) continue;
        out[k++] = c;
    }
    return k;
}

static int ordinal_cmp(const uint16_t *a, int32_t na, const uint16_t *b, int32_t nb, int fold) {
    int32_t n = na < nb ? na : nb;
    for (int32_t i = 0; i < n; i++) {
        uint32_t x = a[i], y = b[i];
        if (fold) { x = w32_nls_upper(x) & 0xffff; y = w32_nls_upper(y) & 0xffff; }
        if (x != y) return x < y ? -1 : 1;
    }
    return na < nb ? -1 : na > nb;
}

static int strength_mode(uint32_t flags) {
    int nocase = (flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE)) != 0;
    int nodiac = (flags & (NORM_IGNORENONSPACE | LINGUISTIC_IGNOREDIACRITIC)) != 0;
    return nodiac ? (nocase ? 3 : 2) : nocase;
}

static int coll_cmp(void *c, const uint16_t *a, int32_t na, const uint16_t *b, int32_t nb, int fold) {
    if (c) return icu.strcoll(c, a, na, b, nb);
    return ordinal_cmp(a, na, b, nb, fold);
}

/* -1/0/1 for two host UTF-16 strings under CompareString flags. */
int w32_nls_compare(uint32_t lcid, uint32_t flags, const uint16_t *a, int32_t na, const uint16_t *b, int32_t nb) {
    void *c = icu_ready() ? icu.coll[lcid == LOCALE_INVARIANT][strength_mode(flags)] : NULL;
    int fold = (flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE)) != 0;
    int symbols = (flags & NORM_IGNORESYMBOLS) != 0, word = !(flags & SORT_STRINGSORT);
    uint16_t *sa = (uint16_t *)malloc(2u * (size_t)na + 2), *sb = (uint16_t *)malloc(2u * (size_t)nb + 2);
    int32_t ka = strip(a, na, sa, symbols, word), kb = strip(b, nb, sb, symbols, word);
    int r = coll_cmp(c, sa, ka, sb, kb, fold);
    if (!r && word) { /* tie-break: fewer hyphens/apostrophes first, then the full strings */
        int32_t fa = strip(a, na, sa, symbols, 0), fb = strip(b, nb, sb, symbols, 0);
        r = (fa - ka) - (fb - kb);
        if (!r) r = coll_cmp(c, sa, fa, sb, fb, fold);
    }
    free(sa);
    free(sb);
    return r < 0 ? -1 : r > 0;
}

/* CompareStringW(Locale, dwCmpFlags, lpString1, cchCount1, lpString2, cchCount2): CSTR_LESS_THAN
 * (1), CSTR_EQUAL (2), CSTR_GREATER_THAN (3), or 0 with ERROR_INVALID_FLAGS/PARAMETER
 * [doc: CompareStringW]. A negative count means NUL-terminated. */
W32_IMPORT(kernel32, CompareStringW) {
    uint32_t lcid = SG_ARG(0), flags = SG_ARG(1), s1 = SG_ARG(2), s2 = SG_ARG(4);
    int32_t n1 = (int32_t)SG_ARG(3), n2 = (int32_t)SG_ARG(5), l1, l2;
    if (flags & ~CMP_FLAGS) { hle_set_last_error(ctx, mem, ERROR_INVALID_FLAGS); SG_RETURN_STDCALL(0, 24); }
    if (!s1 || !s2 || (lcid != LOCALE_INVARIANT && !resolve_lcid(lcid))) {
        hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 24);
    }
    uint16_t *a = wstr_dup(mem, s1, n1, &l1), *b = wstr_dup(mem, s2, n2, &l2);
    int r = w32_nls_compare(lcid, flags, a, l1, b, l2);
    free(a);
    free(b);
    SG_RETURN_STDCALL(r + 2, 24);
}

/* CompareStringOrdinal(lpString1, cchCount1, lpString2, cchCount2, bIgnoreCase) (Vista+, not
 * imported by this exe): code-unit comparison, upper-casing first when asked [doc]. */
W32_IMPORT(kernel32, CompareStringOrdinal) {
    int32_t l1, l2;
    if (!SG_ARG(0) || !SG_ARG(2)) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); SG_RETURN_STDCALL(0, 20); }
    uint16_t *a = wstr_dup(mem, SG_ARG(0), (int32_t)SG_ARG(1), &l1), *b = wstr_dup(mem, SG_ARG(2), (int32_t)SG_ARG(3), &l2);
    int r = ordinal_cmp(a, l1, b, l2, SG_ARG(4) != 0);
    free(a);
    free(b);
    SG_RETURN_STDCALL(r + 2, 20);
}

/* ---- LCMapString --------------------------------------------------------------------------- */

/* Sort key of a host UTF-16 string, consistent with w32_nls_compare under the same flags: the
 * word-sort key (hyphens/apostrophes removed), a byte counting them, then the full key, each
 * key NUL-terminated, so byte-wise comparison of keys orders like CompareString. Returns bytes including the
 * final NUL; writes at most cap bytes when out != NULL. */
size_t w32_nls_sortkey(uint32_t lcid, uint32_t flags, const uint16_t *s, int32_t n, uint8_t *out, size_t cap) {
    void *c = icu_ready() ? icu.coll[lcid == LOCALE_INVARIANT][strength_mode(flags)] : NULL;
    int symbols = (flags & NORM_IGNORESYMBOLS) != 0, word = !(flags & SORT_STRINGSORT);
    uint16_t *t = (uint16_t *)malloc(2u * (size_t)n + 2);
    size_t total = 0;
    uint8_t *tmp = NULL;
    int32_t stripped = word ? strip(s, n, t, symbols, 1) : 0, full = strip(s, n, t, symbols, 0);
    for (int pass = word ? 0 : 1; pass < 2; pass++) {
        int32_t k = strip(s, n, t, symbols, pass == 0);
        if (pass == 1 && word) { /* the tie-break count, as one byte that is never 0 */
            int32_t d = full - stripped + 1;
            if (out && total < cap) out[total] = (uint8_t)(d > 255 ? 255 : d);
            total++;
        }
        size_t len;
        if (c) {
            int32_t need = icu.sortkey(c, t, k, NULL, 0);
            tmp = (uint8_t *)realloc(tmp, (size_t)need + 1);
            icu.sortkey(c, t, k, tmp, need);
            len = (size_t)need; /* includes ICU's terminating 0 */
        } else {
            tmp = (uint8_t *)realloc(tmp, 2u * (size_t)k + 1);
            len = 0;
            for (int32_t i = 0; i < k; i++) {
                uint32_t u = (flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE)) ? w32_nls_upper(t[i]) : t[i];
                u = (u & 0xffff) + 1; /* never a 0 byte pair */
                tmp[len++] = (uint8_t)(u >> 8 | 1);
                tmp[len++] = (uint8_t)u;
            }
            tmp[len++] = 0;
        }
        if (out) for (size_t i = 0; i < len && total + i < cap; i++) out[total + i] = tmp[i];
        total += len;
    }
    free(tmp);
    free(t);
    return total;
}

/* LCMapStringW/A(Locale, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest) [doc: LCMapStringW]:
 * LCMAP_LOWERCASE / UPPERCASE (one-to-one, the count stays the same, a counted -1 source includes
 * its NUL), LCMAP_BYTEREV, LCMAP_SORTKEY (bytes, NORM_* flags allowed). cchDest 0 returns the size
 * needed. Upper and lower together, or a sort key with case mapping, give ERROR_INVALID_FLAGS. */
uint32_t w32_lcmap(sg_ctx *ctx, uint8_t *mem, uint32_t lcid, uint32_t flags, uint32_t src, int32_t n, uint32_t dst,
                   int32_t cap, int wide) {
    if (!src || n == 0 || cap < 0 || (cap && !dst) || (lcid != LOCALE_INVARIANT && !resolve_lcid(lcid))) {
        hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint32_t casing = flags & (LCMAP_LOWERCASE | LCMAP_UPPERCASE);
    if (casing == (LCMAP_LOWERCASE | LCMAP_UPPERCASE) || ((flags & LCMAP_SORTKEY) && (casing || (flags & LCMAP_BYTEREV))) ||
        (!(flags & LCMAP_SORTKEY) && (flags & CMP_FLAGS & ~(NORM_LINGUISTIC_CASING))) ||
        !(flags & (LCMAP_LOWERCASE | LCMAP_UPPERCASE | LCMAP_SORTKEY | LCMAP_BYTEREV))) {
        hle_set_last_error(ctx, mem, ERROR_INVALID_FLAGS);
        return 0;
    }
    /* read the source as UTF-16 (A: code page 1252) */
    int32_t len;
    uint16_t *s;
    int with_nul = n < 0;
    if (wide) s = wstr_dup(mem, src, n, &len);
    else {
        if (n < 0) { n = 0; while (mem[src + (uint32_t)n]) n++; }
        len = n;
        s = (uint16_t *)malloc(2u * (size_t)len + 2);
        for (int32_t i = 0; i < len; i++) s[i] = (uint16_t)w32_cp1252_to_unicode(mem[src + (uint32_t)i]);
        s[len] = 0;
    }
    uint32_t ret = 0;
    if (flags & LCMAP_SORTKEY) {
        size_t need = w32_nls_sortkey(lcid, flags, s, len, NULL, 0);
        if (!cap) ret = (uint32_t)need;
        else if ((size_t)cap < need) hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER);
        else { w32_nls_sortkey(lcid, flags, s, len, mem + dst, (size_t)cap); ret = (uint32_t)need; }
        free(s);
        return ret;
    }
    for (int32_t i = 0; i < len; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < len && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) {
            uint32_t cp = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            uint32_t m = casing == LCMAP_LOWERCASE ? w32_nls_lower(cp) : casing ? w32_nls_upper(cp) : cp;
            if (m >= 0x10000) { m -= 0x10000; s[i] = (uint16_t)(0xd800 + (m >> 10)); s[i + 1] = (uint16_t)(0xdc00 + (m & 0x3ff)); }
            i++;
            continue;
        }
        if (casing) {
            uint32_t m = casing == LCMAP_LOWERCASE ? w32_nls_lower(c) : w32_nls_upper(c);
            if (m < 0x10000) s[i] = (uint16_t)m;
        }
    }
    if (flags & LCMAP_BYTEREV)
        for (int32_t i = 0; i < len; i++) s[i] = (uint16_t)(s[i] << 8 | s[i] >> 8);
    uint32_t units = (uint32_t)len + (with_nul ? 1u : 0u);
    if (!cap) ret = units;
    else if ((uint32_t)cap < units) hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER);
    else {
        for (uint32_t i = 0; i < units; i++) {
            if (wide) sg_st16(mem, dst + 2 * i, s[i]);
            else {
                int b = w32_unicode_to_cp1252(s[i]);
                mem[dst + i] = (uint8_t)(b < 0 ? '?' : b);
            }
        }
        ret = units;
    }
    free(s);
    return ret;
}
W32_IMPORT(kernel32, LCMapStringW) {
    SG_RETURN_STDCALL(w32_lcmap(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), (int32_t)SG_ARG(3), SG_ARG(4), (int32_t)SG_ARG(5), 1), 24);
}
W32_IMPORT(kernel32, LCMapStringA) {
    SG_RETURN_STDCALL(w32_lcmap(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), (int32_t)SG_ARG(3), SG_ARG(4), (int32_t)SG_ARG(5), 0), 24);
}
