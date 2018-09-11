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
            "USAGE: %s <image file path> <absolute path on disk image>\n",
            argv[0]);
        exit(1);
    }

    init_disk(argv[1]);

    char *path = argv[2]; 
    char *new_dir;
    char parent_dir[strlen(path) + 1];
    char path_copy[strlen(path) + 1];
    
    unsigned int parent_inode = 0;

    /* User should not be able to pass in a relative path 
     * on the disk image */
    if (IS_ABSOLUTE(path)) {
        
        /* Get the inode of the parent directory, if it exists */
        memcpy(parent_dir, path, strlen(path));
        parent_dir[strlen(path)] = '\0';

        memcpy(parent_dir, dirname(parent_dir), strlen(path));
        parent_dir[strlen(parent_dir)] = '\0';
        
        parent_inode = get_inode_at_path(parent_dir);
    }

    if (parent_inode && is_dir(parent_inode)) {
        /* If the desired parent is a valid directory, and none of the errors
         * below apply, we may proceed */
        memcpy(path_copy, path, strlen(path));
        path_copy[strlen(path)] = '\0';
        new_dir = basename(path_copy);

        if (strlen(new_dir) > EXT2_NAME_LEN) {
            fprintf(stderr, "ERROR: Directory name too long\n");
            return ENAMETOOLONG;
        }

        if (find_entry(parent_inode, new_dir)) {
            fprintf(stderr, "ERROR: Directory already exists\n");
            return EEXIST;
        }

        unsigned int new_inode = allocate_inode();
        create_entry(parent_inode, new_inode, new_dir, EXT2_FT_DIR);
    
    } else {
        fprintf(stderr, "ERROR: Parent path must be absolute and valid\n");
        return ENOENT;
    }

    return 0;
}
