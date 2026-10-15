/* net_wsock.c -- wsock32 start-up (reached by the boot to WinMain).
 *
 * The static constructor 0x1300330 calls WSAStartup (wsock32 ordinal 115) during _initterm.
 * Winsock start-up has no host state to create on macOS (BSD sockets need none), so these are
 * real implementations of the documented contract, not stubs:
 *   int WSAStartup(WORD wVersionRequired, LPWSADATA lpWSAData)  stdcall, 8 bytes
 *   int WSACleanup(void)                                        stdcall, 0 bytes
 * WSADATA (x86, 400 bytes): wVersion @0, wHighVersion @2, szDescription[257] @4,
 * szSystemStatus[129] @261, iMaxSockets @390, iMaxUdpDg @392, lpVendorInfo @396.
 * Version negotiation follows the Winsock 2 documentation: the highest version supported is 2.2;
 * a request with major version 0 fails with WSAVERNOTSUPPORTED (10092); otherwise wVersion is the
 * lower of the request and 2.2. For versions >= 2, iMaxSockets and iMaxUdpDg are 0 (ignored);
 * for 1.x they are 32767 and 65467 [inf: values seen on Windows 7, from documentation]. */
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "hle_imports.h"

#define WSAVERNOTSUPPORTED 10092u
#define WSAEFAULT 10014u
#define WSANOTINITIALISED 10093u

static volatile int wsa_refs;

static int ver_less(uint32_t a, uint32_t b) { /* LOBYTE major, HIBYTE minor */
    return (a & 0xff) != (b & 0xff) ? (a & 0xff) < (b & 0xff) : ((a >> 8) & 0xff) < ((b >> 8) & 0xff);
}

SG_IMPORT(wsock32, ord115) { /* WSAStartup */
    uint32_t req = SG_ARG(0) & 0xffffu, data = SG_ARG(1), ver;
    if ((req & 0xff) == 0) {
        ctx->eax = WSAVERNOTSUPPORTED;
        SG_RET(8);
    }
    if (!data) {
        ctx->eax = WSAEFAULT;
        SG_RET(8);
    }
    ver = ver_less(req, 0x0202u) ? req : 0x0202u;
    memset(mem + data, 0, 400);
    sg_st16(mem, data + 0, (uint16_t)ver);
    sg_st16(mem, data + 2, 0x0202u);
    memcpy(mem + data + 4, "WinSock 2.0", 12);
    memcpy(mem + data + 261, "Running", 8);
    sg_st16(mem, data + 390, (ver & 0xff) >= 2 ? 0 : 32767);
    sg_st16(mem, data + 392, (ver & 0xff) >= 2 ? 0 : 65467);
    __atomic_add_fetch(&wsa_refs, 1, __ATOMIC_SEQ_CST);
    ctx->eax = 0;
    SG_RET(8);
}

SG_IMPORT(wsock32, ord116) { /* WSACleanup: SOCKET_ERROR (-1) if WSAStartup was not called */
    int left = __atomic_sub_fetch(&wsa_refs, 1, __ATOMIC_SEQ_CST);
    if (left < 0) {
        __atomic_add_fetch(&wsa_refs, 1, __ATOMIC_SEQ_CST);
        ctx->eax = 0xffffffffu; /* WSAGetLastError would report WSANOTINITIALISED */
    } else {
        ctx->eax = 0;
    }
    SG_RET(0);
}

/* ---- Sockets: real BSD sockets by default (since 2026-10-13), the offline stub with SPORE_ONLINE=0 ----------------
 *
 * Offline stub (SPORE_ONLINE=0; the only mode before 2026-10-13):
 * [obs] The AudioSystem thread (0xa22810 -> 0xa45100 -> 0x94d320) calls socket(af, type, proto)
 * at 0x94d33a. On INVALID_SOCKET the wrapper calls WSAGetLastError and its error callback and
 * returns false, so the game handles a host with no usable sockets. socket() therefore fails with
 * WSAENETDOWN, as on a PC whose network stack is down. WSAGetLastError reads a per-process value,
 * which is enough while every socket call fails the same way. closesocket on any handle reports
 * WSAENOTSOCK. WSASocketA (once an unimplemented stub) fails the same way as socket().
 *
 * SPORE_ONLINE=1 (the default):
 * every WSOCK32/WS2_32 import the exe uses maps onto a BSD socket with Winsock semantics. The
 * game's own UTFInternet HTTP client and its own OpenSSL 0.9.8g do everything above the socket:
 * these shims pass the game's bytes through unchanged and add nothing (no header, no proxy, no
 * TLS layer, no identifier). Details:
 *   - SOCKET handles are small integers from a table (0x100 + 4*slot, never a host fd).
 *   - WSAGetLastError/WSASetLastError use the TEB's LastErrorValue, i.e. per thread, as ws2_32
 *     does on Windows (WSAGetLastError is GetLastError there); host errno maps to WSAE* codes.
 *   - A non-blocking connect reports WSAEWOULDBLOCK (Winsock), not EINPROGRESS. select() reports
 *     its completion in writefds on success and in exceptfds on failure (Winsock), not as
 *     "writable" both ways (BSD); getsockopt(SO_ERROR) then returns the WSAE* code.
 *   - select converts Winsock fd_sets (u_int count + SOCKET array) to poll() and back.
 *   - get/setsockopt: the options the game uses (0x94d850-0x94e130 [obs]: SO_DEBUG, SO_REUSEADDR,
 *     SO_KEEPALIVE, SO_DONTROUTE, SO_BROADCAST, SO_LINGER, SO_OOBINLINE, SO_SNDBUF, SO_RCVBUF,
 *     SO_SNDTIMEO, SO_RCVTIMEO, TCP_NODELAY) plus SO_ERROR/SO_TYPE; others are logged and succeed.
 *   - gethostbyname returns a per-thread hostent in guest memory (IPv4 only, as Winsock 1.1).
 *   - WSAIoctl(SIO_GET_INTERFACE_LIST) (0x94c170) lists the host's IPv4 interfaces.
 * Logging (SPORE_NET_LOG=1; 2 adds every send/recv/select): one line per socket event with the
 * handle, peer address and port, byte counts and error codes. Payload bytes are never logged: the
 * login request carries the player's credentials. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define WSAEINTR 10004u
#define WSAEACCES 10013u
#define WSAEINVAL 10022u
#define WSAEMFILE 10024u
#define WSAEWOULDBLOCK 10035u
#define WSAEINPROGRESS 10036u
#define WSAEALREADY 10037u
#define WSAENOTSOCK 10038u
#define WSAEDESTADDRREQ 10039u
#define WSAEMSGSIZE 10040u
#define WSAEPROTOTYPE 10041u
#define WSAENOPROTOOPT 10042u
#define WSAEPROTONOSUPPORT 10043u
#define WSAESOCKTNOSUPPORT 10044u
#define WSAEOPNOTSUPP 10045u
#define WSAEPFNOSUPPORT 10046u
#define WSAEAFNOSUPPORT 10047u
#define WSAEADDRINUSE 10048u
#define WSAEADDRNOTAVAIL 10049u
#define WSAENETDOWN 10050u
#define WSAENETUNREACH 10051u
#define WSAENETRESET 10052u
#define WSAECONNABORTED 10053u
#define WSAECONNRESET 10054u
#define WSAENOBUFS 10055u
#define WSAEISCONN 10056u
#define WSAENOTCONN 10057u
#define WSAESHUTDOWN 10058u
#define WSAETIMEDOUT 10060u
#define WSAECONNREFUSED 10061u
#define WSAEHOSTDOWN 10064u
#define WSAEHOSTUNREACH 10065u
#define WSAHOST_NOT_FOUND 11001u
#define WSATRY_AGAIN 11002u
#define WSANO_RECOVERY 11003u
#define WSANO_DATA 11004u
#define SOCKET_ERROR 0xffffffffu
#define INVALID_SOCKET 0xffffffffu

static volatile uint32_t wsa_last_error; /* offline mode only (see above) */

