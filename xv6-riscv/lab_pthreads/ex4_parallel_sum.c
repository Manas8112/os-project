/* ex4_parallel_sum.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define ARRAY_SIZE 20000000
#define DEFAULT_THREADS 4

double *array;
double *partial_sums;
int num_threads;

typedef struct {
    int start, end; /* [start, end) chunk boundaries */
    int thread_id;
} chunk_t;

void *sum_chunk(void *arg) {
    chunk_t *c = (chunk_t *) arg;
    double s = 0.0;
    for (int i = c->start; i < c->end; i++)
        s += array[i];
    partial_sums[c->thread_id] = s;
    return NULL;
}

double sequential_sum(void) {
    double s = 0.0;
    for (int i = 0; i < ARRAY_SIZE; i++) s += array[i];
    return s;
}

static double dabs(double x) { return x < 0 ? -x : x; }

static double ms_between(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

int main(int argc, char *argv[]) {
    num_threads = (argc > 1) ? atoi(argv[1]) : DEFAULT_THREADS;
    if (num_threads <= 0) {
        fprintf(stderr, "usage: %s [num_threads > 0]\n", argv[0]);
        return 1;
    }

    array = malloc((size_t) ARRAY_SIZE * sizeof(double));
    partial_sums = malloc(num_threads * sizeof(double));
    pthread_t *tids = malloc(num_threads * sizeof(pthread_t));
    chunk_t *chunks = malloc(num_threads * sizeof(chunk_t));
    if (!array || !partial_sums || !tids || !chunks) {
        perror("malloc");
        return 1;
    }

    srand(42);
    for (int i = 0; i < ARRAY_SIZE; i++) array[i] = (double) (rand() % 100) / 3.0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double seq_total = sequential_sum();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("Sequential total = %.2f, time = %.3f ms\n", seq_total, ms_between(t0, t1));

    int chunk_size = ARRAY_SIZE / num_threads;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    int created = 0;
    for (int i = 0; i < num_threads; i++) {
        chunks[i].start = i * chunk_size;
        chunks[i].end = (i == num_threads - 1) ? ARRAY_SIZE : (i + 1) * chunk_size;
        chunks[i].thread_id = i;
        int rc = pthread_create(&tids[i], NULL, sum_chunk, &chunks[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
            break;
        }
        created++;
    }
    int status = (created == num_threads) ? 0 : 1;
    for (int i = 0; i < created; i++) {
        int rc = pthread_join(tids[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_join failed: %s\n", strerror(rc));
            status = 1;
        }
    }
    if (status != 0) return status;

    double par_total = 0.0;
    for (int i = 0; i < num_threads; i++) par_total += partial_sums[i];
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("Parallel total = %.2f, threads = %d, time = %.3f ms\n", par_total,
           num_threads, ms_between(t0, t1));

    double diff = dabs(seq_total - par_total);
    printf("Difference = %.6g (%s)\n", diff,
           diff <= 1e-9 * dabs(seq_total) ? "totals agree" : "MISMATCH");

    free(array); free(partial_sums); free(tids); free(chunks);
    return 0;
}
