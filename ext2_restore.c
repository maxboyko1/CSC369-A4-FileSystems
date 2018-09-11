#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <errno.h>
#include "ext2_utils.h"

unsigned char *disk = NULL;


int main (int argc, char **argv) 
{
    if (argc != 3) {
        fprintf(stderr, 
            "Usage: %s <image file path> <absolute path to file or link on disk image>\n", 
            argv[0]);
        exit(1);
    }

    init_disk(argv[1]);
    
    char *target_path = argv[2];
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
    if (!target_inode) {
        fprintf(stderr, "ERROR: Target file could not be found\n");
        return ENOENT;
    } else if (is_dir(target_inode)) {
        fprintf(stderr, "ERROR: Target entry is a directory\n");
        return EISDIR;
    }

    /* Note that, since we know target_inode refers to a directory, the only 
     * possible return values for is_recoverable() are 0 and 1 */
    if (!is_recoverable(target_inode, TRUE)) {
        fprintf(stderr, "ERROR: Target file could not be restored\n");
        return ENOENT;
    }

    restore_entry(parent_inode, target_name);

    return 0;
}