/* ---- mode and logging --------------------------------------------------------------------------- */

static int net_online = -1, net_log = -1;

static void net_read_env(void) {
    const char *s = getenv("SPORE_ONLINE"), *l = getenv("SPORE_NET_LOG");
    net_log = l ? atoi(l) : 0;
    /* On by default since 2026-10-13, when an EA login through these shims was confirmed to work.
     * SPORE_ONLINE=0 restores the offline stub. The game's own login.prop decides whether it contacts EA. */
    net_online = !s || !*s || atoi(s) != 0;
    if (net_online && net_log)
        fprintf(stderr, "[net] real sockets (the default; SPORE_ONLINE=0 for the offline stub); the game's own HTTP and OpenSSL code does the talking\n");
}
static int online(void) {
    if (__atomic_load_n(&net_online, __ATOMIC_ACQUIRE) < 0) net_read_env();
    return net_online;
}
/* Test hook (runtime/hle/tests/test_net.c): re-reads SPORE_ONLINE and SPORE_NET_LOG. */
void hle_net_reread_env(void) { net_read_env(); }

static void nlog(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void nlog(int level, const char *fmt, ...) {
    char line[512];
    va_list ap;
    if (net_log < level) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[net] %s\n", line);
}

/* ---- errors ------------------------------------------------------------------------------------- */

static uint32_t wsa_from_errno(int e) {
    switch (e) {
    case EINTR: return WSAEINTR;
    case EACCES: case EPERM: return WSAEACCES;
    case EFAULT: return 10014u; /* WSAEFAULT */
    case EINVAL: return WSAEINVAL;
    case EMFILE: case ENFILE: return WSAEMFILE;
    case EWOULDBLOCK: return WSAEWOULDBLOCK; /* == EAGAIN on macOS */
    case EINPROGRESS: return WSAEWOULDBLOCK;  /* a non-blocking connect, as Winsock reports it */
    case EALREADY: return WSAEALREADY;
    case EBADF: case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EPFNOSUPPORT: return WSAEPFNOSUPPORT;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET: return WSAENETRESET;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: case EPIPE: return WSAECONNRESET; /* send on a reset connection */
    case ENOBUFS: case ENOMEM: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ESHUTDOWN: return WSAESHUTDOWN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTDOWN: return WSAEHOSTDOWN;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default: return WSAEINVAL;
    }
}
static void set_err(sg_ctx *ctx, uint8_t *mem, uint32_t e) { hle_set_last_error(ctx, mem, e); }
static void set_errno_err(sg_ctx *ctx, uint8_t *mem) { set_err(ctx, mem, wsa_from_errno(errno)); }

/* ---- the socket table ----------------------------------------------------------------------------- */

#define NET_MAX 1024u
#define NET_BASE 0x100u
typedef struct net_sock {
    int fd;            /* host fd, -1 = free slot */
    uint8_t type;      /* SOCK_STREAM / SOCK_DGRAM */
    uint8_t nonblock;  /* FIONBIO */
    uint8_t connecting;/* a non-blocking connect is in flight */
    uint32_t pending;  /* WSAE* from a finished non-blocking connect, for getsockopt(SO_ERROR) */
    uint32_t peer_ip;  /* network order, for the log */
    uint16_t peer_port;
    uint64_t sent, rcvd;
} net_sock;
static net_sock socks[NET_MAX];
static pthread_mutex_t socks_lock = PTHREAD_MUTEX_INITIALIZER;
static int socks_init;

static uint32_t sock_alloc(int fd, int type) {
    uint32_t h = INVALID_SOCKET;
    pthread_mutex_lock(&socks_lock);
    if (!socks_init) {
        for (unsigned i = 0; i < NET_MAX; i++) socks[i].fd = -1;
        socks_init = 1;
    }
    for (unsigned i = 0; i < NET_MAX; i++)
        if (socks[i].fd < 0) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].fd = fd;
            socks[i].type = (uint8_t)type;
            h = NET_BASE + 4u * i;
            break;
        }
    pthread_mutex_unlock(&socks_lock);
    return h;
}
/* The slot behind a guest SOCKET, or NULL. The slot stays valid until closesocket frees it; the
 * game does not close a socket that another of its threads is using [inf], as on Windows. */
static net_sock *sock_get(uint32_t h) {
    net_sock *s = NULL;
    if (h < NET_BASE || (h - NET_BASE) % 4u || (h - NET_BASE) / 4u >= NET_MAX) return NULL;
    pthread_mutex_lock(&socks_lock);
    if (socks_init && socks[(h - NET_BASE) / 4u].fd >= 0) s = &socks[(h - NET_BASE) / 4u];
    pthread_mutex_unlock(&socks_lock);
    return s;
}
static const char *ipstr(uint32_t ip_net, char *buf) { /* buf >= 16 */
    struct in_addr a = {ip_net};
    inet_ntop(AF_INET, &a, buf, 16);
    return buf;
}

/* Winsock sockaddr_in (16 bytes): u16 sin_family (LE), u16 sin_port (BE), u32 sin_addr (BE), zero[8].
 * BSD's has sin_len and an 8-bit family in the same 16 bytes. */
static int sa_from_guest(uint8_t *mem, uint32_t p, uint32_t len, struct sockaddr_in *sa, uint32_t *err) {
    if (!p || len < 16) { *err = 10014u; return -1; }
    if (sg_ld16(mem, p) != AF_INET) { *err = WSAEAFNOSUPPORT; return -1; }
    memset(sa, 0, sizeof *sa);
    sa->sin_len = sizeof *sa;
    sa->sin_family = AF_INET;
    memcpy(&sa->sin_port, mem + p + 2, 2);
    memcpy(&sa->sin_addr, mem + p + 4, 4);
    return 0;
}
/* Writes a host address back to (p, *lenp) the Winsock way. Returns 0, or -1 with WSAEFAULT. */
static int sa_to_guest(uint8_t *mem, uint32_t p, uint32_t lenp, const struct sockaddr_in *sa) {
    if (!p) return 0;
    if (!lenp || (int32_t)sg_ld32(mem, lenp) < 16) return -1;
    memset(mem + p, 0, 16);
    sg_st16(mem, p, AF_INET);
    memcpy(mem + p + 2, &sa->sin_port, 2);
    memcpy(mem + p + 4, &sa->sin_addr, 4);
    sg_st32(mem, lenp, 16);
    return 0;
}

