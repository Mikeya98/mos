/**
 * @file    sd.c
 * @brief   SD 卡驱动 — SDHCI 控制器 + PIO 轮询
 *
 * 为什么从这里开始碰"真实块设备":
 *   前面版本的 MOS-FS 一直跑在 RAM 盘上 —— 读写就是 memcpy, 掉电就
 *   没了。SD 卡是第一个真正把数据写到"外面"的设备: 要通过命令协议
 *   让卡进入传输状态, 再按块搬数据。做完这一步, 文件系统才算真正
 *   落在持久存储上。
 *
 * 寄存器布局见 include/drivers/sd.h 的注释。所有访问都是轮询:
 * 发命令后等 Normal Interrupt Status 的 Command Complete 位, 读写
 * 数据时等 Buffer Ready 位。不用中断, 因为这里只想验证协议本身。
 */

#include "drivers/sd.h"
#include "lib/printf.h"

/* =========================== 寄存器 =========================== */

#define REG32(addr)     (*(volatile u32 *)(addr))
#define REG16(addr)     (*(volatile u16 *)(addr))
#define REG8(addr)      (*(volatile u8 *)(addr))

#define SDHCI(off)      REG32(SDHCI_BASE + (off))

/* 寄存器偏移 (按 SD Host Controller Simplified Spec 3.0) */
#define SD_BLOCK_SIZE       0x04
#define SD_BLOCK_COUNT      0x06
#define SD_ARGUMENT         0x08
#define SD_TRANSFER_MODE    0x0C
#define SD_COMMAND          0x0E
#define SD_RESPONSE0        0x10
#define SD_RESPONSE1        0x14
#define SD_RESPONSE2        0x18
#define SD_RESPONSE3        0x1C
#define SD_BUFFER_PORT      0x20
#define SD_PRESENT_STATE    0x24
#define SD_HOST_CONTROL     0x28
#define SD_POWER_CONTROL    0x29
#define SD_CLOCK_CONTROL    0x2C
#define SD_TIMEOUT_CONTROL  0x2E
#define SD_SOFTWARE_RESET   0x2F
#define SD_NORMAL_STATUS    0x30
#define SD_ERROR_STATUS     0x32
#define SD_NIS_ENABLE       0x34
#define SD_EIS_ENABLE       0x36
#define SD_NIS_SIGNAL       0x38
#define SD_EIS_SIGNAL       0x3A
#define SD_CAPABILITIES     0x40
#define SD_HOST_VERSION     0xFE

/* 命令寄存器位 */
#define RESP_NONE           0x0000
#define RESP_136            0x0001
#define RESP_48             0x0002
#define RESP_48_BUSY        0x0003
#define CMD_CRC             (1u << 3)
#define CMD_INDEX           (1u << 4)
#define CMD_DATA            (1u << 5)

/* 传输模式位 */
#define TM_BLOCK_COUNT      (1u << 1)
#define TM_READ             (1u << 4)
#define TM_MULTI            (1u << 5)

/* Normal Interrupt Status 位 */
#define NIS_CMD_COMPLETE    (1u << 0)
#define NIS_TRANS_COMPLETE  (1u << 1)
#define NIS_BUF_WRITE_RDY   (1u << 4)
#define NIS_BUF_READ_RDY    (1u << 5)
#define NIS_ERROR           (1u << 15)

/* Present State 位 */
#define PS_CMD_INHIBIT      (1u << 0)
#define PS_DAT_INHIBIT      (1u << 1)

/* 软件复位位 */
#define RESET_ALL           0x01
#define RESET_CMD           0x02
#define RESET_DAT           0x04

/*
 * 时钟控制寄存器 (16 位):
 *   bit0    内部时钟使能
 *   bit1    内部时钟稳定 (只读)
 *   bit2    SD 时钟使能 (输出到卡)
 *   [7:6]   分频值高 2 位
 *   [15:8]  分频值低 8 位
 */
#define CLOCK_INT_EN        (1u << 0)
#define CLOCK_STABLE        (1u << 1)
#define CLOCK_SD_EN         (1u << 2)

