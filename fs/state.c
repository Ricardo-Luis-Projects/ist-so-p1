#include "state.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Persistent FS state  (in reality, it should be maintained in secondary
 * memory; for simplicity, this project maintains it in primary memory) */

/* I-node table */
static inode_t inode_table[INODE_TABLE_SIZE];
static char free_inode_ts[INODE_TABLE_SIZE];

/* Data blocks */
static char fs_data[BLOCK_SIZE * DATA_BLOCKS];
static char free_blocks[DATA_BLOCKS];

/* Volatile FS state */

/* Open file table */
static open_file_entry_t open_file_table[MAX_OPEN_FILES];
static char free_open_file_entries[MAX_OPEN_FILES];
static int open_file_count;

/* Mutexes and rwlocks */
static pthread_rwlock_t inode_lock_table[INODE_TABLE_SIZE];
static pthread_mutex_t inode_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t data_blocks_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t open_file_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t open_file_table_cond = PTHREAD_COND_INITIALIZER;

static inline bool valid_inumber(int inumber) {
    return inumber >= 0 && inumber < INODE_TABLE_SIZE;
}

static inline bool valid_block_number(int block_number) {
    return block_number >= 0 && block_number < DATA_BLOCKS;
}

static inline bool valid_file_handle(int file_handle) {
    return file_handle >= 0 && file_handle < MAX_OPEN_FILES;
}

/**
 * We need to defeat the optimizer for the insert_delay() function.
 * Under optimization, the empty loop would be completely optimized away.
 * This function tells the compiler that the assembly code being run (which is
 * none) might potentially change *all memory in the process*.
 *
 * This prevents the optimizer from optimizing this code away, because it does
 * not know what it does and it may have side effects.
 *
 * Reference with more information: https://youtu.be/nXaxk27zwlk?t=2775
 *
 * Exercise: try removing this function and look at the assembly generated to
 * compare.
 */
static void touch_all_memory() { __asm volatile("" : : : "memory"); }

/*
 * Auxiliary function to insert a delay.
 * Used in accesses to persistent FS state as a way of emulating access
 * latencies as if such data structures were really stored in secondary memory.
 */
static void insert_delay() {
    for (int i = 0; i < DELAY; i++) {
        touch_all_memory();
    }
}

/*
 * Initializes FS state
 */
int state_init() {
    for (size_t i = 0; i < INODE_TABLE_SIZE; i++) {
        free_inode_ts[i] = FREE;
        if (pthread_rwlock_init(&inode_lock_table[i], NULL)) {
            return -1;
        }
    }

    for (size_t i = 0; i < DATA_BLOCKS; i++) {
        free_blocks[i] = FREE;
    }

    for (size_t i = 0; i < MAX_OPEN_FILES; i++) {
        free_open_file_entries[i] = FREE;
        if (pthread_mutex_init(&open_file_table[i].of_mutex, NULL)) {
            return -1;
        }
    }

    open_file_count = 0;

    return 0;
}

int state_destroy() { /* nothing to do */
    for (size_t i = 0; i < INODE_TABLE_SIZE; i++) {
        if (pthread_rwlock_destroy(&inode_lock_table[i])) {
            return -1;
        }
    }

    for (size_t i = 0; i < MAX_OPEN_FILES; i++) {
        if (pthread_mutex_destroy(&open_file_table[i].of_mutex)) {
            return -1;
        }
    }

    return 0;
}

int state_destroy_after_all_closed() {
    if (pthread_mutex_lock(&open_file_table_mutex)) {
        return -1;
    }

    if (pthread_cond_wait(&open_file_table_cond, &open_file_table_mutex)) {
        pthread_mutex_unlock(&open_file_table_mutex);
        return -1;
    }

    if (pthread_mutex_unlock(&open_file_table_mutex)) {
        return -1;
    }

    state_destroy();
    return 0;
}

/*
 * Allocated a new data block
 * Returns: block index if successful, -1 otherwise
 */
static int data_block_alloc() {
    if (pthread_mutex_lock(&data_blocks_mutex)) {
        return -1;
    }

    for (int i = 0; i < DATA_BLOCKS; i++) {
        if (i * (int)sizeof(allocation_state_t) % BLOCK_SIZE == 0) {
            insert_delay(); // simulate storage access delay to free_blocks
        }

        if (free_blocks[i] == FREE) {
            free_blocks[i] = TAKEN;
            if (pthread_mutex_unlock(&data_blocks_mutex)) {
                return -1;
            }
            return i;
        }
    }

    pthread_mutex_unlock(&data_blocks_mutex);
    return -1;
}

