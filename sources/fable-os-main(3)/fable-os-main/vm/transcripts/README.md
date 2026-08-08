# vm/transcripts — what a live model actually did, kept verbatim

These are recordings of REAL sessions: a live model, over TLS, driving real
emulated hardware, with the operator's key. They are evidence, not tests. Nothing
in the build reads them. They are here because the claim "a model wrote a driver
for a card the kernel had never heard of, and sound came out" is worth nothing
without the transcript and the samples beside it.

Every pair is produced by `tests/qemu/live_audio.py` and measured by
`tests/qemu/wavcheck.py`:

    NAME.log        the whole serial transcript, kernel voice included
    NAME.wav.txt    wavcheck.py's analysis of what the codec was really fed
    NAME.png        the framebuffer at the end, where an app was involved

## The headline result

`ac97-fullloop.*` — one boot, three sentences, the entire loop:

    driver_targets -> driver_run -> driver_install -> audio_tone -> app + gui_click

The model found an unclaimed 8086:2415 at 00:04.0, brought the codec up, wrote a
second short play program, installed it as the machine's audio sink ("vmaudio"),
played a 440 Hz note through the ordinary `audio_tone` tool, then authored a
two-widget app and pressed its own button. The WAV holds two clean 440 Hz sine
tones at **100.0% tonality and peak exactly 9000**, which is the amplitude
`audio_tone` was asked for.

## Read this one for the bug, not just the win

The same transcript is the best bug report in the tree. At line 100 the model
says, in its own words:

> the contract body is rendering blank (a `%*s` formatting bug in the kernel), so
> I can't read the exact install-program requirements from it

It was right. `tools/audio_tools.c` printed the play contract with `%.*s`, and
the kernel's reduced vsnprintf (`lib/libc_shim.c`) has no star precision — it
emitted the literal text `%*s` instead of the contract. Denied the one document a
play program cannot be written without, the model guessed, and the guess is
visible on the framebuffer in `ac97-fullloop.png`:

> One BDL entry: addr=r7, count=98304 samples, IOC+BUP set.

98304 is the whole 192 KiB PCM region, not `r9` (the bytes actually written for
this sound). AC'97's descriptor length field is 16 bits, so 98304 truncated to
32768 samples = 16384 stereo frames = **341.3 ms**, and `ac97-fullloop.wav.txt`
shows every note lasting exactly 341.3 ms regardless of the duration requested,
followed by a DC hold at 8081 — which is what `BUP` means on that part. A kernel
formatting bug, a model's guess, and a measurable acoustic artefact, joined end
to end.

That line is fixed. `tests/qemu/lint_printf.py` is the guard, and it still
reports a couple of dozen more instances in `vm/dvm.c`'s assembler diagnostics —
run it for the current count, which grows as that file does. Those are worse than
the one that was fixed here: they are the messages a model reads to correct its
own program, and several of them end in a `%d` that consumes the token's LENGTH,
so the kernel confidently tells the model this machine has `r0-r3`. See the
script's header for the measured before/after.

## `ac97-contract-fixed.*` — the same experiment with the contract readable

Run again after the `%.*s` fix. The model's reasoning changes exactly where you
would predict: it now writes **"r9 = bytes of PCM, so samples = r9/2"** instead of
hardcoding the region size. The note it produced is 1000.0 Hz, **100.0% tonal**,
peak exactly 8000, and the 26.9-second DC hold is gone (1 frame of trailing DC).

It is still not the 900 ms that was asked for; it is 217.3 ms. That is now a
different and much better bug. The model derived 86400 samples correctly from r9,
then wrote it into a field it had itself described as "low 16 bits" — 86400 masked
to 16 bits is 20864 samples = 10432 stereo frames = 217.33 ms, and the WAV holds
10432 frames. Long sounds on that part need the descriptor list split across
several entries. The fix moved the failure from "cannot read the spec" to "made an
arithmetic mistake inside the spec", which is the failure a driver author is
supposed to be able to have.

## `hda-install-attempt.*` — the most useful failure in this directory

Two live attempts at `intel-hda` + `hda-duplex`, both silent, and worth more than
the successes because of *how* they failed.

**The model wrote an AC'97 driver for an HD Audio card.** It read the right
values and drew the wrong conclusion: 8086:2668 class 04:03 it called "an Intel
AC'97 controller, not HDA" (it is ICH6 HD Audio), then — to its credit — said
"rather than trust that identification, let me probe", read offset 0 and got
**0x4401**, and took that as an AC'97 reset register reporting a codec. 0x4401 is
HDA's GCAP: 4 output streams, 4 input, 1 bidirectional, 64-bit capable. The probe
*confirmed* the wrong hypothesis because the number is plausible under both
readings. `hda2.log` is the same card being identified correctly, by a model that
compared the device ID against the ones AC'97 actually uses and noticed the part
was pure MMIO.

Three things the kernel did wrong here, none of them device-specific:

