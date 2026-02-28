#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "exec/hwaddr.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "qemu/timer.h"

#define GLANDA_WIDTH  640
#define GLANDA_HEIGHT 480
#define GLANDA_VRAM_SIZE (GLANDA_WIDTH * GLANDA_HEIGHT * 4)
#define GLANDA_REFRESH_INTERVAL_NS (1000000000 / 60) // 60Hz = ~16.6ms

#define TYPE_GLANDA_GPU "glandagpu"
OBJECT_DECLARE_SIMPLE_TYPE(GlandaGPUState, GLANDA_GPU)

struct GlandaGPUState {
    SysBusDevice parent_obj;

    MemoryRegion container;
    MemoryRegion vram;
    MemoryRegion mmio;

    qemu_irq irq;

    // Registers
    uint32_t status; // 0x00
    uint32_t ctrl;   // 0x04
    uint32_t coord0; // 0x08
    uint32_t coord1; // 0x0C
    uint32_t color;  // 0x10
    uint32_t isr;    // 0x14
    uint32_t ier;    // 0x18

    QemuConsole *con; // display
    QEMUTimer *vsync_timer;
};

// MMIO Read
static uint64_t glandagpu_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    GlandaGPUState *s = GLANDA_GPU(opaque);

    switch (offset) {
        case 0x00: 
            return s->status;
        case 0x04: 
            return s->ctrl;
        case 0x08: 
            return s->coord0;
        case 0x0C: 
            return s->coord1;
        case 0x10: 
            return s->color;
        case 0x14: 
            return s->isr;
        case 0x18: 
            return s->ier;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "GlandaGPU: Bad read at offset 0x%lx\n", offset);
            return 0;
    }
}

// Helper draw pixel
static void glandagpu_draw_pixel(GlandaGPUState *s, int x, int y, uint32_t color)
{
    if (x >= 0 && x < 640 && y >= 0 && y < 480) {
        uint32_t *vram = (uint32_t *)memory_region_get_ram_ptr(&s->vram);
        vram[y * 640 + x] = color;
    }
}

static void glandagpu_update_irq(GlandaGPUState *s)
{
    // Check if any enabled interrupt is pending
    if (s->isr & s->ier) {
        qemu_set_irq(s->irq, 1);
    } else {
        qemu_set_irq(s->irq, 0);
    }
}

static void glandagpu_execute_command(GlandaGPUState *s)
{
    uint8_t cmd = s->ctrl & 0xF;

    int x0 = s->coord0 & 0x3FF;
    int y0 = (s->coord0 >> 16) & 0x3FF;
    int x1_w = s->coord1 & 0x3FF;
    int y1_h = (s->coord1 >> 16) & 0x3FF;
    uint32_t color = s->color & 0xFFF;

    // Set BUSY
    s->status |= 0x1;

    switch (cmd) {
    case 0x1: // Clear Screen
    {
        uint32_t *vram = (uint32_t *)memory_region_get_ram_ptr(&s->vram);
        for (int i = 0; i < (640 * 480); i++) {
            vram[i] = color;
        }
        break;
    }
    case 0x2: // Rectangle
    {
        int w = x1_w;
        int h = y1_h;
        for (int y = y0; y < y0 + h; y++) {
            for (int x = x0; x < x0 + w; x++) {
                glandagpu_draw_pixel(s, x, y, color);
            }
        }
        break;
    }
    case 0x3: // Line (Bresenham's)
    {
        int x1 = x1_w;
        int y1 = y1_h;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy, e2;

        while (1) {
            glandagpu_draw_pixel(s, x0, y0, color);
            if (x0 == x1 && y0 == y1) break;
            e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
        break;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "GlandaGPU: Unknown CMD 0x%x\n", cmd);
        break;
    }

    // Clear BUSY
    s->status &= ~0x1; 

    //Raise Done interrupt
    s->isr |= 0x1;
    glandagpu_update_irq(s);
}

// MMIO Write
static void glandagpu_mmio_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    GlandaGPUState *s = GLANDA_GPU(opaque);

    switch (offset) {
    case 0x00: 
        break;
    case 0x04: // CTRL
        s->ctrl = val;
        if (val & (1 << 4)) {
            glandagpu_execute_command(s);
        }
        break;
    case 0x08: 
        s->coord0 = val; 
        break;
    case 0x0C: 
        s->coord1 = val; 
        break;
    case 0x10: 
        s->color = val; 
        break;
    case 0x14: // W1C
        s->isr &= ~val;
        glandagpu_update_irq(s);
        break;
    case 0x18: // IER Write to enable/disable interrupts
        s->ier = val;
        glandagpu_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "GlandaGPU: Bad write 0x%lx\n", offset);
    }
}

