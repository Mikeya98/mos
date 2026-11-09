/*
 * 独立用户程序的链接脚本 (v0.6 阶段11)
 *
 * 程序固定链接到 0x09000000, 正好是 MOS 给每个任务预留的 1MB
 * 程序窗口。.data 强制从新的一页开始, 避免代码段和数据段共享
 * 同一个 4KB 页 —— 加载器按页映射, 一页只有一种权限。
 */

OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

SECTIONS
{
    . = 0x09000000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(4096);

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }
}