/* Frees a data block
 * Input
 * 	- the block index
 * Returns: 0 if success, -1 otherwise
 */
static int data_block_free(int block_number) {
    if (!valid_block_number(block_number)) {
        return -1;
    }

    insert_delay(); // simulate storage access delay to free_blocks

    if (pthread_mutex_lock(&data_blocks_mutex)) {
        return -1;
    }

    free_blocks[block_number] = FREE;

    if (pthread_mutex_unlock(&data_blocks_mutex)) {
        return -1;
    }

    return 0;
}

/* Returns a pointer to the contents of a given block
 * Input:
 * 	- Block's index
 * Returns: pointer to the first byte of the block, NULL otherwise
 */
static void *data_block_get(int block_number) {
    if (!valid_block_number(block_number)) {
        return NULL;
    }

    insert_delay(); // simulate storage access delay to block
    return &fs_data[block_number * BLOCK_SIZE];
}

/*
 * Extends the i-node's data blocks by adding a new data block unsafely.
 * Input:
 * - inumber: i-node's number
 *  Returns: the block number if successful, -1 if failed
 */
static int inode_extend_unsafe(int inumber) {
    if (!valid_inumber(inumber) || free_inode_ts[inumber] == FREE) {
        return -1;
    }

    /* Allocates a new data block */
    int b = data_block_alloc();
    if (b == -1) {
        return -1;
    }

    if (inode_table[inumber].i_data_block_count < INODE_DIRECT_REFS) {
        /* Add a direct reference to the block */
        size_t bc = inode_table[inumber].i_data_block_count;
        inode_table[inumber].i_data_block[bc] = b;
    } else if (inode_table[inumber].i_data_block_count <
               INODE_DIRECT_REFS + MAX_INDIRECT_REFS) {
        /* Create the indirect reference block */
        if (inode_table[inumber].i_data_block_count == INODE_DIRECT_REFS) {
            inode_table[inumber].i_data_extension_block = data_block_alloc();
            if (inode_table[inumber].i_data_extension_block == -1) {
                data_block_free(b);
                return -1;
            }
        }

        /* Add a indirect reference to the block */
        size_t bc = inode_table[inumber].i_data_block_count - INODE_DIRECT_REFS;
        int *indirect_refs =
            (int *)data_block_get(inode_table[inumber].i_data_extension_block);
        if (indirect_refs == NULL) {
            data_block_free(b);
            return -1;
        }

        indirect_refs[bc] = b;
    } else {
        data_block_free(b);
        return -1;
    }

    inode_table[inumber].i_data_block_count += 1;
    return b;
}

/*
 * Creates a new i-node in the i-node table unsafely.
 * Input:
 *  - n_type: the type of the node (file or directory)
 * Returns:
 *  new i-node's number if successfully created, -1 otherwise
 */
static int inode_create_unsafe(inode_type n_type) {
    for (int inumber = 0; inumber < INODE_TABLE_SIZE; inumber++) {
        if ((inumber * (int)sizeof(allocation_state_t) % BLOCK_SIZE) == 0) {
            insert_delay(); // simulate storage access delay (to freeinode_ts)
        }

        /* Finds first free entry in i-node table */
        if (free_inode_ts[inumber] == FREE) {
            /* Found a free entry, so takes it for the new i-node*/
            free_inode_ts[inumber] = TAKEN;
            insert_delay(); // simulate storage access delay (to i-node)
            inode_table[inumber].i_node_type = n_type;
            inode_table[inumber].i_size = 0;
            inode_table[inumber].i_data_block_count = 0;

            if (n_type == T_DIRECTORY) {
                /* Initializes directory (filling its first block with empty
                 * entries, labeled with inumber==-1) */
                int b = inode_extend_unsafe(inumber);
                if (b == -1) {
                    free_inode_ts[inumber] = FREE;
                    return -1;
                }

                dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(b);
                if (dir_entry == NULL) {
                    free_inode_ts[inumber] = FREE;
                    return -1;
                }

                for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
                    dir_entry[i].d_inumber = -1;
                }
            }

            return inumber;
        }
    }

    return -1;
}