static int host_flags(uint32_t f) { /* MSG_OOB 1, MSG_PEEK 2, MSG_DONTROUTE 4 match; MSG_WAITALL 8 does not */
    return (int)(f & 7u) | ((f & 8u) ? MSG_WAITALL : 0);
}

/* ---- socket / WSASocketA ------------------------------------------------------------------------ */

static uint32_t do_socket(sg_ctx *ctx, uint8_t *mem, uint32_t af, uint32_t type, uint32_t proto, const char *who) {
    int fd, one = 1;
    uint32_t h;
    if (af != AF_INET) {
        set_err(ctx, mem, WSAEAFNOSUPPORT);
        nlog(1, "%s(af %u, type %u, proto %u) = INVALID_SOCKET (WSAEAFNOSUPPORT)", who, af, type, proto);
        return INVALID_SOCKET;
    }
    fd = socket(AF_INET, (int)type, (int)proto);
    if (fd < 0) {
        set_errno_err(ctx, mem);
        nlog(1, "%s(af %u, type %u, proto %u) = INVALID_SOCKET (errno %d -> %u)", who, af, type, proto, errno,
             hle_get_last_error(ctx, mem));
        return INVALID_SOCKET;
    }
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); /* EPIPE, not SIGPIPE, as on Windows */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    h = sock_alloc(fd, (int)type);
    if (h == INVALID_SOCKET) {
        close(fd);
        set_err(ctx, mem, WSAEMFILE);
        return INVALID_SOCKET;
    }
    nlog(1, "%s(af %u, type %s, proto %u) = 0x%x", who, af, type == SOCK_STREAM ? "stream" : type == SOCK_DGRAM ? "dgram" : "other",
         proto, h);
    return h;
}

SG_IMPORT(wsock32, ord23) { /* socket(af, type, protocol) */
    if (!online()) {
        static int logged;
        if (!logged++)
            hle_log(HLE_LOG_INFO, "socket(af %u, type %u, proto %u) = INVALID_SOCKET (WSAENETDOWN; no network sockets yet)",
                    SG_ARG(0), SG_ARG(1), SG_ARG(2));
        wsa_last_error = WSAENETDOWN;
        SG_RETURN_STDCALL(0xffffffffu, 12);
    }
    SG_RETURN_STDCALL(do_socket(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), "socket"), 12);
}

/* SOCKET WSASocketA(af, type, protocol, lpProtocolInfo, g, dwFlags): 0x94c146 calls it with
 * (AF_INET, SOCK_DGRAM, 0, NULL, 0, 0) to enumerate interfaces. No protocol info or groups. */
SG_IMPORT(ws2_32, WSASocketA) {
    if (!online()) {
        wsa_last_error = WSAENETDOWN;
        SG_RETURN_STDCALL(INVALID_SOCKET, 24);
    }
    if (SG_ARG(3) || SG_ARG(4)) nlog(1, "WSASocketA: protocol info / group ignored");
    SG_RETURN_STDCALL(do_socket(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), "WSASocketA"), 24);
}

SG_IMPORT(wsock32, ord111) { /* WSAGetLastError */
    SG_RETURN_STDCALL(online() ? hle_get_last_error(ctx, mem) : wsa_last_error, 0);
}
SG_IMPORT(ws2_32, ord112) { /* void WSASetLastError(int) (OpenSSL's clear_socket_error) */
    if (online()) set_err(ctx, mem, SG_ARG(0));
    else wsa_last_error = SG_ARG(0);
    SG_RET(4);
}

SG_IMPORT(wsock32, ord3) { /* closesocket(s) */
    net_sock *s;
    if (!online() || !(s = sock_get(SG_ARG(0)))) {
        if (online()) set_err(ctx, mem, WSAENOTSOCK);
        else wsa_last_error = WSAENOTSOCK;
        SG_RETURN_STDCALL(SOCKET_ERROR, 4);
    }
    {
        char ip[16];
        int fd = s->fd;
        nlog(1, "closesocket(0x%x) peer %s:%u sent %llu received %llu", SG_ARG(0), ipstr(s->peer_ip, ip),
             ntohs(s->peer_port), (unsigned long long)s->sent, (unsigned long long)s->rcvd);
        pthread_mutex_lock(&socks_lock);
        s->fd = -1;
        pthread_mutex_unlock(&socks_lock);
        close(fd);
    }
    SG_RETURN_STDCALL(0, 4);
}

/* Shared prologue for calls on a SOCKET: offline or unknown handle -> SOCKET_ERROR, WSAENOTSOCK. */
#define NET_SOCK(var, bytes, what)                                                               \
    net_sock *var;                                                                               \
    if (!online()) {                                                                             \
        static int logged;                                                                       \
        if (!logged++) hle_log(HLE_LOG_INFO, what "(socket 0x%x) = SOCKET_ERROR (WSAENOTSOCK)", SG_ARG(0)); \
        wsa_last_error = WSAENOTSOCK;                                                            \
        SG_RETURN_STDCALL(SOCKET_ERROR, bytes);                                                  \
    }                                                                                            \
    if (!(var = sock_get(SG_ARG(0)))) {                                                          \
        set_err(ctx, mem, WSAENOTSOCK);                                                          \
        nlog(1, what "(0x%x) = SOCKET_ERROR (WSAENOTSOCK)", SG_ARG(0));                          \
        SG_RETURN_STDCALL(SOCKET_ERROR, bytes);                                                  \
    }

/* ---- connect / bind / listen / accept ------------------------------------------------------------ */

