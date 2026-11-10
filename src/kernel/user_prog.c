/**
 * @file    user_prog.c
 * @brief   文件映射的用户程序映像 (v0.6 阶段12)
 *
 * 这里只做两件事: 查段, 按页读文件。真正把页框填进页表的是
 * mm/vm.c 的 vm_prog_fault —— 缺页处理属于内存管理, 文件读取属于
 * 程序装载, 两者分开。
 */

#include "kernel/user_prog.h"
#include "fs/vfs.h"
#include "lib/string.h"

const prog_seg_t *user_prog_seg(const prog_image_t *img, u32 page)
{
    u32 i;

    if (!img || !img->nseg) {
        return NULL;
    }

    for (i = 0; i < img->nseg && i < PROG_SEG_MAX; i++) {
        const prog_seg_t *s = &img->seg[i];
        /*
         * 段的最后一页按 memsz 向上取整: 比如 memsz=0x24 的数据段
         * 也要覆盖整个 0x09001000 页, 否则写到 .bss 末尾会 fault。
         */
        u32 span = (s->memsz + 0xFFFu) & ~0xFFFu;

        if (page >= s->vaddr && page < s->vaddr + span) {
            return s;
        }
    }
    return NULL;
}

int user_prog_read_page(const prog_image_t *img, u32 page, void *dst)
{
    const prog_seg_t *s;
    u32 off;
    u32 chunk;
    int fd;
    int n;

    if (!img || !dst) {
        return -1;
    }

    s = user_prog_seg(img, page);
    if (!s) {
        return -1;
    }

    /* 先清零: 新页不能泄漏上一个使用者的内容, .bss 也靠这一步 */
    memset(dst, 0, 4096);

    off = page - s->vaddr;
    if (off >= s->filesz) {
        return 0;                       /* 整页都在 .bss 里 */
    }

    chunk = s->filesz - off;
    if (chunk > 4096u) {
        chunk = 4096u;
    }

    fd = mos_open(img->path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    if (mos_lseek(fd, (int)(s->offset + off), SEEK_SET) < 0) {
        mos_close(fd);
        return -1;
    }

    n = mos_read(fd, dst, chunk);
    mos_close(fd);

    return (n == (int)chunk) ? 0 : -1;
}
