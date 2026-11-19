/* ex1_thread_create.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#define DEFAULT_THREADS 5

typedef struct {
    int serial;
} thread_arg_t;

void *thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *) arg;
    printf("Thread %d (tid = %lu) says hello\n", targ->serial,
           (unsigned long) pthread_self());
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    int n = (argc > 1) ? atoi(argv[1]) : DEFAULT_THREADS;
    if (n <= 0) {
        fprintf(stderr, "usage: %s [num_threads > 0]\n", argv[0]);
        return 1;
    }

    pthread_t *tids = malloc(n * sizeof(pthread_t));
    thread_arg_t *args = malloc(n * sizeof(thread_arg_t));
    if (!tids || !args) {
        perror("malloc");
        free(tids);
        free(args);
        return 1;
    }

    int created = 0;
    for (int i = 0; i < n; i++) {
        args[i].serial = i;
        int rc = pthread_create(&tids[i], NULL, thread_func, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
            break;  /* join the threads already started, then exit with error */
        }
        created++;
    }

    int status = (created == n) ? 0 : 1;
    for (int i = 0; i < created; i++) {
        int rc = pthread_join(tids[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_join failed: %s\n", strerror(rc));
            status = 1;
        }
    }

    if (status == 0)
        printf("All threads completed\n");

    free(tids);
    free(args);
    return status;
}