SG_IMPORT(wsock32, ord4) { /* connect(s, name, namelen) */
    NET_SOCK(s, 12, "connect")
    struct sockaddr_in sa;
    uint32_t e;
    char ip[16];
    if (sa_from_guest(mem, SG_ARG(1), SG_ARG(2), &sa, &e)) {
        set_err(ctx, mem, e);
        SG_RETURN_STDCALL(SOCKET_ERROR, 12);
    }
    s->peer_ip = sa.sin_addr.s_addr;
    s->peer_port = sa.sin_port;
    if (connect(s->fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        nlog(1, "connect(0x%x, %s:%u) = 0", SG_ARG(0), ipstr(sa.sin_addr.s_addr, ip), ntohs(sa.sin_port));
        SG_RETURN_STDCALL(0, 12);
    }
    e = wsa_from_errno(errno);
    if (errno == EINPROGRESS) s->connecting = 1, s->pending = 0;
    set_err(ctx, mem, e);
    nlog(1, "connect(0x%x, %s:%u) = SOCKET_ERROR (%u%s)", SG_ARG(0), ipstr(sa.sin_addr.s_addr, ip), ntohs(sa.sin_port), e,
         errno == EINPROGRESS ? ", in progress" : "");
    SG_RETURN_STDCALL(SOCKET_ERROR, 12);
}

SG_IMPORT(wsock32, ord2) { /* bind(s, name, namelen) */
    NET_SOCK(s, 12, "bind")
    struct sockaddr_in sa;
    uint32_t e;
    char ip[16];
    if (sa_from_guest(mem, SG_ARG(1), SG_ARG(2), &sa, &e)) {
        set_err(ctx, mem, e);
        SG_RETURN_STDCALL(SOCKET_ERROR, 12);
    }
    if (bind(s->fd, (struct sockaddr *)&sa, sizeof sa)) {
        set_errno_err(ctx, mem);
        nlog(1, "bind(0x%x, %s:%u) = SOCKET_ERROR (%u)", SG_ARG(0), ipstr(sa.sin_addr.s_addr, ip), ntohs(sa.sin_port),
             hle_get_last_error(ctx, mem));
        SG_RETURN_STDCALL(SOCKET_ERROR, 12);
    }
    nlog(1, "bind(0x%x, %s:%u) = 0", SG_ARG(0), ipstr(sa.sin_addr.s_addr, ip), ntohs(sa.sin_port));
    SG_RETURN_STDCALL(0, 12);
}

SG_IMPORT(wsock32, ord13) { /* listen(s, backlog) */
    NET_SOCK(s, 8, "listen")
    int32_t bl = (int32_t)SG_ARG(1);
    if (bl > SOMAXCONN || bl < 0) bl = SOMAXCONN; /* SOMAXCONN on Windows is 0x7fffffff */
    if (listen(s->fd, bl)) {
        set_errno_err(ctx, mem);
        nlog(1, "listen(0x%x) = SOCKET_ERROR (%u)", SG_ARG(0), hle_get_last_error(ctx, mem));
        SG_RETURN_STDCALL(SOCKET_ERROR, 8);
    }
    nlog(1, "listen(0x%x, %d) = 0", SG_ARG(0), bl);
    SG_RETURN_STDCALL(0, 8);
}

SG_IMPORT(wsock32, ord1) { /* accept(s, addr, addrlen) */
    NET_SOCK(s, 12, "accept")
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = accept(s->fd, (struct sockaddr *)&sa, &sl), one = 1;
    uint32_t h;
    char ip[16];
    if (fd < 0) {
        set_errno_err(ctx, mem);
        nlog(errno == EWOULDBLOCK ? 2 : 1, "accept(0x%x) = INVALID_SOCKET (%u)", SG_ARG(0), hle_get_last_error(ctx, mem));
        SG_RETURN_STDCALL(INVALID_SOCKET, 12);
    }
    if (SG_ARG(1) && sa_to_guest(mem, SG_ARG(1), SG_ARG(2), &sa)) {
        close(fd);
        set_err(ctx, mem, 10014u);
        SG_RETURN_STDCALL(INVALID_SOCKET, 12);
    }
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    { /* Winsock: the accepted socket has the listener's non-blocking mode; set it explicitly */
        int fl = fcntl(fd, F_GETFL);
        if (fl >= 0) fcntl(fd, F_SETFL, s->nonblock ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
    }
    h = sock_alloc(fd, SOCK_STREAM);
    if (h == INVALID_SOCKET) {
        close(fd);
        set_err(ctx, mem, WSAEMFILE);
        SG_RETURN_STDCALL(INVALID_SOCKET, 12);
    }
    {
        net_sock *n = sock_get(h);
        n->nonblock = s->nonblock;
        n->peer_ip = sa.sin_addr.s_addr;
        n->peer_port = sa.sin_port;
    }
    nlog(1, "accept(0x%x) = 0x%x from %s:%u", SG_ARG(0), h, ipstr(sa.sin_addr.s_addr, ip), ntohs(sa.sin_port));
    SG_RETURN_STDCALL(h, 12);
}

static void name_call(sg_ctx *ctx, uint8_t *mem, net_sock *s, int peer) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int r = peer ? getpeername(s->fd, (struct sockaddr *)&sa, &sl) : getsockname(s->fd, (struct sockaddr *)&sa, &sl);
    if (r) {
        set_errno_err(ctx, mem);
        ctx->eax = SOCKET_ERROR;
    } else if (sa.sin_family != AF_INET || sa_to_guest(mem, SG_ARG(1), SG_ARG(2), &sa) || !SG_ARG(1)) {
        set_err(ctx, mem, 10014u);
        ctx->eax = SOCKET_ERROR;
    } else {
        ctx->eax = 0;
    }
}
SG_IMPORT(wsock32, ord5) { /* getpeername(s, name, namelen) */
    NET_SOCK(s, 12, "getpeername")
    name_call(ctx, mem, s, 1);
    SG_RET(12);
}
SG_IMPORT(wsock32, ord6) { /* getsockname(s, name, namelen) */
    NET_SOCK(s, 12, "getsockname")
    name_call(ctx, mem, s, 0);
    SG_RET(12);
}

SG_IMPORT(wsock32, ord22) { /* shutdown(s, how): SD_RECEIVE 0 / SD_SEND 1 / SD_BOTH 2 = SHUT_RD/WR/RDWR */
    NET_SOCK(s, 8, "shutdown")
    if (SG_ARG(1) > 2u) {
        set_err(ctx, mem, WSAEINVAL);
        SG_RETURN_STDCALL(SOCKET_ERROR, 8);
    }
    if (shutdown(s->fd, (int)SG_ARG(1))) {
        set_errno_err(ctx, mem);
        nlog(1, "shutdown(0x%x, %u) = SOCKET_ERROR (%u)", SG_ARG(0), SG_ARG(1), hle_get_last_error(ctx, mem));
        SG_RETURN_STDCALL(SOCKET_ERROR, 8);
    }
    nlog(1, "shutdown(0x%x, %u) = 0", SG_ARG(0), SG_ARG(1));
    SG_RETURN_STDCALL(0, 8);
}

/* ---- send / recv / sendto / recvfrom ------------------------------------------------------------- */

static uint32_t io_result(sg_ctx *ctx, uint8_t *mem, net_sock *s, uint32_t h, ssize_t n, int sending, const char *what) {
    if (n < 0) {
        /* A blocking socket whose SO_RCVTIMEO/SO_SNDTIMEO expired: Winsock says WSAETIMEDOUT. */
        uint32_t e = errno == EWOULDBLOCK && !s->nonblock ? WSAETIMEDOUT : wsa_from_errno(errno);
        set_err(ctx, mem, e);
        nlog(e == WSAEWOULDBLOCK ? 2 : 1, "%s(0x%x) = SOCKET_ERROR (%u)", what, h, e);
        return SOCKET_ERROR;
    }
    if (sending) s->sent += (uint64_t)n;
    else s->rcvd += (uint64_t)n;
    nlog(2, "%s(0x%x) = %zd bytes", what, h, n);
    return (uint32_t)n;
}

SG_IMPORT(wsock32, ord19) { /* send(s, buf, len, flags) */
    NET_SOCK(s, 16, "send")
    int32_t len = (int32_t)SG_ARG(2);
    if (len < 0 || (len && !SG_ARG(1))) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 16); }
    SG_RETURN_STDCALL(io_result(ctx, mem, s, SG_ARG(0), send(s->fd, mem + SG_ARG(1), (size_t)len, host_flags(SG_ARG(3))), 1, "send"), 16);
}
SG_IMPORT(wsock32, ord16) { /* recv(s, buf, len, flags) */
    NET_SOCK(s, 16, "recv")
    int32_t len = (int32_t)SG_ARG(2);
    if (len < 0 || (len && !SG_ARG(1))) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 16); }
    SG_RETURN_STDCALL(io_result(ctx, mem, s, SG_ARG(0), recv(s->fd, mem + SG_ARG(1), (size_t)len, host_flags(SG_ARG(3))), 0, "recv"), 16);
}
SG_IMPORT(wsock32, ord20) { /* sendto(s, buf, len, flags, to, tolen) */
    NET_SOCK(s, 24, "sendto")
    int32_t len = (int32_t)SG_ARG(2);
    struct sockaddr_in sa;
    uint32_t e;
    ssize_t n;
    if (len < 0 || (len && !SG_ARG(1))) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 24); }
    if (!SG_ARG(4)) {
        n = send(s->fd, mem + SG_ARG(1), (size_t)len, host_flags(SG_ARG(3)));
    } else {
        char ip[16];
        if (sa_from_guest(mem, SG_ARG(4), SG_ARG(5), &sa, &e)) { set_err(ctx, mem, e); SG_RETURN_STDCALL(SOCKET_ERROR, 24); }
        n = sendto(s->fd, mem + SG_ARG(1), (size_t)len, host_flags(SG_ARG(3)), (struct sockaddr *)&sa, sizeof sa);
        nlog(2, "sendto(0x%x, %s:%u) %d bytes", SG_ARG(0), ipstr(sa.sin_addr.s_addr, ip), ntohs(sa.sin_port), len);
    }
    SG_RETURN_STDCALL(io_result(ctx, mem, s, SG_ARG(0), n, 1, "sendto"), 24);
}
SG_IMPORT(wsock32, ord17) { /* recvfrom(s, buf, len, flags, from, fromlen) */
    NET_SOCK(s, 24, "recvfrom")
    int32_t len = (int32_t)SG_ARG(2);
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    ssize_t n;
    uint32_t r;
    if (len < 0 || (len && !SG_ARG(1))) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 24); }
    if (SG_ARG(4) && (!SG_ARG(5) || (int32_t)sg_ld32(mem, SG_ARG(5)) < 16)) {
        set_err(ctx, mem, 10014u);
        SG_RETURN_STDCALL(SOCKET_ERROR, 24);
    }
    memset(&sa, 0, sizeof sa);
    n = recvfrom(s->fd, mem + SG_ARG(1), (size_t)len, host_flags(SG_ARG(3)), (struct sockaddr *)&sa, &sl);
    r = io_result(ctx, mem, s, SG_ARG(0), n, 0, "recvfrom");
    if (n >= 0 && SG_ARG(4) && sa.sin_family == AF_INET) sa_to_guest(mem, SG_ARG(4), SG_ARG(5), &sa);
    SG_RETURN_STDCALL(r, 24);
}

