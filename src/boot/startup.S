/**
 * @file    startup.S
 * @brief   MOS RTOS 启动代码 (汇编)
 *
 * 这是整个系统最早执行的代码。
 *
 * 执行顺序:
 *   硬件上电 / FSBL 跳转 → reset_handler → kernel_main
 *
 * CPU 初始状态 (FSBL 交出时):
 *   CPSR: SVC 模式, IRQ/FIQ 屏蔽
 *   MMU:  关闭
 *   Cache: 不确定 (必须无效化)
 *   SP:    FSBL 的栈 (不能信任)
 *   其他:  不信任任何东西
 *
 * ARMv7-A 异常向量表 (8 入口, 各 4 字节):
 *   0x00  Reset               → reset_handler
 *   0x04  Undefined Instr     → undef_handler
 *   0x08  SVC (Supervisor Call) → svc_handler
 *   0x0C  Prefetch Abort      → prefetch_handler
 *   0x10  Data Abort          → data_handler
 *   0x14  保留                → (未使用)
 *   0x18  IRQ                 → irq_handler
 *   0x1C  FIQ                 → fiq_handler
 */

/* =========================== 外部符号 =========================== */
/* C 入口函数 (main.c) */
.extern kernel_main
/* 系统调用分发 (syscall.c) */
.extern svc_dispatch
/* 异常处理 (exception.c) */
.extern data_abort_handler
.extern prefetch_abort_handler

/* =========================== 链接脚本符号 =========================== */
/* 数据段边界 (copy data from LMA to VMA) */
.extern _data_start
.extern _data_end
.extern _data_lma

/* BSS 段边界 (清零) */
.extern _bss_start
.extern _bss_end

/* 各模式栈顶 (kernel.lds 定义) */
.extern _stack_svc_top
.extern _stack_irq_top
.extern _stack_fiq_top
.extern _stack_abt_top
.extern _stack_und_top
.extern _stack_sys_top

/* =========================== 宏 =========================== */
/* CPU 模式位 (写入 CPSR[4:0]) */
.equ MODE_USR,  0x10
.equ MODE_FIQ,  0x11
.equ MODE_IRQ,  0x12
.equ MODE_SVC,  0x13
.equ MODE_ABT,  0x17
.equ MODE_UND,  0x1B
.equ MODE_SYS,  0x1F

/* CPSR 初始值: SVC 模式 + IRQ 屏蔽 + FIQ 屏蔽 */
.equ INIT_CPSR, (MODE_SVC | (1 << 7) | (1 << 6))

/* =========================== 代码段 =========================== */
.section .vectors, "ax"
.balign 32
.global _vector_table

_vector_table:
    LDR     pc, =reset_handler         @ 0x00  Reset
    LDR     pc, =undef_handler         @ 0x04  Undefined Instruction
    LDR     pc, =svc_handler           @ 0x08  Supervisor Call
    LDR     pc, =prefetch_handler      @ 0x0C  Prefetch Abort
    LDR     pc, =data_handler          @ 0x10  Data Abort
    NOP                                 @ 0x14  保留
    LDR     pc, =irq_handler           @ 0x18  IRQ Interrupt
    LDR     pc, =fiq_handler           @ 0x1C  FIQ Interrupt

/* =========================== 启动代码 =========================== */
.section .text
.global reset_handler

