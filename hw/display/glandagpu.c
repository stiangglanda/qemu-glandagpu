#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "exec/hwaddr.h"
#include "qapi/error.h"
#include "ui/console.h"

#define GLANDA_WIDTH  640
#define GLANDA_HEIGHT 480
#define GLANDA_VRAM_SIZE (GLANDA_WIDTH * GLANDA_HEIGHT * 4)

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

// MMIO Write
static void glandagpu_mmio_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    GlandaGPUState *s = GLANDA_GPU(opaque);

    switch (offset) {
        case 0x04: 
            s->ctrl = val;
            // TODO: If Bit 4 (START) is 1, execute the 2D command here
            printf("GlandaGPU: CMD triggered! CTRL=0x%08lx\n", val);
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
        case 0x14: 
            s->isr &= ~val; // W1C 
            break; 
        case 0x18: 
            s->ier = val; 
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "GlandaGPU: Bad write at offset 0x%lx\n", offset);
            break;
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

            // Write to QEMU Surface (Format XRGB8888)
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