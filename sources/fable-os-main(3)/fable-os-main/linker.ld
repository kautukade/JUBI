/* Link the kernel at the 1 MiB mark — the conventional load address that keeps
 * us clear of low-memory BIOS/VGA regions. The multiboot header must come
 * first so QEMU's loader finds it near the start of the file. */

ENTRY(start)

SECTIONS {
    . = 1M;

    .boot :
    {
        /* multiboot header must be early in the image */
        *(.multiboot)
    }

    /* __text_start/__text_end bound the bytes the CPU actually executes, and
     * nothing else. They exist so a live code patch can be refused unless its
     * target is a real instruction: include/fault.h's fault_window_t already had
     * a code range, but that one is deliberately a superset (it also admits
     * .rodata and .data, because a return address only has to be plausible).
     * A patch writes bytes that WILL be executed, so it gets the exact range.
     * Defining a symbol inside the section changes no layout. */
    .text :
    {
        __text_start = .;
        *(.text)
        __text_end = .;
    }

    .rodata :
    {
        *(.rodata)
    }

    /* Self-registering driver table. KEEP() stops --gc-sections dropping the
     * entries; __start_/__stop_ are provided automatically by the linker. */
    .driver_table :
    {
        __start_driver_table = .;
        KEEP(*(driver_table))
        __stop_driver_table = .;
    }

    /* Self-registering tool table — the model's syscall surface (see tool.h).
     * Same mechanism as driver_table: a new tool is a new .c file with no
     * central list to edit. */
    .tool_table :
    {
        __start_tool_table = .;
        KEEP(*(tool_table))
        __stop_tool_table = .;
    }

    .data :
    {
        *(.data)
    }

    .bss :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        __bss_end = .;
    }
}