/* ---- ioctlsocket / WSAIoctl ---------------------------------------------------------------------- */

#define W_FIONBIO 0x8004667eu
#define W_FIONREAD 0x4004667fu
#define W_SIOCATMARK 0x40047307u
SG_IMPORT(wsock32, ord12) { /* ioctlsocket(s, cmd, argp) -- wsock32 1.1 numbering, see below */
    NET_SOCK(s, 12, "ioctlsocket")
    uint32_t cmd = SG_ARG(1), argp = SG_ARG(2);
    if (!argp) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 12); }
    if (cmd == W_FIONBIO) {
        int fl = fcntl(s->fd, F_GETFL), on = sg_ld32(mem, argp) != 0;
        if (fl < 0 || fcntl(s->fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) < 0) {
            set_errno_err(ctx, mem);
            SG_RETURN_STDCALL(SOCKET_ERROR, 12);
        }
        s->nonblock = (uint8_t)on;
        nlog(2, "ioctlsocket(0x%x, FIONBIO, %d) = 0", SG_ARG(0), on);
        SG_RETURN_STDCALL(0, 12);
    }
    if (cmd == W_FIONREAD) {
        int n = 0;
        if (ioctl(s->fd, FIONREAD, &n) < 0) { set_errno_err(ctx, mem); SG_RETURN_STDCALL(SOCKET_ERROR, 12); }
        sg_st32(mem, argp, (uint32_t)n);
        SG_RETURN_STDCALL(0, 12);
    }
    if (cmd == W_SIOCATMARK) {
        int m = sockatmark(s->fd);
        if (m < 0) { set_errno_err(ctx, mem); SG_RETURN_STDCALL(SOCKET_ERROR, 12); }
        sg_st32(mem, argp, m ? 1u : 0u); /* Winsock: TRUE when no OOB data is waiting */
        SG_RETURN_STDCALL(0, 12);
    }
    nlog(1, "ioctlsocket(0x%x, 0x%x) = SOCKET_ERROR (WSAEINVAL; command not supported)", SG_ARG(0), cmd);
    set_err(ctx, mem, WSAEINVAL);
    SG_RETURN_STDCALL(SOCKET_ERROR, 12);
}

/* int WSAIoctl(s, dwIoControlCode, lpvInBuffer, cbInBuffer, lpvOutBuffer, cbOutBuffer,
 *              lpcbBytesReturned, lpOverlapped, lpCompletionRoutine): 36 bytes.
 * SIO_GET_INTERFACE_LIST fills INTERFACE_INFO records (76 bytes: u32 iiFlags, then iiAddress,
 * iiBroadcastAddress, iiNetmask as 24-byte sockaddr_gen holding a sockaddr_in). 0x94c19d reads
 * iiAddress.sin_addr of the record it wants [obs]. */
