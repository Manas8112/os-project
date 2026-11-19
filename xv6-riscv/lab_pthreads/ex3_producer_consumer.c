/* ex3_producer_consumer.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define BUFFER_SIZE 5
#define NUM_ITEMS 20

int buffer[BUFFER_SIZE];
int count = 0, in = 0, out = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

static void check(int rc, const char *what) {
    if (rc != 0) {
        fprintf(stderr, "%s failed: %s\n", what, strerror(rc));
        exit(1);
    }
}

void *producer(void *arg) {
    (void) arg;
    for (int item = 1; item <= NUM_ITEMS; item++) {
        check(pthread_mutex_lock(&lock), "pthread_mutex_lock");
        while (count == BUFFER_SIZE)
            check(pthread_cond_wait(&not_full, &lock), "pthread_cond_wait");
        buffer[in] = item;
        in = (in + 1) % BUFFER_SIZE;
        count++;
        printf("Produced: %d (count=%d)\n", item, count);
        check(pthread_cond_signal(&not_empty), "pthread_cond_signal");
        check(pthread_mutex_unlock(&lock), "pthread_mutex_unlock");
    }
    return NULL;
}

void *consumer(void *arg) {
    (void) arg;
    for (int i = 0; i < NUM_ITEMS; i++) {
        check(pthread_mutex_lock(&lock), "pthread_mutex_lock");
        while (count == 0)
            check(pthread_cond_wait(&not_empty, &lock), "pthread_cond_wait");
        int item = buffer[out];
        out = (out + 1) % BUFFER_SIZE;
        count--;
        printf("Consumed: %d (count=%d)\n", item, count);
        check(pthread_cond_signal(&not_full), "pthread_cond_signal");
        check(pthread_mutex_unlock(&lock), "pthread_mutex_unlock");
    }
    return NULL;
}

int main(void) {
    pthread_t prod, cons;

    check(pthread_create(&prod, NULL, producer, NULL), "pthread_create(producer)");
    check(pthread_create(&cons, NULL, consumer, NULL), "pthread_create(consumer)");
    check(pthread_join(prod, NULL), "pthread_join(producer)");
    check(pthread_join(cons, NULL), "pthread_join(consumer)");

    check(pthread_mutex_destroy(&lock), "pthread_mutex_destroy");
    check(pthread_cond_destroy(&not_full), "pthread_cond_destroy(not_full)");
    check(pthread_cond_destroy(&not_empty), "pthread_cond_destroy(not_empty)");
    return 0;
}
