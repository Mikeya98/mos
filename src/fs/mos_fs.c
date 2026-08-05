/**
 * @file    mos_fs.c
 * @brief   MOS-FS 极简文件系统 — 磁盘格式底层操作
 *
 * 所有函数操作 block_dev_t, 不维护全局状态。
 * 全局状态 (挂载点、打开文件表) 由 vfs.c 管理。
 *
 * 关键约定:
 *   - inode 0 = 根目录 (在 mkfs 时创建, 不可删除)
 *   - 块号 0 表示 "未分配" (inode.blocks[i] = 0)
 *   - 目录 inode 的 size 必须是 512 的整数倍 (对齐到块)
 *   - dirent 中 inode_num=0 表示空项
 */

#include "fs/mos_fs.h"
#include "lib/string.h"

/* =========================== 内部工具 =========================== */

/** 临时读写缓冲区 (栈上太费, 用静态的, 不可重入) */
static u8 temp_buf[512];

/* =========================== 超级块 =========================== */

int mos_fs_read_superblock(block_dev_t *dev, fs_superblock_t *sb)
{
    if (dev->read(dev, 0, temp_buf) != 0) return -1;

    u32 i;
    u32 *src = (u32 *)temp_buf;
    u32 *dst = (u32 *)sb;
    for (i = 0; i < 11; i++) {
        dst[i] = src[i];
    }
    return 0;
}

int mos_fs_write_superblock(block_dev_t *dev, const fs_superblock_t *sb)
{
    u32 i;
    for (i = 0; i < 128; i++) {
        ((u32 *)temp_buf)[i] = 0;
    }

    u32 *src = (u32 *)sb;
    u32 *dst = (u32 *)temp_buf;
    for (i = 0; i < 32; i++) {
        dst[i] = src[i];
    }
    return dev->write(dev, 0, temp_buf);
}

/* =========================== Inode 管理 =========================== */

int mos_fs_read_inode(block_dev_t *dev, u32 inode_num, fs_inode_t *inode)
{
    if (inode_num >= MOS_FS_MAX_INODES) return -1;

    /* 计算 inode 所在块号和块内偏移 */
    fs_superblock_t sb;
    if (mos_fs_read_superblock(dev, &sb) != 0) return -1;

    u32 block_no = sb.inode_table_start + (inode_num / MOS_FS_INODES_PER_BLOCK);
    u32 offset   = (inode_num % MOS_FS_INODES_PER_BLOCK) * sizeof(fs_inode_t);

    if (dev->read(dev, block_no, temp_buf) != 0) return -1;

    /* 从 temp_buf 复制 inode (使用 sizeof 避免溢出) */
    u32 i;
    u32 *src = (u32 *)(temp_buf + offset);
    u32 *dst = (u32 *)inode;
    for (i = 0; i < MOS_FS_INODE_U32_COUNT; i++) {
        dst[i] = src[i];
    }
    return 0;
}

int mos_fs_write_inode(block_dev_t *dev, u32 inode_num, const fs_inode_t *inode)
{
    if (inode_num >= MOS_FS_MAX_INODES) return -1;

    fs_superblock_t sb;
    if (mos_fs_read_superblock(dev, &sb) != 0) return -1;

    u32 block_no = sb.inode_table_start + (inode_num / MOS_FS_INODES_PER_BLOCK);
    u32 offset   = (inode_num % MOS_FS_INODES_PER_BLOCK) * sizeof(fs_inode_t);

    /* 先读整个块, 修改对应 inode, 再写回 */
    if (dev->read(dev, block_no, temp_buf) != 0) return -1;

    u32 i;
    u32 *src = (u32 *)inode;
    u32 *dst = (u32 *)(temp_buf + offset);
    for (i = 0; i < MOS_FS_INODE_U32_COUNT; i++) {
        dst[i] = src[i];
    }

    return dev->write(dev, block_no, temp_buf);
}

