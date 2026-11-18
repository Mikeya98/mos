/**
 * @file    sd.h
 * @brief   MOS RTOS SD 卡驱动 (SDHCI 控制器 + PIO 传输)
 *
 * Zynq-7000 的 SD 控制器是 Arasan SDHCI 兼容控制器, 基地址 0xE0100000。
 * QEMU 的 xilinx-zynq-a9 也实现了同一个模型, 用
 *   -drive file=build/sd.img,if=sd,format=raw
 * 把镜像挂上去。
 *
 * 这一版只做轮询 + PIO (CPU 逐字搬数据), 不用 DMA、不用中断:
 *   初始化  CMD0 → CMD8 → ACMD41 → CMD2 → CMD3 → CMD9 → CMD7 → CMD16
 *   读块    CMD17 (READ_SINGLE_BLOCK)
 *   写块    CMD24 (WRITE_SINGLE_BLOCK)
 *
 * 没有实现的:
 *   - 命令 CRC / 数据 CRC (QEMU 不校验; 真卡上需要)
 *   - 4-bit 总线宽度、高速模式、多块传输、DMA
 * 这些都不影响把文件系统挂到真实块设备上的验证。
 */

#ifndef _MOS_SD_H
#define _MOS_SD_H

#include "lib/types.h"
#include "fs/block_dev.h"

/* Zynq SDHCI 基地址 */
#define SDHCI_BASE          0xE0100000UL

/* =========================== API =========================== */

/**
 * @brief  初始化 SD 卡
 *
 * 完成控制器复位、时钟配置和卡的上电初始化流程。
 *
 * @return 0=成功, -1=失败 (没有卡 / 协议错误 / 超时)
 */
int sd_init(void);

/** 卡容量, 单位 512 字节块 */
u32 sd_block_count(void);

/** 卡是否已经初始化成功 */
bool sd_is_ready(void);

/** 读一个 512 字节块, 返回 0=成功 */
int sd_read_block(u32 lba, void *buf);

/** 写一个 512 字节块, 返回 0=成功 */
int sd_write_block(u32 lba, const void *buf);

/** 读/写连续多个块, 返回实际完成的块数 (出错时 < count) */
u32 sd_read_blocks(u32 lba, void *buf, u32 count);
u32 sd_write_blocks(u32 lba, const void *buf, u32 count);

/**
 * @brief  把 SD 卡包装成一个块设备, 交给文件系统用
 * @return 未初始化成功时返回 NULL
 */
block_dev_t *sd_block_device(void);

#endif /* _MOS_SD_H */
