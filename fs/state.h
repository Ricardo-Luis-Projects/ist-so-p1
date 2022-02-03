#ifndef STATE_H
#define STATE_H

#include "config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

/*
 * Directory entry
 */
typedef struct {
    char d_name[MAX_FILE_NAME];
    int d_inumber;
} dir_entry_t;

typedef enum { T_FILE, T_DIRECTORY } inode_type;

/*
 * I-node
 */
typedef struct {
    inode_type i_node_type;
    size_t i_size;
    size_t i_data_block_count;
    int i_data_block[INODE_DIRECT_REFS];
    int i_data_extension_block;
    /* in a real FS, more fields would exist here */
} inode_t;

typedef enum { FREE = 0, TAKEN = 1 } allocation_state_t;

/*
 * Open file entry (in open file table)
 */
typedef struct {
    int of_inumber;
    int of_append;
    size_t of_offset;
    pthread_mutex_t of_mutex;
} open_file_entry_t;

#define MAX_DIR_ENTRIES (BLOCK_SIZE / sizeof(dir_entry_t))
#define MAX_INDIRECT_REFS (BLOCK_SIZE / sizeof(int))
#define MAX_FILE_SIZE (BLOCK_SIZE * (INODE_DIRECT_REFS + MAX_INDIRECT_REFS))

int state_init();
int state_destroy();
int state_destroy_after_all_closed();

int inode_create(inode_type n_type);
int inode_delete(int inumber);
int inode_clear(int inumber);

int find_in_dir(int inumber, char const *sub_name);
int create_in_dir(int inumber, inode_type type, char const *sub_name);

int add_to_open_file_table(int inumber, int append);
int remove_from_open_file_table(int fhandle);
ssize_t write_to_open_file(int fhandle, void const *buffer, size_t to_write);
ssize_t read_from_open_file(int fhandle, void *buffer, size_t to_read);

#endif // STATE_H