reset_handler:
    /*
     * 第一步: 禁止中断 + 确保 SVC 模式
     *
     * FSBL 交 CPU 时 IRQ 可能开着, 必须先关。
     * 否则在栈还没设好的情况下发生中断 → 跑飞。
     */
    CPSID   if                          @ 关 IRQ + FIQ
    CPS     #MODE_SVC                   @ 切到 SVC 模式

    /*
     * 第二步: 设置异常向量表基地址 (VBAR)
     *
     * Zynq Boot ROM 占据 0x00000000, 默认异常向量也在此。
     * MOS 代码在 0x00100000, 必须设置 VBAR 指向我们的向量表,
     * 否则 IRQ 等异常会跑到 Boot ROM 的向量表。
     */
    LDR     r0, =_vector_table
    MCR     p15, 0, r0, c12, c0, 0      @ VBAR (Secure) = _vector_table
    DSB
    ISB

    /*
     * 第三步: 无效化 Cache + TLB
     *
     * FSBL 可能启用了 Cache, 留下的脏数据会导致
     * MOS 启动后读到错误的内容。
     * 必须无条件无效化。
     */
    MOV     r0, #0
    MCR     p15, 0, r0, c7, c5, 0      @ 无效化 I-Cache
    MCR     p15, 0, r0, c7, c14, 0     @ 无效化 D-Cache
    MCR     p15, 0, r0, c8, c7, 0      @ 无效化 TLB

    /*
     * 数据同步屏障: 确保以上操作在后续指令执行前完成
     */
    DSB
    ISB

    /*
     * 第三步: 设置所有 CPU 模式的栈指针
     *
     * 每种异常模式都需要独立的栈, 否则:
     *   SVC 模式正常用 SP_svc,
     *   发生 IRQ → CPU 自动切到 IRQ 模式 → 用 SP_irq
     *   如果 SP_irq 没初始化 → 写到随机地址 → 随机崩溃
     *
     * 切换模式的步骤:
     *   1. CPS 切到目标模式
     *   2. 设该模式的 SP
     *   3. CPS 回到 SVC 模式
     */

    @ SVC 栈 (主栈, 8KB)
    LDR     sp, =_stack_svc_top

    @ IRQ 栈 (4KB)
    CPS     #MODE_IRQ
    LDR     sp, =_stack_irq_top

    @ FIQ 栈 (4KB)
    CPS     #MODE_FIQ
    LDR     sp, =_stack_fiq_top

    @ ABT 栈 (4KB)
    CPS     #MODE_ABT
    LDR     sp, =_stack_abt_top

    @ UND 栈 (4KB)
    CPS     #MODE_UND
    LDR     sp, =_stack_und_top

    @ SYS 栈 (4KB)
    CPS     #MODE_SYS
    LDR     sp, =_stack_sys_top

    @ 回到 SVC 模式
    CPS     #MODE_SVC

    /*
     * 第四步: 复制 .data 段
     *
     * .data 段包含已初始化的全局变量。
     * 加载地址 (_data_lma) 在 Flash/BIN 中, 但访问
     * 地址 (_data_start) 在 RAM 中。
     *
     * 如果 LMA == VMA (我们在 DDR 中运行), 跳过复制。
     */
    LDR     r0, =_data_lma
    LDR     r1, =_data_start
    LDR     r2, =_data_end

    CMP     r0, r1
    BEQ     _skip_copy                  @ LMA == VMA → 跳过

_copy_loop:
    CMP     r1, r2
    BEQ     _skip_copy
    LDR     r3, [r0], #4
    STR     r3, [r1], #4
    B       _copy_loop

_skip_copy:

    /*
     * 第五步: 清零 BSS
     *
     * C 语言标准规定未初始化的全局变量必须为零。
     * startup 代码负责清零 BSS 段。
     */
    MOV     r0, #0
    LDR     r1, =_bss_start
    LDR     r2, =_bss_end

_bss_loop:
    CMP     r1, r2
    BEQ     _bss_done
    STR     r0, [r1], #4
    B       _bss_loop

_bss_done:

    /*
     * 第六步: 进入 C 世界
     *
     * 现在 CPU 状态:
     *   - SVC 模式, IRQ/FIQ 屏蔽
     *   - MMU 关闭, Cache 无效化
     *   - 所有模式栈已设
     *   - BSS 已清零
     *
     * kernel_main() 中会依次初始化 UART, GIC, Timer,
     * 创建任务, 然后启动调度器。
     */
    BL      kernel_main

    /*
     * kernel_main 启动了调度器后不会返回这里。
     * 如果因为 bug 返回了 → 死循环。
     */
_dead:
    B       _dead

/* =========================== 异常处理 =========================== */
/*
 * 未定义指令: 仍然是死循环。真到了这里说明代码跑飞, 先停住等调试器。
 */

undef_handler:
    B       .