int mos_fs_alloc_inode(block_dev_t *dev, u16 mode)
{
    fs_superblock_t sb;
    fs_inode_t inode;

    if (mos_fs_read_superblock(dev, &sb) != 0) return -1;
    if (sb.free_inodes == 0) return -1;

    /* 遍历 inode 表找空闲 inode (mode == 0 表示空闲) */
    u32 i;
    for (i = 0; i < sb.inode_count; i++) {
        if (mos_fs_read_inode(dev, i, &inode) != 0) continue;
        if (inode.mode == 0) {
            /* 初始化 inode: 逐字段清零 */
            u32 j;
            inode.mode = mode;
            inode.nlinks = 1;
            inode.size = 0;
            for (j = 0; j < MOS_FS_DIRECT_BLOCKS; j++) inode.blocks[j] = 0;
            inode.indirect = 0;
            for (j = 0; j < 8; j++) inode.reserved[j] = 0;

            mos_fs_write_inode(dev, i, &inode);

            sb.free_inodes--;
            mos_fs_write_superblock(dev, &sb);

            return (int)i;
        }
    }
    return -1;
}

int mos_fs_free_inode(block_dev_t *dev, u32 inode_num)
{
    fs_superblock_t sb;
    fs_inode_t inode;

    if (inode_num >= MOS_FS_MAX_INODES) return -1;
    if (inode_num == 0) return -1;  /* 根目录不能释放 */

    if (mos_fs_read_superblock(dev, &sb) != 0) return -1;
    if (mos_fs_read_inode(dev, inode_num, &inode) != 0) return -1;

    /* 释放所有数据块 */
    u32 i;
    for (i = 0; i < MOS_FS_DIRECT_BLOCKS; i++) {
        if (inode.blocks[i] != 0) {
            mos_fs_free_block(dev, inode.blocks[i]);
            inode.blocks[i] = 0;
        }
    }

    /* 清零 inode: 逐字段清零 */
    u32 j;
    inode.mode = 0;
    inode.nlinks = 0;
    inode.size = 0;
    for (j = 0; j < MOS_FS_DIRECT_BLOCKS; j++) inode.blocks[j] = 0;
    inode.indirect = 0;
    for (j = 0; j < 8; j++) inode.reserved[j] = 0;

    mos_fs_write_inode(dev, inode_num, &inode);

    sb.free_inodes++;
    mos_fs_write_superblock(dev, &sb);

    return 0;
}

/* =========================== 块分配 / 释放 =========================== */

int mos_fs_alloc_block(block_dev_t *dev)
{
    fs_superblock_t sb;
    if (mos_fs_read_superblock(dev, &sb) != 0) return -1;
    if (sb.free_blocks == 0) return -1;

    /* 读块位图 (在 bitmap_start 块) */
    if (dev->read(dev, sb.bitmap_start, temp_buf) != 0) return -1;

    /* 在数据块范围内 (block sb.data_start ~ sb.total_blocks-1) 找空闲位 */
    u32 total_data_blocks = sb.total_blocks - sb.data_start;
    u32 byte_idx, bit_idx;
    int found = -1;

    for (byte_idx = 0; byte_idx < (total_data_blocks + 7) / 8; byte_idx++) {
        if (temp_buf[byte_idx] != 0xFF) {  /* 有空位 */
            for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                u32 block_offset = byte_idx * 8 + bit_idx;
                if (block_offset >= total_data_blocks) break;
                if (!(temp_buf[byte_idx] & (1u << bit_idx))) {
                    /* 找到空闲块 */
                    temp_buf[byte_idx] |= (1u << bit_idx);
                    found = (int)(sb.data_start + block_offset);
                    break;
                }
            }
            if (found >= 0) break;
        }
    }

    if (found < 0) return -1;

    /* 写回位图 */
    dev->write(dev, sb.bitmap_start, temp_buf);

    /* 清零新块 (避免读到旧数据) */
    u32 i;
    for (i = 0; i < 128; i++) ((u32 *)temp_buf)[i] = 0;
    dev->write(dev, (u32)found, temp_buf);

    /* 更新空闲计数 */
    sb.free_blocks--;
    mos_fs_write_superblock(dev, &sb);

    return found;
}

