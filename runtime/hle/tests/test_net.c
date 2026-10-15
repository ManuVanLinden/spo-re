/* WSAStartup / WSACleanup (wsock32 ordinals 115/116): version negotiation and WSADATA layout.
 * The offline "network down" behaviour, then (SPORE_ONLINE=1 set in this test process only)
 * the Winsock calls through the shims against a host-side echo server on 127.0.0.1. */
#include "harness.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

void hle_net_reread_env(void); /* net_wsock.c test hook */

static void test_startup(void) {
    uint32_t d = gbuf(400);
    CHECK_EQ(CALL(sg_imp_wsock32_ord116, 0), 0xffffffffu); /* not started */
    CHECK_EQ(CALL(sg_imp_wsock32_ord115, 8, 0x0000, d), 10092);
    CHECK_EQ(CALL(sg_imp_wsock32_ord115, 8, 0x0202, 0), 10014);
    CHECK_EQ(CALL(sg_imp_wsock32_ord115, 8, 0x0202, d), 0);
    CHECK_EQ(sg_ld16(M, d), 0x0202);
    CHECK_EQ(sg_ld16(M, d + 2), 0x0202);
    CHECK_STR(hstr(d + 4), "WinSock 2.0");
    CHECK_STR(hstr(d + 261), "Running");
    CHECK_EQ(sg_ld16(M, d + 390), 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord115, 8, 0x0101, d), 0);
    CHECK_EQ(sg_ld16(M, d), 0x0101);
    CHECK_EQ(sg_ld16(M, d + 390), 32767);
    CHECK_EQ(CALL(sg_imp_wsock32_ord115, 8, 0x0303, d), 0); /* above 2.2: 2.2 is offered */
    CHECK_EQ(sg_ld16(M, d), 0x0202);
    CHECK_EQ(CALL(sg_imp_wsock32_ord116, 0), 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord116, 0), 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord116, 0), 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord116, 0), 0xffffffffu);
}

#define WSAGetLastError() CALL0(sg_imp_wsock32_ord111, 0)
#define SOCKET_ERROR 0xffffffffu

/* SPORE_ONLINE=0 (the default until 2026-10-13, now the opt-out): the offline stub. */
static void test_offline(void) {
    uint32_t set = gbuf(8), tv = gbuf(8);
    setenv("SPORE_ONLINE", "0", 1);
    hle_net_reread_env();
    CHECK_EQ(CALL(sg_imp_wsock32_ord23, 12, 2, 1, 6), 0xffffffffu); /* socket */
    CHECK_EQ(WSAGetLastError(), 10050);                             /* WSAENETDOWN */
    CHECK_EQ(CALL(sg_imp_ws2_32_WSASocketA, 24, 2, 2, 0, 0, 0, 0), 0xffffffffu);
    CHECK_EQ(WSAGetLastError(), 10050);
    CHECK_EQ(CALL(sg_imp_wsock32_ord4, 12, 0x100, gbuf(16), 16), SOCKET_ERROR); /* connect */
    CHECK_EQ(WSAGetLastError(), 10038);                                         /* WSAENOTSOCK */
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, 0x100), SOCKET_ERROR);               /* closesocket */
    CHECK_EQ(CALL(sg_imp_wsock32_ord52, 4, gstr("localhost")), 0);             /* gethostbyname */
    CHECK_EQ(WSAGetLastError(), 11004);
    sg_st32(M, set, 1), sg_st32(M, set + 4, 0x100);
    CHECK_EQ(CALL(sg_imp_wsock32_ord18, 20, 0, set, 0, 0, tv), SOCKET_ERROR);  /* select */
    CHECK_EQ(CALL(sg_imp_wsock32_ord151, 8, 0x100, set), 0);                   /* __WSAFDIsSet */
    CHECK_EQ(CALL(sg_imp_wsock32_ord10, 4, gstr("10.1.2.3")), 0x0302010au);   /* inet_addr */
}

/* ---- host echo server ------------------------------------------------------------------------- */

