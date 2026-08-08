; boot.asm — Multiboot header, 32-bit entry, and the jump into 64-bit long mode.
;
; QEMU's built-in multiboot loader reads the header below and starts us in
; 32-bit protected mode. We verify the CPU supports long mode, build a minimal
; set of page tables (identity-mapping the first 1 GiB), enable paging + long
; mode, load a 64-bit GDT, and finally far-jump into 64-bit code.

global start
global multiboot_info_phys
extern kernel_main
extern __bss_start
extern __bss_end

; ---------------------------------------------------------------------------
; Multiboot v1 header — must be in the first 8 KiB of the file, 4-byte aligned.
;
; FLAGS BIT 2 ASKS FOR A LINEAR FRAMEBUFFER.
;   Setting it means "I have a preferred video mode", and the four fields at
;   offsets 32..44 say which: mode_type 0 = linear graphics (1 would be EGA
;   text), then width, height and bits per pixel. A loader that honours the
;   request sets bit 12 in the information structure and fills in the
;   framebuffer address, pitch and channel layout; lib/fb.c reads exactly those
;   fields. GRUB does this. QEMU's built-in `-kernel` loader does NOT — it
;   ignores the request and never sets bit 12 — which is why lib/fb.c has a
;   second way to find a framebuffer and a VGA text fallback behind that. The
;   request is harmless either way: a loader that cannot honour it is required
;   to boot us anyway.
;
;   The five dwords between the checksum and the video fields are the a.out
;   kludge (header_addr..entry_addr). They are only READ when flag bit 16 is set,
;   which it is not, but the video fields are found by fixed offset, so the space
;   has to be there.
;
;   1024x768x32 is the mode every emulated adapter and every VBE BIOS supports.
;   At 8x16 glyphs it is a 128x48 character console. lib/fb.c declines anything
;   larger than FB_MAX_WIDTH x FB_MAX_HEIGHT rather than overrun its buffer, so a
;   loader that substitutes a bigger mode costs us the framebuffer, not memory
;   safety.
; ---------------------------------------------------------------------------
MB_MAGIC    equ 0x1BADB002
MB_VIDEO    equ 1 << 2
MB_FLAGS    equ MB_VIDEO
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    dd 0, 0, 0, 0, 0            ; offsets 12..28: a.out kludge, unused
    dd 0                        ; mode_type: 0 = linear graphics
    dd 1024                     ; width  (pixels)
    dd 768                      ; height (pixels)
    dd 32                       ; depth  (bits per pixel)

; ---------------------------------------------------------------------------
; 32-bit entry point
; ---------------------------------------------------------------------------
section .text
bits 32
start:
    mov esp, stack_top          ; set up a stack
    mov ebp, eax                ; preserve the multiboot magic (eax)
    mov esi, ebx                ; preserve the multiboot INFO pointer (ebx)

    ; The multiboot loader reserves .bss but does not zero it. Do that now,
    ; before anything (page tables, C globals) relies on zero-init. We are
    ; pre-paging here, so this runs against physical memory directly.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; Hand the information structure to C. It carries the framebuffer the loader
    ; gave us (or did not) and the command line, and lib/fb.c is the only reader.
    ; This has to land AFTER the zeroing, because the variable lives in .bss —
    ; and the pointer itself has to survive the zeroing, which is what ESI is
    ; for (rep stosb clobbers EDI/ECX/EAX and nothing else).
    mov [multiboot_info_phys], esi

    mov eax, ebp                ; restore the multiboot magic
    call check_multiboot
    call check_cpuid
    call check_long_mode

    call set_up_page_tables
    call enable_paging

    lgdt [gdt64.pointer]        ; load the 64-bit GDT
    jmp gdt64.code:long_mode_start  ; far jump into 64-bit code

    hlt

; --- Sanity checks --------------------------------------------------------

