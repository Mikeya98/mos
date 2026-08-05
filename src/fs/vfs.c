/**
 * @file    vfs.c
 * @brief   VFS 层实现 — 文件描述符管理 + POSIX 风格 API
 *
 * 核心数据结构关系:
 *   fd_table[fd]  ──of_idx──→  open_files[of_idx]
 *        ↓                           ↓
 *   inode_num, offset          inode_num, inode(副本), dev, refcount
 *
 * fd → of_idx 直接索引, 不需要扫描。
 */

#include "fs/vfs.h"
#include "kernel/task.h"
#include "lib/string.h"

/* =========================== 全局状态 =========================== */

static block_dev_t     *mounted_dev = NULL;
static fs_superblock_t  mounted_sb;
static open_file_t      open_files[MAX_OPEN_FILES];

/* =========================== 目录遍历状态 =========================== */

#define MAX_OPEN_DIRS  4

typedef struct {
    u16 in_use;
    u32 dir_inode_num;
    u32 current_block;
    u32 current_entry;
} dir_handle_t;

static dir_handle_t dir_handles[MAX_OPEN_DIRS];

/* =========================== 内部工具 =========================== */

static file_desc_t *get_fd_table(void)
{
    return get_current()->fd_table;
}

static int alloc_open_file(block_dev_t *dev, u32 inode_num)
{
    u32 i;
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            open_files[i].in_use    = 1;
            open_files[i].inode_num = inode_num;
            open_files[i].refcount  = 1;
            open_files[i].dev       = dev;
            if (mos_fs_read_inode(dev, inode_num, &open_files[i].inode) != 0) {
                open_files[i].in_use = 0;
                return -1;
            }
            return (int)i;
        }
    }
    return -1;
}

static void free_open_file(u32 of_idx)
{
    if (of_idx < MAX_OPEN_FILES) {
        open_files[of_idx].in_use = 0;
        open_files[of_idx].refcount = 0;
        open_files[of_idx].dev = NULL;
        open_files[of_idx].inode_num = 0;
    }
}

/* =========================== 路径解析 =========================== */

/**
 * @brief  解析绝对路径, 返回目标 inode 号
 *
 * 支持 "/"  "/file.txt"  "/dir/file.txt" (最多一层子目录)
 *
 * @param  parent_inode  输出: 父目录 inode 号 (创建文件时需要)
 * @return 目标 inode 号, 失败返回 -1
 */
static int resolve_path(const char *path, u32 *parent_inode)
{
    fs_inode_t dir_inode;
    char component[MOS_FS_NAME_MAX];
    const char *p;
    u32 comp_len;
    int ino;

    if (!mounted_dev) return -1;
    if (!path || path[0] != '/') return -1;

    path++;
    if (*path == '\0') {
        if (parent_inode) *parent_inode = 0;
        return 0;
    }

    /* 提取第一级分量 */
    p = path;
    comp_len = 0;
    while (*p && *p != '/' && comp_len < MOS_FS_NAME_MAX - 1) {
        component[comp_len++] = *p;
        p++;
    }
    component[comp_len] = '\0';

    if (mos_fs_read_inode(mounted_dev, 0, &dir_inode) != 0) return -1;

    ino = mos_fs_lookup(mounted_dev, &dir_inode, component);
    if (ino < 0) {
        if (parent_inode) *parent_inode = 0;
        return -1;
    }

    if (*p == '/') {
        /* 第二级 */
        fs_inode_t sub_inode;
        if (mos_fs_read_inode(mounted_dev, (u32)ino, &sub_inode) != 0)
            return -1;
        if (sub_inode.mode != FT_DIR) return -1;

        p++;
        comp_len = 0;
        while (*p && *p != '/' && comp_len < MOS_FS_NAME_MAX - 1) {
            component[comp_len++] = *p;
            p++;
        }
        component[comp_len] = '\0';

        int sub_ino = mos_fs_lookup(mounted_dev, &sub_inode, component);
        if (sub_ino < 0) {
            if (parent_inode) *parent_inode = (u32)ino;
            return -1;
        }
        if (parent_inode) *parent_inode = (u32)ino;
        return sub_ino;
    }

    if (parent_inode) *parent_inode = 0;
    return ino;
}

/** 从路径中提取最后一级文件名 */
static const char *get_filename(const char *path)
{
    const char *name = path;
    const char *p = path;
    while (*p) {
        if (*p == '/') name = p + 1;
        p++;
    }
    return name;
}

/* =========================== 挂载 =========================== */

