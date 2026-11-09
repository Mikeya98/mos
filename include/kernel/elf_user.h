/**
 * @file    elf_user.h
 * @brief   用户程序 ELF 加载器 (v0.6 阶段11)
 *
 * 之前的 exec 只能跳到共享用户代码段里的另一个函数, 本质上是
 * "换一个入口地址"。真正的 exec 要做的是:
 *
 *   1. 从文件系统读一个独立编译出来的 ELF 文件
 *   2. 解析 program header, 把每个 PT_LOAD 段按页装进任务的
 *      程序窗口 (0x09000000)
 *   3. 把入口地址写回用户态的 PC
 *
 * 这个加载器只支持 ELF32 / 小端 / EM_ARM / ET_EXEC, 够跑裸机
 * 用户程序。段必须页对齐, 代码段映射成用户只读可执行, 数据段
 * 映射成用户读写不可执行 —— 和链接脚本里的布局一致。
 */

#ifndef _MOS_ELF_USER_H
#define _MOS_ELF_USER_H

#include "lib/types.h"
#include "mm/addr_space.h"

/* 返回值 */
#define ELF_USER_OK         0
#define ELF_USER_ENOEXEC   (-1)     /* 不是可加载的 ELF */
#define ELF_USER_ENOMEM    (-2)     /* 页框不够 */
#define ELF_USER_EFAULT    (-3)     /* 段地址越界 / 文件截断 */

/**
 * @brief  把一段 ELF 镜像装进任务的程序窗口
 *
 * 成功时会把 us 原有的私有页和程序页全部释放 (等价于 exec 的
 * "换一个新程序"), 然后按 PT_LOAD 重新映射。
 *
 * 失败时不会动 us 原来的内容: 段表、地址范围、页框都在重置
 * 之前检查/预分配完, 失败路径只归还刚拿到的帧。
 *
 * @param  us        目标任务地址空间
 * @param  image     ELF 镜像起始地址 (内核可直接读)
 * @param  size      镜像字节数
 * @param  entry_out 输出入口虚拟地址
 * @return ELF_USER_OK / ELF_USER_ENOEXEC / ELF_USER_ENOMEM / ELF_USER_EFAULT
 */
int user_elf_load(user_space_t *us, const u8 *image, u32 size,
                  u32 *entry_out);

#endif /* _MOS_ELF_USER_H */
