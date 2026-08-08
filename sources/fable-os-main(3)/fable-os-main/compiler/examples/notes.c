/* notes.c -- compiled code that produces the compiler's next input.
 *
 * #include "shared.h" reads a file from the compiler's own private directory,
 * which is exactly where a compiled program's file_write() lands. So one program
 * can write a header and the next can compile against it. That is the whole
 * self-extension loop, in two tool calls and no shell.
 *
 * Step 1, from any program (or from write_file on /work/shared.h):
 *     file_write("shared.h",
 *                "#define GREETING \"hello from a header this machine wrote\"\n"
 *                "#define LIMIT 5\n", 74);
 *
 * Step 2: compile THIS. It will fail with a diagnostic naming shared.h if step 1
 * has not happened, which is the correct behaviour and worth seeing once.
 */

#include "shared.h"

long main(void)
{
    long i;

    for (i = 0; i < LIMIT; i++) {
        print_num(i);
        print(": ");
        print(GREETING);
        print("\n");
    }
    return LIMIT;
}
