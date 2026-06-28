# ===========================
# MOS RTOS — 构建系统
# ===========================
# 用法:
#   make         编译所有, 生成 build/mos.elf
#   make qemu    编译 + 启动 QEMU 仿真
#   make clean   清除构建产物
#   make dump    反汇编 elf → build/mos.lst (调试用)
#   make size    打印各段大小
# ===========================

# =========================== 工具链 ===========================
CROSS    ?= arm-none-eabi-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump
SIZE     = $(CROSS)size

# =========================== 目录 ===========================
SRC_DIR  = src
INC_DIR  = include
BLD_DIR  = build
OBJ_DIR  = $(BLD_DIR)/obj

# =========================== 编译选项 ===========================
# CPU:        ARM Cortex-A9
# 架构:       ARMv7-A
# FPU:        VFPv3 (硬件浮点, v0.1 不使用)
# 优化:       -Og (调试友好优化)
# 特殊选项:   -ffreestanding (独立环境, 无标准库)
#            -nostdlib      (不链接 libc)
#            -fno-common    (防止 common 符号合并)
ARCH_FLAGS  = -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=soft
WARN_FLAGS  = -Wall -Wextra -Wshadow -Wpointer-arith \
              -Wcast-align -Wundef -Wconversion \
              -Wno-unused-parameter
OPT_FLAGS   = -Og -g3
STD_FLAGS   = -ffreestanding -nostdlib -fno-common \
              -fno-builtin -fno-stack-protector

CFLAGS      = $(ARCH_FLAGS) $(WARN_FLAGS) $(OPT_FLAGS) $(STD_FLAGS) \
              -I$(INC_DIR)

# 汇编文件编译选项
ASFLAGS     = $(ARCH_FLAGS) -I$(INC_DIR)

# 链接选项 (用 gcc 链接以便自动链接 libgcc)
LDFLAGS     = -Wl,-T,kernel.lds -nostdlib -Wl,-Map,$(BLD_DIR)/mos.map

# =========================== 源文件 ===========================
# 汇编
ASM_SRCS = $(SRC_DIR)/boot/startup.S \
           $(SRC_DIR)/kernel/sched_asm.S

# C 库
LIB_SRCS = $(SRC_DIR)/lib/string.c \
           $(SRC_DIR)/lib/printf.c

# 驱动
DRV_SRCS = $(SRC_DIR)/drivers/uart.c \
           $(SRC_DIR)/drivers/gic.c \
           $(SRC_DIR)/drivers/timer.c

# 内核
KERNEL_SRCS = $(SRC_DIR)/kernel/main.c \
              $(SRC_DIR)/kernel/task.c

# IPC
IPC_SRCS = $(SRC_DIR)/ipc/semaphore.c \
           $(SRC_DIR)/ipc/mutex.c

# 所有 C 源文件
C_SRCS   = $(LIB_SRCS) $(DRV_SRCS) $(KERNEL_SRCS) $(IPC_SRCS)

# =========================== 目标文件 ===========================
C_OBJS   = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(C_SRCS))
ASM_OBJS = $(patsubst $(SRC_DIR)/%.S, $(OBJ_DIR)/%.o, $(ASM_SRCS))
ALL_OBJS = $(C_OBJS) $(ASM_OBJS)

# =========================== 输出 ===========================
TARGET   = $(BLD_DIR)/mos.elf
TARGET_BIN = $(BLD_DIR)/mos.bin
TARGET_LST = $(BLD_DIR)/mos.lst

# =========================== 目标 ===========================

.PHONY: all qemu clean dump size dirs

all: dirs $(TARGET) size

# 生成 ELF (用 gcc 链接, -lgcc 必须在 .o 之后)
$(TARGET): $(ALL_OBJS) kernel.lds
	@echo "  LD      $@"
	@$(CC) $(ARCH_FLAGS) -o $@ $(ALL_OBJS) $(LDFLAGS) -lgcc
	@echo "  OBJCOPY $(TARGET_BIN)"
	@$(OBJCOPY) -O binary $@ $(TARGET_BIN)

# 编译 C 文件
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

# 编译汇编文件
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	@$(CC) $(ASFLAGS) -c -o $@ $<

# QEMU 仿真 (带 GDB server)
qemu: all
	@echo ""
	@echo "======================================"
	@echo "  Starting QEMU (xilinx-zynq-a9)"
	@echo "======================================"
	@echo ""
	qemu-system-arm -M xilinx-zynq-a9 \
		-device loader,file=$(TARGET),cpu-num=0 \
		-nographic \
		-serial mon:stdio \
		-s -S &
	@echo ""
	@echo "QEMU started with GDB server on :1234"
	@echo "Connect: gdb-multiarch -ex 'target remote :1234' build/mos.elf"

# 仅编译 + QEMU (直接运行)
qemu-run: all
	qemu-system-arm -M xilinx-zynq-a9 \
		-device loader,file=$(TARGET),cpu-num=0 \
		-nographic \
		-serial mon:stdio

# 反汇编
dump: all
	@echo "  OBJDUMP $(TARGET_LST)"
	@$(OBJDUMP) -d $(TARGET) > $(TARGET_LST)
	@echo "Disassembly written to $(TARGET_LST)"

# 各段大小
size: $(TARGET)
	@$(SIZE) $(TARGET)

# 目录
dirs:
	@mkdir -p $(BLD_DIR) $(OBJ_DIR)

# 清理
clean:
	@echo "  CLEAN"
	@rm -rf $(BLD_DIR)
	@echo "Build directory removed."

# 帮助
help:
	@echo "MOS RTOS Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make          Build mos.elf + mos.bin"
	@echo "  make qemu     Build + launch QEMU (with GDB stub)"
	@echo "  make qemu-run Build + launch QEMU (no GDB, direct run)"
	@echo "  make dump     Build + disassemble → mos.lst"
	@echo "  make clean    Remove build directory"
	@echo "  make size     Print section sizes"
	@echo ""
	@echo "Requirements:"
	@echo "  arm-none-eabi-gcc  (ARM cross-compiler)"
	@echo "  qemu-system-arm    (QEMU with xilinx-zynq-a9)"
