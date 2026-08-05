/**
 * @file    block_dev.h
 * @brief   块设备抽象层 — 统一块存储访问接口
 *
 * 设计思路:
 *   不同的存储设备 (RAM 盘 / SD 卡 / Flash) 实现同一套接口,
 *   上层文件系统不需要关心底层是什么硬件。
 *
 *   每个块设备提供 read_block / write_block 两个操作,
 *   块大小可以不同 (通常 512 字节), 上层通过 dev->block_size 获知。
 */

#ifndef _FS_BLOCK_DEV_H
#define _FS_BLOCK_DEV_H

#include "lib/types.h"

/* =========================== 块设备结构 =========================== */

typedef struct block_device {
    const char *name;       /* 设备名, 调试用, 如 "ram0" */
    u32 block_size;         /* 块大小 (字节), 通常 512 */
    u32 total_blocks;       /* 总块数 */

    /**
     * @brief  读取第 block_no 块到 buf
     * @return 0=成功, -1=失败 (块号越界)
     */
    int (*read)(struct block_device *dev, u32 block_no, void *buf);

    /**
     * @brief  将 buf 写入第 block_no 块
     * @return 0=成功, -1=失败 (块号越界)
     */
    int (*write)(struct block_device *dev, u32 block_no, const void *buf);

    void *priv;             /* 驱动私有数据 (RAM 盘的内存指针等) */
} block_dev_t;

/* =========================== RAM 盘 =========================== */

/**
 * @brief  创建一个内存模拟的块设备 (RAM 盘)
 *
 * 内部调用 buddy_alloc 分配 size_bytes 字节的连续内存,
 * 填充 block_dev_t 结构并返回。
 *
 * 块大小固定为 512 字节, total_blocks = size_bytes / 512。
 *
 * @param  size_bytes  磁盘总大小 (字节), 应 >= 4096 且是 512 的倍数
 * @return 成功返回块设备指针, 内存不足返回 NULL
 */
block_dev_t *ramdisk_create(u32 size_bytes);

/**
 * @brief  销毁 RAM 盘, 释放内存
 */
void ramdisk_destroy(block_dev_t *dev);

#endif /* _FS_BLOCK_DEV_H */
