/* square.c -- the smallest useful saved program.
 *
 * Save it:  cc_compile {"source": <this file>, "name":"square",
 *                       "doc":"square(n): n*n, and says so on the console",
 *                       "params":["n"], "args":[7]}
 * Call it:  cc_call    {"name":"square","args":[9]}
 *
 * Two things to notice, because they are the whole calling convention:
 *   - the entry point is main(), and its parameters ARE the program's
 *     parameters, in order, as whole numbers;
 *   - what main() returns is what the caller gets. print() is for the operator.
 */

long main(long n)
{
    long result = n * n;

    print("square(");
    print_num(n);
    print(") = ");
    print_num(result);
    print("\n");

    return result;
}
