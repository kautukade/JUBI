/* ctype.h — freestanding shim: what lwIP gets for <ctype.h>.
 *
 * lwIP leaves LWIP_NO_CTYPE_H at 0, so lwip/arch.h maps its lwip_isdigit() and
 * friends onto these. The compiled set reaches isdigit/isxdigit/islower/isspace
 * (core/ipv4/ip4_addr.c, ipaddr_aton) and tolower (core/dns.c). The remaining
 * four complete the set arch.h maps, so enabling a vendored file that needs one
 * — LWIP_IPV6 turns on core/ipv6/ip6_addr.c, which does — is a config change
 * rather than a link error. Nothing outside lwIP includes this. */
#ifndef PORT_CTYPE_H
#define PORT_CTYPE_H

static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int isalpha(int c)  { return islower(c) || isupper(c); }
static inline int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
static inline int tolower(int c)  { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int toupper(int c)  { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

#endif /* PORT_CTYPE_H */
