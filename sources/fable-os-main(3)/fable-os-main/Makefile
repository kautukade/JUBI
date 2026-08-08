# Build the fable-os bare-metal kernel on macOS.
#
#   make            build kernel.bin
#   make run        boot in QEMU (graphics window + serial on stdio)
#   make run ZOOM=3 same, but a 3x window instead of the default 2x
#   make run-nox    boot headless (serial only)
#   make clean
#
# The Anthropic API key is NOT a build input; see "THE API KEY" below.

ASM     := nasm
CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy

# ===========================================================================
# THE API KEY IS NOT A BUILD INPUT
#
# It used to be: `-DFABLEOS_API_KEY='"$(KEY)"'` put the live secret inside
# net.o, kernel.elf, kernel.bin, fableos.iso AND .build-flags (which recorded
# the flag set verbatim, i.e. `KEY=sk-ant-...` in plain text). Five artifacts,
# kept out of git by a .gitignore — one `git add -f`, one CI cache, one shared
# build directory and the key is published. That is the wrong place for a
# safety property, so the hazard is gone rather than ignored: there is no KEY
# variable and no -D anywhere below, and `grep -r sk-ant` over every build
# output finds nothing because the compiler is never told.
#
# The key now travels host -> guest at RUN time over QEMU's fw_cfg channel and
# lives only in RAM. See the run targets (KEY_ARGS) and drivers/fwcfg/.
#
# Refuse KEY= loudly rather than ignore it: silently dropping a key someone
# believes they passed produces a kernel that gets 401 and an operator who
# does not know why.
# ===========================================================================
#
# Test $(origin KEY), NOT $(KEY). Make imports the environment as make
# variables, so a plain `ifneq ($(strip $(KEY)),)` also fires when KEY merely
# happens to be exported in the caller's shell — which is exactly what happens
# when a zsh/bash dotenv plugin auto-sources .env on cd. The operator then
# gets told "KEY= is gone" for a command they never typed, with a key sitting
# correctly in the file the message tells them to use. Only a command-line
# assignment is a mistake worth stopping the build for.
ifeq ($(origin KEY),command line)
$(error KEY= on the command line is gone: the API key is never compiled in. Put \
KEY="sk-ant-..." in .env (gitignored) and run `make run` — it is handed to the \
guest at boot over fw_cfg. See the README section "The API key never enters the \
build".)
endif

# One-off diagnostic builds. Empty in every normal build; the only current use
# is the exception self-test, which is compiled out entirely without it:
#   make EXTRA_CFLAGS=-DFABLEOS_FAULT_TEST=FAULT_INJECT_PF_WRITE
# See arch/x86_64/fault_inject.c.
EXTRA_CFLAGS ?=

INCLUDES := -Iinclude -Iport -Ilwip/src/include -Imbedtls/include
CFLAGS  := -ffreestanding -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -fno-stack-protector -fno-pic -fno-builtin \
           -fno-tree-loop-distribute-patterns -Wall -Wextra -std=gnu11 \
           -DMBEDTLS_CONFIG_FILE='"mbedtls_config.h"' \
           $(INCLUDES) $(EXTRA_CFLAGS) -MMD -MP -c

# A build that silently disagrees with its own flags is worse than one that
# fails. Two things used to be untracked, and both of them could make the kernel
# behave one way while printing that it behaves another:
#
#   headers   Editing include/mbedtls_config.h or port/lwipopts.h rebuilt
#             nothing, so the defines that switch certificate verification on
#             never reached the mbedTLS and lwIP objects that consume them.
#             -MMD -MP above emits a .d per object; they are included below.
#
#   flags     `make EXTRA_CFLAGS=-DFABLEOS_VERIFY_CERTS` in an already-built tree
#             printed "Nothing to be done" and left the previous kernel in
#             place. Worse, recompiling ONE object without the flag afterwards
#             produced a kernel that verified nothing and still printed
#             "[tls] verified: chain trusted".
#
# HOW THAT IS ENFORCED, AND WHY IT IS NOT AN ORDINARY PREREQUISITE.
#
# The obvious shape — every object depends on a stamp holding the flags, and the
# stamp is rewritten when they change — DOES NOT WORK with the make this project
# is built with. /usr/bin/make here is GNU Make 3.81 (Apple's), whose
# is-the-prerequisite-newer test has WHOLE-SECOND granularity. The stamp is
# rewritten at the start of the new build, i.e. almost always inside the same
# wall-clock second as the tail of the previous build's objects, so those objects
# compare as up to date and are silently NOT recompiled. Measured: 51 to 184 of
# 184 objects skipped, non-deterministically, and a second identical make then
# recompiles ZERO, so the mixed-flag kernel is permanent until `make clean`. The
# worst observed outcome was an exit-0 build that recorded VERIFY_CERTS in the
# stamp while ten of the twelve flag-sensitive objects were still the
# non-verifying ones — precisely the failure the stamp was added to prevent, and
# it also hides the link error that a genuinely flagged build would surface.
#
# So the check does not rely on mtimes at all. It runs at PARSE time, before the
# dependency graph is built, and DELETES the objects the flag change invalidates.
# Deletion is a fact; mtime ordering is a guess. The cost is that alternating
# flag sets always pays a full rebuild — which is the honest price, and this tree
# builds clean in a couple of seconds.
#
# This stamp used to hold `KEY=sk-ant-...` in plain text, which made an
# untracked build artifact the second-easiest place to find the key. There is no
# KEY any more, so there is nothing secret in here to leak.
# Parse-time work must not happen under `make -n`: a dry run has to be free of
# side effects, or it stops being a way to find out what a build WOULD do.
# GNU Make puts the dry-run flag in the first word of MAKEFLAGS.
DRY_RUN := $(findstring n,$(firstword -$(MAKEFLAGS)))

