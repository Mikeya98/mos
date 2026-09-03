/**
 * @file    sched_asm.S
 * @brief   MOS RTOS 上下文切换 (汇编实现)
 *
 * 这是调度器中唯一不能用 C 实现的函数。
 *
 * C 函数签名: void context_switch(task_t *old, task_t *new);
 * 调用约定:   r0 = old, r1 = new
 *
 * 实现策略 (per-task kernel stack):
 *   每个任务有自己的内核栈 (从 task stack pool 分配)。
 *   上下文切换时将寄存器 PUSH 到 old 的栈上,
 *   保存 sp 到 old->sp, 然后从 new->sp 加载 sp,
 *   从 new 的栈上 POP 寄存器, BX lr 返回。
 *
 *   首次启动 (old==NULL): 跳过 PUSH, 只做 POP。
 *
 * 栈帧布局 (由下至上, STMDB 顺序):
 *   sp → r4  (4B)     ← LDMIA 先弹出
 *        r5  (4B)
 *        r6  (4B)
 *        r7  (4B)
 *        r8  (4B)
 *        r9  (4B)
 *        r10 (4B)
 *        r11 (4B)
 *        lr  (4B)     ← 最后弹出, 作为 BX 的目标地址
 */

.section .text
.global context_switch
.extern current_task

context_switch:
    @ ============================================
    @ 1. 关中断
    @ ============================================
    CPSID   i

    @ ============================================
    @ 2. 如果 old != NULL → 保存旧任务上下文
    @ ============================================
    CMP     r0, #0
    BEQ     _do_restore

    @ PUSH {r4-r11, lr} 到 old 任务的内核栈
    @ STMDB sp!, {r4-r11, lr}:
    @   先减 sp, 再按 r4→r11→lr 顺序存储
    @   lr 存在最高地址, r4 存在最低地址 (新 sp 指向 r4)
    STMDB   sp!, {r4-r11, lr}

    @ 保存 sp 到 old->sp (offset = 48)
    STR     sp, [r0, #48]

    @ ============================================
    @ 3. 恢复新任务上下文
    @ ============================================
_do_restore:
    @ 加载 new->sp
    LDR     sp, [r1, #48]

    @ POP {r4-r11, lr} 从 new 任务的内核栈
    @ LDMIA sp!, {r4-r11, lr}:
    @   从 sp 指向的位置加载 r4→r11→lr
    @   加载后 sp 递增, 指向栈顶
    LDMIA   sp!, {r4-r11, lr}

    @ ============================================
    @ 4. 更新 current_task
    @ ============================================
    LDR     r3, =current_task
    STR     r1, [r3]

    @ ============================================
    @ 5. 开中断 + 返回
    @    lr = new 的返回地址:
    @     - 如果 new 是首次运行 → lr = entry 函数地址
    @     - 如果 new 是被换出过的 → lr = context_switch 的返回地址
    @ ============================================
    CPSIE   i
    BX      lr

/* ============================================================
 * 用户态任务首次进入的跳板 (v0.6 阶段2)
 *
 * 首次被调度到时, context_switch 恢复 {r4-r11, lr} 后 BX lr 跳到这里。
 * 入口约定 (由 stack_init_user 伪造):
 *   r4 = 用户入口函数地址
 *   r5 = 用户栈顶 (SP_usr)
 *   lr = 本跳板地址
 *
 * 要做三件事:
 *   1. 设置用户栈 SP_usr (通过 SYS 模式, 因为 SYS 与 USR 共享 SP)
 *   2. 设置 SPSR_svc = USR 模式 (0x10)
 *   3. 用 MOVS pc, lr 异常返回 → 恢复 CPSR 并切换到用户态
 * ============================================================ */
.equ T_MOD_SYS, 0x1F
.equ T_MOD_SVC, 0x13
.equ T_MOD_USR, 0x10

.global user_entry_trampoline
user_entry_trampoline:
    @ 1. 切到 SYS 模式设置用户栈 (不能直接 CPS 到 USR, 只能异常返回进入)
    CPS     #T_MOD_SYS
    MOV     sp, r5

    @ 2. 回到 SVC 模式, 准备异常返回
    CPS     #T_MOD_SVC

    @ 3. lr = 用户入口, SPSR_svc = USR 模式
    MOV     lr, r4
    MOV     r4, #T_MOD_USR
    MSR     SPSR_cxsf, r4

    @ 4. 异常返回: pc = lr, CPSR = SPSR(USR) → 进入用户态执行入口
    MOVS    pc, lr