/*
 * Frees all data blocks of an i-node unsafely.
 * Input:
 * - inumber: i-node's number
 * Returns: 0 if successful, -1 if failed
 */
static int inode_clear_unsafe(int inumber) {
    if (free_inode_ts[inumber] == FREE) {
        return -1;
    }

    /* Free direct data blocks */
    size_t i = 0;
    while (i < inode_table[inumber].i_data_block_count &&
           i < INODE_DIRECT_REFS) {
        if (data_block_free(inode_table[inumber].i_data_block[i++]) == -1) {
            return -1;
        }
    }

    if (i < inode_table[inumber].i_data_block_count) {
        /* Get indirect block */
        int indirect_refs_block = inode_table[inumber].i_data_extension_block;
        int *indirect_refs = (int *)data_block_get(indirect_refs_block);
        if (indirect_refs == NULL) {
            return -1;
        }

        /* Free all indirect references */
        while (i < inode_table[inumber].i_data_block_count) {
            if (i >= MAX_INDIRECT_REFS + INODE_DIRECT_REFS) {
                return -1;
            }

            if (data_block_free(indirect_refs[i++ - INODE_DIRECT_REFS]) == -1) {
                return -1;
            }
        }

        if (data_block_free(indirect_refs_block) == -1) {
            return -1;
        }
    }

    inode_table[inumber].i_size = 0;
    inode_table[inumber].i_data_block_count = 0;

    return 0;
}

/*
 * Creates a new i-node in the i-node table.
 * Input:
 *  - n_type: the type of the node (file or directory)
 * Returns:
 *  new i-node's number if successfully created, -1 otherwise
 */
int inode_create(inode_type n_type) {
    if (pthread_mutex_lock(&inode_table_mutex)) {
        return -1;
    }

    int inumber = inode_create_unsafe(n_type);

    if (pthread_mutex_unlock(&inode_table_mutex)) {
        return -1;
    }

    return inumber;
}

/*
 * Frees all data blocks of an i-node.
 * Input:
 * - inumber: i-node's number
 * Returns: 0 if successful, -1 if failed
 */
int inode_clear(int inumber) {
    if (!valid_inumber(inumber)) {
        return -1;
    }

    if (pthread_rwlock_wrlock(&inode_lock_table[inumber])) {
        return -1;
    }

    int result = inode_clear_unsafe(inumber);

    if (pthread_rwlock_unlock(&inode_lock_table[inumber])) {
        return -1;
    }

    return result;
}

/*
 * Deletes the i-node.
 * Input:
 *  - inumber: i-node's number
 * Returns: 0 if successful, -1 if failed
 */
int inode_delete(int inumber) {
    // simulate storage access delay (to i-node and freeinode_ts)
    insert_delay();
    insert_delay();

    if (pthread_mutex_lock(&inode_table_mutex)) {
        return -1;
    }

    if (pthread_rwlock_wrlock(&inode_lock_table[inumber])) {
        pthread_mutex_unlock(&inode_table_mutex);
        return -1;
    }

    if (!valid_inumber(inumber) || free_inode_ts[inumber] == FREE) {
        pthread_rwlock_unlock(&inode_lock_table[inumber]);
        pthread_mutex_unlock(&inode_table_mutex);
        return -1;
    }

    if (inode_clear_unsafe(inumber) == -1) {
        pthread_rwlock_unlock(&inode_lock_table[inumber]);
        pthread_mutex_unlock(&inode_table_mutex);
        return -1;
    }

    free_inode_ts[inumber] = FREE;

    if (pthread_rwlock_unlock(&inode_lock_table[inumber])) {
        pthread_mutex_unlock(&inode_table_mutex);
        return -1;
    }

    if (pthread_mutex_unlock(&inode_table_mutex)) {
        return -1;
    }

    return 0;
}

/*
 * Returns a pointer to an existing i-node.
 * Input:
 *  - inumber: identifier of the i-node
 * Returns: pointer if successful, NULL if failed
 */
static inode_t *inode_get(int inumber) {
    if (!valid_inumber(inumber)) {
        return NULL;
    }

    insert_delay(); // simulate storage access delay to i-node
    return &inode_table[inumber];
}

/*
 * Returns an i-node's block number from its index unsafely.
 * Input:
 * - inumber: identifier of the i-node
 * - index: index of the block
 * Returns: block number if successful, -1 if failed
 */
