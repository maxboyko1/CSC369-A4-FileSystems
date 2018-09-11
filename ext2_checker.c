#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ext2_utils.h"

unsigned char *disk = NULL;


/*
 * Repair any initial inconsistencies between the block and inode bitmaps
 * and their respective free block and inode counters in the superblock and
 * block group descriptor, trusting the bitmaps. Note that these bitmaps may
 * be corrupted, in which case they will be fixed and the counters will be 
 * re-updated in a later step. Return the number of fixes in this step.
 */
int initial_counter_fix () 
{
    struct ext2_super_block *sb = get_super_block();
    struct ext2_group_desc *gd = get_group_desc();
    unsigned char *block_bitmap = get_block_bitmap();
    unsigned char *inode_bitmap = get_inode_bitmap();

    int bit, byte, num_bytes;
    int diff; 
    int free_blocks = 0;
    int free_inodes = 0;
    int num_fixes = 0;

    /* Get actual number of blocks marked as free in bitmap */
    num_bytes = sb->s_blocks_count / NUM_BITS;
    bit = 0; byte = 0;

    while (byte < num_bytes) {
        while (bit < NUM_BITS) {
            if (!IN_USE(block_bitmap, byte, bit))
                free_blocks++;
            bit++;
        }

        bit = 0;
        byte++;
    }
    
    /* Repair free block counters, if necessary */
    if (free_blocks != sb->s_free_blocks_count) {
        diff = abs(free_blocks - sb->s_free_blocks_count);
        sb->s_free_blocks_count = free_blocks;

        printf("Fixed: superblock's free blocks counter was off by %d compared to the bitmap\n",
            diff);
        num_fixes += diff;
    }

    if (free_blocks != gd->bg_free_blocks_count) {
        diff = abs(free_blocks - gd->bg_free_blocks_count);
        gd->bg_free_blocks_count = free_blocks;

        printf("Fixed: block group's free blocks counter was off by %d compared to the bitmap\n",
            diff);
        num_fixes += diff;
    }

    /* Get actual number of inodes marked as free in bitmap */
    num_bytes = sb->s_inodes_count / NUM_BITS;
    bit = 0; byte = 0;

    while (byte < num_bytes) {
        while (bit < NUM_BITS) {
            if (!IN_USE(inode_bitmap, byte, bit))
                free_inodes++;
            bit++;
        }

        bit = 0;
        byte++;
    }

    /* Repair free inode counters, if necessary */
    if (free_inodes != sb->s_free_inodes_count) {
        diff = abs(free_inodes - sb->s_free_inodes_count);
        sb->s_free_inodes_count = free_inodes;

        printf("Fixed: superblock's free inodes counter was off by %d compared to the bitmap\n",
            diff);
        num_fixes += diff;
    }

    if (free_inodes != gd->bg_free_inodes_count) {
        diff = abs(free_inodes - gd->bg_free_inodes_count);
        gd->bg_free_inodes_count = free_inodes;

        printf("Fixed: block group's free inodes counter was off by %d compared to the bitmap\n",
            diff);
        num_fixes += diff;
    }

    return num_fixes;
}

/*
 * If there is a mismatch between the given entry's file type and the
 * corresponding inode's mode, update the file type and return 1. Otherwise,
 * return 0.
 */
int fix_file_type (struct ext2_dir_entry *entry) 
{
    struct ext2_inode *ino = get_inode(entry->inode);

    if (TYPE_MASK(ino->i_mode) != get_imode(entry->file_type)) {
        entry->file_type = get_file_type(ino->i_mode);
        printf("Fixed: Entry type vs inode mismatch: inode [%d]\n", 
            entry->inode);
        return 1;
    }

    return 0;
}

/*
 * If the given entry's inode is not marked as allocated in the inode bitmap,
 * set it, update the free inode counters and return 1. Otherwise, return 0.
 */
int fix_inode_bitmap (struct ext2_dir_entry *entry) 
{
    unsigned char *inode_bitmap = get_inode_bitmap();
    int bit = GET_BIT(entry->inode);
    int byte = GET_BYTE(entry->inode);

    if (!IN_USE(inode_bitmap, byte, bit)) {
        MARK_AS_USED(inode_bitmap, byte, bit);
        get_super_block()->s_free_inodes_count--;
        get_group_desc()->bg_free_inodes_count--;

        printf("Fixed: inode [%d] not marked as in-use\n",
            entry->inode);
        return 1;
    }

    return 0;
}

/*
 * If the given entry's inode has its deletion time set to a value
 * greater than 0, reset it and return 1. Otherwise, return 0.
 */