int mos_fs_free_block(block_dev_t *dev, u32 block_no)
{
    fs_superblock_t sb;
    if (mos_fs_read_superblock(dev, &sb) != 0) return -1;
    if (block_no < sb.data_start || block_no >= sb.total_blocks) return -1;

    /* 读位图 */
    if (dev->read(dev, sb.bitmap_start, temp_buf) != 0) return -1;

    /* 清位 */
    u32 block_offset = block_no - sb.data_start;
    u32 byte_idx = block_offset / 8;
    u32 bit_idx  = block_offset % 8;
    temp_buf[byte_idx] &= ~(1u << bit_idx);

    /* 写回位图 */
    dev->write(dev, sb.bitmap_start, temp_buf);

    sb.free_blocks++;
    mos_fs_write_superblock(dev, &sb);

    return 0;
}

/* =========================== 目录操作 =========================== */

int mos_fs_lookup(block_dev_t *dev, const fs_inode_t *dir_inode,
                  const char *name)
{
    fs_dirent_t dirent;
    u32 blocks_used;
    u32 block_idx, entry_idx;

    if (dir_inode->mode != FT_DIR) return -1;

    blocks_used = (dir_inode->size + 511) / 512;
    if (blocks_used > MOS_FS_DIRECT_BLOCKS) blocks_used = MOS_FS_DIRECT_BLOCKS;

    for (block_idx = 0; block_idx < blocks_used; block_idx++) {
        u32 phys_block = dir_inode->blocks[block_idx];
        if (phys_block == 0) continue;

        if (dev->read(dev, phys_block, temp_buf) != 0) continue;

        for (entry_idx = 0; entry_idx < MOS_FS_DIRENTS_PER_BLOCK; entry_idx++) {
            /* 从 temp_buf 读取 dirent (32 bytes, 8 个 u32) */
            u32 *src = (u32 *)(temp_buf + entry_idx * sizeof(fs_dirent_t));
            u32 *dst = (u32 *)&dirent;
            u32 k;
            for (k = 0; k < 8; k++) dst[k] = src[k];

            if (dirent.inode_num == 0) continue;  /* 空项 */

            /* 比较文件名 (使用 memcmp) */
            int match = 1;
            u32 ci;
            for (ci = 0; ci < MOS_FS_NAME_MAX; ci++) {
                if (dirent.name[ci] != name[ci]) {
                    match = 0;
                    break;
                }
                if (name[ci] == '\0') break;
            }
            if (match) {
                return (int)dirent.inode_num;
            }
        }
    }
    return -1;  /* 未找到 */
}

int mos_fs_add_dirent(block_dev_t *dev, fs_inode_t *dir_inode,
                      const char *name, u32 inode_num)
{
    fs_dirent_t new_ent;
    u32 block_idx, entry_idx;
    u32 blocks_used;

    if (dir_inode->mode != FT_DIR) return -1;

    /* 构造目录项 */
    new_ent.inode_num = inode_num;
    u32 ni;
    for (ni = 0; ni < MOS_FS_NAME_MAX && name[ni]; ni++) {
        new_ent.name[ni] = name[ni];
    }
    new_ent.name[ni >= MOS_FS_NAME_MAX ? MOS_FS_NAME_MAX - 1 : ni] = '\0';

    blocks_used = (dir_inode->size + 511) / 512;
    if (blocks_used > MOS_FS_DIRECT_BLOCKS) blocks_used = MOS_FS_DIRECT_BLOCKS;

    /* 先找空闲 dirent 槽 */
    for (block_idx = 0; block_idx < blocks_used; block_idx++) {
        u32 phys_block = dir_inode->blocks[block_idx];
        if (phys_block == 0) continue;

        if (dev->read(dev, phys_block, temp_buf) != 0) continue;

        for (entry_idx = 0; entry_idx < MOS_FS_DIRENTS_PER_BLOCK; entry_idx++) {
            u32 *slot = (u32 *)(temp_buf + entry_idx * sizeof(fs_dirent_t));
            if (slot[0] == 0) {  /* inode_num == 0 → 空槽 */
                /* 写入新 dirent (8 个 u32 = 32 bytes) */
                u32 *src = (u32 *)&new_ent;
                u32 k;
                for (k = 0; k < 8; k++) slot[k] = src[k];
                dev->write(dev, phys_block, temp_buf);
                return 0;
            }
        }
    }

    /* 没有空闲槽 → 需要分配新块 */
    if (blocks_used >= MOS_FS_DIRECT_BLOCKS) return -1;  /* 目录满 */

    int new_block = mos_fs_alloc_block(dev);
    if (new_block < 0) return -1;

    dir_inode->blocks[blocks_used] = (u32)new_block;

    /* 清零新块 */
    u32 i;
    for (i = 0; i < 128; i++) ((u32 *)temp_buf)[i] = 0;

    /* 写入新 dirent 到第一个槽 */
    u32 *slot = (u32 *)temp_buf;
    u32 *src = (u32 *)&new_ent;
    for (i = 0; i < 8; i++) slot[i] = src[i];

    dev->write(dev, (u32)new_block, temp_buf);

    /* 更新目录大小 (对齐到块边界) */
    dir_inode->size = (blocks_used + 1) * 512;

    return 0;
}