static int inode_get_block_unsafe(int inumber, int index) {
    if (!valid_inumber(inumber) || free_inode_ts[inumber] == FREE) {
        return -1;
    }

    if (index < 0 || index >= inode_table[inumber].i_data_block_count) {
        return -1;
    }

    if (index < INODE_DIRECT_REFS) {
        return inode_table[inumber].i_data_block[index];
    } else {
        int *refs =
            (int *)data_block_get(inode_table[inumber].i_data_extension_block);
        if (refs == NULL) {
            return -1;
        }

        return refs[index - INODE_DIRECT_REFS];
    }
}

/*
 * Adds an entry to the i-node directory data.
 * Input:
 *  - inumber: identifier of the i-node
 *  - sub_inumber: identifier of the sub i-node entry
 *  - sub_name: name of the sub i-node entry
 * Returns: SUCCESS or FAIL
 */
static int add_dir_entry_unsafe(int inumber, int sub_inumber,
                                char const *sub_name) {
    if (!valid_inumber(inumber) || !valid_inumber(sub_inumber)) {
        return -1;
    }

    insert_delay(); // simulate storage access delay to i-node with inumber

    if (inode_table[inumber].i_node_type != T_DIRECTORY) {
        return -1;
    }

    if (strlen(sub_name) == 0) {
        return -1;
    }

    /* Locates the block containing the directory's entries */
    dir_entry_t *dir_entry =
        (dir_entry_t *)data_block_get(inode_table[inumber].i_data_block[0]);
    if (dir_entry == NULL) {
        return -1;
    }

    /* Finds and fills the first empty entry */
    for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir_entry[i].d_inumber == -1) {
            dir_entry[i].d_inumber = sub_inumber;
            strncpy(dir_entry[i].d_name, sub_name, MAX_FILE_NAME - 1);
            dir_entry[i].d_name[MAX_FILE_NAME - 1] = 0;
            return 0;
        }
    }

    return -1;
}

/* Looks for a given name inside a directory, unsafely
 * Input:
 * 	- parent directory's i-node number
 * 	- name to search
 * 	Returns i-number linked to the target name, -1 if not found
 */
static int find_in_dir_unsafe(int inumber, char const *sub_name) {
    insert_delay(); // simulate storage access delay to i-node with inumber

    if (inode_table[inumber].i_node_type != T_DIRECTORY) {
        return -1;
    }

    /* Locates the block containing the directory's entries */
    dir_entry_t *dir_entry =
        (dir_entry_t *)data_block_get(inode_table[inumber].i_data_block[0]);
    if (dir_entry == NULL) {
        return -1;
    }

    /* Iterates over the directory entries looking for one that has the target
     * name */
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if ((dir_entry[i].d_inumber != -1) &&
            (strncmp(dir_entry[i].d_name, sub_name, MAX_FILE_NAME) == 0)) {
            return dir_entry[i].d_inumber;
        }
    }

    return -1;
}

/* Looks for a given name inside a directory
 * Input:
 * 	- parent directory's i-node number
 * 	- name to search
 * 	Returns i-number linked to the target name, -1 if not found
 */
int find_in_dir(int inumber, char const *sub_name) {
    if (!valid_inumber(inumber)) {
        return -1;
    }

    if (pthread_rwlock_rdlock(&inode_lock_table[inumber])) {
        return -1;
    }

    int result = find_in_dir_unsafe(inumber, sub_name);
    if (result == -1) {
        pthread_rwlock_unlock(&inode_lock_table[inumber]);
        return -1;
    }

    if (pthread_rwlock_unlock(&inode_lock_table[inumber])) {
        return -1;
    }

    return result;
}

/* Looks for a given name inside a directory, and if not found, creates a new
 * i-node for it.
 * Input:
 * 	- parent directory's i-node number
 *  - i-node type
 * 	- name to search
 * 	Returns i-number linked to the target name, -1 if not found
 */
