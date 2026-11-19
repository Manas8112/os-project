/* ex2_race_mutex.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define NUM_THREADS 4
#define INCREMENTS 100000

long counter = 0;
pthread_mutex_t counter_lock;

static void check(int rc, const char *what) {
    if (rc != 0) {
        fprintf(stderr, "%s failed: %s\n", what, strerror(rc));
        exit(1);
    }
}

void *increment_unsafe(void *arg) {
    (void) arg;
    for (int i = 0; i < INCREMENTS; i++) {
        counter = counter + 1; /* NOT synchronized: race condition */
    }
    return NULL;
}

void *increment_safe(void *arg) {
    (void) arg;
    for (int i = 0; i < INCREMENTS; i++) {
        check(pthread_mutex_lock(&counter_lock), "pthread_mutex_lock");
        counter = counter + 1;
        check(pthread_mutex_unlock(&counter_lock), "pthread_mutex_unlock");
    }
    return NULL;
}

static double ms_between(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

/* Run NUM_THREADS threads executing fn; return elapsed wall-clock ms. */
static double run(void *(*fn)(void *)) {
    pthread_t threads[NUM_THREADS];
    struct timespec t0, t1;

    counter = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < NUM_THREADS; i++)
        check(pthread_create(&threads[i], NULL, fn, NULL), "pthread_create");
    for (int i = 0; i < NUM_THREADS; i++)
        check(pthread_join(threads[i], NULL), "pthread_join");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return ms_between(t0, t1);
}

int main(void) {
    const long expected = (long) NUM_THREADS * INCREMENTS;

    check(pthread_mutex_init(&counter_lock, NULL), "pthread_mutex_init");

    double ms = run(increment_unsafe);
    printf("[unsafe] expected = %ld, final = %ld, time = %.3f ms\n",
           expected, counter, ms);

    ms = run(increment_safe);
    printf("[mutex ] expected = %ld, final = %ld, time = %.3f ms\n",
           expected, counter, ms);

    check(pthread_mutex_destroy(&counter_lock), "pthread_mutex_destroy");
    return 0;
}