BUILD_FLAGS   := EXTRA_CFLAGS=$(EXTRA_CFLAGS)
BUILD_STAMP   := .build-flags
FLAGS_CHANGED := $(if $(DRY_RUN),,$(shell \
  if [ "$$(cat $(BUILD_STAMP) 2>/dev/null)" != '$(BUILD_FLAGS)' ]; then \
     if [ -f $(BUILD_STAMP) ]; then echo changed; fi; \
     find . \( -name '*.o' -o -name '*.d' \) -not -path './tests/*' -delete; \
     rm -f tests/qemu/fixtures/*.o tests/qemu/fixtures/*.d; \
     rm -f kernel.elf kernel.bin; \
     printf '%s' '$(BUILD_FLAGS)' > $(BUILD_STAMP); \
  fi))
ifeq ($(FLAGS_CHANGED),changed)
$(info make: EXTRA_CFLAGS changed to "$(EXTRA_CFLAGS)"; discarded every kernel \
object so nothing is built with the previous flag set)
endif
# The `-not -path './tests/*'` above protects tests/build, which has its own flag
# stamp. tests/qemu/fixtures is the exception and is purged explicitly, because a
# fixture object is only in $(KERNEL_OBJS) when its flag is set — so after
# `make EXTRA_CFLAGS=-DFABLEOS_AC97_REFERENCE` the AC'97 fixture's .o and .d
# survived the flip back. The .o was harmless (nothing linked it), but the .d was
# not: it names vm/programs/ac97_bringup.h as a prerequisite, and the audits that
# check THIS KERNEL KNOWS NOTHING ABOUT AUDIO read the -MMD depfiles to find out
# what a default build compiled (tests/qemu/lint_printf.py does exactly that). A
# stale depfile therefore made a source audit report a full AC'97 register map
# inside the default build. The claim being audited is the whole point of this
# project, so the audit must not be able to see a previous build's ghosts.
LDFLAGS := -n -T linker.ld
# libgcc supplies compiler runtime helpers (e.g. __udivti3 for 128-bit division
# used by mbedTLS bignum) that aren't otherwise available freestanding.
LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

# Kernel + drivers. Add a driver by dropping a .c here (or under drivers/).
#
# tools/ is wildcarded on purpose: a tool self-registers through the linker
# (REGISTER_TOOL, see include/tool.h), so adding one to the model's syscall
# surface means dropping in a .c file and editing nothing else at all.
TOOL_SRCS := $(wildcard tools/*.c)

KERNEL_SRCS := kernel/main.c kernel/drivers.c $(TOOL_SRCS) \
               arch/x86_64/idt.c arch/x86_64/fault.c \
               core/kobject.c core/tool.c core/audio.c core/fiber.c \
               core/capability.c core/agenda.c \
               mm/heap.c device/device.c \
               fs/vfs/vfs.c fs/native/ramfs.c fs/fs.c \
               fs/fat/fat_vol.c fs/fat/fat_dir.c fs/fat/fat.c \
               drivers/block/block.c drivers/block/ata.c \
               lib/base.c lib/libc_shim.c lib/kfmt.c lib/trace.c \
               lib/fb.c lib/font.c lib/font_spleen8x16.c \
               gui/wm.c gui/widgets.c gui/gui_demo.c \
               apps/runtime.c apps/expr.c apps/cap.c apps/app_selftest.c \
               apps/app_format_selftest.c apps/app_audio_selftest.c \
               vm/dvm.c \
               compiler/cc.c compiler/cc_lex.c compiler/cc_parse.c \
               compiler/cc_x64.c compiler/cc_sym.c compiler/cc_store.c \
               drivers/serial/serial.c drivers/fwcfg/fwcfg.c drivers/pci/pci.c \
               drivers/acpi/acpi.c drivers/acpi/power.c \
               drivers/acpi/power_selftest.c \
               drivers/input/input.c drivers/input/kbd.c \
               drivers/input/serial_input.c drivers/input/script.c \
               drivers/input/mouse.c \
               drivers/net/e1000.c drivers/rtc/rtc.c \
               net/json.c net/model.c net/chat.c net/sse.c net/net.c \
               net/fetch.c net/faultchat.c net/tls_ca.c

# The exception self-test, in DIAGNOSTIC BUILDS ONLY. fault_inject.c exports
# fault_inject(), which deliberately divides by zero and dereferences an
# unmapped page; its own header argues that a machine whose only interface is
# natural language must not have a "kill yourself" verb sitting one hallucination
# away from the model, and everything that would call it is already behind
# #ifdef FABLEOS_FAULT_TEST. That made "it is unreachable" a convention about the
# source; leaving the object out of a normal image makes it a fact about the
# binary. Verified before the change: no object in the kernel link had an
# undefined reference to fault_inject or fault_inject_name.
ifneq ($(findstring FABLEOS_FAULT,$(EXTRA_CFLAGS)),)
KERNEL_SRCS += arch/x86_64/fault_inject.c
endif

# THE REFERENCE AUDIO BRING-UP, IN THE FIXTURE BUILD ONLY — same argument as
# fault_inject.c above, for a stronger reason.
#
# This kernel's premise is that it knows NOTHING about the sound card an operator
# attaches: the model is supposed to write that driver at run time through the
# driver VM. A hand-written AC'97 driver in the image contradicts the premise and
# also prevents the experiment, because it claims the device and initialises the
# codec before anyone can type a sentence. So the reference bring-up moved out of
# vm/programs/ to tests/qemu/fixtures/ and is linked ONLY when a build asks for it
# by name. It is still a boot-time driver in that build, which is what lets
# tests/qemu/cases/ac97.case assert it and keeps the proof that the VM can drive
# real silicon from rotting.
#
#   make EXTRA_CFLAGS=-DFABLEOS_AC97_REFERENCE
#
# The case's `build-cflags:` line makes the harness do exactly that and then put
# the tree back; the fixture also #errors if compiled without the define. A
# default `make` links no audio code at all — verify with:
#   nm kernel.elf | grep -i ac97      (expect nothing)
ifneq ($(findstring FABLEOS_AC97_REFERENCE,$(EXTRA_CFLAGS)),)
KERNEL_SRCS += tests/qemu/fixtures/ac97_boot.c
endif

KERNEL_OBJS := $(KERNEL_SRCS:.c=.o)

# Vendored lwIP (core + IPv4 + Ethernet + the altcp_tls mbedTLS port).
LWIP_SRCS := $(wildcard lwip/src/core/*.c) \
             $(wildcard lwip/src/core/ipv4/*.c) \
             lwip/src/netif/ethernet.c \
             lwip/src/apps/altcp_tls/altcp_tls_mbedtls.c \
             lwip/src/apps/altcp_tls/altcp_tls_mbedtls_mem.c
LWIP_OBJS := $(LWIP_SRCS:.c=.o)

# Vendored mbedTLS (whole library; disabled modules compile to ~nothing).
MBEDTLS_SRCS := $(wildcard mbedtls/library/*.c)
MBEDTLS_OBJS := $(MBEDTLS_SRCS:.c=.o)

# isr.asm: the 256 exception stubs. switch.asm: the fiber context switch — the
# only two places in this tree where C cannot express what has to happen.
ARCH_ASM_OBJS := arch/x86_64/isr.o arch/x86_64/switch.o

OBJS    := boot/boot.o $(ARCH_ASM_OBJS) $(KERNEL_OBJS) $(LWIP_OBJS) $(MBEDTLS_OBJS)

# User-mode networking: the guest reaches the host at 10.0.2.2. Nothing runs
# there — the kernel does its own DNS and its own TLS straight to the internet.
NETFLAGS   := -netdev user,id=n0 -device e1000,netdev=n0

# ===========================================================================
# Handing the API key to the guest at RUN time
#
#   .env               KEY="sk-ant-..."   (gitignored; where the key lives)
#   $ANTHROPIC_API_KEY                       used when .env has no key
#   make run FABLEOS_ENV=/path/to/other/env   read a different file
#
# WHY A TEMP FILE. QEMU's -fw_cfg takes either `string=` or `file=`. `string=`
# is not an option: it puts the secret in QEMU's argv, where `ps` shows it to
# every user on the machine, and where it lands in shell history. So the key is
# written to a file created by mktemp — mode 0600, in $TMPDIR, i.e. OUTSIDE the
# repository and outside anything `make iso` copies into an image — and removed
# however the run ends: normal exit, a QEMU that refused to start, or the Ctrl-C
# that is how you actually leave `make run`. The only way to leave it behind is
# SIGKILL on the shell, which no trap anywhere can catch.
#
# That trap is why every keyed run target is ONE shell command with no `exec`:
# `exec` replaces the shell and takes its EXIT trap with it, and a second recipe
# line would be a second shell that never had the trap. Do not split them.
#
# The key is read inside the recipe's shell and never into a make variable: a
# make variable is visible in `make -p`, is exported to every sub-make, and gets
# echoed with the recipe.
# ===========================================================================
FABLEOS_ENV ?= .env

# WHICH LINE OF .env IS THE KEY. Two rules, both learned the hard way.
#
#   1. The NAME IS MATCHED EXACTLY, against a two-name allowlist. This pattern
#      used to be `[A-Z_]*KEY`, i.e. any variable whose name ends in KEY, with
#      `tail -n 1` picking the LAST match. A .env file is conventionally shared
#      across a project and holds several credentials, so `KEY=sk-ant-...`
#      followed by `AWS_SECRET_ACCESS_KEY=...` sent the AWS secret to
#      api.anthropic.com in the x-api-key header — and the plainly realistic
#      `ANTHROPIC_API_KEY=` / `OPENAI_API_KEY=` pair is alphabetical, so the
#      OpenAI key won. Nothing surfaced it: fwcfg prints a byte count by design,
#      so the boot log said "32 bytes loaded" and then 401, indistinguishable
#      from a typo. Exfiltrating someone else's credential to a third party is
#      not an acceptable failure mode for a misnamed variable.
#
#   2. ORDER IN THE FILE DOES NOT DECIDE ANYTHING. ANTHROPIC_API_KEY wins over
#      KEY because it is the more specific name, not because of where it sits,
#      and `head -n 1` makes a repeated name deterministic too.
#
# Any other *KEY= line is named (NEVER printed) so the operator learns their
# other credential was ignored rather than silently transmitted.
#
# `set +x +v` is the first thing in the recipe, before `key=` is ever assigned.
# On macOS /bin/sh is bash in POSIX mode, and bash honours an INHERITED
# SHELLOPTS: with `export SHELLOPTS=xtrace` in the environment (a CI image, a
# debugging developer) every command below is traced to stderr with the value
# expanded, so `make run 2>&1 | tee build.log` puts the live key in a log file
# four times over. One token prevents it.
READ_KEY = set +x +v; key=; \
	if [ -f '$(FABLEOS_ENV)' ]; then \
	  for n in ANTHROPIC_API_KEY KEY; do \
	    key=$$(sed -n "s/^[[:space:]]*\(export[[:space:]]*\)*$$n[[:space:]]*=[[:space:]]*//p" '$(FABLEOS_ENV)' \
	           | head -n 1 | tr -d '\r\n' \
	           | sed -e 's/^"//' -e 's/"$$//' -e "s/^'//" -e "s/'$$//"); \
	    if [ -n "$$key" ]; then break; fi; \
	  done; \
	  other=$$(sed -n 's/^[[:space:]]*\(export[[:space:]]*\)*\([A-Z_]*KEY\)[[:space:]]*=.*/\2/p' '$(FABLEOS_ENV)' \
	           | grep -vx -e KEY -e ANTHROPIC_API_KEY | sort -u | tr '\n' ' '); \
	  if [ -n "$$other" ]; then \
	    echo "note: $(FABLEOS_ENV) also defines $$other - IGNORED. Only KEY and ANTHROPIC_API_KEY are read as the Anthropic key."; \
	  fi; \
	fi; \
	if [ -z "$$key" ]; then key=$${ANTHROPIC_API_KEY:-}; fi;
#      ^ and NOTHING after it. There is deliberately no `key=$${KEY:-}` fallback
#      for an exported shell KEY. It reads plausible — KEY is this project's own
#      name for the key inside .env — but `KEY` in the ENVIRONMENT is a
#      maximally generic name that a licence key, a GPG key id or an unrelated
#      secret may already be sitting in, and taking it would send that secret to
#      api.anthropic.com in the x-api-key header. That is exactly the
#      exfiltration the two-names rule above exists to prevent; the rule would
#      be worth little if the environment half quietly read a third, vaguer
#      name. The dotenv-plugin problem that motivated the $(origin KEY) guard is
#      already solved by that guard alone: in that scenario .env exists and
#      the loop above wins, so this line would never have fired anyway.

# Expands to the -fw_cfg arguments in "$@", or to nothing at all. No key is the
# supported default: the kernel boots, says it has none, and the API answers 401.
KEY_ARGS = $(READ_KEY) set --; \
	if [ -n "$$key" ]; then \
	  keyfile=$$(mktemp "$${TMPDIR:-/tmp}/fableos-fwcfg.XXXXXXXX") || exit 1; \
	  trap 'rm -f "$$keyfile"' EXIT HUP INT QUIT TERM; \
	  chmod 600 "$$keyfile"; \
	  printf '%s' "$$key" > "$$keyfile"; \
	  key=; \
	  set -- -fw_cfg "name=opt/fableos/apikey,file=$$keyfile"; \
	  echo "note: api key passed to the guest over fw_cfg as opt/fableos/apikey"; \
	else \
	  echo "note: no api key in $(FABLEOS_ENV) and no ANTHROPIC_API_KEY - the API will answer 401"; \
	fi;

all: kernel.bin

boot/boot.o: boot/boot.asm .build-flags
	$(ASM) -f elf64 $< -o $@

# The 256 exception stubs (arch/x86_64/isr.asm), same assembler conventions.
arch/x86_64/%.o: arch/x86_64/%.asm .build-flags
	$(ASM) -f elf64 $< -o $@

%.o: %.c .build-flags
	$(CC) $(CFLAGS) $< -o $@

# Vendored code is not this project's code and cannot be fixed here: lwIP's
# altcp_tls port emits six warnings under -Wall -Wextra (ignored
# mbedtls_ssl_flush_output results, a sign-compare, two unused parameters) on
# every clean build, and their only effect is to bury the next warning in a file
# we DO own. Silence the vendored trees so a warning means something again. Same
# reasoning as -w on TESTSRC_test_tls_chain below. -w changes no code
# generation: the objects are byte-identical with and without it.
lwip/%.o mbedtls/%.o: CFLAGS += -w

# Header dependencies emitted by -MMD. Absent on a clean tree; that is fine,
# there is nothing stale to catch yet.
-include $(OBJS:.o=.d)

# Makefile is a prerequisite because this file decides WHICH OBJECTS ARE LINKED,
# and that is not visible in any timestamp. Removing a source from KERNEL_SRCS
# (say, an audio driver that must not ship) leaves every remaining object older
# than kernel.elf, so make would say "Nothing to be done" and the image would
# still contain the code that was just removed. One prerequisite, one relink.
kernel.elf: $(OBJS) linker.ld Makefile
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

kernel.bin: kernel.elf
	$(OBJCOPY) -O elf32-i386 $< $@

# The console is VGA text mode (720x400). zoom-to-fit lets the window scale, so
# just drag it bigger (or use the green button) and the text scales up.
#
# NOT called DISPLAY: that name is the X11 environment variable, and an
# environment variable beats `?=`. Anyone with XQuartz running, or on `ssh -X`,
# silently got `-display :0` and a QEMU that refused to start, with a Makefile
# that looked correct.
QEMU_DISPLAY ?= cocoa,zoom-to-fit=on

# ...but QEMU opens that window at the raw framebuffer size, which is unreadably
# small on a Retina display, and -display cocoa has no option for the initial
# size. So resize it from the outside once it appears: ZOOM is the multiple of
# the 720x400 console to aim for, +28 covers the title bar, and QEMU snaps the
# result back to the 720:400 aspect itself. The window is moved to the top-left
# first because it can only grow as far as the screen edge — resizing it in
# place silently clamps to whatever room is left to the right.
#
# This needs Accessibility permission for the terminal you run make from
# (System Settings > Privacy & Security > Accessibility). Without it the resize
# is skipped with a one-line warning and you get the small window as before;
# dragging it bigger by hand still works. Runs in the background so the serial
# console on stdio stays in the foreground.
ZOOM ?= 2
RESIZE_WINDOW = ( W=$$((720 * $(ZOOM))); H=$$((400 * $(ZOOM) + 28)); \
	for i in $$(seq 40); do \
	  osascript -e 'tell application "System Events" to tell process "qemu-system-x86_64"' \
	            -e 'set position of window 1 to {40, 45}' \
	            -e "set size of window 1 to {$$W, $$H}" \
	            -e 'end tell' >/dev/null 2>&1 && exit 0; \
	  sleep 0.25; \
	done; \
	echo "note: could not resize the QEMU window (grant your terminal Accessibility permission)" >&2 ) &

# Extra QEMU arguments, appended verbatim. The point of this hook is hardware the
# kernel has NO driver for: PCI enumeration finds it, reports it as unclaimed, and
# the model can then read its config space and write a bring-up program for it in
# the driver VM (include/dvm.h). That is the whole reason vm/ exists.
#
#   make run QEMU_EXTRA="-device AC97 -audiodev coreaudio,id=a0"
#   make run QEMU_EXTRA="-device rtl8139,netdev=n0"     # a NIC we cannot drive
#   make run QEMU_EXTRA="-device nvme,drive=d0,serial=1 -drive id=d0,file=disk.img,if=none"
#
# It goes AFTER $(NETFLAGS) so it can add devices without replacing the NIC, which
# overriding NETFLAGS itself would do — taking networking, and therefore the model,
# down with it.
QEMU_EXTRA ?=

# ===========================================================================
# THE DISK — the only thing on this machine that survives a reboot
#
#   make run                     attaches disk.img, creating it if it is absent
#   make disk                    just create it
#   make disk DISK_MB=512        a bigger one (default 128 MiB)
#   make run DISK_IMG=           boot with NO disk at all (the old behaviour)
#   make disk-mount              attach the image on this Mac and READ IT
#   make disk-unmount            eject it again
#   make disk-erase              throw the machine's memory away and start over
#
# WHY dd AND NOT newfs_msdos. The image this target makes is 128 MiB of zeros
# and nothing else. The KERNEL formats it, on its first boot, with its own
# mkfs (fs/fat/fat_vol.c) — which means `make disk-mount` on this Mac is a test
# of the filesystem this kernel actually writes, not of Apple's. A blank image
# is also the only thing fs/fat/fat.c will format without being asked: anything
# it does not recognise is left untouched, because a disk holding something
# unreadable is far more likely to be data somebody wants than a mistake.
#
# NOTHING SECRET GOES IN HERE. The API key travels host -> guest over fw_cfg
# into RAM (see KEY_ARGS above) and is never written to a file the guest can
# see. The image is gitignored for size and churn, not for secrecy.
#
# `clean` deliberately does NOT delete it: it is the machine's accumulated
# work, and a build system that erases the disk is a build system nobody can
# trust. `make disk-erase` is the deliberate way.
# ===========================================================================
DISK_IMG ?= disk.img
DISK_MB  ?= 128

# A literal comma cannot appear inside $(if ...) arguments: make splits on it.
comma := ,
# Empty when DISK_IMG is empty, which is how a no-disk boot is asked for.
DISKFLAGS = $(if $(DISK_IMG),-drive file=$(DISK_IMG)$(comma)format=raw$(comma)if=ide$(comma)index=0$(comma)media=disk,)

$(DISK_IMG):
	@dd if=/dev/zero of=$@ bs=1m count=$(DISK_MB) >/dev/null 2>&1 || \
	   { echo "disk: could not create $@"; exit 1; }
	@echo "disk: created $@, $(DISK_MB) MiB of zeros. The kernel formats it as"
	@echo "      FAT32 on the next boot and mounts it at /disk."

disk: $(DISK_IMG)

disk-erase:
	rm -f $(DISK_IMG)
	@$(MAKE) --no-print-directory $(DISK_IMG)

# Read the machine's disk from macOS. This is the strongest possible check on
# the filesystem: a FAT32 volume written entirely by this kernel, mounted by an
# operating system that has never heard of it.
disk-mount: $(DISK_IMG)
	@hdiutil attach -imagekey diskimage-class=CRawDiskImage $(DISK_IMG) || \
	   { echo "disk: hdiutil refused the image (is it already attached?)"; exit 1; }
	@echo ""
	@ls -la /Volumes/FABLEOS 2>/dev/null || true
	@echo ""
	@echo "eject it with 'make disk-unmount' before booting the machine again."

disk-unmount:
	@diskutil eject /Volumes/FABLEOS 2>/dev/null || \
	   echo "disk: nothing mounted at /Volumes/FABLEOS"

run: kernel.bin $(DISK_IMG)
	@$(RESIZE_WINDOW)
	@$(KEY_ARGS) qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) $(DISKFLAGS) $(QEMU_EXTRA) -display $(QEMU_DISPLAY) -serial stdio "$$@"

run-nox: kernel.bin $(DISK_IMG)
	@$(KEY_ARGS) qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) $(DISKFLAGS) $(QEMU_EXTRA) -display none -serial stdio "$$@"