/* 电源控制: bit0 上电, [3:1] = 111 表示 3.3V */
#define POWER_33V           0x0F

/* 超时: 0x0E = 最大 */
#define TIMEOUT_MAX         0x0E

/* 轮询上限, 防止卡死 */
#define POLL_LIMIT          2000000

/* =========================== 卡状态 =========================== */

static u32 sd_blocks   = 0;      /* 总块数 */
static u32 sd_rca      = 0;      /* 相对卡地址 */
static bool sd_ready   = 0;
static bool sd_high_capacity = 0; /* SDHC/SDXC: CMD 参数是块地址, 否则是字节地址 */
static block_dev_t sd_dev;

/* =========================== 底层辅助 =========================== */

static bool wait_bits(u32 off, u32 mask, u32 value)
{
    u32 i;

    for (i = 0; i < POLL_LIMIT; i++) {
        if ((SDHCI(off) & mask) == value) return true;
    }
    return false;
}

static void sd_delay(u32 loops)
{
    volatile u32 i;
    for (i = 0; i < loops; i++) {
        __asm__ volatile("nop");
    }
}

/**
 * 发一条命令并等待完成。
 *
 * @param  idx   命令号
 * @param  arg   参数
 * @param  resp  响应类型 (RESP_*)
 * @param  data  是否带数据 (CMD17/24 为 1)
 * @param  out   4 个响应字 (可为 NULL)
 */
static int sd_cmd(u32 idx, u32 arg, u32 resp, bool data, u32 *out)
{
    u32 i;
    u32 status;
    u32 cmd;

    /* 控制器和卡都空闲才能发下一条 */
    if (!wait_bits(SD_PRESENT_STATE,
                   PS_CMD_INHIBIT | PS_DAT_INHIBIT, 0)) {
        return -1;
    }

    SDHCI(SD_ARGUMENT) = arg;

    /* 清掉上次的状态 */
    REG16(SDHCI_BASE + SD_NORMAL_STATUS) = 0xFFFF;
    REG16(SDHCI_BASE + SD_ERROR_STATUS)  = 0xFFFF;
    REG16(SDHCI_BASE + SD_NIS_ENABLE)    = 0xFFFF;
    REG16(SDHCI_BASE + SD_EIS_ENABLE)    = 0xFFFF;

    cmd = (idx << 8) | resp;
    if (resp != RESP_NONE) {
        cmd |= CMD_CRC | CMD_INDEX;
    }
    if (data) {
        cmd |= CMD_DATA;
    }
    REG16(SDHCI_BASE + SD_COMMAND) = (u16)cmd;

    /* 等 Command Complete; 中途报错就退出 */
    for (i = 0; i < POLL_LIMIT; i++) {
        status = REG16(SDHCI_BASE + SD_NORMAL_STATUS);
        if (status & NIS_ERROR) {
            printf("[SD] cmd%u error: nis=0x%04X eis=0x%04X\n",
                   idx, status, REG16(SDHCI_BASE + SD_ERROR_STATUS));
            return -2;
        }
        if (status & NIS_CMD_COMPLETE) break;
    }
    if (i == POLL_LIMIT) {
        printf("[SD] cmd%u timeout: nis=0x%04X state=0x%08X\n",
               idx, REG16(SDHCI_BASE + SD_NORMAL_STATUS),
               SDHCI(SD_PRESENT_STATE));
        return -3;
    }

    if (out) {
        out[0] = SDHCI(SD_RESPONSE0);
        out[1] = SDHCI(SD_RESPONSE1);
        out[2] = SDHCI(SD_RESPONSE2);
        out[3] = SDHCI(SD_RESPONSE3);
    }
    return 0;
}

/** 从 136 位响应里取出 16 字节 CSD/CID (RESP3 是最高 32 位) */
static void sd_long_to_bytes(const u32 *r, u8 *out)
{
    u32 i;

    for (i = 0; i < 4; i++) {
        u32 v = r[3 - i];
        out[i * 4 + 0] = (u8)(v >> 24);
        out[i * 4 + 1] = (u8)(v >> 16);
        out[i * 4 + 2] = (u8)(v >> 8);
        out[i * 4 + 3] = (u8)v;
    }
}

