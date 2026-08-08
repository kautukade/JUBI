; switch.asm — the whole machine-dependent part of cooperative concurrency.
;
; Assembled with `nasm -f elf64`, matching boot/boot.asm and isr.asm.
;
; Two functions and one trampoline, declared in include/fiber.h:
;
;   fiber_arch_switch(void **save_sp, void *new_sp)
;   fiber_arch_stack_init(void *stack_top, void (*fn)(void*), void *arg)
;
; WHAT A "CONTEXT" IS HERE, AND WHY IT IS SO SMALL
;   fiber_arch_switch is an ordinary C function call, so the System V AMD64 ABI
;   has already done most of the work: everything the compiler cared about that
;   lives in a CALLER-saved register (rax, rcx, rdx, rsi, rdi, r8-r11) is
;   already spilled to the caller's stack by the caller, and the caller's stack
;   is exactly what we are keeping. So a context is precisely the six
;   callee-saved registers plus the return address:
;
;       rbx  rbp  r12  r13  r14  r15   and  [rsp] = where to resume
;
;   NOT SAVED, deliberately:
;     - x87/MMX/SSE state. The kernel is built -mno-mmx -mno-sse -mno-sse2
;       (see CFLAGS in Makefile), so the compiler emits none of it and no
;       kernel code touches MXCSR or the x87 control word. Saving 512 bytes of
;       FXSAVE area per switch to preserve registers that are never written
;       would be cost with no property attached. If SSE is ever enabled for
;       kernel code, THIS COMMENT IS THE THING THAT HAS TO CHANGE FIRST.
;     - rflags. The ABI does not preserve the arithmetic flags across a call, DF
;       is required to be clear at every call boundary and this kernel never
;       leaves it set, and IF is 0 for the entire life of the machine (there is
;       no preemption; see include/fiber.h). A pushfq/popfq pair would preserve
;       nothing that anyone may rely on.
;     - Segment registers, CR3, the TSS. One address space, one privilege
;       level, one page table, ring 0 throughout. The IST stacks in the TSS are
;       for exceptions and are unrelated to which fiber is running.
;
; THE STACK FRAME THIS FILE OWNS, low address to high:
;
;       new_sp -> r15
;                 r14
;                 r13
;                 r12
;                 rbx
;                 rbp
;                 return address        <- `ret` jumps here
;
;   fiber_arch_stack_init lays out exactly that frame by hand with the return
;   address pointing at fiber_trampoline, and parks fn in the r12 slot and arg
;   in the r13 slot. That is why the two functions must be read together: the
;   push order below and the store order there are one layout, in two places.

global fiber_arch_switch
global fiber_arch_stack_init
extern fiber_exit

section .text
bits 64

; ---------------------------------------------------------------------------
; void fiber_arch_switch(void **save_sp /*rdi*/, void *new_sp /*rsi*/)
;
; The only place in this kernel where rsp stops pointing at one stack and
; starts pointing at another. Between the two `mov`s below there is no valid C
; context at all, which is why this is assembly and not inline asm inside a
; function the compiler is also allowed to reason about.
; ---------------------------------------------------------------------------
fiber_arch_switch:
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    mov     [rdi], rsp              ; hand our stack pointer back to the C side
    mov     rsp, rsi                ; ... and become the other fiber

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret                             ; returns into WHOEVER owns rsp now

; ---------------------------------------------------------------------------
; void *fiber_arch_stack_init(void *stack_top /*rdi*/,
;                             void (*fn)(void*) /*rsi*/, void *arg /*rdx*/)
;
; Write the frame above onto a fresh stack and return the stack pointer that
; fiber_arch_switch should be handed to start it. Pure stores; touches no
; global state; safe to call from any fiber.
;
; ALIGNMENT. The ABI wants rsp % 16 == 0 immediately before a `call`, i.e.
; rsp % 16 == 8 at the callee's first instruction. Rather than reason about
; that here, the top is aligned down to 16 and fiber_trampoline re-aligns rsp
; itself before it calls anything — see the note there.
; ---------------------------------------------------------------------------
fiber_arch_stack_init:
    mov     rax, rdi
    and     rax, -16                ; 16-align the top of the usable region

    lea     rcx, [rel fiber_trampoline]
    sub     rax, 8
    mov     [rax], rcx              ; return address -> the trampoline
    sub     rax, 8
    mov     qword [rax], 0          ; rbp = 0: ends any backtrace cleanly here
    sub     rax, 8
    mov     qword [rax], 0          ; rbx
    sub     rax, 8
    mov     [rax], rsi              ; r12 = fn
    sub     rax, 8
    mov     [rax], rdx              ; r13 = arg
    sub     rax, 8
    mov     qword [rax], 0          ; r14
    sub     rax, 8
    mov     qword [rax], 0          ; r15
    ret                             ; rax = the initial saved stack pointer

; ---------------------------------------------------------------------------
; Where a brand new fiber begins. Never called; only ever `ret`ed into by
; fiber_arch_switch, with r12 = fn and r13 = arg restored from the frame that
; fiber_arch_stack_init wrote.
;
; `and rsp, -16` only ever LOWERS rsp, so it cannot walk off the top of the
; stack, and it makes the alignment a property of this instruction rather than
; of arithmetic done in another function. The trampoline never returns, so
; giving up the ability to `ret` costs nothing.
; ---------------------------------------------------------------------------
fiber_trampoline:
    and     rsp, -16
    xor     ebp, ebp                ; again, for a backtrace that walks rbp
    mov     rdi, r13                ; arg
    call    r12                     ; fn(arg)

    ; The body returned instead of calling fiber_exit(). Same thing: end the
    ; fiber. fiber_exit() does not return, so nothing below this ever runs.
    call    fiber_exit
    ud2                             ; and if it somehow did, stop here loudly
