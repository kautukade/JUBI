/* lwipopts.h — lwIP configuration for fable-os.
 * Bare-metal, single-threaded (NO_SYS), IPv4 + TCP/UDP + DNS, polled. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* No OS: use the raw/callback API, no threads, no sockets/netconn. */
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_NETIF_API              0

/* Single network interface. */
#define LWIP_SINGLE_NETIF           1

/* Memory: lwIP-managed pools/heap (no external malloc). TLS records are up to
 * 16 KiB, so the heap and pools are sized generously. */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (512 * 1024)
#define PBUF_POOL_SIZE              48
#define MEMP_NUM_TCP_SEG            48
#define MEMP_NUM_TCP_PCB            5
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_SYS_TIMEOUT        10
#define MEMP_NUM_ALTCP_PCB          MEMP_NUM_TCP_PCB

/* Protocols. */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             1
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0

/* TCP sizing. */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)

/* Checksums in software (the e1000 only handles the Ethernet CRC). */
#define LWIP_CHKSUM_ALGORITHM       2

/* Trim things we don't use. */
#define LWIP_STATS                  0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_NETIF_HOSTNAME         0

/* Debugging off (we print our own progress). */
#define LWIP_DEBUG                  0

/* ASSERTIONS ARE ON, AND A FAILED ONE HALTS THE MACHINE.
 *
 * LWIP_NOASSERT is deliberately not defined, so every LWIP_ASSERT in the
 * vendored tree is live, and port/arch/cc.h routes LWIP_PLATFORM_ASSERT to
 * panic() — which on this kernel is `for(;;) { cli; hlt; }`. tcp_in.c and
 * pbuf.c alone carry dozens of assertions on the RECEIVE path, so this is a
 * decision about untrusted network input: a peer that trips one takes the
 * machine down permanently, with no interface left to explain it.
 *
 * It is kept that way on purpose for now. This kernel has no memory protection
 * and one address space; an lwIP invariant that has already been violated means
 * the packet path is in a state its authors say is impossible, and continuing
 * from there corrupts silently instead of loudly. Halting is legible: the last
 * line printed is "lwIP ASSERT: <the invariant>".
 *
 * The alternative worth building later is a per-assert reaction that drops the
 * pbuf and returns, rather than either extreme. That needs a change in
 * port/arch/cc.h (not here) and a way to report it to the operator. */

/* TLS via the altcp layer + mbedTLS port.
 *
 * ALTCP_MBEDTLS_AUTHMODE is handed straight to mbedtls_ssl_conf_authmode() by
 * lwip/src/apps/altcp_tls/altcp_tls_mbedtls.c, and it is the switch that
 * decides whether a failed chain verification aborts the handshake or is merely
 * recorded and ignored:
 *
 *   0  MBEDTLS_SSL_VERIFY_NONE      certificate parsed, never trust-checked.
 *                                   Encrypted, NOT authenticated: anything that
 *                                   can answer on port 443 gets the session.
 *   2  MBEDTLS_SSL_VERIFY_REQUIRED  chain must build to a trusted root, the
 *                                   hostname must match, and the validity
 *                                   period must contain "now", or the handshake
 *                                   fails.
 *
 * (The literals are used rather than the mbedTLS names because this header is
 * consumed by lwIP's own headers before any mbedTLS header is available.)
 *
 * Default is 0 — the historical behaviour, unchanged, because other work in
 * this tree boots against it. `make EXTRA_CFLAGS=-DFABLEOS_VERIFY_CERTS` selects
 * 2, at which point net/net.c must also install the CA chain and the hostname;
 * see include/tls_ca.h for the whole picture and for which roots are pinned.
 * VERIFY_OPTIONAL (1) is deliberately not offered: it verifies and then
 * continues anyway, which is the worst of both worlds. */
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
#ifdef FABLEOS_VERIFY_CERTS
#define ALTCP_MBEDTLS_AUTHMODE      2   /* MBEDTLS_SSL_VERIFY_REQUIRED */
#else
#define ALTCP_MBEDTLS_AUTHMODE      0   /* MBEDTLS_SSL_VERIFY_NONE */
#endif

#endif /* LWIPOPTS_H */