/* =========================== 数据块读写 =========================== */

static int sd_transfer(u32 lba, void *buf, bool write)
{
    u32 i;
    u32 *p = (u32 *)buf;
    u32 status;

    if (!sd_ready) return -1;

    REG16(SDHCI_BASE + SD_BLOCK_SIZE)  = 512;
    REG16(SDHCI_BASE + SD_BLOCK_COUNT) = 1;
    REG16(SDHCI_BASE + SD_TRANSFER_MODE) = (u16)(write ? 0 : TM_READ);

    /* CMD17 = READ_SINGLE_BLOCK, CMD24 = WRITE_SINGLE_BLOCK。
     *
     * 地址口径按卡的类型分: SDHC/SDXC 用块地址, SDSC 用字节地址。
     * 这一步写错在 QEMU 上表现为读回全 0 或读到别的块, 很难查。
     */
    {
        u32 arg = sd_high_capacity ? lba : (lba * 512u);
        if (sd_cmd(write ? 24 : 17, arg, RESP_48, true, NULL) != 0) {
            return -2;
        }
    }

    if (write) {
        if (!wait_bits(SD_NORMAL_STATUS, NIS_BUF_WRITE_RDY,
                       NIS_BUF_WRITE_RDY)) {
            return -3;
        }
        for (i = 0; i < 128; i++) {
            SDHCI(SD_BUFFER_PORT) = p[i];
        }
    } else {
        if (!wait_bits(SD_NORMAL_STATUS, NIS_BUF_READ_RDY,
                       NIS_BUF_READ_RDY)) {
            return -3;
        }
        for (i = 0; i < 128; i++) {
            p[i] = SDHCI(SD_BUFFER_PORT);
        }
    }

    /* 等数据传输完成 */
    for (i = 0; i < POLL_LIMIT; i++) {
        status = REG16(SDHCI_BASE + SD_NORMAL_STATUS);
        if (status & NIS_ERROR) return -4;
        if (status & NIS_TRANS_COMPLETE) break;
    }
    if (i == POLL_LIMIT) return -5;

    REG16(SDHCI_BASE + SD_NORMAL_STATUS) = 0xFFFF;
    REG16(SDHCI_BASE + SD_ERROR_STATUS)  = 0xFFFF;
    return 0;
}

/* =========================== 初始化 =========================== */

static void sd_set_clock(u32 base_mhz, u32 khz)
{
    u32 div;
    u32 i;

    /* SDHCI 的分频: 时钟 = base / (2 * (div + 1)), div = 0 表示 /2 */
    if (base_mhz == 0) base_mhz = 50;
    div = (base_mhz * 1000u) / (2u * khz);
    if (div > 0) div -= 1;          /* 寄存器里的 N 从 0 开始 */
    if (div > 1023) div = 1023;

    REG16(SDHCI_BASE + SD_CLOCK_CONTROL) =
        (u16)(CLOCK_INT_EN |
              (((div >> 8) & 0x3) << 6) |
              (((div & 0xFF) << 8)));

    /* 等内部时钟稳定; QEMU 不实现的话超时后继续 */
    for (i = 0; i < POLL_LIMIT; i++) {
        if (REG16(SDHCI_BASE + SD_CLOCK_CONTROL) & CLOCK_STABLE) break;
    }

    REG16(SDHCI_BASE + SD_CLOCK_CONTROL) |= CLOCK_SD_EN;
    sd_delay(1000);
}

