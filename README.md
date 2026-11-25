# MOS — 一个从零手写的实时操作系统（RTOS）

[![Platform](https://img.shields.io/badge/platform-ARM%20Cortex--A9-blue)](https://github.com/Mikeya98/mos)
[![Language](https://img.shields.io/badge/language-C%20%7C%20Assembly-orange)](https://github.com/Mikeya98/mos)
[![Status](https://img.shields.io/badge/status-v0.7%20--%20SD%20card%20%2B%20signals-brightgreen)](https://github.com/Mikeya98/mos)
[![QEMU](https://img.shields.io/badge/QEMU-verified-lightgrey)](https://github.com/Mikeya98/mos)

MOS 是我从零手写的一个实时操作系统内核，跑在 ARM Cortex-A9 上，用 QEMU 仿真验证。每一行代码都是手写的，没有引用任何现成的 RTOS 内核代码。

这个项目的目标，是把"操作系统内核"里那些听起来很重的概念——调度、同步、内存管理、文件系统、MMU、用户态切换——一个个用最小可运行的方式实现出来。写的过程中踩了不少坑，都记在 commit 信息和配套文章里了。

## 现在做到哪了（v0.7）

内核已经跑通的部分：

- **优先级抢占式调度器**：32 级优先级，用 32-bit 位图 + ARM `CLZ` 指令做到 O(1) 选任务；同优先级内时间片轮转
- **同步原语**：计数信号量、互斥量（带优先级继承）、消息队列（环形缓冲 + 双等待队列）
- **内存管理**：Buddy 伙伴分配器（二分分裂 + 异或合并）、SLAB 对象缓存（固定大小对象，O(1) 分配/释放）
- **文件系统**：MOS-FS——VFS 层 + 块设备抽象 + 一套极简磁盘格式；inode 10 个直接块 + 1 个一级间接块（128 个指针），最大文件 69KB；块位图支持多块，能管理大容量设备
- **SD 卡驱动（v0.7）**：Zynq 的 SDHCI 控制器 + PIO 轮询传输，走完整的 SD 初始化流程（CMD0 → CMD8 → ACMD41 → CMD2 → CMD3 → CMD9 → CMD7 → CMD16），CMD17/CMD24 按块读写，支持 SDSC（字节寻址）和 SDHC/SDXC（块寻址）。文件系统优先挂到 SD 卡上，没有卡才退回 RAM 盘；QEMU 用 `-drive file=build/sd.img,if=sd` 挂一张 8MB 镜像，第二次启动能直接读到上次写的文件
- **持久化验证**：`make qemu-run` 第一次运行格式化卡，重启后挂载已有 MOS-FS，`/hello.txt` 内容原样读回；用户程序 `/hello.elf` 也从 SD 卡按需装载，页缓存在 SD 上照常工作
- **MMU**：两级页表（L1 section 1MB + L2 small page 4KB）、identity mapping、DDR 可缓存 / 外设 device 分区、已开启 MMU
- **用户态**：用户态任务（USR/SVC 模式切换）、双栈、trampoline 首次进用户态
- **权限隔离**：内核区 AP=01 仅特权态可访问；用户代码段 AP=10（用户只读、可执行）；DACR 由 manager 改为 client，让页表 AP 位真正生效
- **独立地址空间**：每个用户任务一张 L1 页表 + 一张 L2 表，固定虚拟布局（代码 `0x08000000`、私有数据页 `0x08100000`、栈保护页 `0x08101000`、栈 `0x08102000-0x081FFFFF`），任务之间互相看不到对方的物理页
- **ASID 免刷 TLB**：TTBR0 低 8 位带地址空间编号，私有页打 `nG` 位，切换地址空间只写 TTBR0/CONTEXTIDR，不再全量无效化 TLB
- **资源回收**：任务退出后归还地址空间槽位（L1/L2）、页表里映射过的物理页框、内核栈和 TCB；当前任务踩在自己的栈上，所以走延迟回收队列
- **按需分页**：用户私有区的 L2 表初始全是 fault，第一次访问才从物理页框池（64 帧 × 4KB）分配一帧、清零、填表，然后按地址无效化单个 TLB 项、回到故障指令重试；栈最多可以长到 1016KB 而实际只占用到的几页，保护页负责发现栈溢出；系统调用拷贝用户内存前也会先代用户补页
- **页面换出**：页框池耗尽时，按轮转策略把当前任务的一页写到 swap 区（16 页 × 4KB），L2 表项改写成换出描述符（低两位仍为 00，硬件继续报缺页）；再次访问时从 swap 读回并恢复映射。页表本身就是换出账本，任务退出时扫一遍 L2 把 swap 槽和页框一起回收；演示任务故意多碰 8 页，验证了 8 次换出 + 8 次换入后每个页的内容都没有丢
- **fork + 写时复制**：`SYS_FORK` 复制当前用户任务，新建地址空间槽位；已映射的页父子共享同一个物理帧，两侧页表都改成用户只读，谁先写谁在缺页处理里复制一份私有副本（引用计数减到 1 时直接改回可写）。换出页复制独立的 swap 槽，互不影响；物理帧按引用计数回收
- **每任务用户栈指针**：`SP_usr` 是 CPU 的 banked 寄存器，不随 TCB 自动切换。进入内核时把 `SP_usr` 存进 TCB，调度到某个用户任务之前再写回，避免任务切换后踩到上一个任务的用户栈
- **exec（简化版）**：`SYS_EXEC` 清空当前任务的私有页表和 swap 槽、保留地址空间槽位与共享用户代码段，然后从用户代码段内的新入口重新开始；数据页和栈页在第一次访问时重新按需分配。异常返回前把 `SPSR_svc` 改成 SVC 模式，先在内核跳板里设置好 `SP_usr` 再回用户态
- **真正的 ELF 加载**：`SYS_EXEC_ELF` 从 RAM 盘打开 `/hello.elf`，解析 ELF32 program header，把 PT_LOAD 段装进任务的程序窗口 `0x09000000`，代码段映射成用户只读可执行、数据段映射成用户读写不可执行，`.bss` 自动清零。加载前先校验头部/段范围/页重叠，失败时旧程序保持完整；成功后旧数据页和栈全部释放，入口写回用户态 PC。演示程序由 Makefile 单独编译、打包成 C 数组嵌进内核、开机写进 RAM 盘，走的是完整的“打开文件 → 读取 → 解析”路径
- **按需装载代码页**：阶段12 起，exec 只读 ELF 头部（前 1KB 的 ELF header + program header），把段表登记进 TCB，正文一页都不读；第一次执行/读写到某一页时，Prefetch/Data Abort 进入 `vm_prog_fault`，按页从文件 `open/seek/read` 进来并映射。代码页需要 D-cache clean + I-cache invalidate 之后才能安全取指（QEMU 不模拟 cache，上板才见真章）。`/hello.elf` 16936 字节，exec 时 0 页驻留，运行时按需装载 2 页代码 + 1 页数据
- **文件页缓存**：`mm/page_cache.c` 维护一张 16 项线性表，键是 `(文件路径, 文件偏移)`，值是物理页框。只读 ELF 代码页进缓存，多个进程（包括先后执行的）共享同一份物理内存；每项自带引用计数，`refs==1` 表示只有缓存自己钉着，可以被 LRU 驱逐或直接回收。可写数据段不进缓存。页框池耗尽时先回收空闲缓存页——文件页是干净的，丢弃比换出私有页便宜。演示：子进程第一次 exec 填 2 页缓存，父进程 wait 后再次 exec 同一个文件，2 次缺页全部命中；收尾把空闲缓存页回收掉，64/64 帧完整归还
- **程序区与 fork**：每个地址空间多一张 `0x09000000` 的 L2 页表；fork 时只读代码页如果在页缓存里，直接加引用共享同一帧，可写数据段仍整帧复制（数据页通常只有一两页，比再维护一套程序区 COW 简单）。演示里加载后的程序自己 fork 一次，子进程退出码 3 被父进程 wait 收回
- **僵尸进程与 wait**：`SYS_EXIT` 带退出码，有父进程的任务退出后变成僵尸，TCB 留在任务表里等父进程 `SYS_WAIT` 取走；父进程可以阻塞等任意/指定子进程，子进程退出时被唤醒。僵尸状态的转换放在 `schedule()` 切换前完成，避免父进程在退出任务还踩着自己内核栈时就把它回收
- **实时信号排队 + SIG_IGN（v0.7 阶段4）**：TCB 增加 `sig_count[32]` 和 `sig_ignore` 位图。`SIGRTMIN..SIGRTMAX`（24..31）连续发 3 次会在 pending 里排 3 份，handler 依次跑 3 次；普通信号在 pending 期间重复到达只保留一份（和 POSIX 一致）。投递顺序先看段：实时信号整段优先于普通信号，段内再取编号最小的。`sigaction(sig, SIG_IGN, 0)` 直接设置忽略位，之后投递的信号在 `kill` 时就丢掉、连 pending 都不置；fork 继承忽略位、不继承排队份数，exec 全部复位。`u_signal` 新增三段验证：排队 3 份、RT 优先于更小号的 SIGUSR1、SIG_IGN 后 kill 自己仍存活
- **信号屏蔽与可中断等待（v0.7 阶段3）**：`SYS_SIGPROCMASK`（BLOCK/UNBLOCK/SETMASK）维护每任务屏蔽字，被屏蔽的信号只挂在 pending 里，解除屏蔽后在下一次返回用户态时投递。信号可以打断阻塞中的 `sem_wait`、`wait` 和 `sleep`：目标任务在信号量等待队列上时由 TCB 里的 `block_remove` 回调摘链并退还计数，阻塞的系统调用返回 `-EINTR`。阻塞前也会检查一次 pending，避免"信号先到、任务随后睡死"的窗口
- **信号值 sigqueue（v0.7 阶段5）**：`SYS_SIGQUEUE(pid, sig, value)` 让信号带一个 32 位值，handler 从 r1 拿到（r0 仍是信号号）。普通信号不排队，已 pending 时后到的值丢弃、只保留第一份；实时信号每个值进独立的环形队列，按 FIFO 依次投递。内核在 handler 执行期间自动屏蔽当前信号（POSIX 默认语义），旧屏蔽字存进信号帧第 17 个字、`SIGRETURN` 时恢复，这样同一个实时信号不会嵌套进自己的 handler。`u_signal` 验证了三件事：普通信号合并保留第一个值、RT 三个值按 C1/C2/C3 顺序到达、跨任务把 0x42 发给孩子进程并由退出码带回
- **sa_mask 与 SA_NODEFER（v0.7 阶段6）**：`SYS_SIGACTION_EX(sig, handler, tramp, flags, mask)` 在安装 handler 时指定附加屏蔽字；handler 期间的屏蔽字 = 旧屏蔽字 + `sa_mask` + 当前信号，当前信号那一位可以用 `SA_NODEFER` 关掉，从而允许同一个信号嵌套进自己的 handler。`u_signal` 用深度计数验证了默认不嵌套、NODEFER 下深度到 2；另用 `sa_mask` 让 SIGUSR2 等 SIGUSR1 的 handler 返回再投递。顺带修了两个现场问题：嵌套投递时 r1 参数会覆盖外层 handler 的现场（改成先保存 r0-r12、再写 handler 参数）；用户态 SVC 封装没声明 `lr` 被破坏，编译器把局部变量放在 LR 里，跨信号投递会被 tramp 覆盖
- **siginfo_t、SA_SIGINFO / SA_RESETHAND、队列满通知（v0.7 阶段8）**：信号帧从 20 字扩到 24 字，后面接一个 `mos_siginfo_t`（signo/code/pid/value）。装 `SA_SIGINFO` 时 handler 原型是 `handler(sig, siginfo, ucontext)`，第二个参数指向栈上的发送方信息，`si_code` 区分 `SI_USER`（kill）和 `SI_QUEUE`（sigqueue）；第三个参数暂时没有实现，置 0。`SA_RESETHAND` 在 handler 进入时把处置方式复位成默认动作，同一个实时信号剩下的排队副本从第二份开始按默认动作终止。实时队列满时 `sigqueue` 返回 `-EAGAIN`，不再静默丢信号；未定义的 flags 位仍然报 `EINVAL`
- **fork 的 callee-saved 寄存器修复（v0.7 阶段8）**：查 `SA_RESETHAND` 测试时发现子进程把 `sig_tramp` 的地址读成了 fork 的返回地址。根因是 fork 跳板把 r4/r5 当作用户栈顶和返回 PC 的载体，r6-r11 清零，而 r4-r11 是 AAPCS 的 callee-saved 寄存器，父进程的函数可能正把活值放在里面。现在子进程的 r4-r11 从父进程进 SVC 时的现场原样复制，用户栈顶和 PC 改存 TCB、由 `fork_child_setup()` 写回，跳板只动 caller-saved 寄存器。exec 两条路径同步改成从 TCB 取入口
- **信号返回模式修复（v0.7 阶段7）**：根因是 SPSR 是 CPU 全局寄存器，不跟任务走。IRQ 在内核态打断时 `spsr_irq` 是 SVC，原来照样投递信号，handler 就会在 SVC 模式执行，`bx lr` 跳回内核、`SIGRETURN` 永不执行，用户 SP 停在信号帧上（表现为跨任务 sigqueue 后 `wait(&code)` 偶尔读回 -1，以及三层嵌套跑飞）。现在 IRQ/SVC 两条返回路径都只在 SPSR 模式位是 USR 时投递；exec 临时改 SPSR_svc 前后关中断，避免被 tick 切走后污染别的任务。`SA_NODEFER` 测试改成 handler 内自排队，稳定跑到三层嵌套；QEMU 连续 10 次 0 失败
- **信号（v0.7 阶段2）**：`SYS_SIGACTION / KILL / SIGRETURN / GETPPID`。handler 地址存在 TCB 里，kill 只置 pending 位；任务下一次从内核返回用户态时，内核把 r0-r12/PC/SP/LR 压成信号帧放到用户栈上，把用户态 LR 改成用户传入的 tramp，再跳进 handler。handler 返回时 tramp 发 `SIGRETURN`，内核从信号帧恢复现场继续跑。没装 handler 的信号按默认动作终止进程；fork 继承处理函数但清空 pending，exec 全部复位；`u_signal` 演示了 handler 投递、sigreturn 恢复和默认终止（退出码 -12）三种路径
- **系统调用**：SVC 栈帧传参，`SYS_YIELD / EXIT / WRITE / SLEEP / GETPID / SEM_WAIT / SEM_POST / GET_TTBR0 / VM_STATS / FORK / EXEC / WAIT / EXEC_ELF / SIGACTION / KILL / SIGRETURN / GETPPID / SIGPROCMASK / SIGQUEUE`；`copy_from_user` / `copy_to_user` 先代用户补页，再走当前任务的页表检查用户指针（区分读/写权限）
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
│   │   ├── elf_user.c          # 用户程序 ELF 装载（只读头部 + 段表登记）
│   │   ├── user_prog.c         # 程序映像查段 + 按页读文件（阶段12）
│   │   └── sched_asm.S         # 上下文切换 + 用户态 trampoline
│   ├── user/
│   │   ├── usermain.c          # 用户态 demo 任务（.user_text 段）
│   │   └── prog/hello.c        # 独立编译的用户程序（链接到 0x09000000）
│   ├── drivers/
│   │   ├── uart.c              # UART 驱动（Cadence / 16550）
│   │   ├── gic.c               # GIC PL390 中断控制器
│   │   ├── timer.c             # Cortex-A9 私有定时器 + tick
│   │   └── sd.c                # SDHCI + SD 卡驱动（v0.7）
│   ├── ipc/
│   │   ├── semaphore.c         # 计数信号量
│   │   ├── mutex.c             # 互斥量 + 优先级继承
│   │   └── msgq.c              # 消息队列
│   ├── mm/
│   │   ├── buddy.c             # Buddy 伙伴分配器
│   │   ├── slab.c              # SLAB 对象缓存
│   │   ├── mmu.c               # MMU 页表 + TTBR0/ASID 切换 + 用户指针检查
│   │   ├── frame.c             # 物理页框池（位图 + 所有者记录）
│   │   ├── swap.c              # 页面换出区（槽位位图 + 4KB 拷贝 + 回收）
│   │   ├── vm.c                # 按需分页：缺页分类、补页、页面换出、统计
│   │   ├── page_cache.c        # 文件页缓存：共享代码页 + 引用计数 + LRU
│   │   └── addr_space.c        # 每任务地址空间槽位：分配、ASID、回收页框
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
make qemu-run   # 编译并直接在 QEMU 里跑（自动创建 8MB SD 镜像 build/sd.img）
make qemu       # 编译并启动 QEMU，带 GDB server（:1234）

# 想从空白卡重新验证持久化，删掉镜像再跑一次即可：
rm build/sd.img && make qemu-run
make dump       # 反汇编到 build/mos.lst
make size       # 打印各段大小
make clean      # 清理构建产物
```

启动后串口大致输出（截取关键部分）：

```
========================================
  MOS RTOS v0.6
  MMU + per-task page tables + ASID + demand paging
  + file-backed page cache (shared text pages)
========================================

[INIT] MMU enabled (identity map)
[INIT] Frame allocator ready (64 frames = 256 KB)
[INIT] GIC initialized
[INIT] Buddy allocator ready
[INIT] Task subsystem ready
[INIT] Syscall table ready (SYS_WRITE/EXIT/GETPID/SEM_*)
[INIT] Timer started
...（buddy / slab / 文件系统三组 demo）...
[AS] u_iso_a slot=4 asid=5 L1=240000 L2=251000 (data/stack on demand)
[AS] u_demand slot=6 asid=7 L1=248000 L2=251800 (data/stack on demand)
[DEMO] 8 user tasks created, each with its own L1 page table
Starting scheduler...
--- MOS RTOS is running now ---
[user] u_waiter: sem_wait(0) — should block here
[PF] u_worker: 81FF000 write -> frame 252000 (fault #1)
[user] u_worker: getpid() > 0, stack top at 0x08200000 (on-demand pages)
[user] u_fault : now reading kernel address 0x00100000 ...

========================================
  [ABORT] Data Abort
  task   : u_fault (pid=3)
  mode   : USR (0x10)
  fsr    : D  (permission fault (section))
========================================
[ABORT] killing user task, scheduler continues

[PF] u_iso_a: 8100000 write -> frame 253000 (fault #2)
[user] u_iso_a : wrote A to my 0x08100000, waiting for B
[user] u_iso_a : my TTBR0|ASID = 0x00240005
[user] u_iso_b : read back B — my own page
[user] u_demand : first write to data page 0x08100000 ...
[user] u_demand : data page OK, now recursing 8 levels (touching stack pages)
[PF] u_demand: 81FC000 write -> frame 258000 (fault #5)
[PF] u_demand: 8180000 write -> frame 25A000 (fault #7)
[user] u_demand : faults        = 0x00000007
[user] u_demand : mapped pages  = 0x00000007
[user] u_demand : frames free   = 0x00000037
[user] u_overflow: writing the guard page 0x08101000 ...

========================================
  [ABORT] Data Abort
  task   : u_overflow (pid=8)
  addr   : 8101000
  fsr    : 807  (translation fault (page))
========================================
[ABORT] stack overflow: 8101000 is the guard page (8101000)
[ABORT] killing user task, scheduler continues
[AS] slot 6 (asid 7): reclaimed 7 page frames

[user] u_fork  : before fork, data page = 0x11111111
[FORK] u_fork(pid 7) -> u_fork_c(pid 12), pc=80007E4 sp=81FFFC8
[user] u_fork  : fork() returned child pid = 0x0000000C
[user] u_fork_c: inherited value = 0x11111111
[COW] u_fork_c: 8100000 copied frame 268000 -> 266000 (ref was 2)
[user] u_fork_c: wrote 0x22222222 to my own copy
[user] u_fork  : after child wrote, parent reads 0x11111111

[user] u_exec  : before exec, data page = 0xAAAAAAAA
[user] u_exec  : exec -> u_exec_new (private pages reset)
[user] u_exec2 : after exec, data page = 0x00000000
[user] u_exec2 : value is 0, old address space is gone

[user] u_elf   : exec_elf(/missing.elf) should fail, old image stays alive
[EXEC] u_elf: load /missing.elf failed (-3), old image kept
[user] u_elf   : exec_elf(/hello.elf) — real ELF from RAM disk
[ELF] /hello.elf: 2 segments, 2 pages pending (lazy), entry 90000B8
[EXEC] u_elf: /hello.elf mapped, entry 90000B8 (code pages load on demand)
[ELF-PF] u_elf: 9000000 <- /hello.elf+1000 (frame 272000, page #1)
[ELF-PF] u_elf: 9001000 <- /hello.elf+2000 (frame 275000, page #2)
[elf] .data loaded, .bss is zero
[elf] hello — this image was loaded from /hello.elf
[elf] pid = 9
[elf] child : forked, program pages copied to new frames
[elf] parent: wait reaped child, code = 3
[elf] exit(0)

[POOL] free slots: 14 / 14, free frames: 64 / 64
[POOL] wave 0: created 8 tasks, free slots now 5, free frames 64
[user] u_spawn : running in a recycled address space
[POOL] wave 0: spawn tasks gone, free slots = 14, free frames = 64
    （wave 1 / 2 重复两轮）
[POOL] done — 14 of 14 slots free, 64 of 64 frames free
```

这些输出分别验证了：

1. `u_waiter` 阻塞在信号量上，被 `u_worker` post 后唤醒，`sem_wait/sem_post` 系统调用可用；
2. `u_fault` 读内核地址 `0x00100000` 触发 **Permission Fault**（DFSR=0xD），异常处理杀掉该任务，其余任务继续运行；
3. `u_badptr` 两种非法指针都被拒绝：内核地址过不了范围检查，用户范围里未映射的 `0x08200000` 过不了页表遍历；
4. `u_iso_a` / `u_iso_b` 往同一个虚拟地址 `0x08100000` 写不同标记，各自读回自己的值，TTBR0 低 8 位是各自的 ASID，说明地址空间确实分开了；
5. `u_demand` 只用到 7 页就报了 7 次缺页：数据页 1 页、递归压栈 5 页、统计缓冲区 1 页（这一页是内核 `copy_to_user` 里 `vm_touch` 代补的）。虚拟栈上限 1016KB，物理上只占了实际碰到的几页；
6. `u_overflow` 写保护页 `0x08101000`，内核判定栈溢出并杀掉任务，数据页不会被栈悄悄覆盖；
7. `u_fork` 先往数据页写 `0x11111111`，fork 之后子进程读到同样的值，改成 `0x22222222` 后退出；父进程读回仍然是 `0x11111111`。日志里的 `[COW]` 行是子进程第一次写共享页时触发的页复制，父进程的数据没有被改动；
8. `u_exec` 往数据页写 `0xAAAAAAAA`，然后 exec 到 `u_exec_new`；新入口读到的数据页是 0，说明私有地址空间确实被清空重建了，共享的用户代码段不受影响；
9. `u_elf` 先从 RAM 盘装载独立编译的 `/hello.elf`：exec 日志只有“2 pages pending”，正文页在第一次取指/读数据时才出现两条 `[ELF-PF]`；程序自己 fork 一次，子进程退出码 3 被父进程 wait 收回。加载不存在的 `/missing.elf` 返回 `-ENOENT`，旧镜像继续运行；
10. `u_signal` 给父进程发 `SIGUSR1`：handler 在父进程下一次系统调用返回时被投递，写私有数据页后正常返回；`SIGRETURN` 恢复被打断的 yield 循环，等待循环继续往下跑。子进程 B 没有装 `SIGUSR2` handler，kill 自己后按默认动作终止，父进程 wait 拿到的退出码是 -12；
11. `pool_mgr` 连续三轮各创建 8 个短命任务（14 个地址空间槽位里占 8 个），每轮结束后槽位全部归还；这些任务不碰栈也不碰数据页，所以**一个物理页框都不占**（free frames 始终 64）——同一份 256KB 页框池，按阶段4 的预分配方案连 4 个任务都撑不住。

> QEMU 的 zynq 机器默认内存放不下 `0x08000000` 的第二段，`make qemu-run` 里加了 `-m 1024`。

## 版本规划

| 版本 | 里程碑 | 状态 |
|------|--------|------|
| v0.1 | 最小骨架：调度器 + UART + 定时器 + 信号量 | ✅ |
| v0.2 | 同步原语：互斥量（优先级继承）+ 消息队列 | ✅ |
| v0.3 | Buddy 伙伴内存分配器 | ✅ |
| v0.4 | SLAB 对象缓存 + TCB 动态分配 | ✅ |
| v0.5 | MOS-FS 文件系统（VFS + RAM 盘） | ✅ |
| v0.7 | SDHCI + SD 卡驱动、文件系统落到真实块设备、多块位图 | ✅ |
| v0.7 阶段2 | 信号：sigaction/kill/sigreturn、用户栈信号帧、默认终止 | ✅ |
| v0.7 阶段3 | 信号屏蔽、-EINTR 唤醒阻塞任务、LR_usr 随任务切换 | ✅ |
| v0.7 阶段4 | 实时信号排队、投递优先级、SIG_IGN | ✅ |
| v0.7 阶段5 | 信号值 sigqueue、RT FIFO 值队列、handler 自动屏蔽当前信号 | ✅ |
| v0.7 阶段6 | sa_mask、SA_NODEFER、嵌套投递现场保护 | ✅ |
| v0.7 阶段7 | 信号返回模式修复：IRQ/SVC 投递条件、SPSR 临界区、三层嵌套 | ✅ |
| v0.7 阶段8 | siginfo_t、SA_SIGINFO、SA_RESETHAND、RT 队列满 EAGAIN、fork 寄存器现场修复 | ✅ |
| v0.6 | MMU + 用户态 + 权限隔离 + 每任务独立页表 + ASID + 资源回收 + 按需分页 + 页面换出 + fork/COW + exec + ELF 装载 + 僵尸回收（阶段1 开 MMU、2 模式切换、3 隔离/异常恢复、4 TTBR0 切换、5 ASID/reclaim、6 缺页处理、7 swap、8 fork/写时复制、9 exec/重置、10 僵尸/wait、11 真实 ELF、12 代码页按需装载） | ✅ |
| 下一步 | 多核调度、进程间共享内存、块设备/持久化 | 🔜 |

## 构建细节

- **编译器**：ARM GCC（`arm-none-eabi-gcc`）
- **目标**：`-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=soft`
- **链接**：自定义 `kernel.lds`（OCM + DDR 内存布局）
- **QEMU**：`qemu-system-arm -M xilinx-zynq-a9 -m 1024`
- **体积**：text 约 33.7KB（用户态代码单独链接在 `0x08000000`）

## 关于

作者：Mikeya98，嵌入式工程师，关注实时操作系统、FPGA 加速、裸机开发。

这是个人学习项目，从零实现，与任何商业 RTOS 无关。
