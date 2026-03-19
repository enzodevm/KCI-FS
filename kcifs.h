#ifndef KCI_FS_H
#define KCI_FS_H

#include <stdint.h>
#include <stddef.h>

#define KCI_MAGIC 0x4B434946
#define KCI_BLOCK_SIZE 512

#define KCI_MAX_BLOCKS 4096
#define KCI_MAX_INODES 256
#define KCI_MAX_BLOCKS_PER_FILE 12
#define KCI_MAX_NAME 32

#define KCI_TYPE_FILE 1
#define KCI_TYPE_DIR  2

// ======= DRIVER OBRIGATÓRIO =======
extern int block_read(uint64_t lba, void* buf);
extern int block_write(uint64_t lba, const void* buf);

// ======= ESTRUTURAS =======

typedef struct {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t inode_table_lba;
    uint32_t data_start_lba;
} kci_superblock_t;

typedef struct {
    uint8_t used;
    uint8_t type;
    uint32_t size;
    uint32_t blocks[KCI_MAX_BLOCKS_PER_FILE];
} kci_inode_t;

typedef struct {
    char name[KCI_MAX_NAME];
    uint32_t inode_index;
} kci_dir_entry_t;

// ======= ÁREA EM MEMÓRIA =======

static kci_superblock_t kci_sb;
static kci_inode_t kci_inodes[KCI_MAX_INODES];

static uint8_t kci_block_bitmap[KCI_MAX_BLOCKS / 8];
static uint8_t kci_inode_bitmap[KCI_MAX_INODES / 8];

// ======= BITMAP HELPERS =======

static void bitmap_set(uint8_t* map, uint32_t idx) {
    map[idx / 8] |= (1 << (idx % 8));
}

static void bitmap_clear(uint8_t* map, uint32_t idx) {
    map[idx / 8] &= ~(1 << (idx % 8));
}

static int bitmap_test(uint8_t* map, uint32_t idx) {
    return map[idx / 8] & (1 << (idx % 8));
}

// ======= SINCRONIZAÇÃO =======

static int kci_sync_bitmaps() {
    block_write(1, kci_block_bitmap);
    block_write(2, kci_inode_bitmap);
    return 0;
}

static int kci_sync_inodes() {
    return block_write(kci_sb.inode_table_lba, kci_inodes);
}

// ======= FORMATAR =======

static int kci_format(uint32_t total_blocks) {

    kci_sb.magic = KCI_MAGIC;
    kci_sb.total_blocks = total_blocks;
    kci_sb.inode_table_lba = 3;
    kci_sb.data_start_lba = 35;

    block_write(0, &kci_sb);

    for (int i = 0; i < sizeof(kci_block_bitmap); i++)
        kci_block_bitmap[i] = 0;

    for (int i = 0; i < sizeof(kci_inode_bitmap); i++)
        kci_inode_bitmap[i] = 0;

    for (int i = 0; i < KCI_MAX_INODES; i++)
        kci_inodes[i].used = 0;

    // inode 0 = root
    kci_inodes[0].used = 1;
    kci_inodes[0].type = KCI_TYPE_DIR;
    kci_inodes[0].size = 0;
    bitmap_set(kci_inode_bitmap, 0);

    kci_sync_bitmaps();
    kci_sync_inodes();

    return 0;
}

// ======= MONTAR =======

static int kci_mount() {
    if (block_read(0, &kci_sb) != 0)
        return -1;

    if (kci_sb.magic != KCI_MAGIC)
        return -2;

    block_read(1, kci_block_bitmap);
    block_read(2, kci_inode_bitmap);
    block_read(kci_sb.inode_table_lba, kci_inodes);

    return 0;
}

// ======= ALOCAÇÃO =======

static int kci_alloc_block() {
    for (int i = 0; i < KCI_MAX_BLOCKS; i++) {
        if (!bitmap_test(kci_block_bitmap, i)) {
            bitmap_set(kci_block_bitmap, i);
            kci_sync_bitmaps();
            return kci_sb.data_start_lba + i;
        }
    }
    return -1;
}

static int kci_alloc_inode(uint8_t type) {
    for (int i = 1; i < KCI_MAX_INODES; i++) {
        if (!bitmap_test(kci_inode_bitmap, i)) {
            bitmap_set(kci_inode_bitmap, i);
            kci_inodes[i].used = 1;
            kci_inodes[i].type = type;
            kci_inodes[i].size = 0;
            kci_sync_bitmaps();
            return i;
        }
    }
    return -1;
}

