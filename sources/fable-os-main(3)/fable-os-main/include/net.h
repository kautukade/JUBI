/* net.h — bring up networking and ask the AI a question over HTTPS. */
#ifndef NET_H
#define NET_H

/* Initialise lwIP + the e1000 netif, the TLS config, and resolve the API host.
 * Call once, after drivers_init(). Returns 0 on success. */
int net_init(void);

/* Re-attempt ONLY the DNS lookup of the API host, with a short timeout. Returns
 * 0 if the host resolved (the caller may then bind model_tls_transport()), -1
 * otherwise.
 *
 * This exists because net_init() resolves once, and a single lost UDP query at
 * boot otherwise leaves a machine whose only interface is the model with no way
 * to ask for a retry. Returns -1 immediately, without waiting, when the stack
 * never came up at all (no NIC) — there is nothing a lookup could fix, and a
 * per-sentence timeout would look like a hang. Safe to call repeatedly. */
int net_retry_resolve(void);

/* Send `question` to the Anthropic Messages API over TLS and print the reply.
 * Returns 0 if a response was received (even an API error like 401), -1 on a
 * connection/TLS failure. */
int net_ask(const char *question);

#endif /* NET_H */
