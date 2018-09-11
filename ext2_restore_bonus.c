#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <errno.h>
#include "ext2_utils.h"

unsigned char *disk = NULL;


int main (int argc, char **argv) 
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, 
            "Usage: %s <image file path> [-r] <absolute path on disk image>\n", 
            argv[0]);
        exit(1);
    }

    init_disk(argv[1]);
    int ret_val;
    int has_recursive_flag = !strcmp(argv[2], "-r");
    
    char *target_path = (has_recursive_flag) ? argv[3] : argv[2];
    char *target_name;
    char parent_dir[strlen(target_path) + 1];
    char path_copy[strlen(target_path) + 1];
    
    unsigned int parent_inode;
    unsigned int target_inode;

    /* Ensure that the parent directory exists */
    memcpy(parent_dir, target_path, strlen(target_path));
    parent_dir[strlen(target_path)] = '\0';
    memcpy(parent_dir, dirname(parent_dir), strlen(parent_dir));
    parent_dir[strlen(parent_dir)] = '\0';
    
    parent_inode = get_inode_at_path(parent_dir);
    if (!parent_inode || !is_dir(parent_inode)) {
        fprintf(stderr, "ERROR: Invalid parent directory\n");
        return ENOENT;
    }

    /* Ensure that the requested entry is in fact a previously removed
     * (using ext2_rm) entry in the parent directory, and is not itself
     * a directory */
    memcpy(path_copy, target_path, strlen(target_path));
    path_copy[strlen(target_path)] = '\0';
    target_name = basename(path_copy);

    target_inode = find_removed_entry(parent_inode, target_name);
    
    /* If the target entry is not found, we cannot continue and immediately
     * return ENOENT */
    if (!target_inode) {
        fprintf(stderr, "ERROR: Target entry not found\n");
        return ENOENT;
    
    } else {
        /* If the recursive flag has not been entered and the target is a
         * directory, return EISDIR */
        if (!has_recursive_flag && is_dir(target_inode)) {
            fprintf(stderr, "ERROR: Target entry is a directory\n");
            return EISDIR;
        }

        ret_val = is_recoverable(target_inode, TRUE);

        if (ret_val < 0) {
            /* In this case, the entry is a directory that itself can be restored
             * but not all of its entries can, so we will still exit with ENOENT
             * but first we will attempt to restore it and as many of its entries
             * as possible. */
            fprintf(stderr, "ERROR: Target directory only partially restored\n");
            ret_val = ENOENT;
        } else if (!ret_val) {
            /* In this case, the entry is not recoverable at all, so we 
             * immediately return ENOENT. */
            fprintf(stderr, "ERROR: Target entry could not be restored\n");
            return ENOENT;
        } else {
            /* Otherwise, we know the entry is fully recoverable, so we will
             * exit with 0. */
            ret_val = 0;
        }
    }

    restore_entry(parent_inode, target_name);

    return ret_val;
}