// ======= PATH RESOLVE =======

static int kci_find_in_dir(uint32_t dir_inode, const char* name) {

    kci_inode_t* dir = &kci_inodes[dir_inode];

    uint8_t buffer[KCI_BLOCK_SIZE];

    for (int b = 0; b < KCI_MAX_BLOCKS_PER_FILE; b++) {

        if (!dir->blocks[b]) continue;

        block_read(dir->blocks[b], buffer);

        kci_dir_entry_t* entries = (kci_dir_entry_t*)buffer;

        for (int i = 0; i < KCI_BLOCK_SIZE / sizeof(kci_dir_entry_t); i++) {

            int match = 1;
            for (int j = 0; j < KCI_MAX_NAME; j++) {
                if (entries[i].name[j] != name[j]) {
                    match = 0;
                    break;
                }
                if (name[j] == 0) break;
            }

            if (match)
                return entries[i].inode_index;
        }
    }

    return -1;
}

static int kci_resolve_path(const char* path) {

    if (path[0] != '/') return -1;

    uint32_t current = 0;

    char name[KCI_MAX_NAME];
    int n = 0;

    for (int i = 1;; i++) {

        if (path[i] == '/' || path[i] == 0) {

            name[n] = 0;
            int next = kci_find_in_dir(current, name);
            if (next < 0) return -1;

            current = next;
            n = 0;

            if (path[i] == 0)
                return current;

        } else {
            if (n < KCI_MAX_NAME - 1)
                name[n++] = path[i];
        }
    }
}

// ======= CRIAR ENTRADA =======

static int kci_add_dir_entry(uint32_t dir_inode, const char* name, uint32_t inode_idx) {

    kci_inode_t* dir = &kci_inodes[dir_inode];

    if (!dir->blocks[0])
        dir->blocks[0] = kci_alloc_block();

    uint8_t buffer[KCI_BLOCK_SIZE];
    block_read(dir->blocks[0], buffer);

    kci_dir_entry_t* entries = (kci_dir_entry_t*)buffer;

    int max = KCI_BLOCK_SIZE / sizeof(kci_dir_entry_t);

    for (int i = 0; i < max; i++) {

        if (entries[i].inode_index == 0) {

            for (int j = 0; j < KCI_MAX_NAME; j++)
                entries[i].name[j] = name[j];

            entries[i].inode_index = inode_idx;

            block_write(dir->blocks[0], buffer);

            dir->size += sizeof(kci_dir_entry_t);
            kci_sync_inodes();
            return 0;
        }
    }

    return -1;
}

// ======= MKDIR =======

static int kci_mkdir(const char* path) {

    int parent = 0;
    const char* name = path + 1;

    int inode = kci_alloc_inode(KCI_TYPE_DIR);
    if (inode < 0) return -1;

    return kci_add_dir_entry(parent, name, inode);
}

// ======= CREATE FILE =======

static int kci_create(const char* path) {

    int parent = 0;
    const char* name = path + 1;

    int inode = kci_alloc_inode(KCI_TYPE_FILE);
    if (inode < 0) return -1;

    return kci_add_dir_entry(parent, name, inode);
}

// ======= WRITE =======

static int kci_write(const char* path, const void* data, uint32_t size) {

    int inode = kci_resolve_path(path);
    if (inode < 0) return -1;

    kci_inode_t* node = &kci_inodes[inode];

    uint32_t blocks = (size + KCI_BLOCK_SIZE - 1) / KCI_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {

        int lba = kci_alloc_block();
        if (lba < 0) return -1;

        node->blocks[i] = lba;

        block_write(lba,
            (void*)((uint8_t*)data + i * KCI_BLOCK_SIZE));
    }

    node->size = size;
    kci_sync_inodes();
    return 0;
}

// ======= READ =======

static int kci_read(const char* path, void* buffer) {

    int inode = kci_resolve_path(path);
    if (inode < 0) return -1;

    kci_inode_t* node = &kci_inodes[inode];

    uint32_t blocks = (node->size + KCI_BLOCK_SIZE - 1) / KCI_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++)
        block_read(node->blocks[i],
            (void*)((uint8_t*)buffer + i * KCI_BLOCK_SIZE));

    return node->size;
}

#endif
