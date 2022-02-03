#include "fs/operations.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Write N bytes to a file, and close it.
 * Open the file for reading and read 1 byte.
 * Open the file and truncate, and close it.
 * Read N - 1 bytes on the old FD: should return -1.
 */

#define NUM_BYTES_TO_WRITE 10

int main() {
    assert(tfs_init() != -1);

    char buf[NUM_BYTES_TO_WRITE];
    for (int i = 0; i < NUM_BYTES_TO_WRITE; i++) {
        buf[i] = 'a' + (char)i;
    }

    int w_fd = tfs_open("/file", TFS_O_CREAT);
    assert(w_fd != -1);
    assert(tfs_write(w_fd, buf, sizeof(buf)) == sizeof(buf));
    assert(tfs_close(w_fd) == 0);

    int r_fd = tfs_open("/file", 0);
    assert(tfs_read(r_fd, buf, 1) == 1);
    assert(r_fd != -1);

    w_fd = tfs_open("/file", TFS_O_TRUNC);
    assert(w_fd != -1);
    assert(tfs_close(w_fd) == 0);

    assert(tfs_read(r_fd, buf, sizeof(buf) - 1) == -1);
    assert(tfs_close(r_fd) == 0);

    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}
