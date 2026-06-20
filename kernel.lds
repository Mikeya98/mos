OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(reset_handler)

MEMORY
{
    DDR (rwx) : ORIGIN = 0x00100000, LENGTH = 512M
}

SECTIONS
{
    /* =========================== 代码段 =========================== */
    . = 0x00100000;

    /* 异常向量表 — 必须在最前面 (CPU 从 0x00100000 开始执行) */
    .vectors : {
        KEEP(*(.vectors))
    } > DDR

    /* 可执行代码 */
    .text : {
        *(.text*)
    } > DDR

    /* 只读数据 (字符串常量等) */
    .rodata : {
        *(.rodata*)
    } > DDR

    /* =========================== 数据段 =========================== */
    .data : {
        _data_start = .;
        *(.data*)
        _data_end = .;
    } > DDR

    /* 链接时不需要复制 (LMA == VMA), 所以不需要 _data_lma */
    _data_lma = _data_start;

    /* =========================== BSS 段 =========================== */
    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    } > DDR

    /* =========================== 内核栈 =========================== */
    . = ALIGN(8);
    .stack : {
        _stack_start = .;

        /* SVC 栈 (主栈, 8KB) */
        . += 0x2000;
        _stack_svc_top = .;

        /* IRQ 栈 (4KB) */
        . += 0x1000;
        _stack_irq_top = .;

        /* FIQ 栈 (4KB) */
        . += 0x1000;
        _stack_fiq_top = .;

        /* ABT 栈 (4KB) */
        . += 0x1000;
        _stack_abt_top = .;

        /* UND 栈 (4KB) */
        . += 0x1000;
        _stack_und_top = .;

        /* SYS 栈 (4KB, 用于用户态切换时临时用) */
        . += 0x1000;
        _stack_sys_top = .;

        _stack_end = .;
    } > DDR

    /* =========================== 任务栈池 =========================== */
    . = ALIGN(8);
    .task_stacks (NOLOAD) : {
        _task_stack_pool = .;
        . += 0x10000;           /* 64KB 任务栈池 (约 16 个 4KB 任务) */
        _task_stack_pool_end = .;
    } > DDR

    /* =========================== 堆 (v0.3 后用) ========================== */
    . = ALIGN(8);
    .heap (NOLOAD) : {
        _heap_start = .;
        . += 0x100000;          /* 1MB 初始堆 */
        _heap_end = .;
    } > DDR

    /* =========================== 符号 =========================== */
    _kernel_end = .;
}
