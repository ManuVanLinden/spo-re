/* w32_crypt.c -- advapi32 CryptAcquireContextW, CryptGenRandom, CryptReleaseContext.
 *
 * The game's OpenSSL 0.9.8g seeds its PRNG in RAND_poll (0x11aa300) from CryptGenRandom. The import table
 * described these as "backed by arc4random" but only the weak "unimplemented" stubs were linked, so with
 * SPORE_ONLINE=1 the first HTTPS connection would abort. Here they are, strongly: the provider handle is a
 * pseudo handle, CryptGenRandom fills the buffer from arc4random_buf (the kernel CSPRNG), and nothing else of
 * CryptoAPI is offered (the exe imports only these three). Same observable result as Windows: a seeded PRNG.
 */
#include <stdlib.h>
#include <string.h>

#include "w32_int.h"

#define ERROR_INVALID_PARAMETER 87

/* BOOL CryptAcquireContextW(HCRYPTPROV *phProv, LPCWSTR container, LPCWSTR provider, DWORD type, DWORD flags) */
SG_IMPORT(advapi32, CryptAcquireContextW) {
    uint32_t out = SG_ARG(0);
    if (!out) {
        hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 20);
    }
    sg_st32(mem, out, 0xc5c5c001u); /* a pseudo HCRYPTPROV; nothing is stored behind it */
    SG_RETURN_STDCALL(1, 20);
}

/* BOOL CryptGenRandom(HCRYPTPROV hProv, DWORD dwLen, BYTE *pbBuffer) */
SG_IMPORT(advapi32, CryptGenRandom) {
    uint32_t n = SG_ARG(1), buf = SG_ARG(2);
    if (!buf && n) {
        hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 12);
    }
    if (n) arc4random_buf(mem + buf, n);
    SG_RETURN_STDCALL(1, 12);
}

/* BOOL CryptReleaseContext(HCRYPTPROV hProv, DWORD flags) */
SG_IMPORT(advapi32, CryptReleaseContext) { SG_RETURN_STDCALL(1, 8); }