static u32 sd_csd_capacity_at(const u8 *csd)
{
    u32 structure = (csd[0] >> 6) & 0x3u;

    if (structure == 1u) {
        /* CSD v2 (SDHC/SDXC): 容量 = (c_size + 1) * 512KB */
        u32 c_size = ((u32)(csd[7] & 0x3F) << 16) |
                     ((u32)csd[8] << 8) | csd[9];
        return (c_size + 1u) * 1024u;      /* 单位是 512 字节块 */
    }

    if (structure != 0u) return 0;         /* 保留编码 */

    /* CSD v1 (SDSC):
     * capacity = (c_size + 1) * 2^(c_size_mult + 2) * 2^read_bl_len */
    {
        u32 read_bl_len = csd[5] & 0x0Fu;
        u32 c_size, c_size_mult;

        /* READ_BL_LEN 合法值 9..12; 不是的话说明对齐错了 */
        if (read_bl_len < 9u || read_bl_len > 12u) return 0;

        c_size = ((u32)(csd[6] & 0x03) << 10) |
                 ((u32)csd[7] << 2) |
                 ((csd[8] >> 6) & 0x3u);
        c_size_mult = ((u32)(csd[9] & 0x03) << 1) |
                      ((csd[10] >> 7) & 0x1u);

        return (c_size + 1u) << (c_size_mult + 2u + read_bl_len - 9u);
    }
}

/**
 * 解析 CSD 得到容量。
 *
 * 136 位响应塞进 4 个 32 位响应寄存器时, 不同控制器对"前 8 位前缀
 * 放哪里"的处理不一致: 规范说存 128 位数据, QEMU 的 SDHCI 实现把
 * 整个响应左移了一个字节。硬编码一种对齐方式的话, 换一个控制器就
 * 会算出离谱的容量 (第一次跑出来是 2064MB)。
 *
 * 这里把 16 字节 CSD 前面补两个 0, 依次尝试 0/1/2 字节偏移, 用
 * CSD_STRUCTURE 和 READ_BL_LEN 的合法取值挑出正确的那一份。
 */
static u32 sd_read_csd_capacity(const u8 *csd, u32 *shift_out)
{
    u8 buf[18];
    u32 shift;

    {
        u32 i;
        for (i = 0; i < 16; i++) buf[i] = csd[i];
        buf[16] = 0;
        buf[17] = 0;
    }

    for (shift = 0; shift <= 2; shift++) {
        u32 cap = sd_csd_capacity_at(buf + shift);
        if (cap != 0) {
            if (shift_out) *shift_out = shift;
            return cap;
        }
    }
    return 0;
}

