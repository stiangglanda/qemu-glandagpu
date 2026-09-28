#ifndef HW_DISPLAY_GLANDAGPU_H
#define HW_DISPLAY_GLANDAGPU_H

#include "system/memory.h"
#include "exec/hwaddr.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "qemu/timer.h"
#include "ui/console.h"

#define GLANDA_WIDTH  640
#define GLANDA_HEIGHT 480
#define GLANDA_VRAM_SIZE (GLANDA_WIDTH * GLANDA_HEIGHT * 4)
/* PCI BARs must be power-of-two sized; round the VRAM window up. */
#define GLANDA_VRAM_BAR_SIZE 0x200000
#define GLANDA_MMIO_SIZE  0x1000 /* 4K minimum MMIO window */
#define GLANDA_REFRESH_INTERVAL_NS (1000000000 / 60) /* 60 Hz */

/* Shared core state for the PCI and sysbus device wrappers. */
typedef struct GlandaGPUState {
    MemoryRegion vram;
    MemoryRegion mmio;

    /* Registers */
    uint32_t status; /* 0x00 */
    uint32_t ctrl;   /* 0x04 */
    uint32_t coord0; /* 0x08 */
    uint32_t coord1; /* 0x0C */
    uint32_t color;  /* 0x10 */
    uint32_t isr;    /* 0x14 */
    uint32_t ier;    /* 0x18 */

    QemuConsole *con;
    QEMUTimer *vsync_timer;

    qemu_irq irq;
} GlandaGPUState;

void glandagpu_core_realize(GlandaGPUState *s, DeviceState *dev);

void glandagpu_core_exit(GlandaGPUState *s);

#endif