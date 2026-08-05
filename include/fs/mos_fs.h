/**
 * @file    mos_fs.h
 * @brief   MOS-FS 极简文件系统 — 磁盘格式定义与底层操作
 *
 * 磁盘布局 (64KB RAM 盘, 512B 块, 128 块):
 *   Block 0:     超级块
 *   Block 1-2:   Inode 表 (16 inode, 每块 8 个, 每个 64B)
 *   Block 3:     块位图 (128 bits = 16 bytes)
 *   Block 4-127: 数据区 (根目录 + 文件数据)
 *
 * 设计要点:
 *   - inode 0 固定为根目录
 *   - 文件名最长 28 字符, 单层目录结构
 *   - 10 个直接块指针, 最大文件 10 × 512 = 5KB
 *   - 目录内容是 dirent 数组, 以 inode_num=0 标记空项
 */

#ifndef _FS_MOS_FS_H
#define _FS_MOS_FS_H

#include "lib/types.h"
#include "fs/block_dev.h"

/* =========================== 常量 =========================== */

#define MOS_FS_MAGIC        0x4D4F5331  /* "MOS1" */
#define MOS_FS_BLOCK_SIZE   512         /* 块大小 (字节) */
#define MOS_FS_MAX_INODES   16          /* 最大 inode 数 */
#define MOS_FS_INODES_PER_BLOCK  8      /* 每块 8 个 inode (512/64) */
#define MOS_FS_DIRECT_BLOCKS 10        /* 直接块指针数 */
#define MOS_FS_MAX_FILE_SIZE (MOS_FS_DIRECT_BLOCKS * MOS_FS_BLOCK_SIZE)  /* 5KB */
#define MOS_FS_NAME_MAX     28          /* 文件名最大长度 */
#define MOS_FS_DIRENTS_PER_BLOCK 16     /* 每块 16 个目录项 (512/32) */
#define MOS_FS_INODE_U32_COUNT ((u32)(sizeof(fs_inode_t) / 4))  /* fs_inode_t 的 u32 数 */

/* 文件 / 目录类型 */
#define FT_REGULAR  1
#define FT_DIR      2

/* =========================== 磁盘结构 =========================== */

/**
 * @brief  超级块 — 文件系统的元数据, 存储在 Block 0
 */
typedef struct {
    u32 magic;              /* MOS_FS_MAGIC */
    u32 block_size;         /* 512 */
    u32 total_blocks;       /* 128 (对 64KB 盘) */
    u32 inode_count;        /* 16 */
    u32 inode_table_start;  /* inode 表起始块号 (1) */
    u32 inode_table_blocks; /* inode 表占块数 (2) */
    u32 bitmap_start;       /* 位图起始块号 (3) */
    u32 data_start;         /* 数据区起始块号 (4) */
    u32 free_blocks;        /* 当前空闲数据块数 */
    u32 free_inodes;        /* 当前空闲 inode 数 */
    u32 root_inode;         /* 根目录 inode 号 (0) */
    u8  padding[72];        /* 填充到 128 bytes */
} __attribute__((packed)) fs_superblock_t;

/**
 * @brief  Inode — 文件/目录的元数据, 64 bytes
 */
typedef struct {
    u16 mode;               /* FT_REGULAR 或 FT_DIR */
    u16 nlinks;             /* 硬链接数 (文件被几个目录项引用) */
    u32 size;               /* 文件大小 (字节), 目录 = 目录项总字节数 */
    u32 blocks[MOS_FS_DIRECT_BLOCKS];  /* 直接块号, 0=未分配 */
    u32 indirect;           /* 一级间接块 (v0.5 暂不使用) */
    u8  reserved[8];        /* 填充到 64 bytes */
} __attribute__((packed)) fs_inode_t;

/**
 * @brief  目录项 — 文件名 → inode 的映射, 32 bytes
 */
typedef struct {
    u32 inode_num;          /* inode 号, 0=空 (已删除) */
    char name[MOS_FS_NAME_MAX];  /* 文件名 (不含路径), \0 结尾 */
} __attribute__((packed)) fs_dirent_t;