#define W_SIO_GET_INTERFACE_LIST 0x4004747fu
SG_IMPORT(ws2_32, WSAIoctl) {
    if (!online()) {
        wsa_last_error = WSAENOTSOCK;
        SG_RETURN_STDCALL(SOCKET_ERROR, 36);
    }
    {
        net_sock *s = sock_get(SG_ARG(0));
        uint32_t code = SG_ARG(1), out = SG_ARG(4), cap = SG_ARG(5), retp = SG_ARG(6), n = 0;
        struct ifaddrs *ifa = NULL, *i;
        if (!s) { set_err(ctx, mem, WSAENOTSOCK); SG_RETURN_STDCALL(SOCKET_ERROR, 36); }
        if (code != W_SIO_GET_INTERFACE_LIST || SG_ARG(7)) {
            nlog(1, "WSAIoctl(0x%x, 0x%x) = SOCKET_ERROR (WSAEOPNOTSUPP)", SG_ARG(0), code);
            set_err(ctx, mem, WSAEOPNOTSUPP);
            SG_RETURN_STDCALL(SOCKET_ERROR, 36);
        }
        if (!out || !retp) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 36); }
        if (getifaddrs(&ifa)) { set_errno_err(ctx, mem); SG_RETURN_STDCALL(SOCKET_ERROR, 36); }
        for (i = ifa; i; i = i->ifa_next) {
            uint32_t rec = out + 76u * n, fl = 0;
            const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
            if (!a || a->sin_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            if (76u * (n + 1) > cap) {
                freeifaddrs(ifa);
                set_err(ctx, mem, 10014u); /* WSAEFAULT: buffer too small, as Winsock */
                SG_RETURN_STDCALL(SOCKET_ERROR, 36);
            }
            memset(mem + rec, 0, 76);
            fl = 1u | ((i->ifa_flags & IFF_BROADCAST) ? 2u : 0) | ((i->ifa_flags & IFF_LOOPBACK) ? 4u : 0) |
                 ((i->ifa_flags & IFF_POINTOPOINT) ? 8u : 0) | ((i->ifa_flags & IFF_MULTICAST) ? 0x10u : 0);
            sg_st32(mem, rec, fl);
            sg_st16(mem, rec + 4, AF_INET);
            memcpy(mem + rec + 8, &a->sin_addr, 4);
            if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
                sg_st16(mem, rec + 28, AF_INET);
                memcpy(mem + rec + 32, &((const struct sockaddr_in *)i->ifa_broadaddr)->sin_addr, 4);
            }
            if (i->ifa_netmask) {
                sg_st16(mem, rec + 52, AF_INET);
                memcpy(mem + rec + 56, &((const struct sockaddr_in *)i->ifa_netmask)->sin_addr, 4);
            }
            n++;
        }
        freeifaddrs(ifa);
        sg_st32(mem, retp, 76u * n);
        nlog(1, "WSAIoctl(0x%x, SIO_GET_INTERFACE_LIST) = 0 (%u IPv4 interfaces)", SG_ARG(0), n);
        SG_RETURN_STDCALL(0, 36);
    }
}

/* ---- select / __WSAFDIsSet ----------------------------------------------------------------------- */

/* Winsock fd_set: u_int fd_count; SOCKET fd_array[FD_SETSIZE] (the game's FD_SETSIZE is its own
 * business: only fd_count entries are read and the ready ones are written back, compacted). */
#define SEL_MAX 256u
SG_IMPORT(wsock32, ord18) { /* select(nfds, readfds, writefds, exceptfds, timeout) */
    uint32_t sets[3] = {SG_ARG(1), SG_ARG(2), SG_ARG(3)}, tv = SG_ARG(4);
    struct pollfd pf[SEL_MAX];
    net_sock *ps[SEL_MAX];
    uint32_t hs[SEL_MAX];
    uint8_t want[SEL_MAX], got[SEL_MAX];
    unsigned n = 0, total = 0;
    int timeout_ms, r;
    if (!online()) {
        static int logged;
        if (!logged++) hle_log(HLE_LOG_INFO, "select(socket 0x%x) = SOCKET_ERROR (WSAENOTSOCK)", SG_ARG(0));
        wsa_last_error = WSAENOTSOCK;
        SG_RETURN_STDCALL(SOCKET_ERROR, 20);
    }
    for (int k = 0; k < 3; k++) {
        uint32_t cnt;
        if (!sets[k]) continue;
        cnt = sg_ld32(mem, sets[k]);
        for (uint32_t j = 0; j < cnt; j++) {
            uint32_t h = sg_ld32(mem, sets[k] + 4 + 4 * j);
            unsigned m;
            for (m = 0; m < n && hs[m] != h; m++) {}
            if (m == n) {
                net_sock *s = sock_get(h);
                if (!s) {
                    set_err(ctx, mem, WSAENOTSOCK);
                    nlog(1, "select: 0x%x is not a socket = SOCKET_ERROR (WSAENOTSOCK)", h);
                    SG_RETURN_STDCALL(SOCKET_ERROR, 20);
                }
                if (n == SEL_MAX) { set_err(ctx, mem, WSAEINVAL); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
                hs[n] = h, ps[n] = s, want[n] = 0, pf[n].fd = s->fd, pf[n].events = 0;
                n++;
            }
            want[m] |= (uint8_t)(1u << k);
            pf[m].events |= k == 0 ? POLLIN : k == 1 ? POLLOUT : (POLLPRI | (ps[m]->connecting ? POLLOUT : 0));
        }
    }
    if (!n) { /* Winsock: all three sets empty or NULL is WSAEINVAL */
        set_err(ctx, mem, WSAEINVAL);
        SG_RETURN_STDCALL(SOCKET_ERROR, 20);
    }
    if (!tv) timeout_ms = -1;
    else {
        int64_t sec = (int32_t)sg_ld32(mem, tv), usec = (int32_t)sg_ld32(mem, tv + 4);
        int64_t ms = sec * 1000 + (usec + 999) / 1000;
        if (sec < 0 || usec < 0) { set_err(ctx, mem, WSAEINVAL); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
        timeout_ms = ms > 0x7fffffff ? 0x7fffffff : (int)ms;
    }
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            r = poll(pf, n, timeout_ms);
            if (r >= 0 || errno != EINTR) break;
            if (timeout_ms > 0) {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                int el = (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
                if (el >= timeout_ms) { r = 0; break; }
                timeout_ms -= el, t0 = t1;
            }
        }
    }
    if (r < 0) {
        set_errno_err(ctx, mem);
        SG_RETURN_STDCALL(SOCKET_ERROR, 20);
    }
    for (unsigned m = 0; m < n; m++) {
        short re = pf[m].revents;
        net_sock *s = ps[m];
        got[m] = 0;
        if (s->connecting) {
            if (re & (POLLOUT | POLLERR | POLLHUP)) { /* the non-blocking connect finished */
                int e = 0;
                socklen_t el = sizeof e;
                getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &e, &el);
                s->connecting = 0;
                if (e) {
                    char ip[16];
                    s->pending = wsa_from_errno(e);
                    nlog(1, "connect(0x%x, %s:%u) failed: %u", hs[m], ipstr(s->peer_ip, ip), ntohs(s->peer_port), s->pending);
                    got[m] |= 4; /* Winsock: a failed connect shows in exceptfds only */
                } else {
                    char ip[16];
                    nlog(1, "connect(0x%x, %s:%u) completed", hs[m], ipstr(s->peer_ip, ip), ntohs(s->peer_port));
                    got[m] |= 2;
                    if (re & POLLIN) got[m] |= 1;
                }
            }
        } else {
            if (re & (POLLIN | POLLHUP | POLLERR)) got[m] |= 1; /* readable: data, EOF or a reset to report */
            if (re & (POLLOUT | POLLERR)) got[m] |= 2;
            if (re & POLLPRI) got[m] |= 4;
        }
        got[m] &= want[m];
    }
    for (int k = 0; k < 3; k++) {
        uint32_t cnt, w = 0;
        if (!sets[k]) continue;
        cnt = sg_ld32(mem, sets[k]);
        for (uint32_t j = 0; j < cnt; j++) {
            uint32_t h = sg_ld32(mem, sets[k] + 4 + 4 * j);
            unsigned m;
            for (m = 0; m < n && hs[m] != h; m++) {}
            if (m < n && (got[m] & (1u << k))) {
                uint32_t dup = 0;
                for (uint32_t q = 0; q < w; q++) dup |= sg_ld32(mem, sets[k] + 4 + 4 * q) == h;
                if (!dup) sg_st32(mem, sets[k] + 4 + 4 * w++, h), total++;
            }
        }
        sg_st32(mem, sets[k], w);
    }
    nlog(2, "select(%u sockets, timeout %d ms) = %u", n, timeout_ms, total);
    SG_RETURN_STDCALL(total, 20);
}

