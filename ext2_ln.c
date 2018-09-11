#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <errno.h>
#include "ext2_utils.h"

unsigned char *disk = NULL;


int main (int argc, char **argv) 
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr, 
            "Usage: %s <image file path> [-s] <absolute path of file to link to> <absolute path of link>\n", 
            argv[0]);
        exit(1);
    }

    init_disk(argv[1]);
    char *src_path;
    char *dest_path;
    char *link_name;
    
    unsigned int src_inode;
    unsigned int parent_inode;
    int is_hard_link = strcmp(argv[2], "-s");

    if (is_hard_link) {
        src_path = argv[2];
        dest_path = argv[3];
    } else {
        src_path = argv[3];
        dest_path = argv[4];
    }

    char parent_dir[strlen(dest_path) + 1];
    char dest_copy[strlen(dest_path) + 1];
    
    src_inode = get_inode_at_path(src_path);
     
    /* Ensure that the file we are linking to exists and is in fact a file */   
    if (!src_inode) {
        fprintf(stderr, "ERROR: Source file %s does not exist\n", src_path);
        return ENOENT;
    } else if (is_dir(src_inode)) {
        fprintf(stderr, "ERROR: Source file %s is a directory\n", src_path);
        return EISDIR;
    }

    /* A trailing slash in the dest_path is not allowed, since it implies
     * that the source file is a directory */
    if (HAS_TRAILING_SLASH(dest_path)) {
        fprintf(stderr, "ERROR: Link cannot be a directory\n");
        return ENOENT;
    }

    memcpy(parent_dir, dest_path, strlen(dest_path));
    parent_dir[strlen(dest_path)] = '\0';
    memcpy(parent_dir, dirname(parent_dir), strlen(parent_dir));
    parent_dir[strlen(parent_dir)] = '\0';

    /* Ensure that the parent directory exists */
    parent_inode = get_inode_at_path(parent_dir);
    if (!parent_inode) {
        fprintf(stderr, "ERROR: Parent directory %s for destination path is invalid\n", 
            parent_dir);
        return ENOENT;
    }

    memcpy(dest_copy, dest_path, strlen(dest_path));
    dest_copy[strlen(dest_path)] = '\0';
    link_name = basename(dest_copy);

    /* Ensure that the link name is not too long and does not already exist */
    if (strlen(link_name) > EXT2_NAME_LEN) {
        fprintf(stderr, "ERROR: Link name too long\n");
        return ENAMETOOLONG;
    }

    if (find_entry(parent_inode, link_name)) {
        fprintf(stderr, "ERROR: Link name already exists\n");
        return EEXIST;
    }

    if (is_hard_link) {
        /* For hard links, we simply create a new entry, since the inode
         * already exists */
        create_entry(parent_inode, src_inode, link_name, EXT2_FT_REG_FILE);
    } else {
        /* For symlinks, we do need to allocate a new inode, since it is
         * considered a new file */
        unsigned int dest_inode = allocate_inode();
        create_entry(parent_inode, dest_inode, link_name, EXT2_FT_SYMLINK);

        /* A symlink simply contains the path to the file it is linking to,
         * so the size of the symlink is simply the length of this path */
        struct ext2_inode *dest_ino = get_inode(dest_inode);
        dest_ino->i_size = strlen(src_path);
        
        write_to_inode(dest_inode, src_path);
    }

    return 0;
}
