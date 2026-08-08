/* histogram.c -- real C, kernel calls, and a result that outlives the run.
 *
 * Counts how many of the first `n` integers fall in each of eight buckets by
 * their remainder mod 8, prints the histogram, and keeps the counts in a file so
 * a later program (or a later boot) can read them back.
 *
 * What this demonstrates, and each of these is a thing the subset supports:
 *   struct, a struct pointer parameter, arrays, pointer arithmetic, a switch,
 *   nested loops, compound assignment, and six of the twelve kernel calls.
 *
 * Save it:  cc_compile {"source": <this file>, "name":"histogram",
 *                       "doc":"histogram(n): buckets 1..n by n%8, prints them, and keeps the counts in bucket.dat. Returns the widest bucket's count.",
 *                       "params":["n"], "args":[100]}
 */

#define BUCKETS 8
#define DATAFILE "bucket.dat"

struct hist {
    long count[BUCKETS];
    long total;
    long widest;          /* which bucket has the most in it */
};

/* Reset every field. memset is a kernel call; it refuses a length above 64 KiB
 * rather than clamping it, so the return value is not worth checking here but
 * the bound is worth knowing about. */
static void reset(struct hist *h)
{
    memset(h, 0, (long)sizeof(struct hist));
}

static void tally(struct hist *h, long value)
{
    long b = value % BUCKETS;

    h->count[b] += 1;
    h->total += 1;
    if (h->count[b] > h->count[h->widest])
        h->widest = b;
}

/* One bar, drawn with print_char so that the width is computed rather than
 * spelled out. */
static void bar(long width)
{
    long i;
    for (i = 0; i < width; i++)
        print_char('#');
}

static void show(struct hist *h)
{
    long i;

    print("histogram of ");
    print_num(h->total);
    print(" values\n");

    for (i = 0; i < BUCKETS; i++) {
        long scaled;

        print("  ");
        print_num(i);
        print(" | ");

        /* Scale the bar so that the widest bucket is 24 characters. Integer
         * arithmetic only: there is no floating point here, deliberately. */
        scaled = h->count[i] * 24 / h->count[h->widest];
        bar(scaled);

        print(" ");
        print_num(h->count[i]);

        switch (i) {
        case 0:
            print("  (multiples of 8)");
            break;
        case 1:
            print("  (one more than a multiple of 8)");
            break;
        default:
            break;
        }
        print("\n");
    }
}

long main(long n)
{
    struct hist h;
    long i;
    long started;
    long written;
    long read_back[BUCKETS];

    if (n < 1)
        return -1;

    started = time_ms();
    reset(&h);

    for (i = 1; i <= n; i++)
        tally(&h, i);

    show(&h);

    /* Keep the counts. file_write takes a plain NAME, not a path: it lands in
     * this compiler's one private directory, which is not the program store, so
     * a program cannot rewrite the machine's programs. */
    written = file_write(DATAFILE, (char *)h.count, (long)sizeof h.count);

    print("kept ");
    print_num(written);
    print(" bytes in ");
    print(DATAFILE);
    print(", read back: ");

    /* And prove it came back, so the claim is verified rather than asserted. */
    memset(read_back, 0, (long)sizeof read_back);
    if (file_read(DATAFILE, (char *)read_back, (long)sizeof read_back) ==
        (long)sizeof read_back &&
        memcmp(read_back, h.count, (long)sizeof read_back) == 0)
        print("identical");
    else
        print("DIFFERENT - something is wrong");

    print(" (");
    print_num(time_ms() - started);
    print(" ms)\n");

    return h.count[h.widest];
}
