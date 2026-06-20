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

/* =========================== 异常处理 stub =========================== */
/*
 * v0.1 中这些异常处理只有最小功能: 打印错误信息后死循环。
 * 后续版本会完善 (SVC 用于系统调用, Data Abort 用于缺页处理等)。
 */

undef_handler:
    B       .

svc_handler:
    /*
     * SVC 是系统调用入口 (v0.4 实现)
     * v0.1: 暂时不做任何事
     */
    SUBS    pc, lr, #0    @ 直接返回 (不支持 Thumb 的兼容写法)

prefetch_handler:
    B       .

data_handler:
    /*
     * Data Abort — 通常是访问了非法地址
     * v0.1: 死循环 (调试时可用 J-Link 读寄存器)
     */
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
