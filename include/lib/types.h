/**
 * @file    types.h
 * @brief   MOS RTOS 基础类型定义
 *
 * 裸机环境下没有标准 C 库, 需要自己定义所有基本类型。
 * 这是整个项目第一个被 include 的头文件。
 */

#ifndef _MOS_TYPES_H
#define _MOS_TYPES_H

/* =========================== 整数类型 =========================== */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

/* 短别名 (项目内统一使用) */
typedef uint8_t             u8;
typedef uint16_t            u16;
typedef uint32_t            u32;
typedef uint64_t            u64;

typedef int8_t              s8;
typedef int16_t             s16;
typedef int32_t             s32;
typedef int64_t             s64;

/* 指针大小整数 (等价于 C99 uintptr_t, 裸机无 stdint.h) */
typedef u32                 uintptr_t;

/* =========================== 布尔类型 =========================== */
typedef uint8_t             bool;

#define true  1
#define false 0

/* =========================== 指针 =========================== */
#define NULL  ((void *)0)

/* =========================== 编译器辅助 =========================== */
/* 标记函数为内联 (尽量) */
#define INLINE  static inline

/* 标记未使用的变量/参数 (抑制编译器警告) */
#define UNUSED(x)  ((void)(x))

/* 编译时断言 (数组大小为负 → 编译失败) */
#define STATIC_ASSERT(cond)  \
    typedef char _static_assert_##__LINE__[(cond) ? 1 : -1]

/* 取结构体成员偏移 */
#define offsetof(type, member)  ((u32) & ((type *)0)->member)

/* 根据成员指针反推结构体指针 */
#define container_of(ptr, type, member) \
    ((type *)((u8 *)(ptr) - offsetof(type, member)))

#endif /* _MOS_TYPES_H */
