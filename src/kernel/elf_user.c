/**
 * @file    elf_user.c
 * @brief   用户程序 ELF 装载 — 只读头部, 正文按需 (v0.6 阶段12)
 *
 * 执行顺序:
 *
 *   打开文件 → 读前 1KB (ELF header + program headers)
 *   → 校验 magic / 机器 / 入口 / 每个 PT_LOAD 的范围与对齐
 *   → 检查段之间不共享页 (页映射无法表达同一页的两种权限)
 *   → 把段表填进 prog_image_t
 *   → 这时才 user_space_reset() 清掉旧程序
 *
 * 为什么要先把所有失败路径走完再 reset:
 *   reset 之后旧地址空间就没了。阶段11 靠"先预分配页框"来保证
 *   失败时能退回旧程序; 阶段12 不再分配页框, 失败只可能来自文件
 *   格式, 所以把格式校验全部放在 reset 之前就够了。
 *
 * 读 1KB 而不是整文件:
 *   程序头的总长度 = 52 + phnum*32, 最多 564 字节。正文一页都不读,
 *   exec 的耗时才和程序大小无关。
 */

#include "kernel/elf_user.h"
#include "kernel/task.h"
#include "mm/mmu.h"
#include "fs/vfs.h"
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

/* ELF 头 + 程序头最多 564 字节, 读 1KB 足够 */
#define HDR_BUF_SIZE    1024u
static u8 elf_hdr[HDR_BUF_SIZE];

/* =========================== 工具 =========================== */

static int range_in_file(u32 off, u32 len, u32 size)
{
    if (off > size) return 0;
    if (len > size - off) return 0;
    return 1;
}

static int page_bit(const u32 *map, u32 page)
{
    return (map[page >> 5] & (1u << (page & 31u))) != 0;
}

static void page_set(u32 *map, u32 page)
{
    map[page >> 5] |= (1u << (page & 31u));
}

static void copy_path(char *dst, const char *src, u32 max)
{
    u32 i;

    for (i = 0; i + 1u < max && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* =========================== 装载 =========================== */

int user_elf_load(user_space_t *us, const char *path,
                  prog_image_t *img, u32 *entry_out)
{
    const elf32_ehdr_t *eh;
    const elf32_phdr_t *ph;
    prog_image_t tmp;               /* 校验完才拷给调用者, 失败时旧段表不动 */
    u32 page_map[8] = {0};          /* 256 位, 用来查段重叠 */
    u32 size;
    u32 got;
    u32 hdr_len;
    u32 nseg = 0;
    u32 i;
    int fd;

    if (!us || !path || !img || !entry_out) {
        return ELF_USER_ENOEXEC;
    }

    memset(&tmp, 0, sizeof(tmp));

    /* ---------------- 读头部 ---------------- */
    fd = mos_open(path, O_RDONLY);
    if (fd < 0) {
        printf("[ELF] open %s failed\n", path);
        return ELF_USER_EFAULT;
    }
    size = (u32)mos_filesize(fd);
    if (size < sizeof(elf32_ehdr_t)) {
        mos_close(fd);
        printf("[ELF] %s too small (%u bytes)\n", path, size);
        return ELF_USER_ENOEXEC;
    }

    got = 0;
    hdr_len = (size < HDR_BUF_SIZE) ? size : HDR_BUF_SIZE;
    while (got < hdr_len) {
        int n = mos_read(fd, elf_hdr + got, hdr_len - got);
        if (n <= 0) {
            break;
        }
        got += (u32)n;
    }
    mos_close(fd);
    if (got < sizeof(elf32_ehdr_t)) {
        printf("[ELF] %s: short header read %u/%u\n", path, got, hdr_len);
        return ELF_USER_EFAULT;
    }

    /* ---------------- 校验 header ---------------- */
    eh = (const elf32_ehdr_t *)elf_hdr;
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
    /* 程序头必须真的读进了 1KB 缓冲 */
    if (eh->e_phoff + (u32)eh->e_phnum * sizeof(elf32_phdr_t) > got) {
        printf("[ELF] program headers beyond first %u bytes\n", got);
        return ELF_USER_EFAULT;
    }
    if (eh->e_entry < USER_PROG_VADDR || eh->e_entry >= USER_PROG_LIMIT) {
        printf("[ELF] entry %X outside program window\n", eh->e_entry);
        return ELF_USER_EFAULT;
    }

    ph = (const elf32_phdr_t *)(elf_hdr + eh->e_phoff);

    /* ---------------- 校验 + 登记每个 PT_LOAD ---------------- */
    for (i = 0; i < eh->e_phnum; i++) {
        u32 npages;
        u32 page;

        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        if (ph[i].p_memsz == 0) {
            continue;
        }
        if (nseg >= PROG_SEG_MAX) {
            printf("[ELF] too many PT_LOAD segments (max %u)\n", PROG_SEG_MAX);
            return ELF_USER_ENOEXEC;
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
        if (npages > MAX_PAGES) {
            printf("[ELF] segment %u too large\n", i);
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

        tmp.seg[nseg].vaddr    = ph[i].p_vaddr;
        tmp.seg[nseg].memsz    = ph[i].p_memsz;
        tmp.seg[nseg].filesz   = ph[i].p_filesz;
        tmp.seg[nseg].offset   = ph[i].p_offset;
        tmp.seg[nseg].writable = (ph[i].p_flags & PF_W) ? 1u : 0u;
        nseg++;
    }

    if (nseg == 0) {
        printf("[ELF] no PT_LOAD segment\n");
        return ELF_USER_ENOEXEC;
    }

    /* ---------------- 到这里才清掉旧程序 ---------------- */
    user_space_reset(us);

    copy_path(tmp.path, path, PROG_PATH_MAX);
    tmp.entry = eh->e_entry;
    tmp.nseg  = nseg;

    /*
     * 页表里已经没有程序页了, 但 TLB 里可能还留着旧进程在
     * 0x09000000 的翻译。按 ASID 清一遍, 否则 exec 之后第一次
     * 取指可能命中旧页。
     */
    mmu_flush_asid(us->asid);

    {
        u32 pages = 0;
        for (i = 0; i < nseg; i++) {
            pages += (tmp.seg[i].memsz + 0xFFFu) >> 12;
        }
        printf("[ELF] %s: %u segments, %u pages pending (lazy), entry %X\n",
               path, nseg, pages, eh->e_entry);
    }

    *img = tmp;
    *entry_out = eh->e_entry;
    return ELF_USER_OK;
}