int create_in_dir(int inumber, inode_type type, char const *sub_name) {
    if (!valid_inumber(inumber)) {
        return -1;
    }

    if (pthread_mutex_lock(&inode_table_mutex)) {
        return -1;
    }

    if (pthread_rwlock_wrlock(&inode_lock_table[inumber])) {
        pthread_mutex_unlock(&inode_table_mutex);
        return -1;
    }

    int sub_inumber = find_in_dir_unsafe(inumber, sub_name);
    if (sub_inumber >= 0) {
        if (pthread_rwlock_unlock(&inode_lock_table[inumber])) {
            pthread_mutex_unlock(&inode_table_mutex);
            return -1;
        }

        if (pthread_mutex_unlock(&inode_table_mutex)) {
            return -1;
        }

        return sub_inumber;
    }

    /* If the target name is not found, creates a new i-node for it */
    sub_inumber = inode_create_unsafe(type);
    if (sub_inumber == -1) {
        pthread_rwlock_unlock(&inode_lock_table[inumber]);
        pthread_mutex_unlock(&inode_table_mutex);
        return -1;
    }

    if (add_dir_entry_unsafe(inumber, sub_inumber, sub_name) == -1) {
        pthread_rwlock_unlock(&inode_lock_table[inumber]);
        pthread_mutex_unlock(&inode_table_mutex);
        return -1;
    }

    if (pthread_rwlock_unlock(&inode_lock_table[inumber])) {
        pthread_mutex_unlock(&inode_table_mutex);
        return -1;
    }

    if (pthread_mutex_unlock(&inode_table_mutex)) {
        return -1;
    }

    return sub_inumber;
}

/* Add new entry to the open file table
 * Inputs:
 * 	- I-node number of the file to open
 * 	- Non-zero if file was opened in append mode
 * Returns: file handle if successful, -1 otherwise
 */
int add_to_open_file_table(int inumber, int append) {
    if (pthread_mutex_lock(&open_file_table_mutex)) {
        return -1;
    }

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (free_open_file_entries[i] == FREE) {
            free_open_file_entries[i] = TAKEN;
            open_file_table[i].of_inumber = inumber;
            open_file_table[i].of_append = append;
            open_file_table[i].of_offset = 0;
            open_file_count += 1;
            if (pthread_mutex_unlock(&open_file_table_mutex)) {
                return -1;
            }
            return i;
        }
    }

    pthread_mutex_unlock(&open_file_table_mutex);
    return -1;
}

/* Frees an entry from the open file table
 * Inputs:
 * 	- file handle to free/close
 * Returns 0 is success, -1 otherwise
 */
int remove_from_open_file_table(int fhandle) {
    if (pthread_mutex_lock(&open_file_table_mutex)) {
        return -1;
    }

    if (!valid_file_handle(fhandle) ||
        free_open_file_entries[fhandle] != TAKEN) {
        pthread_mutex_unlock(&open_file_table_mutex);
        return -1;
    }
    free_open_file_entries[fhandle] = FREE;
    open_file_count -= 1;
    if (open_file_count == 0) {
        if (pthread_cond_signal(&open_file_table_cond)) {
            return -1;
        }
    }

    if (pthread_mutex_unlock(&open_file_table_mutex)) {
        return -1;
    }
    return 0;
}

/* Writes to an open file handle.
 * Inputs:
 *  - file handle to write to
 *  - buffer to write
 *  - number of bytes to be written
 * Returns the number of bytes written, or -1 if the operation failed.
 */
