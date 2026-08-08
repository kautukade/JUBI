/* kernel.h — shared kernel services: console, strings, memory, time. */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>

/* ---- Console ----
 * One interface, two backends (see lib/base.c): a linear framebuffer with a real
 * font when the machine gives us one, VGA text mode when it does not. Callers
 * cannot tell the difference. The byte stream is UTF-8: kputc() takes one byte,
 * so a multi-byte character arrives across several calls and the console
 * reassembles it. Malformed sequences draw a replacement glyph and never
 * desynchronise the stream. */
void console_init(void);
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);   /* supports %s %c %d %u %x %p %% */

/* Read-only view of the console's own cells, geometry and cursor. Exists so a
 * tool that paints the screen (tools/screen_tools.c) shares one definition of
 * where the screen is and where output goes next, instead of duplicating an
 * address and a geometry that can rot.
 *
 * A cell is attribute<<8 | character, exactly as VGA text mode has always had
 * it. Two caveats, both new with the framebuffer console:
 *   - the geometry is whatever the display gives (128x48 at 1024x768, 80x25 on
 *     the VGA fallback), so ask, never assume;
 *   - the low byte is a slot in the console code page: code page 437 from 0x10
 *     upwards, and the punctuation a language model emits (em dash, curly
 *     quotes, ellipsis) in the slots below. Printable ASCII 0x20-0x7E is
 *     identical in both.
 * Writing through console_fb() does not move the cursor. With a framebuffer the
 * pixels catch up on the next printed character; console_refresh() forces it. */
volatile uint16_t *console_fb(void);
void console_geometry(int *rows, int *cols);
void console_cursor(int *row, int *col);
void console_refresh(void);

/* Monotonic count of scrolls since boot; never resets, never decreases. A
 * compositor drawing over the console subtracts two readings to learn exactly
 * how many rows the screen moved between them — which is how gui/wm.c repairs
 * the band a scroll drags a window's pixels into without repainting the screen.
 * A cursor position is NOT a substitute: the prompt sits on the last row for the
 * whole life of the machine, so "cursor on the last row" is true forever and
 * says nothing about whether anything moved. */
uint64_t console_scrolls(void);

/* ---- Freestanding string/memory ---- */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

/* ---- Kernel heap (kmalloc/kfree/krealloc; see heap.h) ---- */
#include "heap.h"

/* ---- Time (calibrated against the PIT) ---- */
void     time_init(void);
uint64_t millis(void);     /* milliseconds since time_init() */
void     mdelay(uint32_t ms);

/* ---- Panic ---- */
void panic(const char *msg);

#endif /* KERNEL_H */
