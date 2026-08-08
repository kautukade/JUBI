/* tooltable.h — make REGISTER_TOOL's linker-collected table work in a host test.
 *
 * PURPOSE
 *   core/tool.c walks the tool registry as `__start_tool_table .. __stop_tool_table`,
 *   a section the kernel's linker script builds (see linker.ld and
 *   include/tool.h). A host test links with the platform linker instead, so it
 *   has to supply the same table — and it must be the SAME table, walked by the
 *   same unmodified core/tool.c, or the suites would be testing a transcription
 *   of the registry rather than the registry.
 *
 * WHAT THIS SOLVES
 *   1. Mach-O rejects a bare section name and has no __start_/__stop_ symbols.
 *      The `section$start$SEGMENT$SECTION` assembler builtins provide them, and
 *      "__DATA,tool_table" is the spelling the attribute wants.
 *   2. AddressSanitizer. Each REGISTER_TOOL pointer is its own 8-byte global, so
 *      walking start..stop is pointer arithmetic ACROSS distinct objects — legal
 *      only because the linker happens to pack them. With ASan's redzones it is
 *      not even that: the entries land 32 bytes apart and the walk reads a
 *      redzone on its second step. Reproduced before this header existed:
 *        global-buffer-overflow, READ of size 8, tool_find tool.c:103
 *        "0 bytes after global variable '__toolptr_driver_targets_tool'"
 *      no_sanitize keeps the table entries out of ASan's global instrumentation,
 *      so they pack contiguously again. Reads through them are still checked
 *      against the shadow map; only the redzone padding is gone.
 *
 *   Three suites had a byte-for-byte copy of point 1 and none had point 2. Any
 *   host test that needs the real registry should include this instead:
 *
 *       #include "tool.h"
 *       #include "tooltable.h"       // after tool.h, before any REGISTER_TOOL
 *
 * NOT FOR
 *   Suites that build an explicit N-slot table by hand (test_mem_tools.c,
 *   test_fault.c, test_screen_tools.c, test_rtc.c, test_dev_tools.c). That
 *   mechanism — one real array plus `.set stop, start + 8*N` — is well defined
 *   with or without ASan and needs nothing from here; it just cannot be used by
 *   a suite that links a tool source as a separate translation unit, because the
 *   descriptors are static to it.
 */
#ifndef FABLEOS_TEST_TOOLTABLE_H
#define FABLEOS_TEST_TOOLTABLE_H

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define TOOLTABLE_ENTRY_ATTRS __attribute__((no_sanitize("address")))
#  endif
#endif
#ifndef TOOLTABLE_ENTRY_ATTRS
#  define TOOLTABLE_ENTRY_ATTRS
#endif

#ifdef __APPLE__
__asm__(".globl ___start_tool_table\n"
        ".set   ___start_tool_table, section$start$__DATA$tool_table\n"
        ".globl ___stop_tool_table\n"
        ".set   ___stop_tool_table, section$end$__DATA$tool_table\n");
#  undef  REGISTER_TOOL
#  define REGISTER_TOOL(var)                                                   \
      static const tool_t *const __toolptr_##var TOOLTABLE_ENTRY_ATTRS         \
          __attribute__((used, section("__DATA,tool_table"))) = &(var)
#endif

#endif /* FABLEOS_TEST_TOOLTABLE_H */
