#!/usr/bin/env python3
"""
把二进制文件转成一个 C 数组。

用法:
    python tools/mkblob.py <输入文件> <输出的 .c> <数组名>

生成:
    const unsigned char <数组名>[] = { ... };
    const unsigned int   <数组名>_len = N;

内核把用户程序 ELF 打包进镜像, 启动时再写进 RAM 盘 —— 这样
SYS_EXEC_ELF 走的是真实的"打开文件 → 读出来 → 解析 ELF"路径,
而不是直接拿内核里的一个指针糊弄过去。
"""

import sys


def main():
    if len(sys.argv) != 4:
        print("usage: mkblob.py <input> <output.c> <symbol>")
        return 1

    src, dst, sym = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(src, "rb").read()

    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/mkblob.py 自动生成, 不要手改 */\n")
        f.write('#include "lib/types.h"\n\n')
        f.write("const unsigned char %s[] = {\n" % sym)
        for i in range(0, len(data), 12):
            chunk = data[i:i + 12]
            f.write("    " + ", ".join("0x%02X" % b for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write("const unsigned int %s_len = %uu;\n" % (sym, len(data)))

    print("  BLOB    %s (%d bytes) -> %s" % (src, len(data), dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
