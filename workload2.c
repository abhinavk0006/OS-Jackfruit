/*
 * workload2.c -- I/O-bound workload
 * Repeatedly writes and reads a 1 MB temporary file.
 * Spends most time blocked on I/O syscalls.
 *
 * Build: gcc -Wall -O0 -o workload2 workload2.c
 * Run:   ./workload2 [duration_seconds]   (default: 30)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE  (64 * 1024)    /* 64 KB per fwrite */
#define FILE_PATH "/tmp/io_workload_tmp"

int main(int argc, char *argv[])
{
    int duration = (argc > 1) ? atoi(argv[1]) : 30;
    time_t end   = time(NULL) + duration;
    long iter    = 0;

    char *buf = malloc(BUF_SIZE);
    if (!buf) { perror("malloc"); return 1; }
    memset(buf, 'B', BUF_SIZE);

    printf("[workload2] I/O-bound: PID=%d, writing/reading for %d seconds\n",
           (int)getpid(), duration);
    fflush(stdout);

    while (time(NULL) < end) {
        /* Write 1 MB */
        FILE *f = fopen(FILE_PATH, "wb");
        if (!f) { perror("fopen write"); break; }
        for (int i = 0; i < 16; i++)
            fwrite(buf, 1, BUF_SIZE, f);
        fflush(f);
        fclose(f);

        /* Read 1 MB back */
        f = fopen(FILE_PATH, "rb");
        if (!f) { perror("fopen read"); break; }
        while (fread(buf, 1, BUF_SIZE, f) > 0) {}
        fclose(f);

        iter++;
        if (iter % 5 == 0) {
            printf("[workload2] PID=%d completed %ld iterations (%ld MB total)\n",
                   (int)getpid(), iter, iter);
            fflush(stdout);
        }
    }

    unlink(FILE_PATH);
    free(buf);
    printf("[workload2] PID=%d done. %ld iterations completed.\n",
           (int)getpid(), iter);
    fflush(stdout);
    return 0;
}