svc_handler:
    /*
     * SVC 是系统调用入口 (v0.6 阶段2/3)
     *
     * 用户态任务执行 SVC 指令时, CPU 自动:
     *   - 切到 SVC 模式 (SP 自动切到 SP_svc = 任务内核栈)
     *   - CPSR 保存到 SPSR_svc, 返回地址保存到 lr_svc
     *
     * 这里要做:
     *   1. 保存用户态 r0-r12 + 返回地址到任务内核栈
     *   2. 读 SVC 指令携带的立即数 (系统调用号)
     *   3. 调 svc_dispatch(svc_num, frame) — frame 让内核能读写参数/返回值
     *   4. 恢复上下文, MOVS pc, lr 异常返回用户态
     *
     * 栈帧布局 (PUSH {r0-r12, lr}):
     *   sp + 0  → r0   ← 参数 1 / 返回值
     *   sp + 4  → r1   ← 参数 2
     *   ...
     *   sp + 48 → r12
     *   sp + 52 → lr (用户态返回地址)
     *
     * 14 个字 = 56 字节, 8 字节对齐, 满足 AAPCS。
     */

    @ 1. 保存用户态 r0-r12 + 返回地址 (lr_svc) 到任务内核栈
    PUSH    {r0-r12, lr}

    @ 2. frame 指针 = 当前 sp; r0 = SVC 立即数 (系统调用号)
    MOV     r1, sp
    LDR     r0, [lr, #-4]
    BIC     r0, r0, #0xFF000000   @ 取低 24 位 = 系统调用号

    @ 3. 调用 C 分发函数 svc_dispatch(svc_num, frame)
    BL      svc_dispatch

    @ 4. 恢复用户态上下文 (r0 已由 svc_dispatch 改写为返回值)
    POP     {r0-r12, lr}
    MOVS    pc, lr

prefetch_handler:
    /*
     * Prefetch Abort — 取指失败 (PC 指向未映射/无执行权限的地址)
     *
     * lr_abt = 故障指令地址 + 4, 先减掉流水线偏移。
     * SPSR_abt 的低 5 位记录故障发生时的模式 (USR=0x10 / SVC=0x13),
     * 交给 C 判断是用户任务越权还是内核自己跑飞。
     *
     * r0-r3 在异常模式间是共用的, CPS 切到 SVC 后参数仍然有效。
     */
    SUB     lr, lr, #4
    MOV     r0, lr
    MRS     r1, SPSR
    CPS     #MODE_SVC
    BL      prefetch_abort_handler
    B       .

data_handler:
    /*
     * Data Abort — 访问非法地址 (用户态越权访问内核区就是走这里)
     *
     * lr_abt = 故障指令地址 + 8。
     * C 处理函数里读取 DFAR/DFSR, 打印现场, 杀掉出错任务后调度走。
     */
    SUB     lr, lr, #8
    MOV     r0, lr
    MRS     r1, SPSR
    CPS     #MODE_SVC
    BL      data_abort_handler
    B       .

irq_handler:
    /*
     * IRQ 处理流程:
     *   1. 保存 r0-r12, lr 到被中断任务的栈
     *   2. 读 GIC IAR 获取中断 ID
     *   3. 根据 ID 调用对应的 ISR
     *   4. 写 GIC EOIR
     *   5. 检查是否需要调度
     *   6. 恢复上下文
     */

    @ 保存现场: r0-r12, lr_irq → SVC 栈
    SUB     lr, lr, #4              @ IRQ 返回时需要减去 4 (流水线偏移)
    SRSFD   sp!, #MODE_SVC          @ 保存 lr_irq 和 spsr_irq 到 SVC 栈

    @ 切到 SVC 模式执行 ISR
    CPS     #MODE_SVC

    @ 保存 caller-saved 寄存器
    PUSH    {r0-r3, r12, lr}

    @ 调用 C 语言 IRQ 分发函数
    BL      irq_dispatch

    @ 恢复 caller-saved 寄存器
    POP     {r0-r3, r12, lr}

    @ 异常返回 (同时恢复 CPSR)
    RFEFD   sp!

fiq_handler:
    /*
     * FIQ — 快速中断, v0.1 不使用
     * 如果意外触发 → 死循环
     */
    B       .

/* =========================== 外部符号 =========================== */
/*
 * irq_dispatch — 在 main.c 中定义的 C 函数
 * context_switch — 在 sched_asm.S 中定义的汇编函数
 */