SG_IMPORT(wsock32, ord151) { /* __WSAFDIsSet(s, set) */
    uint32_t h = SG_ARG(0), set = SG_ARG(1), r = 0;
    if (online() && set) {
        uint32_t cnt = sg_ld32(mem, set);
        for (uint32_t j = 0; j < cnt && !r; j++) r = sg_ld32(mem, set + 4 + 4 * j) == h;
    }
    SG_RETURN_STDCALL(r, 8);
}

/* ---- getsockopt / setsockopt --------------------------------------------------------------------- */

#define W_SOL_SOCKET 0xffffu
#define W_IPPROTO_TCP 6u
#define W_SO_DONTLINGER 0xff7fu /* ~SO_LINGER */
#define W_SO_EXCLUSIVEADDRUSE 0xfffbu

static const char *opt_name(uint32_t level, uint32_t opt) {
    if (level == W_IPPROTO_TCP) return opt == 1 ? "TCP_NODELAY" : "tcp?";
    switch (opt) {
    case 0x1: return "SO_DEBUG";
    case 0x4: return "SO_REUSEADDR";
    case 0x8: return "SO_KEEPALIVE";
    case 0x10: return "SO_DONTROUTE";
    case 0x20: return "SO_BROADCAST";
    case 0x80: return "SO_LINGER";
    case 0x100: return "SO_OOBINLINE";
    case 0x1001: return "SO_SNDBUF";
    case 0x1002: return "SO_RCVBUF";
    case 0x1005: return "SO_SNDTIMEO";
    case 0x1006: return "SO_RCVTIMEO";
    case 0x1007: return "SO_ERROR";
    case 0x1008: return "SO_TYPE";
    default: return "?";
    }
}
/* Integer options with the same number on both sides (SOL_SOCKET is 0xffff on macOS too). */
static int int_opt(uint32_t level, uint32_t opt, int *hl, int *ho) {
    if (level == W_IPPROTO_TCP && opt == 1) { *hl = IPPROTO_TCP, *ho = TCP_NODELAY; return 1; }
    if (level != W_SOL_SOCKET) return 0;
    switch (opt) {
    case 0x1: case 0x4: case 0x8: case 0x10: case 0x20: case 0x100: case 0x1001: case 0x1002: case 0x1008:
        *hl = SOL_SOCKET, *ho = (int)opt;
        return 1;
    default: return 0;
    }
}

SG_IMPORT(wsock32, ord21) { /* setsockopt(s, level, optname, optval, optlen) */
    NET_SOCK(s, 20, "setsockopt")
    uint32_t level = SG_ARG(1), opt = SG_ARG(2), val = SG_ARG(3), len = SG_ARG(4);
    int hl, ho, r = 0;
    if (!val || (int32_t)len < 1) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
    if (int_opt(level, opt, &hl, &ho)) {
        /* Winsock accepts a BOOL as 1 or 4 bytes; buffers and the rest are ints. */
        int v = len >= 4 ? (int)sg_ld32(mem, val) : mem[val];
        if (level == W_SOL_SOCKET && opt != 0x1001 && opt != 0x1002) v = v != 0;
        r = setsockopt(s->fd, hl, ho, &v, sizeof v);
    } else if (level == W_SOL_SOCKET && (opt == 0x80 || opt == W_SO_DONTLINGER)) {
        struct linger lg;
        if (opt == 0x80) {
            if (len < 4) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
            lg.l_onoff = sg_ld16(mem, val) != 0, lg.l_linger = sg_ld16(mem, val + 2); /* u_short pair, seconds */
        } else {
            lg.l_onoff = (len >= 4 ? sg_ld32(mem, val) : mem[val]) == 0, lg.l_linger = 0;
        }
        r = setsockopt(s->fd, SOL_SOCKET, SO_LINGER_SEC, &lg, sizeof lg);
    } else if (level == W_SOL_SOCKET && (opt == 0x1005 || opt == 0x1006)) {
        uint32_t ms = len >= 4 ? sg_ld32(mem, val) : 0; /* Winsock: DWORD milliseconds; BSD: timeval */
        struct timeval t = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
        r = setsockopt(s->fd, SOL_SOCKET, opt == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, &t, sizeof t);
    } else {
        nlog(1, "setsockopt(0x%x, level 0x%x, option 0x%x %s) ignored = 0", SG_ARG(0), level, opt, opt_name(level, opt));
        SG_RETURN_STDCALL(0, 20);
    }
    if (r) {
        set_errno_err(ctx, mem);
        nlog(1, "setsockopt(0x%x, %s) = SOCKET_ERROR (%u)", SG_ARG(0), opt_name(level, opt), hle_get_last_error(ctx, mem));
        SG_RETURN_STDCALL(SOCKET_ERROR, 20);
    }
    nlog(2, "setsockopt(0x%x, %s) = 0", SG_ARG(0), opt_name(level, opt));
    SG_RETURN_STDCALL(0, 20);
}

