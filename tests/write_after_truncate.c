#include "fs/operations.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Open the file for writing and write a byte.
 * Open the file and truncate.
 * Write one more byte on the old FD: should return -1.
 * Close both FDs.
 */

#define NUM_BYTES_TO_WRITE 10

int main() {
    assert(tfs_init() != -1);

    char c = 'a';

    int w_fd = tfs_open("/file", TFS_O_CREAT);
    assert(w_fd != -1);
    assert(tfs_write(w_fd, &c, sizeof(char)) == sizeof(char));

    int t_fd = tfs_open("/file", TFS_O_TRUNC);
    assert(t_fd != -1);

    assert(tfs_read(w_fd, &c, sizeof(char)) == -1);

    assert(tfs_close(w_fd) == 0);
    assert(tfs_close(t_fd) == 0);

    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}
