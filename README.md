# MOS — 一个从零手写的实时操作系统（RTOS）

[![Platform](https://img.shields.io/badge/platform-ARM%20Cortex--A9-blue)](https://github.com/Mikeya98/mos)
[![Language](https://img.shields.io/badge/language-C%20%7C%20Assembly-orange)](https://github.com/Mikeya98/mos)
[![Status](https://img.shields.io/badge/status-v0.6%20--%20MMU%20%2B%20user%20mode-brightgreen)](https://github.com/Mikeya98/mos)
[![QEMU](https://img.shields.io/badge/QEMU-verified-lightgrey)](https://github.com/Mikeya98/mos)

MOS 是我从零手写的一个实时操作系统内核，跑在 ARM Cortex-A9 上，用 QEMU 仿真验证。每一行代码都是手写的，没有引用任何现成的 RTOS 内核代码。

这个项目的目标，是把"操作系统内核"里那些听起来很重的概念——调度、同步、内存管理、文件系统、MMU、用户态切换——一个个用最小可运行的方式实现出来。写的过程中踩了不少坑，都记在 commit 信息和配套文章里了。

## 现在做到哪了（v0.6）

内核已经跑通的部分：

- **优先级抢占式调度器**：32 级优先级，用 32-bit 位图 + ARM `CLZ` 指令做到 O(1) 选任务；同优先级内时间片轮转
- **同步原语**：计数信号量、互斥量（带优先级继承）、消息队列（环形缓冲 + 双等待队列）
- **内存管理**：Buddy 伙伴分配器（二分分裂 + 异或合并）、SLAB 对象缓存（固定大小对象，O(1) 分配/释放）
- **文件系统**：MOS-FS——VFS 层 + 块设备抽象 + RAM 盘 + 一套极简磁盘格式
- **MMU**：一级页表（section 映射，1MB 粒度）、identity mapping、DDR 可缓存 / 外设 device 分区、已开启 MMU
- **用户态**：用户态任务（USR/SVC 模式切换）、双栈、trampoline 首次进用户态、SVC 系统调用（yield）

## 目录结构

```
mos/
├── src/
│   ├── boot/startup.S          # 启动代码 + 异常向量表 + SVC 处理
│   ├── kernel/
│   │   ├── main.c              # 内核入口 + 各模块 demo
│   │   ├── task.c              # TCB + 调度器 + 系统调用分发
│   │   └── sched_asm.S         # 上下文切换 + 用户态 trampoline
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
│   │   └── mmu.c               # MMU 一级页表 + identity mapping
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
[INIT] Timer started
...（buddy / slab / 文件系统三组 demo）...
[DEMO] user tasks: user_a user_b (both prio=1)
Starting scheduler...
--- MOS RTOS is running now ---
[A] cpsr=0x60000010 mode=16 n=0
[B] mode=16 n=0
...
```

最后两行 `mode=16` 就是 CPU 处于 USR 用户态（`0x10`）的证明——两个用户任务通过 `SVC #0` 主动让出 CPU 轮流运行。

## 版本规划

| 版本 | 里程碑 | 状态 |
|------|--------|------|
| v0.1 | 最小骨架：调度器 + UART + 定时器 + 信号量 | ✅ |
| v0.2 | 同步原语：互斥量（优先级继承）+ 消息队列 | ✅ |
| v0.3 | Buddy 伙伴内存分配器 | ✅ |
| v0.4 | SLAB 对象缓存 + TCB 动态分配 | ✅ |
| v0.5 | MOS-FS 文件系统（VFS + RAM 盘） | ✅ |
| v0.6 | MMU + 用户态（阶段1 开 MMU、阶段2 用户态/模式切换） | ✅ |
| 下一步 | 阶段3 系统调用表、阶段4 每任务独立页表（地址空间隔离） | 🔜 |

## 构建细节

- **编译器**：ARM GCC（`arm-none-eabi-gcc`）
- **目标**：`-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=soft`
- **链接**：自定义 `kernel.lds`（OCM + DDR 内存布局）
- **QEMU**：`qemu-system-arm -M xilinx-zynq-a9`
- **体积**：text 约 20KB，编译 0 warning

## 关于

作者：Mikeya98，嵌入式工程师，关注实时操作系统、FPGA 加速、裸机开发。

这是个人学习项目，从零实现，与任何商业 RTOS 无关。
