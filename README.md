# MOS RTOS — A Minimal Real-Time Operating System from Scratch

[![Platform](https://img.shields.io/badge/platform-ARM%20Cortex--A9%20(Zynq)-blue)](https://github.com/Mikeya98/mos)
[![Language](https://img.shields.io/badge/language-C%20%7C%20Assembly-orange)](https://github.com/Mikeya98/mos)
[![Status](https://img.shields.io/badge/status-v0.1%20--%20QEMU%20verified-brightgreen)](https://github.com/Mikeya98/mos)

**MOS** (My Operating System) is a minimal real-time operating system built from scratch for learning purposes.  
Every line of code is hand-written — no existing RTOS kernel code is used.

---

## Features

- **Priority-based preemptive scheduler**: 32 priority levels, O(1) bitmap lookup (ARM CLZ instruction)
- **Round-robin within same priority**: time-sliced fair scheduling
- **Counting semaphores**: basic task synchronization primitive
- **Bare-metal drivers**: UART (16550-compatible Cadence IP), ARM GIC PL390 interrupt controller, private timer
- **Minimal footprint**: ~6KB code, bootable in QEMU
- **QEMU verified**: runs on `xilinx-zynq-a9` machine model

## Architecture

```
┌─────────────────────────────────────────────┐
│                  Application                 │
│  ┌─────────┐  ┌─────────┐  ┌─────────────┐ │
│  │ Task A  │  │ Task B  │  │  IDLE Task   │ │
│  │ prio=1  │  │ prio=2  │  │  prio=31     │ │
│  └────┬────┘  └────┬────┘  └──────┬──────┘ │
├───────┼────────────┼──────────────┼─────────┤
│       │    Kernel  │              │         │
│  ┌────┴────────────┴──────────────┴───────┐ │
│  │         Preemptive Scheduler           │ │
│  │    O(1) pick_next_task (CLZ bitmap)    │ │
│  └────────────────┬───────────────────────┘ │
│  ┌────────────────┴───────────────────────┐ │
│  │     IPC: Counting Semaphore            │ │
│  └────────────────┬───────────────────────┘ │
├───────────────────┼─────────────────────────┤
│       Drivers     │                         │
│  ┌────────────────┴───────────────────────┐ │
│  │  UART  │  GIC PL390  │  Private Timer  │ │
│  └────────────────────────────────────────┘ │
├─────────────────────────────────────────────┤
│              ARM Cortex-A9                  │
│         (Xilinx Zynq-7045 / QEMU)           │
└─────────────────────────────────────────────┘
```

## Quick Start

### Prerequisites

```bash
# ARM cross-compiler (arm-none-eabi-gcc)
# QEMU with Zynq support (qemu-system-arm)
```

### Build & Run

```bash
# Build
make

# Run in QEMU
make qemu-run
```

Expected output:
```
MOS RTOS v0.1 booting...
Task A [prio=1]: tick=0
Task B [prio=2]: tick=0
Task A [prio=1]: tick=1
Task A [prio=1]: tick=2
Task B [prio=2]: tick=2
...
```

## Project Structure

```
mos/
├── src/
│   ├── boot/startup.S         # Boot: vector table, stack setup, VBAR
│   ├── kernel/
│   │   ├── main.c             # Kernel init + demo tasks
│   │   ├── task.c             # Scheduler + TCB management
│   │   └── sched_asm.S        # Context switch (PendSV handler)
│   ├── drivers/
│   │   ├── uart.c             # UART driver (Cadence / 16550)
│   │   ├── gic.c              # GIC PL390 interrupt controller
│   │   └── timer.c            # ARM private timer + tick handler
│   ├── ipc/semaphore.c        # Counting semaphore
│   └── lib/
│       ├── printf.c           # Minimal printf (no libc)
│       └── string.c           # Basic string operations
├── include/                   # Header files
├── kernel.lds                 # Linker script
└── Makefile                   # Build system
```

## Version Plan

| Version | Milestone |
|---------|-----------|
| **v0.1** ✅ | Minimal skeleton: scheduler + UART + timer + semaphore |
| v0.2 | Mutex with priority inheritance |
| v0.3 | Message queue |
| v0.4 | Dynamic memory allocator |
| v1.0 | Full QEMU + hardware validation |

## Build Details

- **Compiler**: ARM GCC (`arm-none-eabi-gcc`)
- **Target**: `-mcpu=cortex-a9 -mfloat-abi=soft`
- **Linker**: Custom `kernel.lds` (OCM + DDR memory regions)
- **QEMU**: `qemu-system-arm -M xilinx-zynq-a9`
- **ELF size**: ~6KB text, ~4B data, ~1.1MB BSS (idle stack)

## Author

**Mikeya98** — Embedded Systems Engineer  
Focus: real-time operating systems, FPGA acceleration, bare-metal development.

---

*MOS is a personal educational project. It is not derived from or affiliated with any commercial RTOS product.*
