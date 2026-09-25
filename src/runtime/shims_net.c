/*
 * The socket layer -- twenty functions, and the most valuable twenty in the
 * whole import table.
 *
 * client.dll's entire network surface is:
 *
 *   WSAStartup WSACleanup WSAGetLastError socket bind closesocket
 *   sendto recvfrom select ioctlsocket getsockname getsockopt setsockopt
 *   gethostbyname inet_addr inet_ntoa htonl htons ntohl ntohs
 *
 * No connect, no send, no recv, no listen, no accept. The world protocol is
 * **UDP**, and that is why this file exists long before the renderer does:
 * rendering is D3D9 through COM vtables and an asset pipeline, while talking is
 * this. The client can hold a full conversation with a server while the screen
 * is still black, and the client's own protocol code is a better authority on
 * the wire format than any packet capture taken in 2009.
 *
 * Two jobs:
 *
 *   1. **Redirect.** gethostbyname and inet_addr answer with the loopback
 *      sidecar, so the client talks to our server and nothing leaves the
 *      machine. Same shape tstorecomp uses: the server runs inside the app on
 *      127.0.0.1, started at launch.
 *
 *   2. **Log.** Every datagram in and out, with peer, length and a hex window.
 *      That log is the deliverable for the server track.
 *
 * Everything forwards to real Winsock, so the client holds genuine SOCKET
 * handles. Structures it passes (sockaddr, fd_set, timeval) are usable
 * directly: 32-bit host, g_mem_base = 0, so guest pointers are host pointers.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

#define ARG(n)   MEM32(g_esp + 4u + 4u * (n))
#define ARGP(n)  ((void *)(uintptr_t)ARG(n))
#define ARGCS(n) ((const char *)(uintptr_t)ARG(n))
#define RETP(p)  RET((uint32_t)(uintptr_t)(p))

/* Where the sidecar lives. MXO_SERVER overrides it, so pointing the client at
 * a server on another host is an environment variable rather than a rebuild. */
static uint32_t redirect_addr(void)
{
    static uint32_t cached;
    if (cached) return cached;
    const char *env = getenv("MXO_SERVER");
    cached = env ? inet_addr(env) : inet_addr("127.0.0.1");
    if (cached == INADDR_NONE) cached = inet_addr("127.0.0.1");
    return cached;
}

/* ---- packet log ----------------------------------------------------- */

static FILE *g_plog;
static uint64_t g_tx, g_rx, g_tx_bytes, g_rx_bytes;

static void plog_open(void)
{
    if (g_plog) return;
    const char *path = getenv("MXO_PACKET_LOG");
    g_plog = fopen(path ? path : "work/packets.log", "w");
    if (g_plog)
        fprintf(g_plog, "# dir peer:port len  bytes\n");
}

/* A hex window rather than the whole datagram: the protocol's shape lives in
 * the first few dozen bytes (opcode, sequence, flags), and a full dump of a
 * 1 KB world update buries it. Raise MXO_PACKET_BYTES when a specific message
 * needs reading in full. */
static void plog(const char *dir, const struct sockaddr *sa, const void *buf, int len)
{
    plog_open();
    if (!g_plog) return;

    static int window = -1;
    if (window < 0) {
        const char *w = getenv("MXO_PACKET_BYTES");
        window = w ? atoi(w) : 48;
    }

    char peer[64] = "-";
    if (sa && sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        snprintf(peer, sizeof peer, "%s:%u", inet_ntoa(in->sin_addr),
                 ntohs(in->sin_port));
    }

    fprintf(g_plog, "%s %-21s %5d  ", dir, peer, len);
    const unsigned char *p = (const unsigned char *)buf;
    int n = len < window ? len : window;
    for (int i = 0; i < n; i++) fprintf(g_plog, "%02X", p[i]);
    if (len > n) fprintf(g_plog, "... (+%d)", len - n);
    fputc('\n', g_plog);
    fflush(g_plog);
}

void recomp_net_report(void)
{
    if (!g_tx && !g_rx) return;
    printf("  [net] sent %llu datagram(s) / %llu bytes, "
           "received %llu / %llu bytes\n",
           (unsigned long long)g_tx, (unsigned long long)g_tx_bytes,
           (unsigned long long)g_rx, (unsigned long long)g_rx_bytes);
}

/* ---- startup -------------------------------------------------------- */

void imp_WSAStartup(void)
{
    WSADATA wsa;
    int rc = WSAStartup((WORD)ARG(0), &wsa);
    /* The client reads the WSADATA it passed, not ours, so copy it across. */
    if (rc == 0 && ARG(1)) memcpy(ARGP(1), &wsa, sizeof wsa);
    printf("  [net] WSAStartup -> %d (sidecar at %s)\n", rc,
           getenv("MXO_SERVER") ? getenv("MXO_SERVER") : "127.0.0.1");
    RET((uint32_t)rc); STDRET(2);
}

void imp_WSACleanup(void)      { RET((uint32_t)WSACleanup());      STDRET(0); }
void imp_WSAGetLastError(void) { RET((uint32_t)WSAGetLastError()); STDRET(0); }

/* ---- sockets -------------------------------------------------------- */

void imp_socket(void)
{
    SOCKET s = socket((int)ARG(0), (int)ARG(1), (int)ARG(2));
    printf("  [net] socket(af=%u type=%u proto=%u) -> %u%s\n",
           ARG(0), ARG(1), ARG(2), (uint32_t)s,
           ARG(1) == SOCK_DGRAM ? "  (UDP)" : "");
    RET((uint32_t)s); STDRET(3);
}