int fix_deletion_time (struct ext2_dir_entry *entry) 
{
    struct ext2_inode *ino = get_inode(entry->inode);

    if (ino->i_dtime) {
        ino->i_dtime = 0;
        printf("Fixed: valid inode marked for deletion [%d]\n",
            entry->inode);
        return 1;
    }

    return 0;
}

/*
 * If the given block is not marked as allocated in the data block
 * bitmap, set it and return 1. Otherwise, return 0.
 */
int fix_block (unsigned int block) 
{
    unsigned char *block_bitmap = get_block_bitmap();
    int bit = GET_BIT(block);
    int byte = GET_BYTE(block);

    if (!IN_USE(block_bitmap, byte, bit)) {
        MARK_AS_USED(block_bitmap, byte, bit);
        get_super_block()->s_free_blocks_count--;
        get_group_desc()->bg_free_blocks_count--;
        return 1;
    }

    return 0;
}

/*
 * If any of the given entry's data blocks are not marked as allocated 
 * in the data block bitmap, set it and update the free block counters.
 * Return the number of blocks fixed.
 */
int fix_block_bitmap (struct ext2_dir_entry *entry) 
{
    struct ext2_inode *ino = get_inode(entry->inode);
    unsigned int *block_pos;
    unsigned int *block_end;
    
    int k = 0; 
    int blocks_fixed = 0;

    while (k < NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
        blocks_fixed += fix_block(ino->i_block[k]);
        k++;
    }

    /* If the current entry is a file large enough to require a single
     * indirect block, we must check all the data blocks the indirect
     * block points to as well */
    if (k == NUM_INITIAL_DIRECT_BLOCKS && ino->i_block[k]) {
        block_pos = (unsigned int *) get_block(ino->i_block[k]);
        block_end = block_pos + (EXT2_BLOCK_SIZE / sizeof(unsigned int));

        /* Every nonzero entry in the indirect block is a direct block number
         * that should be allocated in the block bitmap */
        while (*block_pos && block_pos < block_end) {
            blocks_fixed += fix_block(*block_pos);
            block_pos++;
        }
    }

    if (blocks_fixed)
        printf("Fixed: %d in-use data blocks not marked in data bitmap for inode [%d]\n",
            blocks_fixed, entry->inode);

    return blocks_fixed;
}

/*
 * Beginning from the given directory entry, recursively repair any
 * other inconsistencies for each directory entry encountered, and return the
 * number of repairs. The second argument notes whether or not this is the 
 * first recursion.
 */
int recursively_fix_dir_entries (struct ext2_dir_entry *entry, int is_first) 
{
    int num_fixes = fix_file_type(entry);
    num_fixes += fix_inode_bitmap(entry);
    num_fixes += fix_deletion_time(entry);
    num_fixes += fix_block_bitmap(entry);

    struct ext2_inode *inode = get_inode(entry->inode);
    char name[EXT2_NAME_LEN + 1];
    memcpy(name, entry->name, entry->name_len);
    name[entry->name_len] = '\0';

    int k; 
    int is_dir = entry->file_type == EXT2_FT_DIR;
    unsigned long block_pos;
    struct ext2_dir_entry *cur_entry;

    /* We only need to recurse on entries that are directories and not .
     * or .., unless it is the . entry in the root at the very beginning */
    if (is_dir && (!IS_DOT_ENTRY(name) || is_first)) {
        k = 0;

        while (k < NUM_INITIAL_DIRECT_BLOCKS && inode->i_block[k]) {
            block_pos = 0;

            while (block_pos < EXT2_BLOCK_SIZE) {
                cur_entry = get_entry(inode->i_block[k], block_pos);
                if (cur_entry->inode)
                    num_fixes += recursively_fix_dir_entries(cur_entry, FALSE);
                block_pos += cur_entry->rec_len;
            }

            k++;
        }
    }

    return num_fixes;
}

int main (int argc, char **argv) 
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image file path>\n", argv[0]);
        exit(1);
    }

    init_disk(argv[1]);

    struct ext2_inode *root_ino = get_inode(EXT2_ROOT_INO);
    struct ext2_dir_entry *root_entry = get_entry(root_ino->i_block[0], 0);
    
    int total_fixes = initial_counter_fix() + 
        recursively_fix_dir_entries(root_entry, TRUE);

    if (total_fixes)
        printf("%d file system inconsistencies repaired!\n", total_fixes);
    else 
        printf("No file system inconsistencies detected!\n");

    return 0;
}
