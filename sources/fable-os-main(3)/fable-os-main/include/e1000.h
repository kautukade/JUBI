/* e1000.h — Intel 82540EM/82545EM ("e1000") gigabit NIC driver.
 *
 * PURPOSE
 *   Move Ethernet frames between the kernel and the wire, and expose the one
 *   piece of hardware state that natural language keeps asking about: is the
 *   link up?
 *
 * RESPONSIBILITIES
 *   - Find the card on PCI, map BAR0, read the MAC out of the receive-address
 *     registers, and program the RX/TX descriptor rings.
 *   - Transmit and receive single frames by polling descriptor status bits.
 *     There are no interrupts: the whole kernel is polled and IF stays 0.
 *   - Implement the driver power lifecycle (driver.h: suspend/resume) for real
 *     — quiesce DMA, power the PHY down, and bring all of it back so that a
 *     TLS connection opened after resume actually completes.
 *
 * PUBLIC API
 *   e1000_init          bring-up; also the driver_t.init hook (self-registers)
 *   e1000_get_mac       the 6-byte station address
 *   e1000_send          transmit one frame (copies)
 *   e1000_receive       poll for one frame
 *   e1000_link_up       is the link up *right now* (live STATUS read)
 *   e1000_is_suspended  has the driver powered this card down?
 *
 * DEPENDENCIES
 *   pci.h (discovery + BAR + bus mastering), io.h (MMIO), device.h/driver.h
 *   (publishes "eth0" and the lifecycle hooks), kernel.h (kprintf, mdelay,
 *   millis). Nothing above this file knows the card exists except net.c.
 *
 * FUTURE EXTENSION POINTS
 *   - e1000_link_up() is the seam a link-state watcher would poll to raise and
 *     lower NETIF_FLAG_LINK_UP on the lwIP netif instead of pinning it up.
 *   - The MDIO helpers (MDIC) are the door to the whole PHY register file:
 *     speed/duplex forcing, loopback, cable diagnostics.
 *   - driver.h advertises probe()/remove(), and this driver implements neither
 *     (nor does any other in the tree). remove() would be the same teardown as
 *     suspend() plus unpublishing the device — they would share the quiesce path
 *     — but there is no device_unregister() to unpublish through yet.
 *   - MMIO to BAR0 goes through the write-back-cacheable identity mapping
 *     boot/boot.asm sets up; there is no PAT or MTRR making the PCI hole
 *     uncacheable. QEMU is unaffected, but on real silicon a register read could
 *     be served from cache and a ring doorbell could sit in a write-combining
 *     buffer, which means transmits that never start. Fixing it belongs in the
 *     page tables, not here.
 */
#ifndef E1000_H
#define E1000_H

#include <stdint.h>

/* Returns 0 on success, negative on failure. */
int  e1000_init(void);

/* Copy out the 6-byte MAC address. */
void e1000_get_mac(uint8_t mac[6]);

/* Transmit a raw Ethernet frame (copies `data`). Returns 0 on success.
 * Fails (nonzero) for a NULL, empty or oversized frame, when the next TX
 * descriptor has not been retired by the card yet, and — rather than blocking
 * forever — when the transmit engine does not retire this descriptor within a
 * bounded wall-clock wait, which is exactly what happens while suspended. */
int  e1000_send(const void *data, uint16_t len);

/* Poll for one received frame. Returns 1 and fills buf/len if one was
 * available (buf must hold at least 2048 bytes), 0 otherwise. */
int  e1000_receive(void *buf, uint16_t *len);

/* Live link state: 1 = up. Reads the STATUS register every call, and reports
 * "down" whenever this driver has powered the card down, because a suspended
 * card cannot carry a frame no matter what the link bit says. Returns 0 before
 * e1000_init() has mapped the card. */
int  e1000_link_up(void);

/* 1 while the driver's suspend hook has the card powered down. */
int  e1000_is_suspended(void);

#endif /* E1000_H */
