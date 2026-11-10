/**
 * @file    elf_user.h
 * @brief   用户程序 ELF 装载 (v0.6 阶段11/12)
 *
 * 阶段11: exec 时把整个 ELF 读进内核, 逐页拷进页框。
 * 阶段12: exec 时只读 header + program header, 校验后把段表记在
 *         TCB 里; 正文页在第一次被取指/读写时按页从文件读入
 *         (见 mm/vm.c 的 vm_prog_fault)。
 *
 * 只支持 ELF32 / 小端 / EM_ARM / ET_EXEC。段必须页对齐, 页不允许
 * 重叠 (两个段共享一页时, 按页映射无法表达不同的权限)。
 */

#ifndef _MOS_ELF_USER_H
#define _MOS_ELF_USER_H

#include "lib/types.h"
#include "mm/addr_space.h"
#include "kernel/user_prog.h"

/* 返回值 */
#define ELF_USER_OK         0
#define ELF_USER_ENOEXEC   (-1)     /* 不是可加载的 ELF */
#define ELF_USER_ENOMEM    (-2)     /* 保留: 阶段12 起装载本身不分配页框 */
#define ELF_USER_EFAULT    (-3)     /* 段地址越界 / 文件截断 */

/**
 * @brief  校验一个 ELF 文件并把段表登记到任务上
 *
 * 不读正文、不分配页框。校验全部通过之后才调用 user_space_reset()
 * 清掉旧程序 —— 失败时旧进程仍然完整。
 *
 * @param  us        目标任务地址空间
 * @param  path      ELF 文件路径 (内核可读的字符串)
 * @param  img       输出: 程序映像段表 (存进 TCB)
 * @param  entry_out 输出: 入口虚拟地址
 * @return ELF_USER_OK / ELF_USER_ENOEXEC / ELF_USER_EFAULT
 */
int user_elf_load(user_space_t *us, const char *path,
                  prog_image_t *img, u32 *entry_out);

#endif /* _MOS_ELF_USER_H */
