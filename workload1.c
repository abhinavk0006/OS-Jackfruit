/*
 * workload1.c -- CPU-bound workload
 * Computes prime numbers continuously for a fixed duration.
 * Never blocks, always consuming CPU.
 *
 * Build: gcc -Wall -O0 -o workload1 workload1.c
 * Run:   ./workload1 [duration_seconds]   (default: 30)
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int is_prime(long n)
{
    if (n < 2) return 0;
    for (long i = 2; i * i <= n; i++)
        if (n % i == 0) return 0;
    return 1;
}

int main(int argc, char *argv[])
{
    int duration = (argc > 1) ? atoi(argv[1]) : 30;
    time_t start = time(NULL);
    time_t end   = start + duration;
    long count = 0;
    long n     = 2;

    printf("[workload1] CPU-bound: PID=%d, computing primes for %d seconds\n",
           (int)getpid(), duration);
    fflush(stdout);

    while (time(NULL) < end) {
        if (is_prime(n)) count++;
        n++;
        if (n % 500000 == 0) {
            printf("[workload1] PID=%d checked up to %ld, primes so far: %ld\n",
                   (int)getpid(), n, count);
            fflush(stdout);
        }
    }

    printf("[workload1] PID=%d done. Primes found up to %ld: %ld\n",
           (int)getpid(), n, count);
    fflush(stdout);
    return 0;
}