ssize_t write_to_open_file(int fhandle, void const *buffer, size_t to_write) {
    if (!valid_file_handle(fhandle)) {
        return -1;
    }

    open_file_entry_t *file = &open_file_table[fhandle];

    /* Lock the file entry mutex */
    if (pthread_mutex_lock(&file->of_mutex)) {
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    if (inode == NULL) {
        pthread_mutex_unlock(&file->of_mutex);
        return -1;
    }

    /* Lock the inode */
    if (pthread_rwlock_wrlock(&inode_lock_table[file->of_inumber])) {
        pthread_mutex_unlock(&file->of_mutex);
        return -1;
    }

    /* If opened in append mode, set offset to end of file */
    if (file->of_append) {
        file->of_offset = inode->i_size;
    }

    /* Check if offset is out of bounds */
    if (file->of_offset > inode->i_size) {
        pthread_rwlock_unlock(&inode_lock_table[file->of_inumber]);
        pthread_mutex_unlock(&file->of_mutex);
        return -1;
    }

    /* Determine how many bytes to write */
    if (to_write > MAX_FILE_SIZE - file->of_offset) {
        to_write = MAX_FILE_SIZE - file->of_offset;
    }

    /* Write the data for each necessary block */
    for (size_t written = 0; written < to_write;) {
        /* Get block index and offset */
        int bi = (int)(file->of_offset / BLOCK_SIZE);
        size_t offset = file->of_offset % BLOCK_SIZE;

        /* Check if an extra block is necessary */
        if (inode->i_data_block_count == bi) {
            if (inode_extend_unsafe(file->of_inumber) == -1) {
                pthread_rwlock_unlock(&inode_lock_table[file->of_inumber]);
                pthread_mutex_unlock(&file->of_mutex);
                return -1;
            }
        }

        /* Get the block */
        int b = inode_get_block_unsafe(file->of_inumber, bi);
        if (b == -1) {
            pthread_rwlock_unlock(&inode_lock_table[file->of_inumber]);
            pthread_mutex_unlock(&file->of_mutex);
            return -1;
        }

        void *block = data_block_get(b);
        if (block == NULL) {
            pthread_rwlock_unlock(&inode_lock_table[file->of_inumber]);
            pthread_mutex_unlock(&file->of_mutex);
            return -1;
        }

        /* Write the data */
        size_t to_write_in_block = offset + to_write - written < BLOCK_SIZE
                                       ? to_write - written
                                       : BLOCK_SIZE - offset;
        memcpy(block + offset, buffer + written, to_write_in_block);
        file->of_offset += to_write_in_block;
        written += to_write_in_block;
    }

    /* Update the size of the file */
    if (file->of_offset > inode->i_size) {
        inode->i_size = file->of_offset;
    }

    if (pthread_rwlock_unlock(&inode_lock_table[file->of_inumber])) {
        pthread_mutex_unlock(&file->of_mutex);
        return -1;
    }

    if (pthread_mutex_unlock(&file->of_mutex)) {
        return -1;
    }

    return (ssize_t)to_write;
}

/* Reads from an open file handle.
 * Inputs:
 *  - file handle to read from
 *  - buffer to read to
 *  - number of bytes read
 * Returns the number of bytes read, or -1 if the operation failed.
 */
ssize_t read_from_open_file(int fhandle, void *buffer, size_t to_read) {
    if (!valid_file_handle(fhandle)) {
        return -1;
    }

    open_file_entry_t *file = &open_file_table[fhandle];

    /* Lock the file entry mutex */
    if (pthread_mutex_lock(&file->of_mutex)) {
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    if (inode == NULL) {
        return -1;
    }

    /* Lock the inode */
    if (pthread_rwlock_rdlock(&inode_lock_table[file->of_inumber])) {
        pthread_mutex_unlock(&file->of_mutex);
        return -1;
    }

    /* If opened in append mode, set offset to end of file */
    if (file->of_append) {
        file->of_offset = inode->i_size;
    }

    /* Check if offset is out of bounds */
    if (file->of_offset > inode->i_size) {
        pthread_rwlock_unlock(&inode_lock_table[file->of_inumber]);
        pthread_mutex_unlock(&file->of_mutex);
        return -1;
    }

    /* Determine how many bytes to read */
    if (to_read > inode->i_size - file->of_offset) {
        to_read = inode->i_size - file->of_offset;
    }

    /* Read the data from each necessary block */
    for (size_t read = 0; read < to_read;) {
        /* Get block index and offset */
        int bi = (int)(file->of_offset / BLOCK_SIZE);
        size_t offset = file->of_offset % BLOCK_SIZE;

        /* Get the block */
        int b = inode_get_block_unsafe(file->of_inumber, bi);
        if (b == -1) {
            pthread_rwlock_unlock(&inode_lock_table[file->of_inumber]);
            pthread_mutex_unlock(&file->of_mutex);
            return -1;
        }

        void *block = data_block_get(b);
        if (block == NULL) {
            pthread_rwlock_unlock(&inode_lock_table[file->of_inumber]);
            pthread_mutex_unlock(&file->of_mutex);
            return -1;
        }

        /* Read the data */
        size_t to_read_in_block = offset + to_read - read < BLOCK_SIZE
                                      ? to_read - read
                                      : BLOCK_SIZE - offset;
        memcpy(buffer + read, block + offset, to_read_in_block);
        file->of_offset += to_read_in_block;
        read += to_read_in_block;
    }

    if (pthread_rwlock_unlock(&inode_lock_table[file->of_inumber])) {
        pthread_mutex_unlock(&file->of_mutex);
        return -1;
    }

    if (pthread_mutex_unlock(&file->of_mutex)) {
        return -1;
    }

    return (ssize_t)to_read;
}