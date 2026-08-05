/**
 * @file    vfs.h
 * @brief   VFS (Virtual File System) 层 — 文件描述符与 POSIX 风格 API
 *
 * 这层是应用程序看到的文件接口:
 *   - mos_open(path, flags) → fd
 *   - mos_read(fd, buf, count) / mos_write(fd, buf, count)
 *   - mos_close(fd)
 *   - mos_opendir(path) / mos_readdir(dd, name) / mos_closedir(dd)
 *
 * 内部维护:
 *   - 每个任务有 fd_table[8], 索引是 fd 号
 *   - 全局 open_files[] 表, 记录每个打开文件的 inode 和引用计数
 *   - 挂载表: 当前只支持挂载一个文件系统
 */

#ifndef _FS_VFS_H
#define _FS_VFS_H

#include "lib/types.h"
#include "fs/block_dev.h"
#include "fs/mos_fs.h"

/* =========================== 常量 =========================== */

#define MAX_FD_PER_TASK   8     /* 每任务最多打开 8 个文件 */
#define MAX_OPEN_FILES    16    /* 全局最多同时打开 16 个文件实例 */
#define MAX_MOUNTS        1     /* 当前只支持挂载一个文件系统 */

/* 文件打开标志 */
#define O_RDONLY  0x01
#define O_WRONLY  0x02
#define O_RDWR    0x03
#define O_CREAT   0x10          /* 文件不存在则创建 */

/* lseek whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* =========================== 数据结构 =========================== */

/**
 * @brief  单个文件描述符 (存储在 TCB 中)
 */
typedef struct {
    u32 inode_num;          /* 文件 inode 号 */
    u32 offset;             /* 当前读写位置 */
    u32 of_idx;             /* 对应 open_files[] 的索引 */
    u16 flags;              /* O_RDONLY / O_WRONLY / O_RDWR */
    u16 in_use;             /* 1=此 fd 槽正在使用, 0=空闲 */
} file_desc_t;

/**
 * @brief  全局打开文件实例 (多个 fd 可能指向同一个 inode)
 */
typedef struct {
    u32 inode_num;          /* 对应的 inode 号 (方便 fd→open_file 查找) */
    fs_inode_t inode;       /* 内存中的 inode 副本 */
    u32 refcount;           /* 多少个 fd 引用此文件 */
    block_dev_t *dev;       /* 所在的块设备 */
    u16 in_use;             /* 1=此槽正在使用 */
} open_file_t;

/* =========================== 挂载点 =========================== */

/**
 * @brief  挂载一个文件系统
 *
 * 当前只支持单挂载点 (覆盖之前挂载的)。
 *
 * @param  dev  已格式化的块设备
 * @return 0=成功, -1=超级块 magic 不匹配
 */
int mos_mount(block_dev_t *dev);

/* =========================== VFS API =========================== */

/**
 * @brief  打开文件
 *
 * @param  path   文件路径, 如 "/hello.txt" 或 "/docs/note.txt"
 *                (支持最多一层子目录)
 * @param  flags  O_RDONLY | O_WRONLY | O_RDWR | O_CREAT
 * @return fd (≥0), 失败返回 -1
 */
int mos_open(const char *path, u16 flags);

/**
 * @brief  读文件
 * @param  fd     open() 返回的文件描述符
 * @param  buf    读入缓冲区
 * @param  count  最多读取字节数
 * @return 实际读取字节数, 0=EOF, -1=错误
 */
int mos_read(int fd, void *buf, u32 count);

/**
 * @brief  写文件
 * @param  fd     open() 返回的文件描述符
 * @param  buf    写出缓冲区
 * @param  count  要写入的字节数
 * @return 实际写入字节数, -1=错误
 */
int mos_write(int fd, const void *buf, u32 count);

/**
 * @brief  关闭文件 (释放 fd 并写回 inode)
 * @return 0=成功, -1=fd 无效
 */
int mos_close(int fd);

/**
 * @brief  移动文件读写位置
 * @param  fd      open() 返回的文件描述符
 * @param  offset  偏移量 (可正可负)
 * @param  whence  SEEK_SET / SEEK_CUR / SEEK_END
 * @return 新的文件偏移, -1=错误
 */
int mos_lseek(int fd, int offset, int whence);

/**
 * @brief  打开目录 (用于遍历)
 * @param  path  目录路径, 如 "/" 或 "/docs"
 * @return 目录描述符 dd (≥0), 失败返回 -1
 */
int mos_opendir(const char *path);

/**
 * @brief  读取下一个目录项
 * @param  dd      opendir() 返回的目录描述符
 * @param  name    输出: 文件/子目录名 (调用者提供缓冲区)
 * @param  maxlen  输出缓冲区大小
 * @return 0=成功, 1=已到末尾, -1=错误
 */
int mos_readdir(int dd, char *name, u32 maxlen);

/**
 * @brief  关闭目录
 * @return 0=成功
 */
int mos_closedir(int dd);

/**
 * @brief  获取文件大小
 * @return 文件大小 (字节), -1=fd 无效
 */
int mos_filesize(int fd);

#endif /* _FS_VFS_H */