# ===========================================================================
# Bootable media — GRUB, an ISO, and a USB stick
#
#   make iso            build fableos.iso (GRUB + kernel, BIOS El Torito,
#                       isohybrid MBR so the same file works on a USB stick)
#   make run-iso        boot the ISO in QEMU through GRUB (-cdrom, no -kernel)
#   make run-iso-nox    same, headless, serial on stdio
#   make usb            print the exact macOS recipe for writing it to a stick
#   make iso-toolchain  brew install what `make iso` needs
#
# WHY: every other run target uses QEMU's -kernel shortcut, which reads the
# Multiboot header directly. No physical machine does that. `make iso` produces
# an artifact a real BIOS will start: GRUB in the boot catalogue, kernel.bin at
# /boot/kernel.bin, boot/grub/grub.cfg as the menu. The kernel is byte-for-byte
# the same file `make run` boots.
#
# grub-mkrescue is not part of the base toolchain and is named differently
# depending on which cross-GRUB you installed, so we look for all three spellings
# and say something useful if none is present. This lookup is lazy (?= makes a
# recursive variable) so a normal `make` never pays for it.
# ===========================================================================

GRUB_MKRESCUE ?= $(shell command -v grub-mkrescue 2>/dev/null || \
                         command -v i686-elf-grub-mkrescue 2>/dev/null || \
                         command -v x86_64-elf-grub-mkrescue 2>/dev/null)