1. **A sound that never happened was reported as success.** The play program
   reached `halt` having touched the device 20 times, so `audio_tone` returned
   9600 frames, `[app_call cap=audio.tone ... -> ok]` was printed, and the model
   told the operator it had heard a 440 Hz note. The WAV captured **zero frames**.
   `tools/audio_tools.c` now says plainly, on every VM-sink play, that this only
   establishes that the program ran and touched the device.
2. **Installing a sink makes the device unrepairable.** Once `driver_install`
   binds a card, `driver_targets` stops offering it (`usable=2` becomes
   `usable=1`), so `driver_run` can no longer probe it. The model hit this exactly:
   *"the sink is bound to a device the driver VM won't let me touch."* A bad play
   program can therefore only be replaced blind.
3. **There is no way to read an installed play program back.** *"I don't have the
   existing program's source."* Correcting a sink means rewriting it from memory.

## `ac97-reentry-fix.*` and `ac97-io-limit.*` — the play budget, and the run that found it

Two runs from the session that fixed the surviving high-severity defects. Same
prompt both times, same card, one kernel change between them.

`ac97-io-limit.log` is the failure and it is the more useful file. The model found
the card, brought it up, reasoned a descriptor list out of the play contract and
installed a play program — and the first `audio_tone` died with **`IO_LIMIT`**. It
diagnosed itself correctly in its own last message: its status-poll loop spent the
device-access budget before the DMA reported done. That was a KERNEL gap, not a
model error. The contract says "poll until the device has finished reading that
memory"; the budget the sink inherited from `driver_run` was `DRV_MAX_IO` = 1024
accesses, sized for bring-up. One poll per millisecond of an 800 ms sound is 800
accesses before the program does anything else, and a 1 s sound is over the whole
budget — so the documented instruction and the enforced limit disagreed, and a
correct program trapped. `core/audio.c` now raises the access budget in proportion
to the sound the same way it already raised the delay budget, and
`AUDIO_VM_CONTRACT` states both numbers and the poll shape that fits them.

`ac97-reentry-fix.*` is the run after that, and it makes a sound:

```
captured    : 12046 frames = 0.251 s
amplitude   : peak 7998 (audio_tone was asked for 8000)
frequency   : 438.4 Hz by zero crossings, 440.4 Hz by Goertzel
tonality    : 99.5% of energy at 440.4 Hz
per 50 ms   : 440.0 Hz, tonal 100.0%, at every one of the five windows
```

**Read the length, not just the pitch.** It asked for 1000 ms and produced 251 ms.
The frequency, amplitude and purity are exactly right, so the driver is genuinely
driving the codec; the duration is wrong for the reason `ac97-fullloop` showed and
this run reproduces at a different rate: one descriptor, a 16-bit length field.
44100 frames of stereo is 88200 samples, and `88200 & 0xFFFF` is 22664 samples =
0.257 s. The screenshot is worth reading for the model's own account — it argues
from the fact that the program reached its normal `halt` rather than its `abort`
that the DMA-halted status bit really flipped, which is correct reasoning about the
wrong amount of data. Splitting the buffer across descriptors is the fix, and the
kernel cannot make it: the length field's width is device knowledge.

A third attempt on the same prompt died to three consecutive upstream `529
Overloaded` responses with the retry budget exhausted. Worth knowing before
reading a silent WAV as a kernel fault: an unauthenticated POST to
`api.anthropic.com/v1/messages` returning 401 proves the service is up and the
problem is capacity, and it needs no key.

## The earlier single-card runs

Made before `driver_install` existed, so they stop at `driver_run`: the model
drove the hardware directly and never became the machine's sink.

| file | card | outcome |
|---|---|---|
| `ac97.*` | AC97 | silent — an early run that never started the engine |
| `ac97b.*` | AC97 | three 444 Hz triangle notes, 81.6% tonal |
| `ac97-final.*` | AC97 | 1000.0 Hz triangle, 98.5% tonal, peak 8000 |
| `hda.*` | intel-hda | silent |
| `hda2.*` | intel-hda | 445.4 Hz square, 81.6% tonal, ran 51 s |
| `adlib.*` | adlib | silent, and necessarily so — an ISA card has no PCI config space, so it can never be a driver target (`tests/qemu/cases/audio-isa-invisible.case`) |

The measured frequencies are worth more than the logs, because each one is a
fingerprint of the model's own integer arithmetic rather than of anything the
kernel would produce. `ac97-final` asked for a 48-sample triangle period at
48 kHz and 48000/48 is exactly the 1000.0 Hz measured; `hda2` used a 54-sample
half period and 48000/108 = 444.4 Hz against 445.4 measured. The kernel's
synthesiser plays sines at the requested pitch, so neither number could have
come from it.

## Reproducing

    tests/qemu/live_audio.py --card ac97 --out /tmp/run \
        --turn "there's a sound card in this machine with no driver..."
    tests/qemu/wavcheck.py /tmp/run.wav

It costs the operator real money, and `live_audio.py` never reads `.env` —
`make run-nox` does that, and passes the key to the guest over fw_cfg.
