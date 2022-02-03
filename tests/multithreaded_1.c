#include "fs/operations.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Multiple threads write to the same file descriptor.
 */

#define NUM_THREADS 100
#define WRITE_SIZE_PER_THREAD 200

typedef struct {
    long wait;
    int fd;
    char id;
} thread_params_t;

void *thread_func(void *params_v) {
    thread_params_t *params = (thread_params_t *)params_v;
    char buf[WRITE_SIZE_PER_THREAD];
    memset(buf, params->id, sizeof(buf));

    struct timespec tim;
    tim.tv_sec = 0;
    tim.tv_nsec = params->wait;
    nanosleep(&tim, NULL);

    assert(tfs_write(params->fd, buf, sizeof(buf)) == sizeof(buf));
    return NULL;
}

int main() {
    char *path = "/f1";

    assert(tfs_init() != -1);

    int fd = tfs_open(path, TFS_O_CREAT);
    assert(fd != -1);

    thread_params_t params[NUM_THREADS];
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        params[i].wait = rand() % 100;
        params[i].fd = fd;
        params[i].id = '0' + (char)i;
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        assert(pthread_create(&threads[i], NULL, thread_func, &params[i]) == 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }

    assert(tfs_close(fd) == 0);

    fd = tfs_open(path, 0);
    assert(fd != -1);

    char buf[NUM_THREADS * WRITE_SIZE_PER_THREAD];
    assert(tfs_read(fd, buf, sizeof(buf)) == sizeof(buf));

    for (int i = 0; i < NUM_THREADS; i++) {
        char c = buf[i * WRITE_SIZE_PER_THREAD];
        for (int j = 0; j < WRITE_SIZE_PER_THREAD; j++) {
            assert(buf[i * WRITE_SIZE_PER_THREAD + j] == c);
        }
    }

    assert(tfs_destroy() != -1);

    printf("Successful test.\n");
    return 0;
}
