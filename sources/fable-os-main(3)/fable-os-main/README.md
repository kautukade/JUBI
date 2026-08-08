# fable-os

<img width="1101" height="867" alt="image" src="https://github.com/user-attachments/assets/7f07a3f0-a03a-456d-8269-fe00effb02d6" />

A from-scratch x86_64 operating system whose **only interface is a sentence**.

There is no shell, no commands, no `ls` and no `cat` — there is not one `strcmp`
on the input path. You type what you want done and the kernel acts on the machine.

The kernel does its own DNS and its own TLS in ring 0, with lwIP and mbedTLS
compiled into `kernel.bin` and no host proxy of any kind. It gives the model **64
real syscalls** into itself, and prints what it dispatched in C, from the real
return value, so you can tell what happened from what you were told:

```
> I want a calculator on screen please
I'll open a calculator window for you.
[gui_open app=calculator at=40,48 size=216x266 widgets=18 -> 1]
The calculator is open and on screen (window id=1), with its display and full
keypad ready to click.

> now use that calculator to work out 7 plus 8, by clicking its keys
[gui_click id=1 widget=107 label=7 at=70,130 -> 1]
[gui_click id=1 widget=5   label=+ at=225,249 -> 1]
[gui_click id=1 widget=108 label=8 at=121,130 -> 1]
[gui_click id=1 widget=3   label== at=148,289 -> 1]
The display reads 15.
```

**Prose is the model. `[Brackets]` are the kernel.** On a machine with no `ls`
that distinction is the only audit trail there is, so it is enforced structurally
rather than promised — five separate defects have been fixed to keep it true.

---

## Quick start

Needs a Mac with Homebrew, or the Linux equivalents.

```sh
make toolchain     # once: nasm, x86_64-elf-gcc, qemu
make run
```

That boots with no key: the kernel still completes a real TLS handshake to
`api.anthropic.com` and gets an honest `401` back, which is itself proof the HTTPS
path works. It will tell you it has no key.

### Your API key

The key is **never a build input**. There is no `-D`, no generated header, and
`make KEY=...` is a hard error. It travels host → guest at run time over QEMU's
`fw_cfg` channel and lives only in RAM, so no build artifact can contain it.

```sh
cp .env.example .env        # .env is gitignored
$EDITOR .env                # KEY="sk-ant-api03-..."
make run
```

Or leave nothing on disk at all:

```sh
export ANTHROPIC_API_KEY="sk-ant-api03-..."
make run
```

Either way you should see `fwcfg: api key: N bytes loaded from fw_cfg`. The kernel
reports the byte count and never the key.

---

## Things to try

### Ask it for an app

```
> I want a calculator
> I want a stopwatch
> I want a window that just says hello
```

It writes the app itself — a document describing widgets and handlers — and
launches it into a real window you can click with the mouse. To see the reference
calculator without spending a turn:

```sh
make run EXTRA_CFLAGS=-DFABLEOS_APP_SELFTEST
```

### Ask it to write a driver for hardware it has never seen

The kernel ships **no audio driver** and no knowledge of any sound chip. Attach a
card and it shows up as an unclaimed PCI device:

```sh
make run QEMU_EXTRA="-audiodev coreaudio,id=a0 -device AC97,audiodev=a0"
```

```
> there's a sound card in this machine with no driver. find it and write one, then play me a tone.
```

It reads PCI config space, writes a bring-up program in the kernel's own driver-VM
instruction set, resets the codec, unmutes the mixer, builds a buffer descriptor
list, starts the DMA engine, and installs itself as the system audio sink. After
that you can just ask it for notes.

`vm/transcripts/` holds real recordings, including the WAV analysis — and one run
where the model correctly diagnosed a bug in this kernel's own `printf` from
inside the guest.

### Ask it to learn something permanently

```
> I need to test whether a number is a perfect square. Make that something this
  machine can just do from now on, and check 10201 for me.
```

It writes a program, saves it as a named capability on a FAT32 disk this kernel
formatted, and verifies it. Power the machine off and on:

```
capability: 1 loaded from /disk/cap (survives a reboot)

> Is 46656 a perfect square?
[cap.call is_perfect_square v1 depth=0 -> 1]
Yes — 46656 is a perfect square (216² = 46656).
```

The second boot assembles nothing. And with **no key at all** and nobody typing, a
scheduled item runs it before the prompt is even printed:

```
agenda: 1 item(s) loaded from /disk/agenda.json (survives a reboot)
[agenda.run selfcheck boot tool runs=1 -> ok]
agenda: 1 boot item(s) ran before anyone typed anything
```

### Watch it stay alive while it thinks

```sh
make run EXTRA_CFLAGS=-DFABLEOS_CONCURRENCY_DEMO
```

Opens a clock window. Ask anything and the seconds keep ticking *during* the turn —
a 1725 ms API call serviced the screen 283 times.

---

## What it has

| | |
|---|---|
| **Interface** | Natural language only. No shell exists; none was written. |
| **Ground truth** | Kernel-emitted trace lines the model cannot forge |
| **Syscalls** | 64 tools: VFS, devices, PCI, memory, screen, faults, clock, audio, network, capabilities, the compiler |
| **Network** | e1000, lwIP, mbedTLS with pinned roots, HTTP/HTTPS fetch, SSE streaming |
| **Display** | 1024×768 framebuffer, real font, UTF-8, window manager, PS/2 mouse |
| **Apps** | The model authors them and launches them into windows |
| **Driver VM** | Bounded ISA whose opcodes are hardware primitives, with DMA and bus mastering |
| **Persistence** | ATA PIO, block layer, FAT32 behind the VFS's `vnode_ops_t` |
| **Concurrency** | Cooperative fibers with guarded stacks |
| **C compiler** | Lexer, preprocessor, parser, type checker, x86-64 codegen — on the machine |
| **Faults** | IDT, TSS with IST stacks, decode, backtrace, and recovery that resumes |
| **Self-repair** | Asks the model about a fault, then patches the running kernel |
| **Power** | ACPI shutdown and reboot from real FADT/DSDT parsing |
| **Boot** | Multiboot via `-kernel`, or `make iso` for GRUB on a CD or USB stick |

## Testing

```sh
make test            # everything
make test-host       # 39 suites, host-native, milliseconds
make test-host-asan  # the same under ASan + UBSan
make test-qemu       # 6 boot cases, plus a formatter lint
```

Pure-logic kernel modules are compiled against the *host* compiler and run
natively, so the inner loop costs no QEMU boot. Hardware behaviour is asserted
from real serial logs by a declarative case runner.

## Layout

```
boot/  arch/        multiboot, long mode, IDT, fault handling
core/               kernel objects, tool registry, capabilities, fibers, agenda
mm/  fs/            heap, VFS, ramfs, FAT32
drivers/            serial, PCI, e1000, PS/2, RTC, ACPI, ATA, fw_cfg
net/                JSON, transport seam, turn loop, TLS, SSE, fetch
vm/  compiler/      the driver VM, and the C compiler
gui/  apps/         window manager, widgets, app runtime
tools/              the model's syscalls, one file per family
tests/              host suites and QEMU boot cases
lwip/  mbedtls/     vendored — 330k lines, not this project's code
```

About **72k lines** of C and assembly here, plus **53k lines of tests**. The
vendored network and TLS stacks are another 330k and are not mine.

`README.os.md` has the deep detail: the trust model, the driver model, the TLS
build, and design notes per subsystem. `AGENTS.md` has the architectural
constraints anyone — or anything — working on this code needs to know first.

Built and run on macOS via QEMU.
