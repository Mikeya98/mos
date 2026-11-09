/**
 * @file    elf_user.c
 * @brief   用户程序 ELF 加载器 (v0.6 阶段11)
 *
 * 加载顺序是刻意安排的:
 *
 *   校验 header → 校验每个 PT_LOAD 的范围/对齐/重叠
 *   → 预分配所有页框 (失败就全退回去)
 *   → 这时才 user_space_reset() 清掉旧程序
 *   → 把文件内容拷进页框, 填 L2 描述符
 *
 * 为什么要先预分配:
 *   reset 之后旧地址空间就没了。如果这时才发现页框不够, 任务既
 *   回不到旧程序, 也没有新程序, 只能等死。把所有可能失败的步骤
 *   放在 reset 之前, 失败时原进程还能继续跑。
 */

#include "kernel/elf_user.h"
#include "kernel/task.h"
#include "mm/mmu.h"
#include "mm/frame.h"
#include "lib/string.h"
#include "lib/printf.h"

/* =========================== ELF32 结构 =========================== */

#define EI_NIDENT       16
#define ET_EXEC         2
#define EM_ARM          40
#define PT_LOAD         1
#define PF_X            0x1
#define PF_W            0x2

typedef struct {
    u8  e_ident[EI_NIDENT];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
} __attribute__((packed)) elf32_phdr_t;

#define MAX_PHDRS       16u
#define MAX_PAGES       (USER_PROG_SIZE / 4096u)    /* 256 */

/* =========================== 工具 =========================== */

static int range_in_file(u32 off, u32 len, u32 size)
{
    if (off > size) return 0;
    if (len > size - off) return 0;
    return 1;
}

static int page_bit(u32 *map, u32 page)
{
    return (map[page >> 5] & (1u << (page & 31u))) != 0;
}

static void page_set(u32 *map, u32 page)
{
    map[page >> 5] |= (1u << (page & 31u));
}

static void release_frames(u32 *frames, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (frames[i] != 0) {
            frame_free(frames[i], 0);
        }
    }
}

/* =========================== 加载 =========================== */