ISO     := fableos.iso
ISOROOT := build/iso

iso: $(ISO)

$(ISO): kernel.bin boot/grub/grub.cfg
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
	   echo "iso: no grub-mkrescue on PATH. Run 'make iso-toolchain'."; exit 1; fi
	@command -v xorriso >/dev/null 2>&1 || { \
	   echo "iso: xorriso not found (grub-mkrescue needs it). Run 'make iso-toolchain'."; exit 1; }
	rm -rf $(ISOROOT)
	mkdir -p $(ISOROOT)/boot/grub
	cp kernel.bin $(ISOROOT)/boot/kernel.bin
	cp boot/grub/grub.cfg $(ISOROOT)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) --compress=xz -o $@ $(ISOROOT)
	@echo "iso: $@ is $$(du -h $@ | cut -f1); boot it with 'make run-iso'"

# Boot through a real bootloader instead of -kernel. This is the target that
# proves the ISO, and the only difference from `make run` is how the kernel got
# into memory.
run-iso: $(ISO)
	@$(RESIZE_WINDOW)
	@$(KEY_ARGS) qemu-system-x86_64 -cdrom $(ISO) $(NETFLAGS) -display $(QEMU_DISPLAY) -serial stdio "$$@"

run-iso-nox: $(ISO)
	@$(KEY_ARGS) qemu-system-x86_64 -cdrom $(ISO) $(NETFLAGS) -display none -serial stdio "$$@"

