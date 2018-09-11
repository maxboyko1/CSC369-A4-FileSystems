#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "ext2_utils.h"

/* 
 * Initialize the disk image at diskpath and map it to memory.
 */
void init_disk (char *diskpath) 
{
    /* Open disk image */
    int fd = open(diskpath, O_RDWR);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    /* Map the disk image into memory */
    disk = mmap(NULL, DISK_BLOCKS*EXT2_BLOCK_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
}

/*
 * Return the inode number of the file or directory at the given absolute
 * path on the current disk, or 0 if the path is invalid.
 */
unsigned int get_inode_at_path (char *path) 
{
    /* Path passed in must be absolute */
    if (!IS_ABSOLUTE(path))
        return 0;

    /* Remove all leading slashes */
    while (IS_ABSOLUTE(path))
        path++;

    /* Starting directory of path walk is always the root */
    unsigned int inode = EXT2_ROOT_INO;
    char current_seg[strlen(path) + 1];

    while (strlen(path)) {
        int path_len = strlen(path);
        int next_slash = strcspn(path, "/");
        
        /* Determine the path segment to search for in the current directory 
         * (i.e. the substring of path prior to the next slash) */
        memcpy(current_seg, path, next_slash);
        current_seg[path_len] = '\0';

        /* If the current path segment is not the last one and not a
         * directory, the path is invalid */
        if (path_len > next_slash && !is_dir(inode))
            return 0;

        /* We proceed if the desired entry exists, and if it is a non-terminal
         * directory we will search it on the next iteration. If our desired 
         * entry does not exist then the path is invalid. */
        inode = find_entry(inode, current_seg);
        if (!inode) 
            return 0;
        
        path += next_slash;

        /* Skip over any repeated slashes before the next path segment */
        while (strlen(path) > 1 && path[1] == '/')
            path++;

        /* Advance to next path segment, if it exists */
        if (strlen(path))
            path++;
    }

    return inode;
}

/*
 * Allocate the lowest currently unused inode for a new file or directory, 
 * mark it as in use in the inode bitmap and return the number of the newly
 * allocated inode.
 */
unsigned int allocate_inode () 
{
    int found = 0;
    
    int byte = 1;
    int bit = 3;
    int num_bytes = get_super_block()->s_inodes_count / NUM_BITS;
    
    unsigned char *inode_bitmap = get_inode_bitmap();

    /* Locate the next available inode, excluding reserved ones */
    while (!found && byte < num_bytes) {
        while (!found && bit < NUM_BITS) {
            if (!IN_USE(inode_bitmap, byte, bit))
                found = 1;
            else bit++;
        }

        if (!found) {
            byte++;
            bit = 0;
        }
    }

    /* Mark the newly allocated bit as used and update free inode counters */
    MARK_AS_USED(inode_bitmap, byte, bit);
    get_super_block()->s_free_inodes_count--;
    get_group_desc()->bg_free_inodes_count--;
    
    /* Return the correct inode number given the final byte and bit indices */
    return byte * NUM_BITS + (bit + 1);
}

/*
 * Allocate the lowest currently unused block, mark it as in use in the block
 * bitmap and return the number of the newly allocated block.
 */
unsigned int allocate_block () 
{
    int found = 0;
    
    int byte = 0;
    int bit = 0; 
    int num_bytes = get_super_block()->s_blocks_count / NUM_BITS;
    
    unsigned char *block_bitmap = get_block_bitmap();

    /* Locate the next available block */
    while (!found && byte < num_bytes) {
        while (!found && bit < NUM_BITS) {
            if (!IN_USE(block_bitmap, byte, bit)) 
                found = 1;
            else bit++;
        }

        if (!found) {
            byte++;
            bit = 0;
        }
    }

    /* Mark the newly allocated bit as used and update free block counters */
    MARK_AS_USED(block_bitmap, byte, bit);
    get_super_block()->s_free_blocks_count--;
    get_group_desc()->bg_free_blocks_count--;
    
    /* Return the correct block number given the final byte and bit indices */
    return byte * NUM_BITS + (bit + 1);
}

/*
 * Search the directory referred to by parent_inode for an entry with
 * the given name, and return its inode number if it is found. Otherwise,
 * return 0.
 */
unsigned int find_entry (unsigned int parent_inode, char *entry_name) 
{
    int k = 0;
    unsigned long block_pos;
    char current_name[EXT2_NAME_LEN + 1];

    struct ext2_inode *parent_ino = get_inode(parent_inode);
    struct ext2_dir_entry *cur_entry;

    /* Iterate through all blocks allocated to this directory */
    while (k < NUM_INITIAL_DIRECT_BLOCKS && parent_ino->i_block[k]) {
        block_pos = 0;

        /* Iterate through all entries in the current block, checking their names */
        while (block_pos < EXT2_BLOCK_SIZE) {
            cur_entry = get_entry(parent_ino->i_block[k], block_pos);

            memcpy(current_name, cur_entry->name, cur_entry->name_len);
            current_name[cur_entry->name_len] = '\0';

            /* Found an entry with the same name, so return its inode number */
            if (!strcmp(current_name, entry_name))
                return cur_entry->inode;

            block_pos += cur_entry->rec_len;
        }

        k++;
    }

    return 0;
}

/*
 * Create a new directory entry with given inode, name and type, with the 
 * directory referred to by parent_inode as its parent.
 */
void create_entry (unsigned int parent_inode, unsigned int entry_inode, 
        char *entry_name, unsigned char type) 
{
    /* The actual size of the new dir_entry we are trying to create is the size
     * of the dir_entry struct plus the length of the name, rounded up to the 
     * nearest multiple of 4 */
    int dir_entry_size = sizeof(struct ext2_dir_entry);
    int new_actual_len = PAD_REC_LEN(dir_entry_size + strlen(entry_name));

    int k = 0;
    int is_inserted = 0;
    unsigned long block_pos;

    struct ext2_inode *parent_ino = get_inode(parent_inode);
    struct ext2_dir_entry *prev;
    struct ext2_dir_entry *cur_entry;

    /* Loop through the parent directory's blocks, looking for a block with enough
     * space at the end to fit our new directory entry */
    while (!is_inserted && k < NUM_INITIAL_DIRECT_BLOCKS && parent_ino->i_block[k]) {
        block_pos = 0;

        while (block_pos < EXT2_BLOCK_SIZE) {
            cur_entry = get_entry(parent_ino->i_block[k], block_pos);
            block_pos += cur_entry->rec_len;
        }

        prev = cur_entry;
        int prev_actual_len = PAD_REC_LEN(dir_entry_size + prev->name_len);

        /* If there is enough space after the final entry of this block to insert
         * our new entry, do so */
        if (new_actual_len <= (prev->rec_len - prev_actual_len)) {
            cur_entry = (struct ext2_dir_entry *) ((unsigned char *)cur_entry +
                prev_actual_len);

            /* Update the record lengths of our new entry and its predecessor */
            cur_entry->rec_len = prev->rec_len - prev_actual_len;
            prev->rec_len = prev_actual_len;

            is_inserted = 1;
        }
        else k++;
    }

    /* If none of the currently allocated blocks had enough space at the end to 
     * fit our new entry, we need to allocate a new block and insert it there */
    if (!is_inserted) {
        unsigned int new_block = allocate_block();
        
        /* Append the location of the newly allocated block to the 
         * parent inode's i_block[] array */
        parent_ino->i_block[k] = new_block;

        /* New 1024-byte block allocated, so we need two more 512-byte ones */
        parent_ino->i_blocks += (EXT2_BLOCK_SIZE / DISK_SECTOR_SIZE);
        parent_ino->i_size += EXT2_BLOCK_SIZE;

        cur_entry = get_entry(parent_ino->i_block[k], 0);
        
        /* Since this is currently the only entry in the block */
        cur_entry->rec_len = EXT2_BLOCK_SIZE;
    }

    /* Set the other fields of our new dir_entry */
    cur_entry->inode = entry_inode;
    cur_entry->name_len = strlen(entry_name);
    cur_entry->file_type = type;

    memcpy(cur_entry->name, entry_name, cur_entry->name_len);
    cur_entry->name[cur_entry->name_len] = '\0';

    struct ext2_inode *entry_ino = get_inode(entry_inode);

    /* If the entry we are creating is for a new file, we need to initialize 
     * the inode struct. Otherwise, simply update the inode's number of 
     * links. */
    if (!entry_ino->i_links_count)
        init_inode(entry_ino, type);
    else entry_ino->i_links_count++;

    /* If the entry we are creating is a new directory, it needs . and .. entries */
    if (type == EXT2_FT_DIR && !IS_DOT_ENTRY(entry_name)) {
        create_entry(entry_inode, entry_inode, ".", EXT2_FT_DIR);
        create_entry(entry_inode, parent_inode, "..", EXT2_FT_DIR);
    }
}

/*
 * Initialize an inode structure with the requested file type.
 */
void init_inode (struct ext2_inode *ino, unsigned char type) 
{
    ino->i_mode = get_imode(type);
    ino->i_uid = 0;
    ino->i_size = 0;
    ino->i_ctime = time(NULL);
    ino->i_dtime = 0;
    ino->i_gid = 0;
    ino->i_links_count = 1;
    ino->i_blocks = 0;
    ino->osd1 = 0;
    memset(ino->i_block, 0, 15 * sizeof(unsigned int));
    ino->i_generation = 0;
    ino->i_file_acl = 0;
    ino->i_dir_acl = 0;
    ino->i_faddr = 0;
    memset(ino->extra, 0, 3 * sizeof(unsigned int));

    if (type == EXT2_FT_DIR)
        get_group_desc()->bg_used_dirs_count++;
}

/*
 * Write the given contents to the data blocks of the specified
 * (currently empty) inode.
 */
void write_to_inode (unsigned int inode, char *contents) 
{
    struct ext2_inode *ino = get_inode(inode);

    int direct_pos = 0;
    int bytes_written = 0;
    int bytes_to_write = ino->i_size;

    int k = 0;
    int bytes_allocated = 0;
    
    unsigned int *indirect_pos = 0;
    unsigned char *cur_block;

    /* Allocate to this inode all blocks that will be necessary to 
     * store the specified contents */
    while (bytes_allocated < bytes_to_write) {

        if (k < NUM_INITIAL_DIRECT_BLOCKS) {
            /* In this case, we allocate a direct block normally */
            ino->i_block[k] = allocate_block();
            ino->i_blocks += (EXT2_BLOCK_SIZE / DISK_SECTOR_SIZE);
            k++;
        
        } else if (k == NUM_INITIAL_DIRECT_BLOCKS) {
            /* In this case, we need to allocate a new pointer to a direct
             * block inside the indirect block */
            if (!indirect_pos) {
                ino->i_block[k] = allocate_block();
                ino->i_blocks += (EXT2_BLOCK_SIZE / DISK_SECTOR_SIZE);
                indirect_pos = (unsigned int *) get_block(ino->i_block[k]);
            }

            *indirect_pos = allocate_block();
            ino->i_blocks += (EXT2_BLOCK_SIZE / DISK_SECTOR_SIZE);
            indirect_pos++;
        }

        bytes_allocated += EXT2_BLOCK_SIZE;
    }

    k = 0;
    indirect_pos = 0;

    /* Now that all the necessary blocks are allocated, we can write the 
     * specified contents to the inode */
    while (bytes_written < bytes_to_write) {

        if (!direct_pos && k < NUM_INITIAL_DIRECT_BLOCKS) {
            cur_block = get_block(ino->i_block[k]);
            k++;    
        
        } else if (!direct_pos && k == NUM_INITIAL_DIRECT_BLOCKS) {
            /* In this case, the contents we need to write are too large 
             * to fit in the initial 12 direct blocks, so we need to use
             * the single indirect block */
            indirect_pos = (!indirect_pos) ? 
                (unsigned int *) get_block(ino->i_block[k]) : 
                (indirect_pos + 1);
            cur_block = get_block(*indirect_pos);
        }

        cur_block[direct_pos] = contents[bytes_written];

        /* Advance the current position in the block by 1, resetting
         * back to 0 at EXT2_BLOCK_SIZE */
        direct_pos = (direct_pos + 1) % EXT2_BLOCK_SIZE;
        bytes_written++;
    }
}

/*
 * Remove the directory entry with the given name from the parent
 * directory referred to by parent_inode.
 */
void remove_entry (unsigned int parent_inode, char *entry_name) 
{
    unsigned int entry_inode = find_entry(parent_inode, entry_name);

    struct ext2_inode *entry_ino = get_inode(entry_inode);
    struct ext2_inode *parent_ino = get_inode(parent_inode);
    struct ext2_dir_entry *cur_entry;
    struct ext2_dir_entry *prev;
    
    int k = 0;
    int found = 0;
    unsigned long block_pos;
    char current_name[EXT2_NAME_LEN + 1];

    /* Search through the parent directory's blocks for the target entry */
    while (!found && k < NUM_INITIAL_DIRECT_BLOCKS && parent_ino->i_block[k]) {
        block_pos = 0;
        
        cur_entry = get_entry(parent_ino->i_block[k], block_pos);
        memcpy(current_name, cur_entry->name, cur_entry->name_len);
        current_name[cur_entry->name_len] = '\0';

        if (!strcmp(current_name, entry_name)) {
            /* If the target entry is the first one in its block, we 
             * simply zero out its inode field, making this entry
             * unrecoverable */
            cur_entry->inode = 0;
            found = 1;
        
        } else {
            /* Otherwise, we check the rest of the block's entries */
            block_pos += cur_entry->rec_len;

            while (!found && block_pos < EXT2_BLOCK_SIZE) {
                prev = cur_entry;
                cur_entry = get_entry(parent_ino->i_block[k], block_pos);

                memcpy(current_name, cur_entry->name, cur_entry->name_len);
                current_name[cur_entry->name_len] = '\0';

                if (!strcmp(current_name, entry_name)) {
                    /* Adjust the previous entry's record length to point to
                     * the entry after the current one */
                    prev->rec_len += cur_entry->rec_len;
                    found = 1;
                } else {
                    block_pos += cur_entry->rec_len;
                }
            }
        }

        k++;
    }
    
    /* If this entry is a directory, or is a file with no other hard links
     * to it remaining, we need to free the inode's resources, and, in the 
     * case of a directory, recursively free the resources of all its 
     * entries that match this description as well. Otherwise, simply 
     * decrement the links count. */
    if (is_dir(entry_inode)) 
        get_group_desc()->bg_used_dirs_count--;

    int is_last_copy = !is_dir(entry_inode) && (entry_ino->i_links_count == 1);
    if (is_dir(entry_inode) || is_last_copy)
        free_resources(entry_inode, entry_name);
    else entry_ino->i_links_count--;
}

/*
 * Deallocate the given inode's blocks and inode number and adjust the 
 * free data block and inode counters accordingly. If this is a directory,
 * recursively deallocate the inodes and blocks of all its entries that are
 * also directories, or files with no remaining hard links.
 */
void free_resources (unsigned int inode_num, char *entry_name) 
{
    struct ext2_inode *ino = get_inode(inode_num);
    struct ext2_inode *cur_ino;
    struct ext2_dir_entry *cur_entry;

    int k = 0;
    int is_last_copy;
    int is_non_dotted_dir;

    unsigned int direct_block;
    unsigned int *indirect_pos;
    unsigned int *indirect_end;
    
    unsigned long block_pos;
    char current_name[EXT2_NAME_LEN + 1];

    /* If this is a directory, we need to recursively free the resources of 
     * all its entries that are directories or files with no hard links */
    if (is_dir(inode_num)) {
        while (k < NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
            block_pos = 0;

            while (block_pos < EXT2_BLOCK_SIZE) {
                cur_entry = get_entry(ino->i_block[k], block_pos);
                
                memcpy(current_name, cur_entry->name, cur_entry->name_len);
                current_name[cur_entry->name_len] = '\0';
                cur_ino = get_inode(cur_entry->inode);

                is_last_copy = !is_dir(cur_entry->inode) && 
                    (cur_ino->i_links_count == 1);
                is_non_dotted_dir = is_dir(cur_entry->inode) && 
                    !IS_DOT_ENTRY(current_name);

                if (is_last_copy || is_non_dotted_dir) {
                    /* If the current entry is a non-dotted directory or a 
                     * file with no remaining hard links, we need to free
                     * its resources as well */
                    free_resources(cur_entry->inode, current_name);
                } else {
                    /* Otherwise, we simply decrement the inode's links
                     * count. Note that, in the case of a dotted entry
                     * (. or ..) we know that this is not the last link
                     * due to the depth-first nature of the recursion. */
                    cur_ino->i_links_count--;
                }

                block_pos += cur_entry->rec_len;
            }

            k++;
        }
    }

    deallocate_inode(inode_num);

    /* Deallocate all the inode's blocks (but don't zero them out) */
    k = 0;
    while (k < NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
        deallocate_block(ino->i_block[k]);
        k++;
    }

    if (k == NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
        indirect_pos = (unsigned int *) get_block(ino->i_block[k]);
        indirect_end = indirect_pos + (EXT2_BLOCK_SIZE / sizeof(unsigned int));
        direct_block = *indirect_pos;

        while (direct_block && indirect_pos < indirect_end) {
            deallocate_block(direct_block);

            indirect_pos++;
            direct_block = *indirect_pos;
        }

        deallocate_block(ino->i_block[k]);
    }

    ino->i_dtime = time(NULL);
    ino->i_links_count--;
}

/*
 * Mark the specified inode number as free and update the free inode counters.
 */
void deallocate_inode (unsigned int inode_num) 
{
    unsigned char *inode_bitmap = get_inode_bitmap();
    int bit = GET_BIT(inode_num);
    int byte = GET_BYTE(inode_num);

    MARK_AS_FREE(inode_bitmap, byte, bit);
    get_super_block()->s_free_inodes_count++;
    get_group_desc()->bg_free_inodes_count++;
}

/*
 * Mark the specified block number as free and update the free block counters.
 */
void deallocate_block (unsigned int block_num) 
{
    unsigned char *block_bitmap = get_block_bitmap();
    int bit = GET_BIT(block_num);
    int byte = GET_BYTE(block_num);

    MARK_AS_FREE(block_bitmap, byte, bit);
    get_super_block()->s_free_blocks_count++;
    get_group_desc()->bg_free_blocks_count++;
}

/*
 * Search the directory referred to by parent_inode for a previously removed
 * (using ext2_rm) entry with the given name. If the entry is found, return
 * its inode number. Otherwise, return 0.
 */
unsigned int find_removed_entry (unsigned int parent_inode, char *entry_name) 
{
    struct ext2_inode *parent_ino = get_inode(parent_inode);
    struct ext2_dir_entry *cur_entry;

    int k = 0;
    int found = 0;
    int dir_entry_size = sizeof(struct ext2_dir_entry);
    int actual_cur_len;
    
    unsigned int target_inode = 0;
    unsigned long block_pos, next_intact_pos;
    
    char current_name[EXT2_NAME_LEN + 1];

    /* Search through the parent inode's blocks for the removed entry */
    while (!found && k < NUM_INITIAL_DIRECT_BLOCKS && parent_ino->i_block[k]) {
        block_pos = 0;

        cur_entry = get_entry(parent_ino->i_block[k], block_pos);
        memcpy(current_name, cur_entry->name, cur_entry->name_len);
        current_name[cur_entry->name_len] = '\0';

        if (!strcmp(current_name, entry_name)) {
            /* If the target entry to restore is the first one in its block,
             * then we know it is unrecoverable */
            found = 1;
        
        } else {
            /* Otherwise, search through the rest of the block's entries */
            while (!found && block_pos < EXT2_BLOCK_SIZE) {
                cur_entry = get_entry(parent_ino->i_block[k], block_pos);
                next_intact_pos = block_pos + cur_entry->rec_len;

                actual_cur_len = PAD_REC_LEN(dir_entry_size + cur_entry->name_len);
                block_pos += actual_cur_len;

                /* Search the gap between the latest intact entry and the next
                 * intact entry, if such a gap exists */
                while (!found && block_pos < next_intact_pos) {
                    cur_entry = get_entry(parent_ino->i_block[k], block_pos);
                    memcpy(current_name, cur_entry->name, cur_entry->name_len);
                    current_name[cur_entry->name_len] = '\0';

                    if (!strcmp(current_name, entry_name)) {
                        /* If this deleted entry has the name we are looking for, then
                         * we are done */
                        target_inode = cur_entry->inode;
                        found = 1;
                    } else {
                        /* Otherwise, advance to the next deleted entry slot */
                        actual_cur_len = PAD_REC_LEN(dir_entry_size + cur_entry->name_len);
                        block_pos += actual_cur_len;
                    }
                }
            }
        }

        k++;
    }

    return target_inode;
}

/*
 * If the previously deallocated inode with the given number is recoverable
 * (i.e. its inode number has not been reused and none of its data blocks have
 * been reallocated), return 1. If the inode is a directory and it itself is
 * recoverable but some of its entries are not, return -1. Otherwise, return 0.
 * The second argument signifies whether or not this is the initial call to the
 * function or a nested recursive call.
 */
int is_recoverable (unsigned int inode_num, int is_first) 
{
    unsigned char *block_bitmap = get_block_bitmap();
    unsigned char *inode_bitmap = get_inode_bitmap();

    struct ext2_inode *ino;
    struct ext2_dir_entry *cur_entry;

    unsigned int direct_block;
    unsigned int *indirect_pos;
    unsigned int *indirect_end;
    unsigned long block_pos;
    
    int k;
    int bit = GET_BIT(inode_num);
    int byte = GET_BYTE(inode_num);
    
    char current_name[EXT2_NAME_LEN + 1];

    /* First, we check if this inode and all its data blocks are recoverable.
     * If any of them are not, we return 0 if this is the initial call to 
     * is_recoverable(), and -1 otherwise. */
    if (IN_USE(inode_bitmap, byte, bit)) 
        return ZERO_OR_NEG_ONE(is_first);
    
    ino = get_inode(inode_num);
    k = 0;

    while (k < NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
        bit = GET_BIT(ino->i_block[k]);
        byte = GET_BYTE(ino->i_block[k]);

        if (IN_USE(block_bitmap, byte, bit))
            return ZERO_OR_NEG_ONE(is_first);;

        k++;
    }

    if (k == NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
        bit = GET_BIT(ino->i_block[k]);
        byte = GET_BYTE(ino->i_block[k]);
            
        if (IN_USE(block_bitmap, byte, bit))
            return ZERO_OR_NEG_ONE(is_first);;

        indirect_pos = (unsigned int *) get_block(ino->i_block[k]);
        indirect_end = indirect_pos + (EXT2_BLOCK_SIZE / sizeof(unsigned int));
        direct_block = *indirect_pos;

        while (direct_block && indirect_pos < indirect_end) {
            bit = GET_BIT(direct_block);
            byte = GET_BYTE(direct_block);
                
            if (IN_USE(block_bitmap, byte, bit))
                return ZERO_OR_NEG_ONE(is_first);;

            indirect_pos++;
            direct_block = *indirect_pos;
        }
    }

    /* Now, if the inode refers to a directory, we recursively check if all 
     * its entries are recoverable, and if any of them are not, return -1 */
    if (is_dir(inode_num)) {
        k = 0;

        while (k < NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
            block_pos = 0;

            while (block_pos < EXT2_BLOCK_SIZE) {
                cur_entry = get_entry(ino->i_block[k], block_pos);

                memcpy(current_name, cur_entry->name, cur_entry->name_len);
                current_name[cur_entry->name_len] = '\0';

                if (!IS_DOT_ENTRY(current_name)) {
                    int ret_val = is_recoverable(cur_entry->inode, FALSE);
                    if (ret_val < 0)
                        return ret_val;
                }

                block_pos += cur_entry->rec_len;
            }

            k++;
        }
    }

    return 1;
}

/*
 * Recover the directory entry with the given name that has previously been
 * removed (using ext2_rm) from the directory referred to by parent_inode. If
 * this entry is itself a directory, also attempt to recover as many of its 
 * entries as possible.
 */
void restore_entry (unsigned int parent_inode, char *entry_name) 
{
    struct ext2_inode *parent_ino = get_inode(parent_inode);
    struct ext2_dir_entry *cur_entry;
    struct ext2_dir_entry *prev_intact;

    int k = 0;
    int found = 0;
    int dir_entry_size = sizeof(struct ext2_dir_entry);
    int actual_cur_len;

    unsigned long block_pos;
    unsigned long next_intact_pos;
    unsigned long prev_intact_distance;

    char current_name[EXT2_NAME_LEN + 1];

    /* Search through the parent inode's blocks for the removed entry */    
    while (!found && k < NUM_INITIAL_DIRECT_BLOCKS && parent_ino->i_block[k]) {
        block_pos = 0;

        while (!found && block_pos < EXT2_BLOCK_SIZE) {
            cur_entry = get_entry(parent_ino->i_block[k], block_pos);
            prev_intact = cur_entry;

            next_intact_pos = block_pos + cur_entry->rec_len;
            prev_intact_distance = 0;

            actual_cur_len = PAD_REC_LEN(dir_entry_size + cur_entry->name_len);
            block_pos += actual_cur_len;

            /* Search the gap between the latest intact entry and the next
             * intact entry, if such a gap exists */
            while (!found && block_pos < next_intact_pos) {
                cur_entry = get_entry(parent_ino->i_block[k], block_pos);
                memcpy(current_name, cur_entry->name, cur_entry->name_len);
                current_name[cur_entry->name_len] = '\0';

                prev_intact_distance += actual_cur_len;

                if (!strcmp(current_name, entry_name)) {
                    cur_entry->rec_len = prev_intact->rec_len - prev_intact_distance;
                    prev_intact->rec_len = prev_intact_distance;    

                    /* We are not restoring any hard links, thus we can assume
                     * that this entry's inode has no other links and its 
                     * resources now need to be reallocated */
                    reallocate_resources(cur_entry->inode);
                    found = 1;
                
                } else {
                    /* Otherwise, advance to the next deleted entry slot */
                    actual_cur_len = PAD_REC_LEN(dir_entry_size + cur_entry->name_len);
                    block_pos += actual_cur_len;
                }
            }
        }

        k++;
    }
}

/*
 * Reallocate the given inode's number as well as all its data blocks, and
 * update the associated counters. If the inode is a directory, also attempt
 * to reallocate the resources of as many of its entries as possible.
 */
void reallocate_resources (unsigned int inode_num) 
{
    struct ext2_inode *ino = get_inode(inode_num);
    struct ext2_inode *cur_ino;
    struct ext2_dir_entry *cur_entry;

    unsigned int direct_block;
    unsigned int *indirect_pos;
    unsigned int *indirect_end;
    
    unsigned long block_pos;
    
    char current_name[EXT2_NAME_LEN + 1];
    
    int k = 0;
    int is_file_with_no_links;
    int is_non_dotted_dir;

    /* If we fail to reallocate the inode of the directory referred to by this
     * inode then we terminate this reallocation call. Note that if this
     * attempt fails, we know the inode is a directory since we know we are
     * not restoring any files with existing links. */
    if (!attempt_inode_reallocation(inode_num))
        return;

    /* If inode reallocation succeeded, we proceed with trying to recover as
     * many of its blocks as possible. */
    while (k < NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
        attempt_block_reallocation(ino->i_block[k]);
        k++;
    }

    if (k == NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
        attempt_block_reallocation(ino->i_block[k]);

        indirect_pos = (unsigned int *) get_block(ino->i_block[k]);
        indirect_end = indirect_pos + (EXT2_BLOCK_SIZE / sizeof(unsigned int));
        direct_block = *indirect_pos;

        while (direct_block && indirect_pos < indirect_end) {
            attempt_block_reallocation(direct_block);
            indirect_pos++;
            direct_block = *indirect_pos;
        }
    }

    /* If this inode is a directory, we need to recursively attempt to free as
     * many of its entries as possible that are also directories, or files
     * with no existing links. */
    if (is_dir(inode_num)) {
        k = 0;

        while (k < NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
            block_pos = 0;

            while (block_pos < EXT2_BLOCK_SIZE) {
                cur_entry = get_entry(ino->i_block[k], block_pos);

                memcpy(current_name, cur_entry->name, cur_entry->name_len);
                current_name[cur_entry->name_len] = '\0';
                cur_ino = get_inode(cur_entry->inode);

                is_file_with_no_links = !is_dir(cur_entry->inode) && 
                    (!cur_ino->i_links_count);
                is_non_dotted_dir = is_dir(cur_entry->inode) && 
                    !IS_DOT_ENTRY(current_name);

                if (is_file_with_no_links || is_non_dotted_dir) {
                    /* In this case, we recursively attempt to reallocate this
                     * entry's resources. */    
                    reallocate_resources(cur_entry->inode);
                
                } else {
                    /* Otherwise, we simply increment its inode's links count
                     * and move on. */
                    cur_ino->i_links_count++;
                }

                block_pos += cur_entry->rec_len;
            }

            k++;
        }
    }

    /* Deletion time for a newly created or reallocated inode should be 
     * set to 0, and its link count should be incremented. */
    ino->i_dtime = 0;
    ino->i_links_count++;
    if (is_dir(inode_num))
        get_group_desc()->bg_used_dirs_count++;
}

/*
 * If the specified inode number is free, mark it as used, update the free 
 * inode counters and return 1. Otherwise, return 0.
 */
int attempt_inode_reallocation (unsigned int inode_num) 
{
    unsigned char *inode_bitmap = get_inode_bitmap();
    int bit = GET_BIT(inode_num);
    int byte = GET_BYTE(inode_num);

    if (!IN_USE(inode_bitmap, byte, bit)) {
        MARK_AS_USED(inode_bitmap, byte, bit);
        get_super_block()->s_free_inodes_count--;
        get_group_desc()->bg_free_inodes_count--;
        return 1;
    }

    return 0;
}

/*
 * If the specified block number is free, mark it as used, update the free
 * block counters.
 */
void attempt_block_reallocation (unsigned int block_num) 
{
    unsigned char *block_bitmap = get_block_bitmap();
    int bit = GET_BIT(block_num);
    int byte = GET_BYTE(block_num);

    if (!IN_USE(block_bitmap, byte, bit)) {
        MARK_AS_USED(block_bitmap, byte, bit);
        get_super_block()->s_free_blocks_count--;
        get_group_desc()->bg_free_blocks_count--;
    }
}

/* 
 * Return 1 if the given inode refers to a directory on the current disk,
 * and returns 0 otherwise.
 */
int is_dir (unsigned int inode) 
{
    struct ext2_inode *ino = get_inode(inode);
    return TYPE_MASK(ino->i_mode) == EXT2_S_IFDIR;
}

/*
 * Return a pointer to the file system's super block.
 */
struct ext2_super_block *get_super_block () 
{
    struct ext2_super_block *sb = (struct ext2_super_block *) (
        (unsigned char *) disk + EXT2_BLOCK_SIZE);
    return sb;
}

/*
 * Return a pointer to the (sole) block group descriptor.
 */
struct ext2_group_desc *get_group_desc () 
{
    struct ext2_group_desc *gd = (struct ext2_group_desc *) (
        (unsigned char *) disk + 2*EXT2_BLOCK_SIZE);
    return gd;
}

/*
 * Return a pointer to the block bitmap on disk.
 */
unsigned char *get_block_bitmap () 
{
    int block_bitmap_offset = get_group_desc()->bg_block_bitmap * EXT2_BLOCK_SIZE;
    unsigned char *block_bitmap = (unsigned char *) (disk + block_bitmap_offset);
    return block_bitmap;
}

/*
 * Return a pointer to the inode bitmap on disk.
 */
unsigned char *get_inode_bitmap () 
{
    int inode_bitmap_offset = get_group_desc()->bg_inode_bitmap * EXT2_BLOCK_SIZE;
    unsigned char *inode_bitmap = (unsigned char *) (disk + inode_bitmap_offset);
    return inode_bitmap;
}

/*
 * Return a pointer to the inode table on disk.
 */
unsigned char *get_inode_table () 
{
    int inode_table_offset = get_group_desc()->bg_inode_table * EXT2_BLOCK_SIZE;
    unsigned char *inode_table = (unsigned char *) (disk + inode_table_offset);
    return inode_table;
}

/*
 * Return a pointer to the inode structure with the given number.
 */
struct ext2_inode *get_inode (unsigned int inode) 
{
    struct ext2_inode *ino = (struct ext2_inode *) ((unsigned char *) get_inode_table() +
        INDEX(inode) * sizeof(struct ext2_inode));
    return ino;
}

/*
 * Return a pointer to the directory entry at the given block and position.
 */
struct ext2_dir_entry *get_entry (unsigned int block_num, unsigned long block_pos) 
{
    struct ext2_dir_entry *entry = (struct ext2_dir_entry *) ((unsigned char *)disk +
        block_num * EXT2_BLOCK_SIZE + block_pos);
    return entry;
}

/*
 * Return a pointer to the beginning of the block with the given number.
 */
unsigned char *get_block (unsigned int block_num) 
{
    unsigned char *block = (unsigned char *) (disk + block_num * EXT2_BLOCK_SIZE);
    return block;
}

/*
 * Return the inode mode corresponding to the given directory entry
 * file type.
 */
unsigned short get_imode (unsigned char type) 
{
    switch (type) {
        case EXT2_FT_DIR:
            return EXT2_S_IFDIR;
        case EXT2_FT_SYMLINK:
            return EXT2_S_IFLNK;
        default:
            return EXT2_S_IFREG;
    }
}

/*
 * Return the directory entry file type corresponding to the given
 * inode mode.
 */
unsigned char get_file_type (unsigned short mode) 
{
    switch (TYPE_MASK(mode)) {
        case EXT2_S_IFDIR:
            return EXT2_FT_DIR;
        case EXT2_S_IFLNK:
            return EXT2_FT_SYMLINK;
        default:
            return EXT2_FT_REG_FILE;
    }
}
