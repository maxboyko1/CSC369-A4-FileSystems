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
    int has_recursive_flag = !strcmp(argv[2], "-r");

    char *target_path = (has_recursive_flag) ? argv[3] : argv[2];
    char *target_name;
    char path_copy[strlen(target_path) + 1];

    unsigned int target_inode;
    unsigned int parent_inode;

    /* Ensure that target entry exists and is a file */
    target_inode = get_inode_at_path(target_path);
    if (!target_inode) {
        fprintf(stderr, "ERROR: Target file does not exist\n");
        return ENOENT;
    }

    /* If the recursive flag has not been entered and the target is a
     * directory, return EISDIR */
    if (!has_recursive_flag && is_dir(target_inode)) {
        fprintf(stderr, "ERROR: Target is a directory\n");
        return EISDIR;
    }

    /* Get inode of parent directory */
    char parent_dir[strlen(target_path) + 1];
    
    memcpy(parent_dir, target_path, strlen(target_path));
    parent_dir[strlen(target_path)] = '\0';
    memcpy(parent_dir, dirname(parent_dir), strlen(parent_dir));
    parent_dir[strlen(parent_dir)] = '\0';
    
    parent_inode = get_inode_at_path(parent_dir);

    /* Get filename of target entry */
    memcpy(path_copy, target_path, strlen(target_path));
    path_copy[strlen(target_path)] = '\0';
    target_name = basename(path_copy);

    remove_entry(parent_inode, target_name);

    return 0;
}
