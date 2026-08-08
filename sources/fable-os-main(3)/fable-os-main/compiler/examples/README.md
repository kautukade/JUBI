# compiler/examples

Four C files that this machine's own compiler accepts or refuses.
`tests/host/test_cc.c` compiles all four **verbatim**, so they cannot drift from
the compiler that reads them.

Every one is pure ASCII, and that is not an accident: the compiler refuses a
source byte outside printable ASCII, tab and newline, naming its offset and its
value. An em dash in a comment is a diagnostic, not a warning — which is why
these files use `--`.

| file | what it is for |
|---|---|
| `square.c` | the smallest useful **saved program**: one parameter in, one number out, a line on the console. This is the shape `cc_compile` saves and `cc_call` runs. |
| `histogram.c` | **calls kernel functions and keeps its result**: `time_ms`, `print`, `print_num`, `print_char`, `memset`, `memcmp`, and `file_write`/`file_read` for a result that outlives the run. Non-trivial C: a struct, a struct-pointer parameter, arrays, pointer casts, a switch, nested loops, static helper functions. |
| `notes.c` | **the self-extension path**: `#include "shared.h"`, a header a compiled program wrote with `file_write`. Compiled code producing the compiler's next input. |
| `broken.c` | **deliberately wrong**, five ways. The diagnostics are the point. |

## Running one

There is no shell; ask in English. The tool call underneath is:

```json
{"source":"<the file's text>","args":[7],
 "name":"square","doc":"square(n): n*n","params":["n"]}
```

`histogram.c` run with `{"args":[100]}` produces 2856 bytes of machine code,
spends 766 units of fuel, and prints:

```
histogram of 100 values
  0 | ###################### 12  (multiples of 8)
  1 | ######################## 13  (one more than a multiple of 8)
  2 | ######################## 13
  3 | ######################## 13
  4 | ######################## 13
  5 | ###################### 12
  6 | ###################### 12
  7 | ###################### 12
kept 64 bytes in bucket.dat, read back: identical (0 ms)
```

`notes.c` needs its header first. Write it once — from any compiled program, or
with `write_file` to `/work/shared.h` (`/disk/work/shared.h` with a disk
attached):

```c
file_write("shared.h",
           "#define GREETING \"hello from a header this machine wrote\"\n"
           "#define LIMIT 5\n", 74);
```

Without it, the diagnostic says exactly what is missing and where `#include`
looks:

```
notes.c:17:10: "shared.h" is not in the include root; #include here reaches
               built-in headers and files saved there, nothing else
    #include "shared.h"
             ^
```

## broken.c, and the five diagnostics

**This compiler stops at the first error.** There is no error recovery and no
resync, so compiling `broken.c` gives you the *first* of the five messages below,
not all five. That is a deliberate trade (argued in `include/cc.h`): a parser that
carries on past a confusion it did not understand invents errors, and for a caller
with one turn to fix things, four invented errors are worse than one true one.

So `broken.c` is a five-step repair loop: delete the mistake it names, compile
again, and it names the next. Each message below is what you get from that
mistake **on its own**, and `tests/host/test_cc.c` asserts every one of them —
message, line, column and caret.

```
1  broken.c:2:22: floating point is not supported: '0.5'. This kernel is built
                  -mno-sse and has no FPU state; use integers, or scale by a
                  power of ten
       static double half = 0.5;
                            ^

2  broken.c:4:15: 'struct point' has no member 'z'. It has: x, y
           return p->z;
                     ^

3  broken.c:3:20: this initialiser: cannot use 'char *' where 'int *' is needed
                  - these point at different types; add a cast if that is
                  deliberate
           int *numbers = text;
                          ^

4  broken.c:7:12: scale takes 2 argument(s) but 1 was given
           return scale(10);
                  ^

5  broken.c:4:1:  labels are not supported (nor is goto): a compiled program's
                  loops must all carry a fuel check
       again:
       ^
```

Why each is fixable in one turn, which is the only test that matters for a
message a model reads:

1. names the construct, quotes the whole literal so it cannot be confused with a
   nearby integer, and says what to do instead (scale by a power of ten).
2. lists the members that exist. `z` was a guess; `x, y` is the answer.
3. names **both** types in the direction of the assignment, and says that a cast
   is the way to mean it on purpose. Note that C would merely warn here.
4. gives both counts and the function's name. Nothing to infer.
5. gives the reason rather than "unsupported", so the model does not try `goto`
   again in a different shape — and points at `while`, `for`, `break`,
   `continue`.