int sd_init(void)
{
    u32 resp[4];
    u8  csd[16];
    u32 base_mhz;
    u32 i;
    u32 ocr;
    bool sdhc = false;

    sd_ready = 0;

    /* 1. 控制器软复位 */
    REG8(SDHCI_BASE + SD_SOFTWARE_RESET) = RESET_ALL;
    for (i = 0; i < POLL_LIMIT; i++) {
        if (!(REG8(SDHCI_BASE + SD_SOFTWARE_RESET) & RESET_ALL)) break;
    }

    /* 2. 超时、电源、时钟 (初始化阶段 400kHz) */
    REG8(SDHCI_BASE + SD_TIMEOUT_CONTROL) = TIMEOUT_MAX;
    REG8(SDHCI_BASE + SD_POWER_CONTROL)   = POWER_33V;

    base_mhz = (SDHCI(SD_CAPABILITIES) >> 8) & 0xFFu;
    sd_set_clock(base_mhz, 400);

    /* 3. CMD0: 复位卡, 不响应 */
    if (sd_cmd(0, 0, RESP_NONE, false, NULL) != 0) {
        printf("[SD] CMD0 failed\n");
        return -1;
    }

    /* 4. CMD8: 询问电压和版本, R7。能应答说明是 v2 及以上的卡，
     *    后面的 ACMD41 才可以带 HCS 位申请高容量。 */
    {
        bool v2 = (sd_cmd(8, 0x1AA, RESP_48, false, resp) == 0);
        if (v2 && (resp[0] & 0xFFFu) != 0x1AAu) {
            printf("[SD] CMD8 echo mismatch: 0x%08X\n", resp[0]);
        } else if (!v2) {
            printf("[SD] CMD8 not supported (SD v1 card)\n");
        }

        /* 5. ACMD41: 上电初始化, OCR 的 bit31 置位表示完成 */
        ocr = 0x00FF8000u | (v2 ? (1u << 30) : 0u);
        {
            bool inited = false;
            for (i = 0; i < 2000; i++) {
                if (sd_cmd(55, 0, RESP_48, false, resp) != 0) break;
                if (sd_cmd(41, ocr, RESP_48, false, resp) != 0) break;
                if (resp[0] & 0x80000000u) {
                    sdhc = (resp[0] & (1u << 30)) != 0;
                    inited = true;
                    break;
                }
                sd_delay(2000);
            }
            if (!inited) {
                printf("[SD] ACMD41 timeout (no card?)\n");
                return -2;
            }
        }
    }

    /* 6. CMD2: 取 CID (这个模拟器只验证流程, 不解析厂商信息) */
    if (sd_cmd(2, 0, RESP_136, false, resp) != 0) {
        printf("[SD] CMD2 failed\n");
        return -3;
    }

    /* 7. CMD3: 让卡返回 RCA */
    if (sd_cmd(3, 0, RESP_48, false, resp) != 0) {
        printf("[SD] CMD3 failed\n");
        return -4;
    }
    sd_rca = resp[0] & 0xFFFF0000u;

    /* 8. CMD9: 读 CSD, 算容量 */
    if (sd_cmd(9, sd_rca, RESP_136, false, resp) != 0) {
        printf("[SD] CMD9 failed\n");
        return -5;
    }
    sd_long_to_bytes(resp, csd);
    {
        u32 shift = 0;
        sd_blocks = sd_read_csd_capacity(csd, &shift);
        if (sd_blocks == 0) {
            printf("[SD] cannot parse CSD\n");
            return -5;
        }
        (void)shift;
    }

    /* 9. CMD7: 选中卡 */
    if (sd_cmd(7, sd_rca, RESP_48_BUSY, false, resp) != 0) {
        printf("[SD] CMD7 failed\n");
        return -6;
    }

    /* 10. CMD16: 块长度 512 */
    if (sd_cmd(16, 512, RESP_48, false, resp) != 0) {
        printf("[SD] CMD16 failed\n");
        return -7;
    }

    /* 11. 切到 25MHz */
    sd_set_clock(base_mhz, 25000);

    sd_high_capacity = sdhc;
    sd_ready = 1;

    printf("[SD] card ready: %s, %u blocks (%u MB), RCA=%x\n",
           sdhc ? "SDHC/SDXC" : "SDSC", sd_blocks,
           (sd_blocks * 512u) / (1024u * 1024u), sd_rca >> 16);
    return 0;
}

/* =========================== 对外接口 =========================== */

u32  sd_block_count(void) { return sd_blocks; }
bool sd_is_ready(void)    { return sd_ready; }

int sd_read_block(u32 lba, void *buf)
{
    return sd_transfer(lba, buf, false);
}

int sd_write_block(u32 lba, const void *buf)
{
    return sd_transfer(lba, (void *)buf, true);
}

u32 sd_read_blocks(u32 lba, void *buf, u32 count)
{
    u32 i;
    u8 *p = (u8 *)buf;

    for (i = 0; i < count; i++) {
        if (sd_read_block(lba + i, p + i * 512u) != 0) break;
    }
    return i;
}

u32 sd_write_blocks(u32 lba, const void *buf, u32 count)
{
    u32 i;
    const u8 *p = (const u8 *)buf;

    for (i = 0; i < count; i++) {
        if (sd_write_block(lba + i, p + i * 512u) != 0) break;
    }
    return i;
}

/* =========================== 块设备封装 =========================== */

static int sd_bdev_read(block_dev_t *dev, u32 block_no, void *buf)
{
    (void)dev;
    return sd_read_block(block_no, buf);
}

static int sd_bdev_write(block_dev_t *dev, u32 block_no, const void *buf)
{
    (void)dev;
    return sd_write_block(block_no, buf);
}

block_dev_t *sd_block_device(void)
{
    if (!sd_ready) return NULL;

    sd_dev.name         = "sd0";
    sd_dev.block_size   = 512;
    sd_dev.total_blocks = sd_blocks;
    sd_dev.read         = sd_bdev_read;
    sd_dev.write        = sd_bdev_write;
    sd_dev.priv         = NULL;
    return &sd_dev;
}
