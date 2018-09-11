#include <string.h>
#include "ext2.h"

/* Macro definitions */
#define DISK_BLOCKS 128
#define DISK_SECTOR_SIZE 512
#define NUM_BITS 8
#define NUM_INITIAL_DIRECT_BLOCKS 12
#define TRUE 1
#define FALSE 0

#define GET_BIT(x) ((x - 1) % NUM_BITS)
#define GET_BYTE(x) ((x - 1) / NUM_BITS)
#define HAS_TRAILING_SLASH(PATH) (PATH[strlen(PATH) - 1] == '/')
#define INDEX(x) (x - 1)
#define IN_USE(BITMAP, BYTE, BIT) (BITMAP[BYTE] & (1 << BIT))
#define IS_ABSOLUTE(PATH) (PATH[0] == '/')
#define IS_DOT_ENTRY(NAME) (!strcmp(NAME, ".") || !strcmp(NAME, ".."))
#define MARK_AS_FREE(BITMAP, BYTE, BIT) (BITMAP[BYTE] &= ~(1 << BIT))
#define MARK_AS_USED(BITMAP, BYTE, BIT) (BITMAP[BYTE] |= (1 << BIT))
#define PAD_REC_LEN(x) ((x + 3) & ~3)
#define TYPE_MASK(x) (x & ~4095)
#define ZERO_OR_NEG_ONE(IS_FIRST) (IS_FIRST ? 0 : -1)

/* Global variable re-declarations */
extern unsigned char *disk;

/* Utility function declarations */
void init_disk (char *diskpath);
unsigned int get_inode_at_path (char *path);
unsigned int allocate_inode ();
unsigned int allocate_block ();
unsigned int find_entry (unsigned int parent_inode, char *entry_name);
void create_entry (unsigned int parent_inode, unsigned int entry_inode, 
        char *entry_name, unsigned char type);
void init_inode (struct ext2_inode *ino, unsigned char type);
void write_to_inode (unsigned int inode, char *contents);
void remove_entry (unsigned int parent_inode, char *entry_name);
void free_resources (unsigned int inode_num, char *entry_name);
void deallocate_inode (unsigned int inode_num);
void deallocate_block (unsigned int block_num);
unsigned int find_removed_entry (unsigned int parent_inode, char *entry_name);
int is_recoverable (unsigned int inode_num, int is_first);
void restore_entry (unsigned int parent_inode, char *entry_name);
void reallocate_resources (unsigned int inode_num);
int attempt_inode_reallocation (unsigned int inode_num);
void attempt_block_reallocation (unsigned int block_num);
int is_dir (unsigned int inode);

struct ext2_super_block *get_super_block ();
struct ext2_group_desc *get_group_desc ();
unsigned char *get_block_bitmap ();
unsigned char *get_inode_bitmap ();
unsigned char *get_inode_table ();
struct ext2_inode *get_inode (unsigned int inode);
struct ext2_dir_entry *get_entry (unsigned int block_num, unsigned long block_pos);
unsigned char *get_block (unsigned int block_num);
unsigned short get_imode (unsigned char type);
unsigned char get_file_type (unsigned short mode);
