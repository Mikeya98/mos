# MOS — 一个从零手写的实时操作系统（RTOS）

[![Platform](https://img.shields.io/badge/platform-ARM%20Cortex--A9-blue)](https://github.com/Mikeya98/mos)
[![Language](https://img.shields.io/badge/language-C%20%7C%20Assembly-orange)](https://github.com/Mikeya98/mos)
[![Status](https://img.shields.io/badge/status-v0.6%20--%20ASID%20%2B%20reclaim-brightgreen)](https://github.com/Mikeya98/mos)
[![QEMU](https://img.shields.io/badge/QEMU-verified-lightgrey)](https://github.com/Mikeya98/mos)

MOS 是我从零手写的一个实时操作系统内核，跑在 ARM Cortex-A9 上，用 QEMU 仿真验证。每一行代码都是手写的，没有引用任何现成的 RTOS 内核代码。

这个项目的目标，是把"操作系统内核"里那些听起来很重的概念——调度、同步、内存管理、文件系统、MMU、用户态切换——一个个用最小可运行的方式实现出来。写的过程中踩了不少坑，都记在 commit 信息和配套文章里了。

## 现在做到哪了（v0.6）

内核已经跑通的部分：

- **优先级抢占式调度器**：32 级优先级，用 32-bit 位图 + ARM `CLZ` 指令做到 O(1) 选任务；同优先级内时间片轮转
- **同步原语**：计数信号量、互斥量（带优先级继承）、消息队列（环形缓冲 + 双等待队列）
- **内存管理**：Buddy 伙伴分配器（二分分裂 + 异或合并）、SLAB 对象缓存（固定大小对象，O(1) 分配/释放）
- **文件系统**：MOS-FS——VFS 层 + 块设备抽象 + RAM 盘 + 一套极简磁盘格式
- **MMU**：两级页表（L1 section 1MB + L2 small page 4KB）、identity mapping、DDR 可缓存 / 外设 device 分区、已开启 MMU
- **用户态**：用户态任务（USR/SVC 模式切换）、双栈、trampoline 首次进用户态
- **权限隔离**：内核区 AP=01 仅特权态可访问；用户代码段 AP=10（用户只读、可执行）；DACR 由 manager 改为 client，让页表 AP 位真正生效
- **独立地址空间**：每个用户任务一张 L1 页表 + 一张 L2 表，固定虚拟布局（代码 `0x08000000`、私有数据页 `0x08100000`、栈 `0x08101000-0x08110FFF`），任务之间互相看不到对方的物理页
- **ASID 免刷 TLB**：TTBR0 低 8 位带地址空间编号，私有页打 `nG` 位，切换地址空间只写 TTBR0/CONTEXTIDR，不再全量无效化 TLB
- **资源回收**：任务退出后归还地址空间槽位（L1/L2/数据页/用户栈）、内核栈和 TCB；当前任务踩在自己的栈上，所以走延迟回收队列
- **系统调用**：SVC 栈帧传参，`SYS_YIELD / EXIT / WRITE / SLEEP / GETPID / SEM_WAIT / SEM_POST / GET_TTBR0`；`copy_from_user` / `copy_to_user` 先走当前任务的页表检查用户指针（区分读/写权限）
- **异常恢复**：捕获 Data Abort / Prefetch Abort，打印任务名、故障地址与 DFSR/DFAR，杀掉出错的用户任务后继续调度，系统不宕机

## 目录结构

```
mos/
├── src/
│   ├── boot/startup.S          # 启动代码 + 异常向量表 + SVC 处理
│   ├── kernel/
│   │   ├── main.c              # 内核入口 + 各模块 demo
│   │   ├── task.c              # TCB + 调度器 + 任务终止
│   │   ├── syscall.c           # 系统调用表 + copy_from_user/copy_to_user
│   │   ├── exception.c         # Data/Prefetch Abort → 杀任务并继续调度
│   │   └── sched_asm.S         # 上下文切换 + 用户态 trampoline
│   ├── user/
│   │   └── usermain.c          # 用户态 demo 任务（.user_text 段）
│   ├── drivers/
│   │   ├── uart.c              # UART 驱动（Cadence / 16550）
│   │   ├── gic.c               # GIC PL390 中断控制器
│   │   └── timer.c             # Cortex-A9 私有定时器 + tick
│   ├── ipc/
│   │   ├── semaphore.c         # 计数信号量
│   │   ├── mutex.c             # 互斥量 + 优先级继承
│   │   └── msgq.c              # 消息队列
│   ├── mm/
│   │   ├── buddy.c             # Buddy 伙伴分配器
│   │   ├── slab.c              # SLAB 对象缓存
│   │   ├── mmu.c               # MMU 页表 + TTBR0/ASID 切换 + 用户指针检查
│   │   └── addr_space.c        # 每任务地址空间槽位：分配、ASID、回收
│   ├── fs/
│   │   ├── block_dev.c         # 块设备抽象 + RAM 盘
│   │   ├── mos_fs.c            # MOS-FS 磁盘格式
│   │   └── vfs.c               # VFS 层
│   └── lib/
│       ├── printf.c            # 极简 printf（不依赖 libc）
│       └── string.c            # 基础字符串操作
├── include/                    # 头文件
├── kernel.lds                  # 链接脚本
└── Makefile                    # 构建系统
```

## 编译运行

依赖 `arm-none-eabi-gcc`（ARM 交叉编译器）和 `qemu-system-arm`（支持 `xilinx-zynq-a9`）。

```bash
make            # 编译，生成 build/mos.elf
make qemu-run   # 编译并直接在 QEMU 里跑（串口输出到终端）
make qemu       # 编译并启动 QEMU，带 GDB server（:1234）
make dump       # 反汇编到 build/mos.lst
make size       # 打印各段大小
make clean      # 清理构建产物
```

启动后串口大致输出（截取关键部分）：

```
========================================
  MOS RTOS v0.6
  MMU + Buddy + SLAB + MOS-FS
========================================

[INIT] MMU enabled (identity map)
[INIT] GIC initialized
[INIT] Buddy allocator ready
[INIT] Task subsystem ready
[INIT] Syscall table ready (SYS_WRITE/EXIT/GETPID/SEM_*)
[INIT] Timer started
...（buddy / slab / 文件系统三组 demo）...
[AS] u_iso_a L1=234000 L2=245000 data=28A000 stack=28B000
[AS] u_iso_b L1=238000 L2=245400 data=29B000 stack=29C000
[DEMO] 6 user tasks created, each with its own L1 page table
Starting scheduler...
--- MOS RTOS is running now ---
[user] u_waiter: sem_wait(0) — should block here
[user] u_worker: getpid() > 0, private stack at 0x08101000-0x08110FFF
[user] u_worker: sem_post(0) done, exiting
[user] u_fault : now reading kernel address 0x00100000 ...

========================================
  [ABORT] Data Abort
  task   : u_fault (pid=3)
  mode   : USR (0x10)
  pc     : 8000210
  addr   : 100000
  fsr    : D  (permission fault (section))
========================================
[ABORT] killing user task, scheduler continues

[user] u_badptr: passing kernel pointer 0x00100000 to sys_write
[SYS] write: pointer 100000 len=16 rejected → EFAULT
[user] u_badptr: rejected with negative errno — pointer check OK
[user] u_badptr: passing unmapped 0x08200000 to sys_write
[SYS] write: pointer 8200000 len=16 rejected → EFAULT
[user] u_badptr: unmapped page rejected by page-table walk
[user] u_iso_a : wrote A to my 0x08100000, waiting for B
[user] u_iso_a : my TTBR0|ASID = 0x0023C005
[user] u_iso_b : wrote B to the same virtual address
[user] u_iso_b : my TTBR0|ASID = 0x00240006
[user] u_iso_b : read back B — my own page
[user] u_waiter: woken by u_worker, exiting
[user] u_iso_a : read back A — address spaces are separate

[POOL] address-space recycling demo
[POOL] free slots: 8 / 8
[POOL] wave 0: created 8 tasks, free slots now 0
[POOL] wave 0: spawn tasks gone, free slots = 8
    （wave 1 / 2 重复两轮）
[POOL] done — 8 of 8 slots free
```

这些输出分别验证了：

1. `u_worker` 的栈地址落在自己的栈页里，读写正常；
2. `u_waiter` 阻塞在信号量上，被 `u_worker` post 后唤醒，`sem_wait/sem_post` 系统调用可用；
3. `u_fault` 读内核地址 `0x00100000` 触发 **Permission Fault**（DFSR=0xD），异常处理杀掉该任务，其余任务继续运行；
4. `u_badptr` 两种非法指针都被拒绝：内核地址过不了范围检查，用户范围里未映射的 `0x08200000` 过不了页表遍历；
5. `u_iso_a` / `u_iso_b` 往同一个虚拟地址 `0x08100000` 写不同标记，各自读回自己的值，且两次读到的 TTBR0 不同（低 8 位是 ASID：5 / 6），说明地址空间确实分开了；
6. `pool_mgr` 连续三轮各创建 8 个短命任务（正好占满 8 个地址空间槽位），每轮结束后 8 个槽位全部归还。如果资源不回收，第二轮就会因为"没有空闲槽位"创建失败。

> QEMU 的 zynq 机器默认内存放不下 `0x08000000` 的第二段，`make qemu-run` 里加了 `-m 1024`。

## 版本规划

| 版本 | 里程碑 | 状态 |
|------|--------|------|
| v0.1 | 最小骨架：调度器 + UART + 定时器 + 信号量 | ✅ |
| v0.2 | 同步原语：互斥量（优先级继承）+ 消息队列 | ✅ |
| v0.3 | Buddy 伙伴内存分配器 | ✅ |
| v0.4 | SLAB 对象缓存 + TCB 动态分配 | ✅ |
| v0.5 | MOS-FS 文件系统（VFS + RAM 盘） | ✅ |
| v0.6 | MMU + 用户态 + 权限隔离 + 每任务独立页表 + ASID + 资源回收（阶段1 开 MMU、阶段2 模式切换、阶段3 隔离/异常恢复、阶段4 TTBR0 切换、阶段5 ASID/reclaim） | ✅ |
| 下一步 | 按需分页（缺页异常 + 按需分配物理页） | 🔜 |

## 构建细节

- **编译器**：ARM GCC（`arm-none-eabi-gcc`）
- **目标**：`-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=soft`
- **链接**：自定义 `kernel.lds`（OCM + DDR 内存布局）
- **QEMU**：`qemu-system-arm -M xilinx-zynq-a9 -m 1024`
- **体积**：text 约 29.6KB（用户态代码单独链接在 `0x08000000`）

## 关于

作者：Mikeya98，嵌入式工程师，关注实时操作系统、FPGA 加速、裸机开发。

这是个人学习项目，从零实现，与任何商业 RTOS 无关。