/* =========================== 文件系统操作 =========================== */

/**
 * @brief  格式化块设备为 MOS-FS
 *
 * 操作:
 *   1. 写入超级块 (magic, 布局参数, 空闲计数)
 *   2. 清零 inode 表
 *   3. 初始化根目录 inode (inode 0, mode=FT_DIR, size=0)
 *   4. 分配根目录数据块 (block 4)
 *   5. 清零块位图, 标记 block 0-4 为已用
 *
 * @param  dev  要格式化的块设备
 * @return 0=成功, -1=失败
 */
int mos_fs_mkfs(block_dev_t *dev);

/**
 * @brief  读取第 inode_num 个 inode 到 buf
 * @return 0=成功, -1=inode 号越界
 */
int mos_fs_read_inode(block_dev_t *dev, u32 inode_num, fs_inode_t *inode);

/**
 * @brief  将 inode 写回磁盘
 * @return 0=成功
 */
int mos_fs_write_inode(block_dev_t *dev, u32 inode_num, const fs_inode_t *inode);

/**
 * @brief  分配一个空闲 inode
 * @param  mode  FT_REGULAR 或 FT_DIR
 * @return inode 号, 无空闲时返回 -1
 */
int mos_fs_alloc_inode(block_dev_t *dev, u16 mode);

/**
 * @brief  释放 inode (及其所有数据块)
 * @return 0=成功
 */
int mos_fs_free_inode(block_dev_t *dev, u32 inode_num);

/**
 * @brief  分配一个空闲数据块
 * @return 块号, 无空闲返回 -1
 */
int mos_fs_alloc_block(block_dev_t *dev);

/**
 * @brief  释放一个数据块
 * @return 0=成功
 */
int mos_fs_free_block(block_dev_t *dev, u32 block_no);

/**
 * @brief  读取超级块到内存
 * @return 0=成功
 */
int mos_fs_read_superblock(block_dev_t *dev, fs_superblock_t *sb);

/**
 * @brief  将超级块写回磁盘
 * @return 0=成功
 */
int mos_fs_write_superblock(block_dev_t *dev, const fs_superblock_t *sb);

/**
 * @brief  在目录 inode 中查找文件名, 返回其 inode 号
 * @param  dir_inode  目录的 inode
 * @param  name       要查找的文件名
 * @return inode 号, 未找到返回 -1
 */
int mos_fs_lookup(block_dev_t *dev, const fs_inode_t *dir_inode,
                  const char *name);

/**
 * @brief  向目录添加一个目录项 (文件名 → inode_num)
 * @param  dir_inode  目录的 inode (会被修改)
 * @param  name       文件名
 * @param  inode_num  目标 inode 号
 * @return 0=成功, -1=目录满
 */
int mos_fs_add_dirent(block_dev_t *dev, fs_inode_t *dir_inode,
                      const char *name, u32 inode_num);

/**
 * @brief  从目录删除一个目录项
 * @return 0=成功, -1=未找到
 */
int mos_fs_remove_dirent(block_dev_t *dev, fs_inode_t *dir_inode,
                         const char *name);

/**
 * @brief  读取文件数据 (从 offset 开始, 最多 count 字节)
 * @param  inode  文件 inode
 * @return 实际读取字节数, <0 表示错误
 */
int mos_fs_read_file(block_dev_t *dev, const fs_inode_t *inode,
                     u32 offset, void *buf, u32 count);

/**
 * @brief  写入文件数据 (从 offset 开始)
 *
 * 如果 offset + count 超过当前文件大小, 自动分配新块。
 *
 * @param  inode  文件 inode (会被修改: size 和 blocks[])
 * @return 实际写入字节数, <0 表示错误
 */
int mos_fs_write_file(block_dev_t *dev, fs_inode_t *inode,
                      u32 offset, const void *buf, u32 count);

#endif /* _FS_MOS_FS_H */