void imp_bind(void)
{
    const struct sockaddr *sa = (const struct sockaddr *)ARGP(1);
    int rc = bind((SOCKET)ARG(0), sa, (int)ARG(2));
    if (sa && sa->sa_family == AF_INET)
        printf("  [net] bind(%u) -> %d  port %u\n", ARG(0), rc,
               ntohs(((const struct sockaddr_in *)sa)->sin_port));
    RET((uint32_t)rc); STDRET(3);
}

void imp_closesocket(void) { RET((uint32_t)closesocket((SOCKET)ARG(0))); STDRET(1); }

void imp_sendto(void)
{
    /* Redirect on the way out. The client resolved an address through
     * gethostbyname, which already answers with the sidecar -- but a hard-coded
     * or cached address would slip past that, so the destination is rewritten
     * here too. Belt and braces, and it means one place to change. */
    struct sockaddr_in *to = (struct sockaddr_in *)ARGP(4);
    if (to && to->sin_family == AF_INET &&
        to->sin_addr.s_addr != redirect_addr()) {
        to->sin_addr.s_addr = redirect_addr();
    }

    int rc = sendto((SOCKET)ARG(0), (const char *)ARGP(1), (int)ARG(2),
                    (int)ARG(3), (const struct sockaddr *)to, (int)ARG(5));
    if (rc > 0) { g_tx++; g_tx_bytes += (uint64_t)rc; }
    plog("TX", (const struct sockaddr *)to, ARGP(1), rc > 0 ? rc : (int)ARG(2));
    RET((uint32_t)rc); STDRET(6);
}

void imp_recvfrom(void)
{
    int rc = recvfrom((SOCKET)ARG(0), (char *)ARGP(1), (int)ARG(2),
                      (int)ARG(3), (struct sockaddr *)ARGP(4), (int *)ARGP(5));
    if (rc > 0) {
        g_rx++; g_rx_bytes += (uint64_t)rc;
        plog("RX", (const struct sockaddr *)ARGP(4), ARGP(1), rc);
    }
    RET((uint32_t)rc); STDRET(6);
}

int gt_select(int, fd_set *, fd_set *, fd_set *, const struct timeval *);  /* guest_threads.c */

/* Yields to other guest threads while waiting -- the receive thread lives here. */
void imp_select(void)
{
    RET((uint32_t)gt_select((int)ARG(0), (fd_set *)ARGP(1), (fd_set *)ARGP(2),
                         (fd_set *)ARGP(3), (const struct timeval *)ARGP(4)));
    STDRET(5);
}

void imp_ioctlsocket(void)
{
    RET((uint32_t)ioctlsocket((SOCKET)ARG(0), (long)ARG(1), (u_long *)ARGP(2)));
    STDRET(3);
}

void imp_getsockname(void)
{
    RET((uint32_t)getsockname((SOCKET)ARG(0), (struct sockaddr *)ARGP(1), (int *)ARGP(2)));
    STDRET(3);
}

void imp_getsockopt(void)
{
    RET((uint32_t)getsockopt((SOCKET)ARG(0), (int)ARG(1), (int)ARG(2),
                             (char *)ARGP(3), (int *)ARGP(4)));
    STDRET(5);
}

void imp_setsockopt(void)
{
    RET((uint32_t)setsockopt((SOCKET)ARG(0), (int)ARG(1), (int)ARG(2),
                             (const char *)ARGP(3), (int)ARG(4)));
    STDRET(5);
}

/* ---- name resolution: the redirect ---------------------------------- */

void imp_gethostbyname(void)
{
    /* Answer every lookup with the sidecar. The client never reaches the
     * network, which is the point: this is a closed loop, and a client that
     * silently found a real host would make the packet log meaningless.
     *
     * The hostent and everything it points at are static host storage, which
     * the client can dereference directly (32-bit host, 1:1 mapping). Real
     * gethostbyname returns a per-thread static too, so the lifetime the client
     * expects is the one it gets.
     */
    static struct hostent he;
    static uint32_t addr;
    static char *addr_list[2];
    static char name[256];

    const char *want = ARGCS(0);
    snprintf(name, sizeof name, "%s", want ? want : "localhost");
    addr = redirect_addr();
    addr_list[0] = (char *)&addr;
    addr_list[1] = NULL;

    he.h_name      = name;
    he.h_aliases   = &addr_list[1];      /* an empty, NULL-terminated list */
    he.h_addrtype  = AF_INET;
    he.h_length    = 4;
    he.h_addr_list = addr_list;

    printf("  [net] gethostbyname(\"%s\") -> sidecar\n", name);
    RETP(&he); STDRET(1);
}

void imp_inet_addr(void)
{
    /* A literal address in a config file must not escape either. */
    const char *s = ARGCS(0);
    uint32_t real = s ? inet_addr(s) : INADDR_NONE;
    if (real != INADDR_NONE && real != htonl(INADDR_LOOPBACK)) {
        printf("  [net] inet_addr(\"%s\") -> sidecar\n", s);
        real = redirect_addr();
    }
    RET(real); STDRET(1);
}

void imp_inet_ntoa(void)
{
    struct in_addr a;
    a.s_addr = ARG(0);
    RETP(inet_ntoa(a)); STDRET(1);
}

/* ---- byte order ----------------------------------------------------- */

void imp_htonl(void) { RET(htonl(ARG(0))); STDRET(1); }
void imp_ntohl(void) { RET(ntohl(ARG(0))); STDRET(1); }
void imp_htons(void) { RET(htons((uint16_t)ARG(0))); STDRET(1); }
void imp_ntohs(void) { RET(ntohs((uint16_t)ARG(0))); STDRET(1); }
