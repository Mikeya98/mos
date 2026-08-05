/**
 * @file    block_dev.c
 * @brief   块设备抽象层实现 — RAM 盘驱动
 *
 * RAM 盘用 buddy_alloc 从堆中分配连续内存,
 * 通过 memcpy 实现块读写。
 */

#include "fs/block_dev.h"
#include "mm/buddy.h"
#include "lib/string.h"

/* =========================== RAM 盘读/写 =========================== */

static int ramdisk_read(block_dev_t *dev, u32 block_no, void *buf)
{
    u8 *disk;
    u32 offset;

    if (block_no >= dev->total_blocks) return -1;

    disk = (u8 *)dev->priv;
    offset = block_no * dev->block_size;
    memcpy(buf, disk + offset, dev->block_size);
    return 0;
}

static int ramdisk_write(block_dev_t *dev, u32 block_no, const void *buf)
{
    u8 *disk;
    u32 offset;

    if (block_no >= dev->total_blocks) return -1;

    disk = (u8 *)dev->priv;
    offset = block_no * dev->block_size;
    memcpy(disk + offset, buf, dev->block_size);
    return 0;
}

/* =========================== 公共 API =========================== */

block_dev_t *ramdisk_create(u32 size_bytes)
{
    block_dev_t *dev;
    u8 *memory;

    if (size_bytes < 4096) return NULL;

    /* 分配块设备结构体 */
    dev = (block_dev_t *)buddy_alloc(sizeof(block_dev_t));
    if (!dev) return NULL;

    /* 分配磁盘空间 */
    memory = (u8 *)buddy_alloc(size_bytes);
    if (!memory) {
        buddy_free(dev);
        return NULL;
    }

    /* 清零磁盘 (模拟空白介质) */
    u32 i;
    for (i = 0; i < size_bytes; i++) {
        memory[i] = 0;
    }

    dev->name         = "ram0";
    dev->block_size   = 512;
    dev->total_blocks = size_bytes / 512;
    dev->read         = ramdisk_read;
    dev->write        = ramdisk_write;
    dev->priv         = memory;

    return dev;
}

void ramdisk_destroy(block_dev_t *dev)
{
    if (!dev) return;
    if (dev->priv) {
        buddy_free(dev->priv);
    }
    buddy_free(dev);
}