int mos_mount(block_dev_t *dev)
{
    fs_superblock_t sb;
    u32 i;

    if (!dev) return -1;
    if (mos_fs_read_superblock(dev, &sb) != 0) return -1;
    if (sb.magic != MOS_FS_MAGIC) return -1;

    mounted_dev = dev;
    {
        u32 *src = (u32 *)&sb;
        u32 *dst = (u32 *)&mounted_sb;
        for (i = 0; i < 32; i++) dst[i] = src[i];
    }

    for (i = 0; i < MAX_OPEN_FILES; i++) {
        open_files[i].in_use = 0;
        open_files[i].refcount = 0;
        open_files[i].dev = NULL;
        open_files[i].inode_num = 0;
    }

    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        dir_handles[i].in_use = 0;
    }

    return 0;
}

/* =========================== VFS 文件操作 =========================== */

int mos_open(const char *path, u16 flags)
{
    file_desc_t *fd_table;
    fs_inode_t parent_inode;
    u32 parent_ino;
    int ino, of_idx;
    u32 fd;

    if (!mounted_dev) return -1;

    fd_table = get_fd_table();

    for (fd = 0; fd < MAX_FD_PER_TASK; fd++) {
        if (!fd_table[fd].in_use) break;
    }
    if (fd >= MAX_FD_PER_TASK) return -1;

    ino = resolve_path(path, &parent_ino);

    if (ino < 0) {
        /* 文件不存在 */
        if (!(flags & O_CREAT)) return -1;

        ino = mos_fs_alloc_inode(mounted_dev, FT_REGULAR);
        if (ino < 0) return -1;

        /* 在父目录添加目录项 */
        if (mos_fs_read_inode(mounted_dev, parent_ino, &parent_inode) != 0) {
            mos_fs_free_inode(mounted_dev, (u32)ino);
            return -1;
        }

        const char *name = get_filename(path);
        if (mos_fs_add_dirent(mounted_dev, &parent_inode, name, (u32)ino) != 0) {
            mos_fs_free_inode(mounted_dev, (u32)ino);
            return -1;
        }
        mos_fs_write_inode(mounted_dev, parent_ino, &parent_inode);
    }

    /* 分配 open_file */
    of_idx = alloc_open_file(mounted_dev, (u32)ino);
    if (of_idx < 0) return -1;

    /* 填充 fd */
    fd_table[fd].in_use    = 1;
    fd_table[fd].inode_num = (u32)ino;
    fd_table[fd].of_idx    = (u32)of_idx;
    fd_table[fd].offset    = 0;
    fd_table[fd].flags     = flags;

    return (int)fd;
}

int mos_read(int fd, void *buf, u32 count)
{
    file_desc_t *fd_table;
    open_file_t *of;
    int result;

    if (!mounted_dev) return -1;

    fd_table = get_fd_table();
    if (fd < 0 || fd >= MAX_FD_PER_TASK || !fd_table[fd].in_use) return -1;

    of = &open_files[fd_table[fd].of_idx];
    result = mos_fs_read_file(mounted_dev, &of->inode,
                              fd_table[fd].offset, buf, count);
    if (result > 0) {
        fd_table[fd].offset += (u32)result;
    }
    return result;
}

int mos_write(int fd, const void *buf, u32 count)
{
    file_desc_t *fd_table;
    open_file_t *of;
    int result;

    if (!mounted_dev) return -1;

    fd_table = get_fd_table();
    if (fd < 0 || fd >= MAX_FD_PER_TASK || !fd_table[fd].in_use) return -1;

    of = &open_files[fd_table[fd].of_idx];
    result = mos_fs_write_file(mounted_dev, &of->inode,
                               fd_table[fd].offset, buf, count);
    if (result > 0) {
        fd_table[fd].offset += (u32)result;
        /* 写回 inode (保持磁盘同步) */
        mos_fs_write_inode(mounted_dev, fd_table[fd].inode_num, &of->inode);
    }
    return result;
}

int mos_close(int fd)
{
    file_desc_t *fd_table;
    open_file_t *of;

    if (!mounted_dev) return -1;

    fd_table = get_fd_table();
    if (fd < 0 || fd >= MAX_FD_PER_TASK || !fd_table[fd].in_use) return -1;

    of = &open_files[fd_table[fd].of_idx];

    /* 写回 inode */
    mos_fs_write_inode(mounted_dev, fd_table[fd].inode_num, &of->inode);

    /* 释放 */
    free_open_file(fd_table[fd].of_idx);
    fd_table[fd].in_use    = 0;
    fd_table[fd].inode_num = 0;
    fd_table[fd].of_idx    = 0;
    fd_table[fd].offset    = 0;

    return 0;
}