int user_elf_load(user_space_t *us, const u8 *image, u32 size,
                  u32 *entry_out)
{
    const elf32_ehdr_t *eh;
    const elf32_phdr_t *ph;
    u32 frames[MAX_PAGES];
    u32 seg_start[MAX_PHDRS];
    u32 seg_pages[MAX_PHDRS];
    u32 page_map[8] = {0};          /* 256 位, 用来查段重叠 */
    u32 nframes = 0;
    u32 nseg = 0;
    u32 i;

    if (!us || !image || !entry_out || size < sizeof(elf32_ehdr_t)) {
        return ELF_USER_ENOEXEC;
    }

    eh = (const elf32_ehdr_t *)image;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        printf("[ELF] bad magic\n");
        return ELF_USER_ENOEXEC;
    }
    if (eh->e_ident[4] != 1 || eh->e_ident[5] != 1) {   /* 32-bit, LE */
        printf("[ELF] only ELF32 little-endian is supported\n");
        return ELF_USER_ENOEXEC;
    }
    if (eh->e_type != ET_EXEC) {
        printf("[ELF] not ET_EXEC (type %u)\n", eh->e_type);
        return ELF_USER_ENOEXEC;
    }
    if (eh->e_machine != EM_ARM) {
        printf("[ELF] not ARM (machine %u)\n", eh->e_machine);
        return ELF_USER_ENOEXEC;
    }
    if (eh->e_phentsize != sizeof(elf32_phdr_t) ||
        eh->e_phnum == 0 || eh->e_phnum > MAX_PHDRS) {
        printf("[ELF] bad program header table\n");
        return ELF_USER_ENOEXEC;
    }
    if (!range_in_file(eh->e_phoff,
                       (u32)eh->e_phnum * sizeof(elf32_phdr_t), size)) {
        printf("[ELF] program headers outside file\n");
        return ELF_USER_EFAULT;
    }
    if (eh->e_entry < USER_PROG_VADDR || eh->e_entry >= USER_PROG_LIMIT) {
        printf("[ELF] entry %X outside program window\n", eh->e_entry);
        return ELF_USER_EFAULT;
    }

    ph = (const elf32_phdr_t *)(image + eh->e_phoff);

    /* ---------------- 第一遍: 校验 + 数页 ---------------- */
    for (i = 0; i < eh->e_phnum; i++) {
        u32 npages;
        u32 page;

        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        if (ph[i].p_memsz == 0) {
            continue;
        }
        if ((ph[i].p_vaddr & 0xFFFu) != 0) {
            printf("[ELF] segment %u vaddr not page aligned: %X\n",
                   i, ph[i].p_vaddr);
            return ELF_USER_EFAULT;
        }
        if (ph[i].p_filesz > ph[i].p_memsz) {
            printf("[ELF] segment %u filesz > memsz\n", i);
            return ELF_USER_EFAULT;
        }
        if (!range_in_file(ph[i].p_offset, ph[i].p_filesz, size)) {
            printf("[ELF] segment %u outside file\n", i);
            return ELF_USER_EFAULT;
        }
        if (ph[i].p_vaddr < USER_PROG_VADDR ||
            ph[i].p_vaddr + ph[i].p_memsz > USER_PROG_LIMIT) {
            printf("[ELF] segment %u [%X, %X) outside program window\n",
                   i, ph[i].p_vaddr, ph[i].p_vaddr + ph[i].p_memsz);
            return ELF_USER_EFAULT;
        }

        npages = (ph[i].p_memsz + 0xFFFu) >> 12;
        if (nframes + npages > MAX_PAGES) {
            printf("[ELF] program too large (%u pages)\n", nframes + npages);
            return ELF_USER_ENOEXEC;
        }

        for (page = (ph[i].p_vaddr - USER_PROG_VADDR) >> 12;
             page < ((ph[i].p_vaddr - USER_PROG_VADDR) >> 12) + npages;
             page++) {
            if (page_bit(page_map, page)) {
                printf("[ELF] segment %u overlaps another segment (page %u)\n",
                       i, page);
                return ELF_USER_ENOEXEC;
            }
            page_set(page_map, page);
        }

        seg_start[nseg] = nframes;
        seg_pages[nseg] = npages;
        nseg++;
        nframes += npages;
    }

    if (nseg == 0) {
        printf("[ELF] no PT_LOAD segment\n");
        return ELF_USER_ENOEXEC;
    }

    /* ---------------- 第二遍: 预分配所有页框 ---------------- */
    memset(frames, 0, sizeof(frames));
    for (i = 0; i < nframes; i++) {
        frames[i] = frame_alloc(0);
        if (frames[i] == 0) {
            printf("[ELF] out of frames (%u/%u)\n", i, nframes);
            release_frames(frames, nframes);
            return ELF_USER_ENOMEM;
        }
        memset((void *)(uintptr_t)frames[i], 0, FRAME_SIZE);
    }

    /* ---------------- 到这里才清掉旧程序 ---------------- */
    user_space_reset(us);

    {
        u32 *l2c = (u32 *)(uintptr_t)us->l2_code_phys;
        u32 seg = 0;

        for (i = 0; i < eh->e_phnum; i++) {
            u32 base_page;
            u32 attr;
            u32 p;

            if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) {
                continue;
            }

            base_page = (ph[i].p_vaddr - USER_PROG_VADDR) >> 12;
            attr = (ph[i].p_flags & PF_W) ? L2_USER_RW_XN : L2_USER_RO;

            /* 文件内容按页拷贝; p_memsz 多出来的部分是 .bss, 已经是 0 */
            for (p = 0; p < seg_pages[seg]; p++) {
                u32 dst = frames[seg_start[seg] + p];
                u32 off = p * 4096u;

                if (off < ph[i].p_filesz) {
                    u32 chunk = ph[i].p_filesz - off;
                    if (chunk > 4096u) {
                        chunk = 4096u;
                    }
                    memcpy((void *)(uintptr_t)dst,
                           image + ph[i].p_offset + off, chunk);
                }
                l2c[base_page + p] = dst | attr;
            }
            seg++;
        }
    }

    /* 页表变了, 这个 ASID 的旧翻译必须全部作废 */
    mmu_flush_asid(us->asid);

    *entry_out = eh->e_entry;
    printf("[ELF] loaded %u pages, entry %X (%u segments)\n",
           nframes, eh->e_entry, nseg);
    return ELF_USER_OK;
}
