#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/stat.h>
#include <errno.h>
#include "ext2_utils.h"

unsigned char *disk = NULL;


int main (int argc, char **argv) 
{
    if (argc != 4) {
        fprintf(stderr, 
            "Usage: %s <image file path> <path on native OS> <absolute path on disk image>\n", 
            argv[0]);
        exit(1);
    }

    init_disk(argv[1]);
    
    char *src_path = argv[2];
    char *dest_path = argv[3];
    char *src_file_name;
    char *base_copy;
    char dest_file_name[strlen(dest_path) + 1];
    
    unsigned int parent_inode;
    unsigned int dest_inode;
    
    struct ext2_inode *dest_ino;
    FILE *src_file;

    if ((src_file = fopen(src_path, "r")) == NULL) {
        fprintf(stderr, "ERROR: Source file does not exist\n");
        return ENOENT;
    }

    /* Get the name of the source file */
    char src_copy[strlen(src_path) + 1];
    memcpy(src_copy, src_path, strlen(src_path));
    src_copy[strlen(src_path)] = '\0';
    src_file_name = basename(src_copy);

    char dest_copy[strlen(dest_path) + 1];
    dest_inode = get_inode_at_path(dest_path);

    if (dest_inode) {

        if (HAS_TRAILING_SLASH(dest_path) && !is_dir(dest_inode)) {
            fprintf(stderr, 
                "ERROR: Destination with trailing slash is not a directory\n");
            return ENOENT;
        }

        dest_ino = get_inode(dest_inode);

        switch (TYPE_MASK(dest_ino->i_mode)) {
            case EXT2_S_IFLNK:
                /* If the destination path is a link we exit with an error */
                fprintf(stderr, "ERROR: Destination path is a symlink\n");
                exit(-1);

            case EXT2_S_IFDIR:
                /* If the destination path is a directory, then it is the parent 
                 * directory of our desired link location, and we need to ensure
                 * that it does not already include an entry with the source
                 * file's name */
                parent_inode = dest_inode;
                memcpy(dest_file_name, src_file_name, strlen(src_file_name));
                dest_file_name[strlen(src_file_name)] = '\0';

                if (find_entry(parent_inode, dest_file_name)) {
                    fprintf(stderr, 
                        "ERROR: File name already exists in destination directory\n");
                    return EEXIST;
                }
                break;

            default:
                /* If the destination path is an existing file, return EEXIST */
                fprintf(stderr, "ERROR: Destination file already exists\n");
                return EEXIST;
        }
    
    } else {
        /* In this case, the destination directory of our copied file is the parent
         * directory of the given destination path, if it exists */
        char parent_dir[strlen(dest_path) + 1];
        
        memcpy(parent_dir, dest_path, strlen(dest_path));
        parent_dir[strlen(dest_path)] = '\0';
        memcpy(parent_dir, dirname(parent_dir), strlen(parent_dir));
        parent_dir[strlen(parent_dir)] = '\0';

        parent_inode = get_inode_at_path(parent_dir);
        if (!parent_inode) {
            fprintf(stderr, 
                "ERROR: Parent directory for destination path is invalid\n");
            return ENOENT;
        }

        /* A trailing slash in the dest_path is not allowed, since it implies
         * that the source file is a directory */
        if (HAS_TRAILING_SLASH(dest_path)) {
            fprintf(stderr, 
                "ERROR: Destination file to create cannot be a directory\n");
            return ENOENT;
        }

        /* The destination file name in this case is the final segment of the 
         * destination path */
        memcpy(dest_copy, dest_path, strlen(dest_path));
        dest_copy[strlen(dest_path)] = '\0';
        base_copy = basename(dest_copy);
        memcpy(dest_file_name, base_copy, strlen(base_copy));
        dest_file_name[strlen(base_copy)] = '\0';

        /* Ensure that the file name isn't already taken */
        if (find_entry(parent_inode, dest_file_name)) {
            fprintf(stderr, 
                "ERROR: File name already exists in destination directory\n");
            return EEXIST;
        }
    }

    if (strlen(dest_file_name) > EXT2_NAME_LEN) {
        fprintf(stderr, "ERROR: Destination file name too long\n");
        return ENAMETOOLONG;
    }

    /* Get size of source file */
    struct stat st;
    if (stat(src_path, &st) < 0) {
        fprintf(stderr, "ERROR: Failed to stat source file\n");
        exit(-1);
    }
    int src_size = st.st_size;

    /* Ensure that source file is not too large for our filesystem, i.e. if its contents
     * cannot fit in the remaining free blocks on the system. Note that since the filesystem
     * only 128 blocks in total, this amount will always be less than the number of blocks
     * theoretically possible given each inode's 12 standard direct blocks plus 256 more 
     * accessible through the singly indirect block. */
    int max_file_size = (src_size > (NUM_INITIAL_DIRECT_BLOCKS * EXT2_BLOCK_SIZE)) ?
        (get_super_block()->s_free_blocks_count - 1) * EXT2_BLOCK_SIZE :
         get_super_block()->s_free_blocks_count * EXT2_BLOCK_SIZE;
    
    if (src_size > max_file_size) {
        fprintf(stderr, "Source file too large to copy\n");
        return ENOSPC;
    }

    /* If none of the above error cases apply, we can safely allocate a new inode for 
     * the destination file */
    dest_inode = allocate_inode();

    /* Read source file into contents array */
    char contents[src_size + 1];
    fread(contents, src_size + 1, 1, src_file);
    fclose(src_file);

    /* Create directory entry for the destination file and write the source file's
     * contents to its inode */
    create_entry(parent_inode, dest_inode, dest_file_name, EXT2_FT_REG_FILE);
    dest_ino = get_inode(dest_inode);
    dest_ino->i_size = src_size;
    write_to_inode(dest_inode, contents);

    return 0;
}