# Deliberately does NOT write anything. Picking the wrong /dev/diskN erases a
# disk, and no build system should guess which one you meant.
usb: $(ISO)
	@echo "$(ISO) is an isohybrid image: it carries a protective MBR, so the"
	@echo "same bytes boot from optical media and from a USB stick."
	@echo ""
	@echo "On macOS:"
	@echo "  1. diskutil list                     # find the stick, e.g. /dev/disk4"
	@echo "  2. diskutil unmountDisk /dev/diskN   # unmount, do NOT eject"
	@echo "  3. sudo dd if=$(ISO) of=/dev/rdiskN bs=4m status=progress"
	@echo "         (rdiskN, with the r, is the raw device and is ~20x faster)"
	@echo "  4. diskutil eject /dev/diskN"
	@echo ""
	@echo "Then boot the target machine from USB with LEGACY BIOS / CSM enabled."
	@echo ""
	@echo "WHAT THIS KERNEL NEEDS FROM THE MACHINE, and what it does when it is"
	@echo "missing. None of the following has been tried on physical hardware -"
	@echo "the ISO is verified only under QEMU (make run-iso):"
	@echo "  * Legacy BIOS or CSM. The image has no UEFI boot path at all, and"
	@echo "    even with one the console is VGA text mode at 0xB8000, which pure"
	@echo "    UEFI does not provide. A UEFI-only laptop will not boot this."
	@echo "  * An Intel 82540EM/82545EM NIC (PCI 8086:100e/1004/100f). Anything"
	@echo "    else - and that is every modern machine - prints 'e1000: no"
	@echo "    supported NIC found' and boots to a prompt with no model behind"
	@echo "    it. There is also no DHCP: the address is hardcoded 10.0.2.15/24"
	@echo "    via 10.0.2.2, which is QEMU's user-mode network, not your LAN."
	@echo "  * QEMU's fw_cfg interface at 0x510, which is where the Anthropic"
	@echo "    API key arrives (it is never compiled in). No physical machine"
	@echo "    has it, so a real box prints 'fwcfg: no QEMU fw_cfg interface'"
	@echo "    and 'no api key', and every request it makes gets a 401. Giving"
	@echo "    real hardware a key needs a different channel - see the FUTURE"
	@echo "    EXTENSION POINTS in include/fwcfg.h."
	@echo "  * A PS/2 keyboard controller (i8042). USB keyboards work only on"
	@echo "    firmware that still does i8042 emulation; there is no USB stack."
	@echo "    A serial console on COM1 at 115200 8N1 always works and is the"
	@echo "    reliable way to talk to this thing on real hardware."

iso-toolchain:
	brew install i686-elf-grub xorriso mtools

# Added as a prerequisite rather than by editing the clean recipe, so this
# block stays self-contained.
clean: clean-iso
clean-iso:
	rm -rf build $(ISO)

# ===========================================================================
# Tests
#
#   make test        run everything (host unit tests + QEMU boot tests)
#   make test-host   host-native unit tests only (milliseconds — the inner loop)
#   make test-host-asan  the same suites under AddressSanitizer + UBSan
#   make test-qemu   boot the kernel in QEMU and assert on the serial log
#
# Pure-logic kernel modules (heap, JSON, trace formatting, the driver VM) are
# compiled with the HOST compiler against tests/host/kshim.c and run natively,
# so iterating on them costs no QEMU boot. Anything hardware-dependent is tested
# in tests/qemu/ instead. See tests/host/test_heap.c for the reference example.
# ===========================================================================

HOSTCC     ?= cc
# NOTE: deliberately no -Iport. port/ holds freestanding shims (stdio.h, time.h,
# string.h) for lwIP/mbedTLS; on the host they would shadow the real libc
# headers. Host tests link against real libc.
HOST_CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Iinclude -Itests/host \
               -DFABLEOS_HOSTTEST
HOST_SHIM  := tests/host/harness.c tests/host/kshim.c