// Display code
static void glandagpu_invalidate_display(void *opaque)
{
    GlandaGPUState *s = GLANDA_GPU(opaque);
    qemu_console_resize(s->con, GLANDA_WIDTH, GLANDA_HEIGHT);
}

static void glandagpu_update_display(void *opaque)
{
    GlandaGPUState *s = GLANDA_GPU(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *dest_pixels;
    uint32_t *src_vram;
    int stride;

    // Pointers QEMU Window surface and VRAM
    dest_pixels = (uint32_t *)surface_data(surface);
    src_vram = (uint32_t *)memory_region_get_ram_ptr(&s->vram);
    stride = surface_stride(surface) / 4; // convert bytes to pixels

    for (int y = 0; y < GLANDA_HEIGHT; y++) {
        for (int x = 0; x < GLANDA_WIDTH; x++) {
            
            // 32-bit pixel (only lower 12 bits)
            uint32_t raw_val = src_vram[y * GLANDA_WIDTH + x];

            // convert 4-bit to 8-bit
            uint8_t r = (raw_val >> 8) & 0xF;
            uint8_t g = (raw_val >> 4) & 0xF;
            uint8_t b = (raw_val >> 0) & 0xF;

            // scale color from 4-bit to 8-bit
            r = (r << 4) | r;
            g = (g << 4) | g;
            b = (b << 4) | b;

            dest_pixels[y * stride + x] = (r << 16) | (g << 8) | b;
        }
    }

    dpy_gfx_update(s->con, 0, 0, GLANDA_WIDTH, GLANDA_HEIGHT);
}

// QEMU UI subsystem interface
static const GraphicHwOps glandagpu_ops = {
    .invalidate = glandagpu_invalidate_display,
    .gfx_update = glandagpu_update_display,
};

static const MemoryRegionOps glandagpu_mmio_ops = {
    .read = glandagpu_mmio_read,
    .write = glandagpu_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void glandagpu_vsync_cb(void *opaque)
{
    GlandaGPUState *s = GLANDA_GPU(opaque);

    //raise VSync interrupt
    s->isr |= (1 << 1);
    glandagpu_update_irq(s);

    timer_mod(s->vsync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GLANDA_REFRESH_INTERVAL_NS);
}

static void glandagpu_realize(DeviceState *dev, Error **errp)
{
    GlandaGPUState *s = GLANDA_GPU(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    // Container region
    memory_region_init(&s->container, OBJECT(s), "glandagpu.container", 0x200020);

    // VRAM at 0x0
    memory_region_init_ram(&s->vram, OBJECT(s), "glandagpu.vram", GLANDA_VRAM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->container, 0x000000, &s->vram);

    // MMIO at 0x200000
    memory_region_init_io(&s->mmio, OBJECT(s), &glandagpu_mmio_ops, s, "glandagpu.mmio", 32);
    memory_region_add_subregion(&s->container, 0x200000, &s->mmio);

    sysbus_init_mmio(sbd, &s->container);
    sysbus_init_irq(sbd, &s->irq);

    // Init console
    s->con = graphic_console_init(DEVICE(dev), 0, &glandagpu_ops, s);
    qemu_console_resize(s->con, GLANDA_WIDTH, GLANDA_HEIGHT);

    // Start VSync timer
    s->vsync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, glandagpu_vsync_cb, s);
    timer_mod(s->vsync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GLANDA_REFRESH_INTERVAL_NS);
}

static void glandagpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = glandagpu_realize;
    dc->desc = "GlandaGPU 2D Hardware Accelerator";
}

static const TypeInfo glandagpu_types[] = {
    {
        .name          = TYPE_GLANDA_GPU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(GlandaGPUState),
        .class_init    = glandagpu_class_init,
    },
};

DEFINE_TYPES(glandagpu_types)