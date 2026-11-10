/**
 * @file    user_prog.h
 * @brief   文件映射的用户程序映像 (v0.6 阶段12)
 *
 * 阶段11 的加载器在 exec 时把整个 ELF 读进内核、逐页拷进页框。这一版
 * 改成"只登记, 不读正文":
 *
 *   exec 时   只读 ELF header + program header, 校验后把段表记在 TCB 里
 *   首次访问  取指 / 读 / 写落到未映射的程序页 → Abort → 按页读文件
 *
 * 这就是文件映射的雏形: 页表项从一个"文件位置"按需生成, 而不是
 * 从一整块已经读进内存的镜像里拷贝。
 *
 * 简化之处: 不持有文件对象, 每次缺页按路径重新 open/seek/read/close。
 * Linux 用 struct file 和 address_space 管理, 我们这里 RAM 盘的开销
 * 可以忽略, 换来的是不用处理 fd 在 fork/exit 时的引用计数。
 */

#ifndef _MOS_USER_PROG_H
#define _MOS_USER_PROG_H

#include "lib/types.h"

#define PROG_SEG_MAX    4       /* 一个用户程序最多 4 个 PT_LOAD 段 */
#define PROG_PATH_MAX   64      /* 文件路径长度上限 (与 SYS_EXEC_ELF 一致) */

/**
 * @brief 一个 PT_LOAD 段在文件里的位置
 *
 * filesz < memsz 时, [filesz, memsz) 是 .bss: 不占文件字节, 加载后必须
 * 是 0。mem 的最后一页可能只有一部分属于这个段, 其余补 0。
 */
typedef struct {
    u32 vaddr;          /* 段起始虚拟地址 (页对齐) */
    u32 memsz;          /* 段在内存中占的字节数 */
    u32 filesz;         /* 文件中有内容的部分 */
    u32 offset;         /* 文件偏移 */
    u32 writable;       /* 1 = 数据段 (可写、不可执行) */
} prog_seg_t;

/**
 * @brief 一个任务当前装载的程序映像 (放在 TCB 里)
 *
 * 只有 prog_valid = 1 时才可信。fork 出来的子进程会复制一份 —— 它
 * 继承父进程的程序, 没被父进程碰过的页在子进程里同样按需装载。
 */
typedef struct {
    char        path[PROG_PATH_MAX];   /* ELF 文件路径, 缺页时重新打开 */
    u32         entry;                 /* 入口虚拟地址 */
    u32         nseg;
    prog_seg_t  seg[PROG_SEG_MAX];
} prog_image_t;

/**
 * @brief  找出覆盖某一页的段
 * @param  img   程序映像
 * @param  page  页对齐的虚拟地址
 * @return 段描述符, 没有段覆盖这一页时返回 NULL
 */
const prog_seg_t *user_prog_seg(const prog_image_t *img, u32 page);

/**
 * @brief  按页从文件里读一段程序内容
 *
 * 先把 4KB 目标缓冲清零, 再把 [page, page+4096) 与段文件内容的重叠
 * 部分读进来; 落在 filesz 之后的部分保持 0 (即 .bss 语义)。
 *
 * @param  img   程序映像
 * @param  page  页对齐的虚拟地址
 * @param  dst   目标缓冲 (4KB)
 * @return 0 成功, -1 没有段覆盖 / 打开或读取失败
 */
int user_prog_read_page(const prog_image_t *img, u32 page, void *dst);

#endif /* _MOS_USER_PROG_H */