SG_IMPORT(wsock32, ord7) { /* getsockopt(s, level, optname, optval, optlen) */
    NET_SOCK(s, 20, "getsockopt")
    uint32_t level = SG_ARG(1), opt = SG_ARG(2), val = SG_ARG(3), lenp = SG_ARG(4), out;
    int hl, ho, v = 0;
    socklen_t sl = sizeof v;
    if (!val || !lenp) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
    if (level == W_SOL_SOCKET && opt == 0x1007) { /* SO_ERROR: a finished non-blocking connect first */
        if (s->pending) out = s->pending, s->pending = 0;
        else {
            if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &v, &sl)) { set_errno_err(ctx, mem); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
            out = v ? wsa_from_errno(v) : 0;
        }
    } else if (int_opt(level, opt, &hl, &ho)) {
        if (getsockopt(s->fd, hl, ho, &v, &sl)) { set_errno_err(ctx, mem); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
        out = (uint32_t)v;
        if (level == W_IPPROTO_TCP || (opt != 0x1001 && opt != 0x1002 && opt != 0x1008)) out = v != 0; /* BOOL (macOS reports TCP_NODELAY as 4) */
    } else if (level == W_SOL_SOCKET && opt == 0x80) {
        struct linger lg;
        socklen_t ll = sizeof lg;
        if ((int32_t)sg_ld32(mem, lenp) < 4) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
        if (getsockopt(s->fd, SOL_SOCKET, SO_LINGER_SEC, &lg, &ll)) { set_errno_err(ctx, mem); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
        sg_st16(mem, val, lg.l_onoff ? 1 : 0);
        sg_st16(mem, val + 2, (uint16_t)lg.l_linger);
        sg_st32(mem, lenp, 4);
        SG_RETURN_STDCALL(0, 20);
    } else if (level == W_SOL_SOCKET && (opt == 0x1005 || opt == 0x1006)) {
        struct timeval t;
        socklen_t tl = sizeof t;
        if (getsockopt(s->fd, SOL_SOCKET, opt == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, &t, &tl)) {
            set_errno_err(ctx, mem);
            SG_RETURN_STDCALL(SOCKET_ERROR, 20);
        }
        out = (uint32_t)(t.tv_sec * 1000 + t.tv_usec / 1000);
    } else {
        nlog(1, "getsockopt(0x%x, level 0x%x, option 0x%x %s) unsupported: returns 0", SG_ARG(0), level, opt,
             opt_name(level, opt));
        out = 0;
    }
    {
        int32_t cap = (int32_t)sg_ld32(mem, lenp);
        if (cap >= 4) sg_st32(mem, val, out), sg_st32(mem, lenp, 4);
        else if (cap >= 1) mem[val] = (uint8_t)out, sg_st32(mem, lenp, 1); /* a 1-byte BOOL, as Winsock allows */
        else { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(SOCKET_ERROR, 20); }
    }
    SG_RETURN_STDCALL(0, 20);
}

/* ---- gethostbyname --------------------------------------------------------------------------------- */

/* Per-thread hostent in guest memory, reused by the next call on the same thread (Winsock keeps
 * one per thread too). Layout (x86): char *h_name @0, char **h_aliases @4, short h_addrtype @8,
 * short h_length @10, char **h_addr_list @12; 0x94c11f reads h_addr_list[0][0] [obs]. */
#define HE_ADDRS 16u
#define HE_NAME 256u
#define HE_SIZE (16u + 4u + 4u * (HE_ADDRS + 1u) + 4u * HE_ADDRS + HE_NAME)
static _Thread_local uint32_t he_block;

SG_IMPORT(wsock32, ord52) { /* gethostbyname(name) */
    char name[HE_NAME];
    struct addrinfo hints, *res = NULL, *ai;
    uint32_t he, aliases, list, addrs, nm, n = 0;
    int r;
    if (!online()) {
        static int logged;
        if (!logged++ && SG_ARG(0)) hle_log(HLE_LOG_INFO, "gethostbyname(\"%.64s\") = NULL (no network yet)", (const char *)mem + SG_ARG(0));
        wsa_last_error = 11004u; /* WSANO_DATA */
        SG_RETURN_STDCALL(0, 4);
    }
    if (!SG_ARG(0)) { set_err(ctx, mem, 10014u); SG_RETURN_STDCALL(0, 4); }
    hle_get_str(mem, SG_ARG(0), name, sizeof name);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    r = getaddrinfo(name, NULL, &hints, &res);
    if (r) {
        uint32_t e = r == EAI_NONAME ? WSAHOST_NOT_FOUND : r == EAI_AGAIN ? WSATRY_AGAIN :
                     r == EAI_NODATA ? WSANO_DATA : WSANO_RECOVERY;
        set_err(ctx, mem, e);
        nlog(1, "gethostbyname(\"%s\") = NULL (%s -> %u)", name, gai_strerror(r), e);
        SG_RETURN_STDCALL(0, 4);
    }
    if (!he_block && !(he_block = hle_guest_alloc(HE_SIZE, 8))) {
        freeaddrinfo(res);
        set_err(ctx, mem, WSAENOBUFS);
        SG_RETURN_STDCALL(0, 4);
    }
    he = he_block, aliases = he + 16, list = aliases + 4, addrs = list + 4 * (HE_ADDRS + 1), nm = addrs + 4 * HE_ADDRS;
    memset(mem + he, 0, HE_SIZE);
    for (ai = res; ai && n < HE_ADDRS; ai = ai->ai_next) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ai->ai_addr;
        uint32_t dup = 0;
        if (ai->ai_family != AF_INET) continue;
        for (uint32_t q = 0; q < n; q++) dup |= memcmp(mem + addrs + 4 * q, &a->sin_addr, 4) == 0;
        if (dup) continue;
        memcpy(mem + addrs + 4 * n, &a->sin_addr, 4);
        sg_st32(mem, list + 4 * n, addrs + 4 * n);
        n++;
    }
    snprintf((char *)mem + nm, HE_NAME, "%s", res->ai_canonname ? res->ai_canonname : name);
    freeaddrinfo(res);
    if (!n) {
        set_err(ctx, mem, WSANO_DATA);
        nlog(1, "gethostbyname(\"%s\") = NULL (no IPv4 address)", name);
        SG_RETURN_STDCALL(0, 4);
    }
    sg_st32(mem, he + 0, nm);
    sg_st32(mem, he + 4, aliases); /* empty, NULL-terminated */
    sg_st16(mem, he + 8, AF_INET);
    sg_st16(mem, he + 10, 4);
    sg_st32(mem, he + 12, list);
    {
        char ip[16];
        uint32_t a0;
        memcpy(&a0, mem + addrs, 4);
        nlog(1, "gethostbyname(\"%s\") = %s (%u address%s)", name, ipstr(a0, ip), n, n == 1 ? "" : "es");
    }
    SG_RETURN_STDCALL(he, 4);
}

/* Ordinals follow wsock32.dll (Winsock 1.1), not ws2_32: 10 is inet_addr and 12 is ioctlsocket.
 * tools/hle/sigs.py once had them the ws2_32 way round (10 ioctlsocket, 12 inet_ntoa); the
 * binary settles it [obs]: 0x94c0f0 pushes one argument to ordinal 10 and compares the result with
 * -1 (INADDR_NONE), and 0x94d5ab calls ordinal 12 with (s, FIONREAD 0x4004667f, &n). A boot failed
 * the return-address check at 0x943107 because ordinal 10 popped 12 bytes; the generated table now
 * has 4 and 12 bytes, and the shims pop the real counts. */
/* unsigned long inet_addr(const char *cp): wsock32 ordinal 10 (see above). Network byte order, as the
 * host's inet_addr; INADDR_NONE (0xffffffff) for a malformed string. */
#include <arpa/inet.h>
SG_IMPORT(wsock32, ord10) {
    char buf[64];
    uint32_t p = SG_ARG(0), r = 0xffffffffu;
    if (p) {
        snprintf(buf, sizeof buf, "%s", (const char *)mem + p);
        r = (uint32_t)inet_addr(buf);
    }
    SG_RETURN_STDCALL(r, 4);
}