static int echo_listen_fd;
static void *echo_thread(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(echo_listen_fd, NULL, NULL);
        char buf[4096];
        ssize_t n;
        if (c < 0) return NULL;
        while ((n = recv(c, buf, sizeof buf, 0)) > 0) send(c, buf, (size_t)n, 0);
        close(c);
    }
}
static uint16_t host_listener(int *fdp) { /* 127.0.0.1, ephemeral port; returns the port (host order) */
    struct sockaddr_in sa = {0};
    socklen_t sl = sizeof sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sa.sin_family = AF_INET, sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&sa, sizeof sa);
    listen(fd, 8);
    getsockname(fd, (struct sockaddr *)&sa, &sl);
    *fdp = fd;
    return ntohs(sa.sin_port);
}
static uint32_t gsockaddr(uint32_t ip_host, uint16_t port) { /* a Winsock sockaddr_in in guest memory */
    uint32_t a = gbuf(16);
    uint16_t pn = htons(port);
    uint32_t in = htonl(ip_host);
    sg_st16(M, a, 2);
    memcpy(M + a + 2, &pn, 2);
    memcpy(M + a + 4, &in, 4);
    return a;
}

static void test_online(void) {
    pthread_t th;
    int refused_fd;
    uint16_t port, refused;
    uint32_t s, s2, buf = gbuf(256), rd = gbuf(4 + 4 * 64), wr = gbuf(4 + 4 * 64), ex = gbuf(4 + 4 * 64);
    uint32_t tv = gbuf(8), arg = gbuf(4), optv = gbuf(8), optl = gbuf(4), sa_out = gbuf(16), sl_out = gbuf(4);
    uint32_t d = gbuf(400);

    setenv("SPORE_ONLINE", "1", 1);
    hle_net_reread_env();
    CHECK_EQ(CALL(sg_imp_wsock32_ord115, 8, 0x0202, d), 0);

    port = host_listener(&echo_listen_fd);
    pthread_create(&th, NULL, echo_thread, NULL);
    refused = host_listener(&refused_fd);
    close(refused_fd); /* nothing listens there now: connect is refused */

    /* WSASetLastError / WSAGetLastError (per thread: the TEB) */
    CALL(sg_imp_ws2_32_ord112, 4, 1234);
    CHECK_EQ(WSAGetLastError(), 1234);
    CHECK_EQ(sg_ld32(M, G_TEB + SG_TEB_LAST_ERROR), 1234);

    /* socket: a table handle, not a host fd */
    s = CALL(sg_imp_wsock32_ord23, 12, 2, 1, 6);
    CHECK(s != 0xffffffffu && s >= 0x100 && (s & 3) == 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord23, 12, 23, 1, 6), 0xffffffffu); /* AF_INET6: not offered */
    CHECK_EQ(WSAGetLastError(), 10047);

    /* blocking connect, send/recv round trip */
    CHECK_EQ(CALL(sg_imp_wsock32_ord4, 12, s, gsockaddr(INADDR_LOOPBACK, port), 16), 0);
    memcpy(M + buf, "hello spore", 11);
    CHECK_EQ(CALL(sg_imp_wsock32_ord19, 16, s, buf, 11, 0), 11);
    memset(M + buf, 0, 32);
    CHECK_EQ(CALL(sg_imp_wsock32_ord16, 16, s, buf, 64, 0), 11);
    CHECK(!memcmp(M + buf, "hello spore", 11));

    /* getpeername / getsockname: Winsock sockaddr_in layout */
    sg_st32(M, sl_out, 16);
    CHECK_EQ(CALL(sg_imp_wsock32_ord5, 12, s, sa_out, sl_out), 0);
    CHECK_EQ(sg_ld16(M, sa_out), 2);
    CHECK_EQ(M[sa_out + 2] << 8 | M[sa_out + 3], port);
    CHECK_EQ(sg_ld32(M, sa_out + 4), 0x0100007fu);
    CHECK_EQ(sg_ld32(M, sl_out), 16);
    sg_st32(M, sl_out, 16);
    CHECK_EQ(CALL(sg_imp_wsock32_ord6, 12, s, sa_out, sl_out), 0);
    CHECK_EQ(sg_ld16(M, sa_out), 2);
    sg_st32(M, sl_out, 8);
    CHECK_EQ(CALL(sg_imp_wsock32_ord6, 12, s, sa_out, sl_out), SOCKET_ERROR); /* too short */
    CHECK_EQ(WSAGetLastError(), 10014);

    /* options the game uses */
    sg_st32(M, optv, 1);
    CHECK_EQ(CALL(sg_imp_wsock32_ord21, 20, s, 6, 1, optv, 4), 0); /* TCP_NODELAY */
    sg_st32(M, optv, 0), sg_st32(M, optl, 4);
    CHECK_EQ(CALL(sg_imp_wsock32_ord7, 20, s, 6, 1, optv, optl), 0);
    CHECK_EQ(sg_ld32(M, optv), 1);
    sg_st32(M, optv, 1);
    CHECK_EQ(CALL(sg_imp_wsock32_ord21, 20, s, 0xffff, 8, optv, 4), 0); /* SO_KEEPALIVE */
    sg_st32(M, optv, 65536);
    CHECK_EQ(CALL(sg_imp_wsock32_ord21, 20, s, 0xffff, 0x1002, optv, 4), 0); /* SO_RCVBUF */
    sg_st32(M, optv, 0), sg_st32(M, optl, 4);
    CHECK_EQ(CALL(sg_imp_wsock32_ord7, 20, s, 0xffff, 0x1002, optv, optl), 0);
    CHECK(sg_ld32(M, optv) >= 65536);
    sg_st32(M, optv, 1500);
    CHECK_EQ(CALL(sg_imp_wsock32_ord21, 20, s, 0xffff, 0x1006, optv, 4), 0); /* SO_RCVTIMEO 1.5 s */
    sg_st32(M, optv, 0), sg_st32(M, optl, 4);
    CHECK_EQ(CALL(sg_imp_wsock32_ord7, 20, s, 0xffff, 0x1006, optv, optl), 0);
    CHECK_EQ(sg_ld32(M, optv), 1500);
    sg_st32(M, optv, 100);
    CHECK_EQ(CALL(sg_imp_wsock32_ord21, 20, s, 0xffff, 0x1006, optv, 4), 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord16, 16, s, buf, 16, 0), SOCKET_ERROR); /* nothing arrives in 100 ms */
    CHECK_EQ(WSAGetLastError(), 10060);                                     /* WSAETIMEDOUT, as Winsock */
    sg_st16(M, optv, 1), sg_st16(M, optv + 2, 3);
    CHECK_EQ(CALL(sg_imp_wsock32_ord21, 20, s, 0xffff, 0x80, optv, 4), 0); /* SO_LINGER {1, 3 s} */
    sg_st32(M, optv, 0), sg_st32(M, optl, 4);
    CHECK_EQ(CALL(sg_imp_wsock32_ord7, 20, s, 0xffff, 0x80, optv, optl), 0);
    CHECK_EQ(sg_ld16(M, optv), 1);
    CHECK_EQ(sg_ld16(M, optv + 2), 3);
    sg_st32(M, optv, 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord21, 20, s, 0xffff, 0xff7f, optv, 4), 0); /* SO_DONTLINGER FALSE: linger on */
    sg_st32(M, optv, 1);
    CHECK_EQ(CALL(sg_imp_wsock32_ord21, 20, s, 0xffff, 0x3001, optv, 4), 0); /* unknown: ignored, success */
    sg_st32(M, optv, 77), sg_st32(M, optl, 4);
    CHECK_EQ(CALL(sg_imp_wsock32_ord7, 20, s, 0xffff, 0x1007, optv, optl), 0); /* SO_ERROR */
    CHECK_EQ(sg_ld32(M, optv), 0);

    /* FIONREAD, shutdown, closesocket */
    memcpy(M + buf, "abc", 3);
    CHECK_EQ(CALL(sg_imp_wsock32_ord19, 16, s, buf, 3, 0), 3);
    sg_st32(M, rd, 1), sg_st32(M, rd + 4, s), sg_st32(M, tv, 2), sg_st32(M, tv + 4, 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord18, 20, 1, rd, 0, 0, tv), 1);
    sg_st32(M, arg, 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord12, 12, s, 0x4004667fu, arg), 0);
    CHECK_EQ(sg_ld32(M, arg), 3);
    CHECK_EQ(CALL(sg_imp_wsock32_ord16, 16, s, buf, 3, 0), 3);
    CHECK_EQ(CALL(sg_imp_wsock32_ord22, 8, s, 1), 0); /* SD_SEND: the echo server closes */
    CHECK_EQ(CALL(sg_imp_wsock32_ord16, 16, s, buf, 16, 0), 0); /* graceful close */
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s), 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s), SOCKET_ERROR);
    CHECK_EQ(WSAGetLastError(), 10038);
    CHECK_EQ(CALL(sg_imp_wsock32_ord19, 16, s, buf, 3, 0), SOCKET_ERROR);
    CHECK_EQ(WSAGetLastError(), 10038);

    /* blocking connect to a refused port */
    s = CALL(sg_imp_wsock32_ord23, 12, 2, 1, 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord4, 12, s, gsockaddr(INADDR_LOOPBACK, refused), 16), SOCKET_ERROR);
    CHECK_EQ(WSAGetLastError(), 10061); /* WSAECONNREFUSED */
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s), 0);

    /* non-blocking connect + select (Winsock: success in writefds) */
    s = CALL(sg_imp_wsock32_ord23, 12, 2, 1, 0);
    sg_st32(M, arg, 1);
    CHECK_EQ(CALL(sg_imp_wsock32_ord12, 12, s, 0x8004667eu, arg), 0); /* FIONBIO */
    {
        uint32_t r = CALL(sg_imp_wsock32_ord4, 12, s, gsockaddr(INADDR_LOOPBACK, port), 16);
        CHECK(r == 0 || (r == SOCKET_ERROR && WSAGetLastError() == 10035)); /* WSAEWOULDBLOCK, not EINPROGRESS */
    }
    sg_st32(M, wr, 1), sg_st32(M, wr + 4, s), sg_st32(M, ex, 1), sg_st32(M, ex + 4, s);
    sg_st32(M, tv, 2), sg_st32(M, tv + 4, 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord18, 20, 0, 0, wr, ex, tv), 1);
    CHECK_EQ(CALL(sg_imp_wsock32_ord151, 8, s, wr), 1);
    CHECK_EQ(CALL(sg_imp_wsock32_ord151, 8, s, ex), 0);
    CHECK_EQ(sg_ld32(M, ex), 0);
    /* non-blocking recv with nothing to read */
    CHECK_EQ(CALL(sg_imp_wsock32_ord16, 16, s, buf, 16, 0), SOCKET_ERROR);
    CHECK_EQ(WSAGetLastError(), 10035);
    /* select with a timeout and nothing ready: 0 and the set emptied */
    sg_st32(M, rd, 1), sg_st32(M, rd + 4, s), sg_st32(M, tv, 0), sg_st32(M, tv + 4, 20000);
    CHECK_EQ(CALL(sg_imp_wsock32_ord18, 20, 1, rd, 0, 0, tv), 0);
    CHECK_EQ(sg_ld32(M, rd), 0);
    memcpy(M + buf, "nonblocking", 11);
    CHECK_EQ(CALL(sg_imp_wsock32_ord19, 16, s, buf, 11, 0), 11);
    sg_st32(M, rd, 1), sg_st32(M, rd + 4, s), sg_st32(M, tv, 2), sg_st32(M, tv + 4, 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord18, 20, 1, rd, 0, 0, tv), 1);
    CHECK_EQ(CALL(sg_imp_wsock32_ord151, 8, s, rd), 1);
    memset(M + buf, 0, 16);
    CHECK_EQ(CALL(sg_imp_wsock32_ord16, 16, s, buf, 16, 0), 11);
    CHECK(!memcmp(M + buf, "nonblocking", 11));
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s), 0);

    /* non-blocking connect to a refused port: exceptfds only, SO_ERROR = WSAECONNREFUSED */
    s = CALL(sg_imp_wsock32_ord23, 12, 2, 1, 0);
    sg_st32(M, arg, 1);
    CHECK_EQ(CALL(sg_imp_wsock32_ord12, 12, s, 0x8004667eu, arg), 0);
    {
        uint32_t r = CALL(sg_imp_wsock32_ord4, 12, s, gsockaddr(INADDR_LOOPBACK, refused), 16);
        uint32_t e = WSAGetLastError();
        CHECK(r == SOCKET_ERROR && (e == 10035 || e == 10061));
        if (e == 10035) {
            sg_st32(M, wr, 1), sg_st32(M, wr + 4, s), sg_st32(M, ex, 1), sg_st32(M, ex + 4, s);
            sg_st32(M, tv, 2), sg_st32(M, tv + 4, 0);
            CHECK_EQ(CALL(sg_imp_wsock32_ord18, 20, 0, 0, wr, ex, tv), 1);
            CHECK_EQ(sg_ld32(M, wr), 0);
            CHECK_EQ(CALL(sg_imp_wsock32_ord151, 8, s, ex), 1);
            sg_st32(M, optv, 0), sg_st32(M, optl, 4);
            CHECK_EQ(CALL(sg_imp_wsock32_ord7, 20, s, 0xffff, 0x1007, optv, optl), 0);
            CHECK_EQ(sg_ld32(M, optv), 10061);
        } /* else refused at once (loopback may do that): nothing is pending */
    }
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s), 0);

    /* select errors: no sockets at all; a handle that is not a socket */
    sg_st32(M, rd, 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord18, 20, 0, rd, 0, 0, tv), SOCKET_ERROR);
    CHECK_EQ(WSAGetLastError(), 10022);
    sg_st32(M, rd, 1), sg_st32(M, rd + 4, 0x12345);
    CHECK_EQ(CALL(sg_imp_wsock32_ord18, 20, 0, rd, 0, 0, tv), SOCKET_ERROR);
    CHECK_EQ(WSAGetLastError(), 10038);

    /* bind / listen / accept on the guest side, a host client connects */
    s = CALL(sg_imp_wsock32_ord23, 12, 2, 1, 6);
    CHECK_EQ(CALL(sg_imp_wsock32_ord2, 12, s, gsockaddr(INADDR_LOOPBACK, 0), 16), 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord13, 8, s, 0x7fffffff), 0); /* SOMAXCONN */
    sg_st32(M, sl_out, 16);
    CHECK_EQ(CALL(sg_imp_wsock32_ord6, 12, s, sa_out, sl_out), 0);
    {
        uint16_t lp = (uint16_t)(M[sa_out + 2] << 8 | M[sa_out + 3]);
        int c = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in to = {0};
        to.sin_family = AF_INET, to.sin_port = htons(lp), to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK_EQ(connect(c, (struct sockaddr *)&to, sizeof to), 0);
        sg_st32(M, sl_out, 16);
        s2 = CALL(sg_imp_wsock32_ord1, 12, s, sa_out, sl_out);
        CHECK(s2 != 0xffffffffu && s2 != s);
        CHECK_EQ(sg_ld16(M, sa_out), 2);
        CHECK_EQ(sg_ld32(M, sa_out + 4), 0x0100007fu);
        send(c, "xy", 2, 0);
        CHECK_EQ(CALL(sg_imp_wsock32_ord16, 16, s2, buf, 8, 0), 2);
        close(c);
        CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s2), 0);
    }
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s), 0);

    /* UDP: sendto / recvfrom between two guest sockets */
    s = CALL(sg_imp_wsock32_ord23, 12, 2, 2, 17);
    s2 = CALL(sg_imp_wsock32_ord23, 12, 2, 2, 17);
    CHECK_EQ(CALL(sg_imp_wsock32_ord2, 12, s, gsockaddr(INADDR_LOOPBACK, 0), 16), 0);
    sg_st32(M, sl_out, 16);
    CHECK_EQ(CALL(sg_imp_wsock32_ord6, 12, s, sa_out, sl_out), 0);
    {
        uint16_t up = (uint16_t)(M[sa_out + 2] << 8 | M[sa_out + 3]);
        uint32_t from = gbuf(16), froml = gbuf(4);
        memcpy(M + buf, "datagram", 8);
        CHECK_EQ(CALL(sg_imp_wsock32_ord20, 24, s2, buf, 8, 0, gsockaddr(INADDR_LOOPBACK, up), 16), 8);
        memset(M + buf, 0, 16);
        sg_st32(M, froml, 16);
        CHECK_EQ(CALL(sg_imp_wsock32_ord17, 24, s, buf, 16, 0, from, froml), 8);
        CHECK(!memcmp(M + buf, "datagram", 8));
        CHECK_EQ(sg_ld16(M, from), 2);
        CHECK_EQ(sg_ld32(M, from + 4), 0x0100007fu);
    }
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s), 0);
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s2), 0);

    /* WSASocketA + WSAIoctl(SIO_GET_INTERFACE_LIST), as 0x94c130 calls them */
    s = CALL(sg_imp_ws2_32_WSASocketA, 24, 2, 2, 0, 0, 0, 0);
    CHECK(s != 0xffffffffu);
    {
        uint32_t list = gbuf(0x5f0), got = gbuf(4), n, lo = 0;
        CHECK_EQ(CALL(sg_imp_ws2_32_WSAIoctl, 36, s, 0x4004747fu, 0, 0, list, 0x5f0, got, 0, 0), 0);
        n = sg_ld32(M, got) / 76;
        CHECK(n >= 1 && sg_ld32(M, got) % 76 == 0);
        for (uint32_t i = 0; i < n; i++)
            if (sg_ld32(M, list + 76 * i + 8) == 0x0100007fu) lo = sg_ld32(M, list + 76 * i);
        CHECK_EQ(lo & 5u, 5u); /* IFF_UP | IFF_LOOPBACK */
        CHECK_EQ(CALL(sg_imp_ws2_32_WSAIoctl, 36, s, 0x4004747fu, 0, 0, list, 10, got, 0, 0), SOCKET_ERROR);
        CHECK_EQ(WSAGetLastError(), 10014);
        CHECK_EQ(CALL(sg_imp_ws2_32_WSAIoctl, 36, s, 0x98000001u, 0, 0, list, 0x5f0, got, 0, 0), SOCKET_ERROR);
        CHECK_EQ(WSAGetLastError(), 10045);
    }
    CHECK_EQ(CALL(sg_imp_wsock32_ord3, 4, s), 0);

    /* gethostbyname: hostent in guest memory; the game reads h_addr_list[0][0] (0x94c11f) */
    {
        uint32_t he = CALL(sg_imp_wsock32_ord52, 4, gstr("127.0.0.1"));
        CHECK(he != 0);
        if (he) {
            CHECK_EQ(sg_ld16(M, he + 8), 2);
            CHECK_EQ(sg_ld16(M, he + 10), 4);
            CHECK_EQ(sg_ld32(M, sg_ld32(M, sg_ld32(M, he + 12))), 0x0100007fu);
            CHECK_EQ(sg_ld32(M, sg_ld32(M, he + 4)), 0); /* no aliases */
        }
        he = CALL(sg_imp_wsock32_ord52, 4, gstr("localhost"));
        CHECK(he != 0);
        if (he) CHECK_EQ(sg_ld32(M, sg_ld32(M, sg_ld32(M, he + 12))), 0x0100007fu);
        CHECK_EQ(CALL(sg_imp_wsock32_ord52, 4, 0), 0);
        CHECK_EQ(WSAGetLastError(), 10014);
    }

    shutdown(echo_listen_fd, SHUT_RDWR);
    close(echo_listen_fd);
    pthread_join(th, NULL);
    CHECK_EQ(CALL(sg_imp_wsock32_ord116, 0), 0);
    setenv("SPORE_ONLINE", "0", 1);
    hle_net_reread_env();
    CHECK_EQ(CALL(sg_imp_wsock32_ord23, 12, 2, 1, 6), 0xffffffffu); /* back to "network down" */
    CHECK_EQ(WSAGetLastError(), 10050);
}

void test_net(void) {
    int before = t_pass;
    test_startup();
    test_offline();
    test_online();
    printf("  net: %d checks (loopback echo with SPORE_ONLINE=1 in this process only)\n", t_pass - before);
}
