/*
 * memory_hog.c -- Memory-consuming workload
 * Gradually allocates and touches memory until killed by the kernel monitor.
 * Used to demonstrate soft and hard memory limit enforcement.
 *
 * Build: gcc -Wall -O0 -o memory_hog memory_hog.c
 * Run:   ./memory_hog [mb_per_step] [sleep_ms]   (defaults: 10 MB, 500 ms)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int mb_step  = (argc > 1) ? atoi(argv[1]) : 10;
    int sleep_ms = (argc > 2) ? atoi(argv[2]) : 500;
    long total   = 0;

    printf("[memory_hog] allocating %d MB every %d ms. PID=%d\n",
           mb_step, sleep_ms, (int)getpid());
    fflush(stdout);

    while (1) {
        void *p = malloc((size_t)mb_step * 1024 * 1024);
        if (!p) {
            fprintf(stderr, "[memory_hog] malloc failed at %ld MB total\n", total);
            break;
        }
        /* Touch every page so RSS actually increases */
        memset(p, 0xAB, (size_t)mb_step * 1024 * 1024);
        total += mb_step;
        printf("[memory_hog] total allocated = %ld MB\n", total);
        fflush(stdout);
        usleep((unsigned int)sleep_ms * 1000);
    }

    /* If we reach here the kernel didn't kill us yet */
    printf("[memory_hog] exiting normally (expected: killed by monitor)\n");
    return 0;
}