int mos_fs_remove_dirent(block_dev_t *dev, fs_inode_t *dir_inode,
                         const char *name)
{
    u32 block_idx, entry_idx;
    u32 blocks_used;

    if (dir_inode->mode != FT_DIR) return -1;

    blocks_used = (dir_inode->size + 511) / 512;
    if (blocks_used > MOS_FS_DIRECT_BLOCKS) blocks_used = MOS_FS_DIRECT_BLOCKS;

    for (block_idx = 0; block_idx < blocks_used; block_idx++) {
        u32 phys_block = dir_inode->blocks[block_idx];
        if (phys_block == 0) continue;

        if (dev->read(dev, phys_block, temp_buf) != 0) continue;

        for (entry_idx = 0; entry_idx < MOS_FS_DIRENTS_PER_BLOCK; entry_idx++) {
            u32 *slot = (u32 *)(temp_buf + entry_idx * sizeof(fs_dirent_t));
            if (slot[0] == 0) continue;  /* 空项 */

            /* 比较文件名 */
            char *ent_name = (char *)(slot + 1);  /* slot[1..7] = name */
            int match = 1;
            u32 ci;
            for (ci = 0; ci < MOS_FS_NAME_MAX; ci++) {
                if (ent_name[ci] != name[ci]) {
                    match = 0;
                    break;
                }
                if (name[ci] == '\0') break;
            }
            if (match) {
                /* 清零此 dirent */
                u32 k;
                for (k = 0; k < 8; k++) slot[k] = 0;
                dev->write(dev, phys_block, temp_buf);
                return 0;
            }
        }
    }
    return -1;  /* 未找到 */
}

/* =========================== 文件读写 =========================== */

int mos_fs_read_file(block_dev_t *dev, const fs_inode_t *inode,
                     u32 offset, void *buf, u32 count)
{
    u32 remaining, total_read;
    u8 *dst;

    if (offset >= inode->size) return 0;
    if (offset + count > inode->size) {
        count = inode->size - offset;
    }

    remaining = count;
    total_read = 0;
    dst = (u8 *)buf;

    while (remaining > 0) {
        u32 block_idx = offset / 512;
        u32 block_off = offset % 512;
        u32 chunk = 512 - block_off;
        if (chunk > remaining) chunk = remaining;

        if (block_idx >= MOS_FS_DIRECT_BLOCKS) break;

        u32 phys_block = inode->blocks[block_idx];
        if (phys_block == 0) break;

        if (dev->read(dev, phys_block, temp_buf) != 0) break;

        /* 从 temp_buf 复制到用户 buf */
        u32 i;
        for (i = 0; i < chunk; i++) {
            dst[total_read + i] = temp_buf[block_off + i];
        }

        offset     += chunk;
        remaining  -= chunk;
        total_read += chunk;
    }

    return (int)total_read;
}