# tests/host/test_cc.c does something no other suite does: it EXECUTES the x86-64
# machine code its subject emits, in-process, so that the code generator is tested
# against reality and not against an expected byte string. That requires the test
# binary itself to be an x86-64 process. On an Apple-silicon Mac that is one flag
# and Rosetta; on an x86-64 host it is already true and this is empty; on any
# other host it is also empty and the suite still compiles, still checks every
# diagnostic, and skips only the tests that run code (cc_run returns CC_ENOEXEC
# and the suite says so out loud rather than reporting a green it did not earn).
ifeq ($(shell uname -s)-$(shell uname -m),Darwin-arm64)
CC_HOST_ARCH := -arch x86_64
else
CC_HOST_ARCH :=
endif
# Each test compiles its sources in one command, so there are no stale objects to
# worry about here — but the BINARY was previously newer than any header it was
# built from, so editing a header left the suite testing the old contract. The
# headers are cheap to depend on wholesale.
HOST_HDRS  := $(wildcard include/*.h) $(wildcard tests/host/*.h)

# ...and the same argument as .build-flags, one directory over. The host
# binaries depended on sources and headers only, so in an already-built tree
#   make test-host HOST_CFLAGS="... -fsanitize=address"
# compiled nothing, ran the previous uninstrumented binaries, and printed
# "host tests: PASS" — a green that says nothing about the flags you asked for.
# The stamp holds the compiler and every flag set that reaches a host binary,
# including the per-test ones, so changing any of them forces an honest rebuild.
# It has the same whole-second mtime hole as .build-flags above and is closed the
# same way: the check runs at parse time and DELETES the stale binaries, because
# `make test-host` silently running yesterday's binaries is exactly the failure
# a stamp is supposed to prevent.
# HOST_BUILD_FLAGS is recursive on purpose: HOSTCFLAGS_* and HOST_TESTS are not
# known yet. The parse-time check that consumes it therefore lives further down,
# immediately after HOST_TESTS is defined — see "host flag stamp" below.
HOST_BUILD_FLAGS = HOSTCC=$(HOSTCC) HOST_CFLAGS=$(HOST_CFLAGS) \
                   $(foreach t,$(HOST_TESTS),$(t):$(HOSTCFLAGS_$(t)))
HOST_STAMP       := tests/build/.host-flags

# Kernel sources under test, per test binary. Add a line when adding a test.
TESTSRC_test_heap := mm/heap.c
# test_fiber links the REAL heap, because a fiber stack is a kmalloc'd block and
# "the poison band caught the overrun instead of the allocator finding it later"
# is only a claim worth making against the allocator that would have found it.
# It supplies its OWN fiber_arch_switch/fiber_arch_stack_init: the kernel's live
# in arch/x86_64/switch.asm and this host is arm64. See the head of the suite.
TESTSRC_test_fiber := core/fiber.c mm/heap.c
TESTSRC_test_json := net/json.c
TESTSRC_test_model_mock := net/json.c net/model.c net/model_mock.c
TESTSRC_test_input := drivers/input/input.c drivers/input/script.c
TESTSRC_test_mouse := drivers/input/mouse.c
TESTSRC_test_dev_tools := core/tool.c core/kobject.c device/device.c mm/heap.c lib/trace.c net/json.c
TESTSRC_test_e1000 := core/kobject.c device/device.c mm/heap.c
TESTSRC_test_fwcfg := drivers/fwcfg/fwcfg.c
TESTSRC_test_mem_tools := tools/mem_tools.c core/tool.c mm/heap.c lib/trace.c net/json.c
TESTSRC_test_screen_tools := tools/screen_tools.c core/tool.c lib/trace.c net/json.c
TESTSRC_test_vfs_tools := tools/vfs_tools.c core/tool.c core/kobject.c mm/heap.c fs/vfs/vfs.c fs/native/ramfs.c lib/trace.c net/json.c
TESTSRC_test_tool := core/tool.c lib/trace.c net/json.c
# The persistence stack, whole: FAT32 on the block layer on a synthetic disk,
# plus the real ATA driver against a synthetic drive (test_fs_disk.c #includes
# drivers/block/ata.c, hence the prerequisite further down). device/device.c is
# there because the ATA driver publishes each disk into the device model, and
# drivers/rtc/rtc.c because FAT timestamps come from the wall clock.
TESTSRC_test_fs_disk := fs/fat/fat.c fs/fat/fat_dir.c fs/fat/fat_vol.c \
                        fs/vfs/vfs.c drivers/block/block.c \
                        core/kobject.c device/device.c mm/heap.c drivers/rtc/rtc.c
TESTSRC_test_rtc := drivers/rtc/rtc.c tools/time_tools.c core/tool.c lib/trace.c net/json.c
TESTSRC_test_acpi := drivers/acpi/acpi.c drivers/acpi/power.c tools/power_tools.c \
                     core/tool.c lib/trace.c net/json.c
TESTSRC_test_tls_verify := net/tls_ca.c drivers/rtc/rtc.c
# test_tls_chain links the REAL vendored mbedTLS and asks it for verdicts, which
# is the only way to test the verifier rather than a transcription of it. It is
# the one host test that needs the kernel's mbedTLS config and FABLEOS_VERIFY_CERTS
# — before this, nothing in the tree built with that flag at all, so a
# path-validation check could be switched off with `make test` still green.
# -w because the vendored library is not warning-clean under -Wall -Wextra on the
# host; the sources under test here are tls_ca.c and rtc.c, both covered warning-
# clean by test_tls_verify.
TESTSRC_test_tls_chain := net/tls_ca.c drivers/rtc/rtc.c $(MBEDTLS_SRCS)
HOSTCFLAGS_test_tls_chain := -DFABLEOS_VERIFY_CERTS -Imbedtls/include \
                             -DMBEDTLS_CONFIG_FILE='"mbedtls_config.h"' -w
TESTSRC_test_fb := lib/fb.c lib/font.c lib/font_spleen8x16.c
TESTSRC_test_gui := gui/wm.c gui/widgets.c gui/gui_demo.c tools/gui_tools.c \
                    lib/fb.c lib/font.c lib/font_spleen8x16.c \
                    drivers/input/mouse.c core/tool.c lib/trace.c net/json.c
# tools/gui_tools.c is linked in unmodified so the golden transcript can press
# the app's keys with gui_click, exactly as the model does.
# apps/cap.c is the capability broker, so every suite that links the app runtime
# links it — and it calls core/audio.c, which needs the driver VM (the DMA arena
# the samples live in) and the device model (a sink is published there). That is a
# real dependency and not test scaffolding: it is the same object file the kernel
# links, so what these suites exercise is the path a document really takes to a
# driver. See include/app.h DECISION 4.
APPSRC := apps/runtime.c apps/expr.c apps/cap.c core/audio.c vm/dvm.c \
          device/device.c core/kobject.c mm/heap.c
TESTSRC_test_app := $(APPSRC) tools/app_tools.c \
                    tools/gui_tools.c gui/wm.c gui/widgets.c gui/gui_demo.c \
                    lib/fb.c lib/font.c lib/font_spleen8x16.c \
                    drivers/input/mouse.c core/tool.c lib/trace.c \
                    net/json.c net/model.c net/model_mock.c net/chat.c
# test_app_format covers the format additions (tick, the time snapshot, rand, at,
# round) and the shipped examples. No tools and no chat: it drives app_launch(),
# app_tick() and gui_click_widget() directly, so it needs only the runtime and the
# window manager.
TESTSRC_test_app_format := $(APPSRC) gui/wm.c gui/widgets.c \
                    lib/fb.c lib/font.c lib/font_spleen8x16.c \
                    drivers/input/mouse.c net/json.c lib/trace.c
# test_app_audio covers the capability call end to end: the statement compiles, a
# bound is enforced, a synthetic sink receives real PCM, and a handler provably
# does NOT reach the driver (only app_service() does). Same source list, plus the
# window manager to click with and trace.c to read the ground-truth line back out.
TESTSRC_test_app_audio := $(APPSRC) gui/wm.c gui/widgets.c \
                    lib/fb.c lib/font.c lib/font_spleen8x16.c \
                    drivers/input/mouse.c net/json.c lib/trace.c \
                    core/tool.c tools/app_tools.c
# calculator_json.h is generated from calculator.json (apps/examples/gen_header.py);
# both the kernel object that embeds it and the test that diffs it must follow it,
# and examples_json.h is the same arrangement for the rest of apps/examples/.
apps/runtime.o tests/build/test_app: apps/examples/calculator_json.h
apps/runtime.o tests/build/test_app_format: apps/examples/examples_json.h
# beeper.json is the worked example for a capability call; it reaches the kernel
# through the same generated header, and the audio self-test embeds it.
apps/app_audio_selftest.o tests/build/test_app_audio: apps/examples/examples_json.h
# lib/kfmt.c is the kernel's OWN vsnprintf. Under FABLEOS_HOSTTEST it renames
# itself to kfmt_vsnprintf/kfmt_snprintf so it can be linked ALONGSIDE the host
# libc, which is the entire point: the suite prints one format string through
# both formatters and diffs them. Host suites otherwise only ever exercise the
# host libc, and that is how a star-width defect lived in every model-facing
# assembler error in vm/dvm.c without a single test going red.
TESTSRC_test_kfmt := lib/kfmt.c
TESTSRC_test_trace := lib/trace.c
# tools/capability_tools.c is #included by the suite, not compiled beside it
# (REGISTER_TOOL again — see the prerequisite block below), so it is deliberately
# absent from this list and present there instead.
TESTSRC_test_capability := core/capability.c core/tool.c core/kobject.c \
                           mm/heap.c fs/vfs/vfs.c fs/native/ramfs.c \
                           vm/dvm.c lib/trace.c net/json.c
# The C compiler. tools/cc_tools.c is #included by the suite (REGISTER_TOOL), so
# it is deliberately absent here and present in the prerequisite block below.
# fs/ and core/kobject.c are here because the program store is real files in the
# real VFS: a host test that saves a program, "reboots" and calls it is testing
# the same code the kernel runs, not a stand-in.
TESTSRC_test_cc := compiler/cc.c compiler/cc_lex.c compiler/cc_parse.c \
                   compiler/cc_x64.c compiler/cc_sym.c compiler/cc_store.c \
                   core/tool.c core/kobject.c mm/heap.c \
                   fs/vfs/vfs.c fs/native/ramfs.c lib/trace.c net/json.c
# -fno-sanitize=function, for `make test-host-asan`. UBSan's `function` check
# reads a magic word and a type hash from the EIGHT BYTES BEFORE any indirect
# call's target, to verify the prototype. This suite's whole point is to call
# bytes the code generator just emitted, and byte 0 of that buffer is the
# generated entry trampoline, so there is nothing in front of it to read: the
# check faults with SIGBUS before the program runs. Excluding it here rather
# than in ASAN_HOST_CFLAGS keeps the check on for every other suite. Everything
# else about the sanitizers is unchanged, and the emitted code is still executed.
HOSTCFLAGS_test_cc := -Icompiler $(CC_HOST_ARCH) -fno-sanitize=function
TESTSRC_test_dvm := vm/dvm.c lib/trace.c
TESTSRC_test_dvm_mem := vm/dvm.c lib/trace.c
TESTSRC_test_dvm_ac97 := vm/dvm.c lib/trace.c
# core/audio.c is here because driver_install calls audio_register_vm(): the tool
# family that writes a driver is also the one that installs it as the audio sink,
# so the suite that tests those tools has to link the service they install into.
TESTSRC_test_dvm_tools := vm/dvm.c lib/trace.c core/tool.c core/kobject.c \
                          device/device.c mm/heap.c net/json.c net/model.c \
                          net/model_mock.c net/chat.c core/audio.c \
                          fs/vfs/vfs.c fs/native/ramfs.c
# test_audio links the REAL driver VM, so the play path a model-authored driver
# takes is exercised end to end against a synthetic device on the host: the suite
# assembles a play program, registers it as the sink, plays a tone, and then reads
# the PCM back out of the address the program handed its device. Without vm/dvm.c
# that path would only be testable by booting.
TESTSRC_test_audio := core/audio.c vm/dvm.c core/tool.c core/kobject.c \
                      device/device.c mm/heap.c lib/trace.c net/json.c
# A test that #includes a kernel .c (the REGISTER_TOOL / REGISTER_DRIVER linker
# sections do not survive a Mach-O host link any other way) does not get that
# file as a prerequisite from TESTSRC_*, so it needs one spelled out here.
# Without it `make test-host` re-runs a STALE binary and prints PASS for code
# that is no longer in the tree — the same failure the header dependency above
# was added to stop. Every #include of a ../../*.c must appear below:
#   $ grep -n '#include "\.\./\.\./.*\.c"' tests/host/*.c
tests/build/test_dev_tools: tools/dev_tools.c
tests/build/test_e1000: drivers/net/e1000.c
tests/build/test_fs_disk: drivers/block/ata.c fs/fat/fat.h
tests/build/test_audio: tools/audio_tools.c
# test_dvm_tools.c #includes the tool source and the recorded session, so it
# must follow both.
tests/build/test_dvm_tools: tools/dvm_tools.c vm/transcripts/dvm_bringup.h
tests/build/test_capability: tools/capability_tools.c
# The suite reads all four shipped examples off the disk and compiles them
# verbatim, so all four are prerequisites: an example edited without the suite
# being rebuilt is an example nothing checks.
tests/build/test_cc: tools/cc_tools.c compiler/cc_int.h \
                     compiler/examples/square.c compiler/examples/histogram.c \
                     compiler/examples/notes.c compiler/examples/broken.c
# ac97_bringup.h holds the reference driver program as a C string literal,
# derived from vm/programs/ac97_bringup.dvm by vm/programs/gen_header.py. There
# is deliberately NO rule to run that generator: the header is checked in, the
# build needs no python3, and drift is caught where it can be explained —
# test_dvm_ac97.c diffs the two and fails with "run python3
# vm/programs/gen_header.py". These two only need to follow the header itself.
# (The .o is under tests/ because the caller that runs the program on real
# hardware is a fixture, not kernel behaviour — see KERNEL_SRCS above.)
tests/qemu/fixtures/ac97_boot.o tests/build/test_dvm_ac97: vm/programs/ac97_bringup.h
TESTSRC_test_fault := arch/x86_64/fault.c tools/fault_tools.c core/tool.c \
                      lib/trace.c net/json.c
TESTSRC_test_fault_diagnose := net/faultchat.c arch/x86_64/fault.c \
                      tools/fault_tools.c core/tool.c lib/trace.c net/json.c \
                      net/model.c net/model_mock.c
# test_repair drives the whole self-repair loop: a fault, an escape to a guard,
# a diagnosis against the mock transport, a validated code patch, a rollback, and
# an agenda item that survives all of it. It therefore links the fault module,
# net/faultchat.c, core/agenda.c and BOTH tool families — the point of the suite
# is that these pieces work together, so stubbing any of them out would test
# something other than the loop. The VFS is here because the agenda store is a
# real file in a real ramfs.
TESTSRC_test_repair := arch/x86_64/fault.c net/faultchat.c core/agenda.c \
                      tools/fault_tools.c tools/agenda_tools.c \
                      core/tool.c core/kobject.c lib/trace.c net/json.c \
                      net/model.c net/model_mock.c mm/heap.c \
                      fs/vfs/vfs.c fs/native/ramfs.c
TESTSRC_test_chat := net/json.c net/model.c net/model_mock.c net/chat.c \
                     core/tool.c lib/trace.c
# test_agency drives whole multi-step jobs through the real loop against the real
# VFS and agent tool families, so it links both of them plus the filesystem.
TESTSRC_test_agency := net/json.c net/model.c net/model_mock.c net/chat.c \
                     core/tool.c core/kobject.c lib/trace.c mm/heap.c \
                     fs/vfs/vfs.c fs/native/ramfs.c \
                     tools/vfs_tools.c tools/agent_tools.c
TESTSRC_test_sse := net/json.c net/model.c net/sse.c
# net/fetch.c is the whole network path minus the sockets: the suite supplies its
# own fetch_backend_t, so no lwIP and no mbedTLS are linked. The VFS is here
# because net_fetch's save_path writes a real file into a real ramfs.
TESTSRC_test_net_tools := tools/net_tools.c net/fetch.c core/tool.c lib/trace.c \
                     net/json.c core/kobject.c mm/heap.c \
                     fs/vfs/vfs.c fs/native/ramfs.c

HOST_TESTS := $(patsubst tests/host/%.c,%,$(wildcard tests/host/test_*.c))
HOST_BINS  := $(addprefix tests/build/,$(HOST_TESTS))

# host flag stamp — the deferred half of HOST_BUILD_FLAGS above. Every per-test
# HOSTCFLAGS_* is now known, so the stamp can be compared for real.
HOST_FLAGS_CHANGED := $(if $(DRY_RUN),,$(shell \
  mkdir -p tests/build; \
  if [ "$$(cat $(HOST_STAMP) 2>/dev/null)" != '$(HOST_BUILD_FLAGS)' ]; then \
     if [ -f $(HOST_STAMP) ]; then echo changed; fi; \
     find tests/build -type f ! -name '.host-flags' -delete; \
     printf '%s' '$(HOST_BUILD_FLAGS)' > $(HOST_STAMP); \
  fi))
ifeq ($(HOST_FLAGS_CHANGED),changed)
$(info make: host test flags changed; discarded every binary in tests/build so \
none of them is yesterday's)
endif
# The stamp is written at parse time, so it always exists by the time the link
# rules below name it. This empty recipe stops make looking for a way to make it.
$(HOST_STAMP): ;
$(BUILD_STAMP): ;

# Generate one link rule per test so each pulls in only the sources it needs.
define HOST_TEST_RULE
tests/build/$(1): tests/host/$(1).c $$(HOST_SHIM) $$(TESTSRC_$(1)) $$(HOST_HDRS) \
                  tests/build/.host-flags
	@mkdir -p tests/build
	@$$(HOSTCC) $$(HOST_CFLAGS) $$(HOSTCFLAGS_$(1)) -o $$@ tests/host/$(1).c $$(HOST_SHIM) $$(TESTSRC_$(1))
endef
$(foreach t,$(HOST_TESTS),$(eval $(call HOST_TEST_RULE,$(t))))

test-host: $(HOST_BINS)
	@fail=0; for b in $(HOST_BINS); do \
	   echo "[host] $$b"; ./$$b || fail=1; \
	 done; \
	 if [ $$fail -eq 0 ]; then echo "host tests: PASS"; \
	 else echo "host tests: FAIL"; exit 1; fi

# Nine suites rest their central claim on a sanitizer — test_tool.c's "ASan
# proves it" and "ASan's redzones catch any write outside the window",
# test_json.c's "visible under ASan", test_trace.c, test_tls_verify.c — and
# until this target existed there was no way to build with one, so an
# out-of-bounds read on exactly those paths went undetected. Adding it found two
# real defects immediately: a read below mm/heap.c's arena in test_mem_tools.c,
# and a queued reply whose stack buffer had already gone out of scope in
# test_chat.c. Same rules and same sources as test-host; only the flags differ,
# and tests/build/.host-flags is what makes the rebuild honest.
#
# THE ONE EXCEPTION. On Mach-O the tool registry is emulated by letting the
# linker pack one 8-byte global per REGISTER_TOOL into "__DATA,tool_table" and
# walking it as an array; ASan's per-global redzones make that walk read padding.
# tests/host/tooltable.h annotates the entries it owns so the walk stays legal,
# but two tool sources carry their own copy of that shim (REGISTER_VFS_TOOL in
# tools/vfs_tools.c, the same in tools/agent_tools.c) and cannot be annotated
# from here, so the two suites that link them switch global instrumentation off.
# Heap, stack, use-after-free/scope and every UBSan check stay on for all of
# them. The real fix is one attribute in those two files.
#
# THE SECOND EXCEPTION, for the same reason one level down. test_fiber runs real
# code on stacks carved out of mm/heap.c's arena, which is a global. ASan's STACK
# instrumentation writes frame redzones into the shadow of whatever memory a
# frame occupies, so a fiber's frames poison the shadow of the arena — and it is
# never unpoisoned, because a suspended fiber's frames never return. The next
# kfree() of that block then trips a "stack-buffer-underflow ... inside global
# variable default_arena" inside the allocator's own poisoning memset. That is
# ASan describing this design back to itself, not a defect: a green run with
# -asan-stack=0 is the honest one. Heap, globals, use-after-free and every UBSan
# check stay on, and the poison bands in core/fiber.c are the stack-overflow
# detector here anyway — tests/host/test_fiber.c proves they fire.
ASAN_HOST_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_NO_GLOBALS  := -mllvm -asan-globals=0
ASAN_NO_STACK    := -mllvm -asan-stack=0
test-host-asan:
	@$(MAKE) --no-print-directory test-host \
	   HOST_CFLAGS="$(HOST_CFLAGS) $(ASAN_HOST_CFLAGS)" \
	   HOSTCFLAGS_test_vfs_tools="$(HOSTCFLAGS_test_vfs_tools) $(ASAN_NO_GLOBALS)" \
	   HOSTCFLAGS_test_agency="$(HOSTCFLAGS_test_agency) $(ASAN_NO_GLOBALS)" \
	   HOSTCFLAGS_test_fiber="$(HOSTCFLAGS_test_fiber) $(ASAN_NO_STACK)"

# The printf lint runs FIRST and gates the boots. It reads source, so it costs
# nothing, and it catches the one class of defect no test in this tree can:
# host suites link the real libc, so a format string the kernel's own formatter
# (lib/kfmt.c) cannot render works perfectly everywhere except in the binary
# that boots. It was clean when it was wired in; it is a lint, not a boot, so a
# failure here means someone typed a conversion kfmt does not implement.
test-qemu: kernel.bin
	@python3 tests/qemu/lint_printf.py
	@if [ -x tests/qemu/run.sh ]; then tests/qemu/run.sh; \
	 else echo "test-qemu: tests/qemu/run.sh not built yet"; exit 1; fi

test: test-host test-qemu

clean:
	rm -f boot/boot.o $(ARCH_ASM_OBJS) $(KERNEL_OBJS) $(LWIP_OBJS) $(MBEDTLS_OBJS) kernel.elf kernel.bin
	rm -f $(OBJS:.o=.d) .build-flags
	rm -rf tests/build
	# The AC'97 fixture object is only in $(KERNEL_OBJS) when the flag that
	# builds it is set, so a plain `make clean` would leave it behind.
	rm -f tests/qemu/fixtures/*.o tests/qemu/fixtures/*.d

toolchain:
	brew install nasm x86_64-elf-gcc qemu

.PHONY: all run run-nox clean toolchain test test-host test-host-asan test-qemu \
        iso run-iso run-iso-nox usb iso-toolchain clean-iso \
        disk disk-erase disk-mount disk-unmount
