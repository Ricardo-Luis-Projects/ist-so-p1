#include "operations.h"
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tfs_init() {
    if (state_init() == -1) {
        return -1;
    }

    /* create root inode */
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() { return state_destroy(); }
int tfs_destroy_after_all_closed() { return state_destroy_after_all_closed(); }

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}

int tfs_lookup(char const *name) {
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;

    return find_in_dir(ROOT_DIR_INUM, name);
}

int tfs_create(char const *name, inode_type type) {
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;

    return create_in_dir(ROOT_DIR_INUM, type, name);
}

int tfs_open(char const *name, int flags) {
    int inum;

    /* Checks if the path name is valid */
    if (!valid_pathname(name)) {
        return -1;
    }

    inum = flags & TFS_O_CREAT ? tfs_create(name, T_FILE) : tfs_lookup(name);
    if (inum == -1) {
        return -1;
    }

    /* Truncate (if requested) */
    if (flags & TFS_O_TRUNC) {
        if (inode_clear(inum) == -1) {
            return -1;
        }
    }

    /* Add entry to the open file table and return the file descriptor */
    return add_to_open_file_table(inum, flags & TFS_O_APPEND);

    /* Note: for simplification, if file was created with TFS_O_CREAT and there
     * is an error adding an entry to the open file table, the file is not
     * opened but it remains created */
}

int tfs_close(int fhandle) { return remove_from_open_file_table(fhandle); }

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    return write_to_open_file(fhandle, buffer, to_write);
}

ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    return read_from_open_file(fhandle, buffer, len);
}

int tfs_copy_to_external_fs(char const *source_path, char const *dest_path) {
    /* Open the source file */
    int fd = tfs_open(source_path, 0);
    if (fd == -1) {
        return -1;
    }

    FILE *dst = fopen(dest_path, "w");
    if (dst == NULL) {
        tfs_close(fd);
        return -1;
    }

    /* Read blocks until EOF */
    char buf[BLOCK_SIZE];
    ssize_t read;
    while ((read = tfs_read(fd, buf, BLOCK_SIZE)) > 0) {
        /* Write the block to the external file */
        if (fwrite(buf, 1, (size_t)read, dst) != read) {
            tfs_close(fd);
            fclose(dst);
            return -1;
        }
    }

    if (fclose(dst)) {
        tfs_close(fd);
        return -1;
    }

    if (read == -1) {
        tfs_close(fd);
        return -1;
    }

    return tfs_close(fd);
}
