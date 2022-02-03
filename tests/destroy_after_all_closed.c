#include "fs/operations.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Multiple threads opening multiple files, all of which are closed before the
 * file system is destroyed.
 */

#define NUM_THREADS 20

typedef struct {
    long wait;
    int fd;
} thread_params_t;

void *thread_func(void *params_v) {
    thread_params_t *params = (thread_params_t *)params_v;

    struct timespec tim;
    tim.tv_sec = 0;
    tim.tv_nsec = params->wait;
    nanosleep(&tim, NULL);

    assert(tfs_close(params->fd) != -1);
    return NULL;
}

int main() {
    assert(tfs_init() != -1);

    thread_params_t params[NUM_THREADS];
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        params[i].wait = rand() % 100;
        char path[3] = {'/', '0' + (char)i, '\0'};
        params[i].fd = tfs_open(path, TFS_O_CREAT);
        assert(params[i].fd != -1);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        assert(pthread_create(&threads[i], NULL, thread_func, &params[i]) == 0);
    }

    assert(tfs_destroy_after_all_closed() != -1);

    for (int i = 0; i < NUM_THREADS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }

    printf("Successful test.\n");

    return 0;
}
