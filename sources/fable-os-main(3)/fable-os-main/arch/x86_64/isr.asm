; isr.asm — 256 exception/interrupt entry stubs and the one common save path.
;
; Assembled with `nasm -f elf64`, matching boot/boot.asm's conventions.
;
; WHAT THIS FILE EXISTS TO SOLVE
;   x86-64 hands an exception handler two different stack layouts depending on
;   which vector fired. Ten vectors (#DF #TS #NP #SS #GP #PF #AC #CP #VC #SX)
;   arrive with an error code already pushed; the other 246 do not. A handler
;   that gets this wrong reads RIP out of the error-code slot, reports a
;   plausible-looking lie, and then IRETQs to a garbage address.
;
;   So the asymmetry is erased here, once, in the only place that can know which
;   vector it is: the stub for a no-error-code vector pushes a zero placeholder.
;   Everything above this file sees one layout for all 256 vectors.
;
; THE FRAME, low address to high, exactly matching fault_frame_t in fault.h:
;
;       rsp ->  cr2            <- read and pushed here, before anything can
;               reserved          disturb it; `reserved` is a zero that also
;               r15               restores 16-byte alignment for the C call
;               r14
;               ...
;               rax
;               vector         <- pushed by the stub
;               error_code     <- pushed by the CPU, or a zero by the stub
;               rip            <- pushed by the CPU
;               cs
;               rflags
;               rsp               (long mode pushes SS:RSP even with no CPL change)
;               ss
;
;   Pointer arithmetic: the CPU aligns RSP to 16 before pushing its 5 qwords, so
;   after the error code, the vector and the 15 GPRs we are 22 qwords in — back
;   to 16-byte alignment. `reserved` and `cr2` make 24, still aligned, which is
;   what the System V ABI requires at the CALL instruction. That is the entire
;   reason `reserved` exists.
;
; NOTHING IS CLOBBERED
;   Every GPR is saved and restored, `add rsp, 16` drops the two words the stub
;   synthesised, and IRETQ restores RIP/CS/RFLAGS/RSP/SS from the frame. The C
;   handler receives a pointer to the live frame rather than a copy, which is
;   deliberate: the recovery path (fault_recover) writes to it and returns.
;
;   Segment registers are untouched. boot.asm left DS/ES/FS/GS/SS holding the
;   null selector, which is legal at CPL 0 in 64-bit mode, and idt_init() does
;   not change that — so there is nothing here to reload and no chance of
;   loading something wrong.
;
;   The kernel is built with -mno-red-zone, so pushing onto the interrupted
;   stack cannot corrupt a leaf function's locals.

global isr_stub_table
extern isr_dispatch

; Vectors for which the CPU pushes an error code. Anything not listed gets a
; synthetic zero so the frame layout stays uniform.
;   8  #DF  double fault              (always zero, but pushed)
;   10 #TS  invalid TSS               17 #AC  alignment check
;   11 #NP  segment not present       21 #CP  control protection
;   12 #SS  stack-segment fault       29 #VC  VMM communication
;   13 #GP  general protection        30 #SX  security exception
;   14 #PF  page fault
%define HAS_ERRCODE(v) ((v) == 8  || (v) == 10 || (v) == 11 || (v) == 12 || \
                        (v) == 13 || (v) == 14 || (v) == 17 || (v) == 21 || \
                        (v) == 29 || (v) == 30)

section .text
bits 64

; --- the common path ------------------------------------------------------
; Entered with the error code and the vector already on the stack.
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    push qword 0                ; fault_frame_t.reserved (also re-aligns RSP)
    mov  rax, cr2               ; only meaningful for #PF, always captured
    push rax                    ; fault_frame_t.cr2

    mov  rdi, rsp               ; SysV arg 0: pointer to the live frame
    cld                         ; the ABI wants DF clear; the faulting code
                                ; may have left it set
    call isr_dispatch

    add  rsp, 16                ; drop cr2 + reserved

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    add  rsp, 16                ; drop the vector + error code
    iretq

; --- the 256 stubs --------------------------------------------------------

%macro ISR_NOERR 1
isr_stub_%+%1:
    push qword 0                ; placeholder error code
    push qword %1               ; vector
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%+%1:
                                ; the CPU already pushed the error code
    push qword %1               ; vector
    jmp  isr_common
%endmacro

%assign v 0
%rep 256
    %if HAS_ERRCODE(v)
        ISR_ERR v
    %else
        ISR_NOERR v
    %endif
    %assign v v+1
%endrep

; --- the address table idt.c reads ---------------------------------------
; 256 qwords, index == vector. Building the IDT from this table rather than
; from 256 hand-written extern declarations is what makes "no vector was
; forgotten" a property of the assembler instead of a code review.
section .rodata
align 8
isr_stub_table:
%assign v 0
%rep 256
    dq isr_stub_%+v
    %assign v v+1
%endrep
