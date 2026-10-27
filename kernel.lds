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

    /* =========================== MMU 页表 =========================== */
    /* L1 页表 16KB, 必须 16KB (14-bit) 对齐, 由 mmu_init 运行时填充 */
    .mmu_table (NOLOAD) : ALIGN(16384) {
        _mmu_table_base = .;
        . += 0x4000;
        _mmu_table_end = .;
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
        . += 0x18000;           /* 96KB 任务栈池 (约 24 个 4KB 任务) */
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

    /* =========================== 用户地址空间物理池 (v0.6 阶段4/6) ===========================
     *
     * 每个用户任务需要 16KB L1 页表 + 1KB L2 页表, 页表有严格对齐要求,
     * buddy_alloc 返回的是块头 +4 字节, 拿不到页对齐地址, 所以这里单独
     * 划静态池, 由 mm/addr_space.c 按槽位切分。
     *
     * 数据页和栈页在阶段6 改成从下面的 .frame_pool 按需取, 不在这里预分配。
     *
     * 注意: 必须放在 0x08000000 之前的内核区。放在用户代码段后面的话,
     * 池子会落进 0x08000000 那个 1MB section, 被映射成"用户可读",
     * 一个任务就能顺着 0x080xxxxx 读到别的任务的栈。
     *
     * 这些地址在内核页表里是普通内存 (identity map), 内核可以直接读写;
     * 用户任务通过自己的页表把数据页/栈映射到 0x08100000 起。
     */
    /* 对齐必须写在段头里: 段外的 ". = ALIGN(...)" 对 NOLOAD 段不生效,
     * 链接器会按输入段的对齐 (4 字节) 放, 结果页表没对齐, TTBR0 一换就飞。 */
    .l1_pool (NOLOAD) : ALIGN(16384) {
        _l1_pool_start = .;
        . += 0x20000;           /* 8 × 16KB L1 页表 */
        _l1_pool_end = .;
    } > DDR

    .l2_pool (NOLOAD) : ALIGN(1024) {
        _l2_pool_start = .;
        . += 0x2000;            /* 8 × 1KB L2 页表 */
        _l2_pool_end = .;
    } > DDR

    /* 物理页框池 (v0.6 阶段6 按需分页)
     *
     * 用户的数据页和栈页不再按任务预分配, 而是第一次访问时从这个
     * 池子里取一帧, 填进该任务的 L2 页表。任务退出时按页表把帧还回来。
     *
     * 64 帧 = 256KB。按阶段4 的预分配方案, 每个任务要占 17 帧
     * (1 数据 + 16 栈), 8 个任务就是 136 帧, 这个池子根本装不下;
     * 改成按需分配之后, 典型任务只碰 2-3 页, 同样 256KB 能撑住
     * 8 个并发任务。
     */
    .frame_pool (NOLOAD) : ALIGN(4096) {
        _frame_pool_start = .;
        . += 0x40000;           /* 64 × 4KB */
        _frame_pool_end = .;
    } > DDR

    /* =========================== 用户空间 (v0.6 阶段4) ===========================
     *
     * 用户代码/只读数据放在 0x08000000 起的独立 1MB section, 所有任务共享,
     * 映射成 AP=10 (用户只读, 可执行)。
     *
     * 用户任务不能调用任何内核函数 (printf / memcpy 等), 只能通过 SVC 进内核,
     * 所以用户代码全部放在 .user_text, 用户态用的字符串放 .user_rodata。
     */
    .user_text 0x08000000 : ALIGN(4096) {
        _user_text_start = .;
        *(.user_text*)
        _user_text_end = .;
    } > DDR

    .user_rodata : ALIGN(4) {
        _user_rodata_start = .;
        *(.user_rodata*)
        _user_rodata_end = .;
    } > DDR

    /* 注意: 这一段的权限是"用户只读", 只放代码和只读字符串。
     * 需要可写的用户全局变量, 请放到私有数据页 (0x08100000),
     * 不要往 .user_data 里放。 */
    .user_data : ALIGN(4) {
        _user_data_start = .;
        *(.user_data*)
        _user_data_end = .;
    } > DDR
}
