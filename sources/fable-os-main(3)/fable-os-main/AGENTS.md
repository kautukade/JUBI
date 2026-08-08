# AGENTS.md

Context for anyone — human or model — working on this codebase. Read it before
touching anything: several of the facts below look like bugs and are load-bearing,
and a few look harmless and will cost you a day.

## What this machine is

A from-scratch x86_64 kernel whose only interface is natural language. There is no
shell and no command parsing. A language model reaches real kernel state through
64 tools registered in `tools/`, writes device drivers at run time in a bounded VM,
compiles C on the machine itself, saves capabilities to a FAT32 disk, and can patch
its own running code.

## Architectural facts that are load-bearing

**Every page is RWX.** `boot/boot.asm` maps 2 MiB huge pages present+writable and
never sets NX. The `LOAD segment with RWX permissions` linker warning is expected
and intentional — **never "fix" it.** Three separate features depend on it: the
driver VM's generated code, live `.text` patching, and the C compiler emitting into
a `.bss` buffer it then calls.

**The low 4 GiB is identity-mapped**, so a virtual address below 4 GiB is its own
physical address. That is what makes DMA feasible here at all.

**A null dereference does not fault.** Low memory is mapped writable, and page 0
shares its 2 MiB huge page with the VGA framebuffer at `0xB8000`, so it cannot be
unmapped. Use `0x1000000000` (64 GiB, canonical, unmapped) for a guaranteed `#PF`.

**Everything is polled.** `IF` stays 0 and all 16 PIC lines are masked. There are
no device interrupts — only CPU exceptions, through the IDT in `arch/x86_64/`. Do
not enable interrupt vectors casually: as the BIOS leaves the PICs, IRQ0 sits on
vector 8, which is `#DF`, so an unmasked timer would report a fake double fault
forever.

**Concurrency is cooperative, never preemptive.** `core/fiber.c` switches only at
explicit yields. This is deliberate: lwIP and mbedTLS are configured `NO_SYS`
single-threaded, and preemption would require every one of ~72k lines to be
reentrant. A fiber that never yields hangs the machine and nothing can take the CPU
back. The one yield that matters is at the end of `net_service()`, the single point
every network wait passes through while lwIP is quiescent.

**Ring 0 only.** No processes, no scheduler, no userspace, no memory protection, no
SMP, no USB, no IPC, no ELF loader. No virtual memory management.

## The trust model — do not weaken it

The operator has no `ls`, so a bracketed line printed by the kernel is the only
audit trail that exists. The invariant:

> **A kernel trace line is a `[` in column zero, and nothing model-controlled may
> ever put one there.**

Five separate defects have been fixed to keep that true — trace arguments, trace op
names, model prose in `net/chat.c`, `write_screen` painting into the console grid,
and one more. `tests/host/test_chat.c` and `test_screen_tools.c` guard it. If you
add any path by which model-controlled bytes reach the console, check it against
this first.

`tool_dispatch()` also guarantees every model action emits a line, measured with
`trace_emissions()` — a monotonic counter of bytes actually written, immune to
`trace_reset()`. Do not substitute `trace_count()`; it advances while suppressed and
can move backwards.

## Security boundaries, stated plainly

**No privilege separation.** Model-authored apps and compiled C reach the kernel
through a curated twelve-entry symbol table, and that validation *is* the security
boundary — there is no MMU behind it. Nothing bounds a pointer: a wild store from
compiled code halts the machine.

**No IOMMU.** A driver program's register access is sandboxed by an allowlist
derived from its target's own BARs; its **DMA is not**. Once a program writes an
address into a device's DMA register, that transfer happens.
`test_the_vm_cannot_police_dma` proves this gap on purpose rather than hiding it.
The honest description is "a trusted driver whose register accesses are sandboxed
and whose DMA is not" — the same trust any kernel extends to a driver it loads.

**Certificate verification is behind `FABLEOS_VERIFY_CERTS` and off by default.**
On, it enforces two pinned roots plus hostname and validity. Off, traffic is
encrypted but not authenticated.

**Entropy** is RDRAND with a TSC fallback, not a vetted CSPRNG.

**The API key is never a build input.** No `-D`, no generated header, and
`make KEY=...` is a hard error. It arrives over `fw_cfg` at run time and lives only
in RAM. Never log it, hash it, or fingerprint it — a byte count is the only
diagnostic, which is why the transcripts in `vm/transcripts/` are publishable.

## Traps that have each cost real time

**`EXTRA_CFLAGS` is tracked in a build stamp.** `make run FOO=1` without the same
`EXTRA_CFLAGS` silently rebuilds *without* your flag, and the log looks plausible.
Put build flags and `QEMU_EXTRA` on the same command line.

**Hardcoded constants that mirror computed values go stale and break the build.**
`CHAT_REGISTRY_BYTES` has been 4 KiB wrong twice, each time turning three QEMU
cases red. If you add one, make something assert the two agree and print both.

**Names and lengths are coupled in the tests, on purpose.** Renaming the OS broke
six assertions: an ACPI OEM ID is a 6-byte field, `/etc/motd`'s length is asserted,
two `memcpy`s had exact byte counts, a hex-dump assertion spelled the old name in
ASCII, and the SSE suite splits the name across three events to test reassembly.
Those assertions are correct — a motd that reads back the wrong size means the VFS
lied about a write.

**Host tests link the real libc, so they cannot see kernel formatter bugs.**
`lib/kfmt.c` is a reduced `printf`. A format string it cannot render works
everywhere except in the binary that boots. `tests/qemu/lint_printf.py` reads source
to catch this and runs as part of `make test-qemu`. One such bug was found by the
*model*, from inside the guest, because the kernel was rendering a document it
needed as literal `%*s`.

## Testing

```sh
make test            # everything
make test-host       # 39 suites, host-native, milliseconds
make test-host-asan  # the same under ASan + UBSan
make test-qemu       # 6 boot cases, plus the formatter lint
```

Pure-logic modules compile against the *host* compiler and run natively, so the
inner loop costs no QEMU boot. Make hardware backends injectable so tests can supply
a synthetic device — `tests/host/test_dvm_tools.c` shows the pattern well.

**What the tests do and do not prove.** A live model has driven these tools, but the
transcripts here are a handful of turns. What holds the 64-tool surface up is
millions of host assertions against a scripted transport (`net/model_mock.c`), not a
live model's judgement at scale. Assume the failure modes under a confused or
adversarial model are unexplored, because they are.

**It has never booted on physical hardware.** Everything here is QEMU. `make iso`
produces a GRUB image that boots via `-cdrom`, which is not the same claim.

## House conventions

Every header carries an architecture comment: PURPOSE / RESPONSIBILITIES / PUBLIC
API / DEPENDENCIES / FUTURE EXTENSION POINTS. Match `include/vfs.h` or
`include/tool.h`.

Drivers and tools self-register through linker sections (`REGISTER_DRIVER`,
`REGISTER_TOOL`), so adding either is a new file and no central list to edit.
`tools/*.c` is wildcarded in the Makefile — a new tool needs no Makefile change.

Never write new JSON parsing. `net/json.c` is hardened, fuzzed and ASan-clean, and
is what all model output arrives through.

Comments explain *why*, including rejected alternatives and known limitations. When
you find a defect, the comment recording the causal chain is worth more than the
fix.
