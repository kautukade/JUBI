/* broken.c -- deliberately wrong, and the diagnostic is the point.
 *
 * THIS FILE DOES NOT COMPILE. It holds five mistakes a language model actually
 * makes, in the order the compiler reaches them.
 *
 * READ THIS FIRST, because it is the honest part: this compiler STOPS AT THE
 * FIRST ERROR. There is no error recovery and no resync, so compiling this file
 * gives you ONE diagnostic -- the float below -- and not five. That is a
 * deliberate trade, argued in include/cc.h: a parser that carries on after a
 * confusion it did not understand produces a cascade, and for a caller with one
 * turn to fix things, four invented errors are worse than one true one.
 *
 * So this is a five-step repair loop. Delete the mistake the compiler names and
 * it names the next one. All five messages, and the five snippets that produce
 * them on their own, are in README.md and are asserted by tests/host/test_cc.c.
 *
 * What every one of them has in common: a line, a column, the offending source
 * line with a caret under it, and -- the part that matters -- what to write
 * instead.
 */

struct point { int x; int y; };

/* 1. Floating point. Refused at the LEXER, with the whole literal quoted, so
 *    that 0.5 can never be silently truncated to 0. */
static double half = 0.5;

/* 2. A member that does not exist. The diagnostic lists the members there ARE,
 *    which is usually enough to fix it without another turn. */
static int bad_member(struct point *p)
{
    return p->z;
}

/* 3. Mixing pointer types. Real C warns; here it is an error, because there is
 *    no build log for a warning to sit in and the caller is a model. */
static int wrong_pointer(char *text)
{
    int *numbers = text;
    return numbers[0];
}

/* 4. The wrong number of arguments. Both counts are in the message. */
static int scale(int value, int factor)
{
    return value * factor;
}

static int too_few(void)
{
    return scale(10);
}

/* 5. goto. Refused with the REASON, not just "unsupported": every loop
 *    back-edge carries the fuel check that makes a compiled program terminate,
 *    and a jump to a label could step around it. */
long main(void)
{
    int i = 0;
again:
    i++;
    if (i < 10)
        goto again;
    return i + too_few();
}