int mos_lseek(int fd, int offset, int whence)
{
    file_desc_t *fd_table;
    open_file_t *of;
    u32 new_offset;
    u32 file_size;

    fd_table = get_fd_table();
    if (fd < 0 || fd >= MAX_FD_PER_TASK || !fd_table[fd].in_use) return -1;

    of = &open_files[fd_table[fd].of_idx];
    file_size = of->inode.size;

    switch (whence) {
    case SEEK_SET:
        new_offset = (u32)offset;
        break;
    case SEEK_CUR:
        if (offset >= 0)
            new_offset = fd_table[fd].offset + (u32)offset;
        else {
            if ((u32)(-offset) > fd_table[fd].offset) return -1;
            new_offset = fd_table[fd].offset - (u32)(-offset);
        }
        break;
    case SEEK_END:
        if (offset >= 0)
            new_offset = file_size + (u32)offset;
        else {
            if ((u32)(-offset) > file_size) return -1;
            new_offset = file_size - (u32)(-offset);
        }
        break;
    default:
        return -1;
    }

    fd_table[fd].offset = new_offset;
    return (int)new_offset;
}

int mos_filesize(int fd)
{
    file_desc_t *fd_table;

    fd_table = get_fd_table();
    if (fd < 0 || fd >= MAX_FD_PER_TASK || !fd_table[fd].in_use) return -1;

    return (int)open_files[fd_table[fd].of_idx].inode.size;
}

/* =========================== 目录操作 =========================== */

int mos_opendir(const char *path)
{
    u32 parent_ino;
    fs_inode_t inode;
    int ino;
    u32 dd;

    if (!mounted_dev) return -1;

    ino = resolve_path(path, &parent_ino);
    if (ino < 0) return -1;

    if (mos_fs_read_inode(mounted_dev, (u32)ino, &inode) != 0) return -1;
    if (inode.mode != FT_DIR) return -1;

    for (dd = 0; dd < MAX_OPEN_DIRS; dd++) {
        if (!dir_handles[dd].in_use) break;
    }
    if (dd >= MAX_OPEN_DIRS) return -1;

    dir_handles[dd].in_use        = 1;
    dir_handles[dd].dir_inode_num = (u32)ino;
    dir_handles[dd].current_block = 0;
    dir_handles[dd].current_entry = 0;

    return (int)dd;
}

int mos_readdir(int dd, char *name, u32 maxlen)
{
    fs_inode_t dir_inode;
    u8 block_buf[512];
    u32 blocks_used;

    if (dd < 0 || dd >= MAX_OPEN_DIRS || !dir_handles[dd].in_use) return -1;
    if (!mounted_dev) return -1;

    if (mos_fs_read_inode(mounted_dev, dir_handles[dd].dir_inode_num,
                          &dir_inode) != 0)
        return -1;

    blocks_used = (dir_inode.size + 511) / 512;

    while (1) {
        u32 block_idx = dir_handles[dd].current_block;
        u32 entry_idx = dir_handles[dd].current_entry;

        if (block_idx >= MOS_FS_DIRECT_BLOCKS || block_idx >= blocks_used)
            return 1;

        u32 phys_block = dir_inode.blocks[block_idx];
        if (phys_block == 0) return 1;

        mounted_dev->read(mounted_dev, phys_block, block_buf);

        while (entry_idx < MOS_FS_DIRENTS_PER_BLOCK) {
            u32 *slot = (u32 *)(block_buf +
                                entry_idx * sizeof(fs_dirent_t));

            /* 推进指针 (便于下次调用) */
            entry_idx++;
            if (entry_idx >= MOS_FS_DIRENTS_PER_BLOCK) {
                dir_handles[dd].current_block++;
                dir_handles[dd].current_entry = 0;
            } else {
                dir_handles[dd].current_entry = entry_idx;
            }

            if (slot[0] != 0) {
                /* 复制文件名: slot[1..7] = name[28] */
                char *ent_name = (char *)(&slot[1]);
                u32 name_len = 0;
                while (name_len < maxlen - 1
                       && name_len < MOS_FS_NAME_MAX - 1
                       && ent_name[name_len]) {
                    name[name_len] = ent_name[name_len];
                    name_len++;
                }
                name[name_len] = '\0';
                return 0;
            }
        }
    }
}

int mos_closedir(int dd)
{
    if (dd < 0 || dd >= MAX_OPEN_DIRS) return -1;
    dir_handles[dd].in_use = 0;
    return 0;
}