int mos_fs_write_file(block_dev_t *dev, fs_inode_t *inode,
                      u32 offset, const void *buf, u32 count)
{
    u32 remaining, total_written;
    const u8 *src;

    remaining = count;
    total_written = 0;
    src = (const u8 *)buf;

    while (remaining > 0) {
        u32 block_idx = offset / 512;
        u32 block_off = offset % 512;
        u32 chunk = 512 - block_off;
        if (chunk > remaining) chunk = remaining;

        if (block_idx >= MOS_FS_DIRECT_BLOCKS) break;

        /* 如果该块未分配, 先分配 */
        if (inode->blocks[block_idx] == 0) {
            int new_block = mos_fs_alloc_block(dev);
            if (new_block < 0) break;
            inode->blocks[block_idx] = (u32)new_block;
        }

        u32 phys_block = inode->blocks[block_idx];

        /* 如果不是整块写入, 先读旧内容 */
        if (chunk < 512 || block_off != 0) {
            dev->read(dev, phys_block, temp_buf);
        }

        /* 复制数据 */
        u32 i;
        for (i = 0; i < chunk; i++) {
            temp_buf[block_off + i] = src[total_written + i];
        }

        dev->write(dev, phys_block, temp_buf);

        offset        += chunk;
        remaining     -= chunk;
        total_written += chunk;
    }

    /* 更新文件大小 */
    if (offset > inode->size) {
        inode->size = offset;
    }

    return (int)total_written;
}

/* =========================== 格式化 =========================== */

int mos_fs_mkfs(block_dev_t *dev)
{
    fs_superblock_t sb;
    fs_inode_t root_inode;
    u32 i;

    if (dev->total_blocks < 8) return -1;  /* 至少需要 8 块 */

    /* 1. 构造超级块参数 */
    sb.magic              = MOS_FS_MAGIC;
    sb.block_size         = dev->block_size;
    sb.total_blocks       = dev->total_blocks;

    sb.inode_count        = MOS_FS_MAX_INODES;
    sb.inode_table_start  = 1;
    sb.inode_table_blocks = (MOS_FS_MAX_INODES + MOS_FS_INODES_PER_BLOCK - 1)
                             / MOS_FS_INODES_PER_BLOCK;

    sb.bitmap_start       = sb.inode_table_start + sb.inode_table_blocks;
    sb.data_start         = sb.bitmap_start + 1;

    sb.free_blocks        = dev->total_blocks - sb.data_start;
    sb.free_inodes        = MOS_FS_MAX_INODES;
    sb.root_inode         = 0;

    /* 先写超级块 (write_inode 需要读超级块获取 inode 表位置) */
    mos_fs_write_superblock(dev, &sb);

    /* 2. 清零 inode 表 */
    for (i = 0; i < 128; i++) ((u32 *)temp_buf)[i] = 0;
    for (i = 0; i < sb.inode_table_blocks; i++) {
        dev->write(dev, sb.inode_table_start + i, temp_buf);
    }

    /* 3. 初始化根目录 inode */
    {
        u32 j;
        root_inode.mode  = FT_DIR;
        root_inode.nlinks = 2;
        root_inode.size  = 0;
        for (j = 0; j < MOS_FS_DIRECT_BLOCKS; j++) root_inode.blocks[j] = 0;
        root_inode.indirect = 0;
        for (j = 0; j < 8; j++) root_inode.reserved[j] = 0;
    }
    mos_fs_write_inode(dev, 0, &root_inode);

    /* 4. 初始化块位图 */
    {
        u32 j;
        for (j = 0; j < 128; j++) ((u32 *)temp_buf)[j] = 0;
    }
    dev->write(dev, sb.bitmap_start, temp_buf);

    /* 5. 更新超级块 (free_inodes 减 1) */
    sb.free_inodes--;
    mos_fs_write_superblock(dev, &sb);

    return 0;
}
