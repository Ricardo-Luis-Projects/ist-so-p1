#include "fs/operations.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Multiple threads write to the their own file a large amount of data, then
 * read it back, and then truncate it to 0 bytes and start over.
 */

#define NUM_THREADS 20
#define NUM_OF_LOOPS 100
#define NUM_OF_WRITES_PER_LOOP 30
#define WRITE_SIZE (BLOCK_SIZE + 1)

typedef struct {
    long wait;
    char id;
} thread_params_t;

void *thread_func(void *params_v) {
    thread_params_t *params = (thread_params_t *)params_v;
    char path[3] = {'/', params->id, '\0'};
    char buf[WRITE_SIZE];
    memset(buf, params->id, sizeof(buf));

    struct timespec tim;
    tim.tv_sec = 0;
    tim.tv_nsec = params->wait;
    nanosleep(&tim, NULL);

    for (int i = 0; i < NUM_OF_LOOPS; i++) {
        int fd = tfs_open(path, TFS_O_CREAT | TFS_O_TRUNC);
        assert(fd != -1);

        for (int j = 0; j < NUM_OF_WRITES_PER_LOOP; j++) {
            assert(tfs_write(fd, buf, WRITE_SIZE) == WRITE_SIZE);
        }

        assert(tfs_close(fd) != -1);

        fd = tfs_open(path, 0);
        assert(fd != -1);

        for (int j = 0; j < NUM_OF_WRITES_PER_LOOP; j++) {
            assert(tfs_read(fd, buf, WRITE_SIZE) == WRITE_SIZE);
            for (int k = 0; k < WRITE_SIZE; k++) {
                assert(buf[k] == params->id);
            }
        }

        assert(tfs_close(fd) != -1);
    }

    return NULL;
}

int main() {
    assert(tfs_init() != -1);

    thread_params_t params[NUM_THREADS];
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        params[i].wait = rand() % 1000;
        params[i].id = '0' + (char)i;
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        assert(pthread_create(&threads[i], NULL, thread_func, &params[i]) == 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }

    assert(tfs_destroy() != -1);

    printf("Successful test.\n");
    return 0;
}