check_multiboot:
    cmp eax, 0x2BADB002         ; multiboot v1 loader leaves this magic in eax
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "0"
    jmp error

check_cpuid:
    ; CPUID is supported if we can flip bit 21 (ID) in EFLAGS.
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "1"
    jmp error

check_long_mode:
    mov eax, 0x80000000         ; is the extended CPUID leaf available?
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29           ; LM bit
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, "2"
    jmp error

; --- Paging ---------------------------------------------------------------

set_up_page_tables:
    ; Link P4[0] -> P3.
    mov eax, p3_table
    or eax, 0b11                ; present + writable
    mov [p4_table], eax

    ; Link P3[0..3] -> the four P2 tables, covering 4 GiB. We need this much
    ; because device MMIO (the e1000's registers) lives near the top of the
    ; 32-bit address space (~0xFEBCxxxx), well above the first 1 GiB.
    mov ecx, 0
.map_p3:
    mov eax, p2_tables
    mov edx, ecx
    shl edx, 12                 ; ecx * 4096 (one P2 table per GiB)
    add eax, edx
    or eax, 0b11                ; present + writable
    mov [p3_table + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .map_p3

    ; Identity-map 4 GiB using 2048 * 2 MiB huge pages.
    mov ecx, 0
.map_p2_table:
    mov eax, 0x200000           ; 2 MiB
    mul ecx                     ; edx:eax = 2 MiB * ecx (fits in 32 bits for <4 GiB)
    or eax, 0b10000011          ; present + writable + huge
    mov [p2_tables + ecx * 8], eax
    mov [p2_tables + ecx * 8 + 4], edx  ; high dword (0 below 4 GiB)

    inc ecx
    cmp ecx, 2048
    jne .map_p2_table
    ret

enable_paging:
    mov eax, p4_table           ; load P4 into CR3
    mov cr3, eax

    mov eax, cr4                ; enable PAE
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or eax, 1 << 8              ; set long-mode bit
    wrmsr

    mov eax, cr0                ; enable paging
    or eax, 1 << 31
    mov cr0, eax
    ret

; Print "ERR: X" (X = error code in al) to the top-left of the screen, then halt.
error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte  [0xb800a], al
    hlt

; ---------------------------------------------------------------------------
; 64-bit entry point
; ---------------------------------------------------------------------------
bits 64
long_mode_start:
    ; Reload all data segment registers with the null selector.
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call kernel_main            ; hand off to C

.hang:
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; Reserved memory: page tables + stack
; ---------------------------------------------------------------------------
section .bss
align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096
p2_tables:
    resb 4096 * 4               ; four P2 tables = 2048 entries = 4 GiB

; The root (boot) stack. Its bounds are EXPORTED so C can see them, because what
; sits immediately below stack_bottom is p2_tables — the identity map itself. An
; overflow of even a few bytes rewrites the page tables covering the top of the
; 4 GiB window (where MMIO lives) and the CPU triple-faults with no #PF, no fault
; record and nothing on the serial line. Exporting these two symbols lets
; core/fiber.c lay a poison band at the bottom and measure the true high-water
; mark, which turns that silent triple fault into a panic that names the culprit.
global stack_bottom
global stack_top
stack_bottom:
    resb 4096 * 16              ; 64 KiB stack
stack_top:

; Physical address of the multiboot information structure, as the loader left it
; in EBX. Read by lib/fb.c (the framebuffer tag and the kernel command line).
; Last on purpose: `align` in a nobits section makes NASM try to emit fill bytes,
; so putting anything before the page tables would either warn or, worse, leave
; p4_table not 4 KiB aligned.
alignb 4
multiboot_info_phys:
    resd 1

; ---------------------------------------------------------------------------
; 64-bit GDT: one null descriptor + one 64-bit code descriptor.
; ---------------------------------------------------------------------------
section .rodata
gdt64:
    dq 0                                        ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)    ; exec + descriptor type + present + 64-bit
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
