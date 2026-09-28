#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "qemu/timer.h"

#include "glandagpu.h"

/* MMIO Read */
static uint64_t glandagpu_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    GlandaGPUState *s = opaque;

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

/* Helper draw pixel */
static void glandagpu_draw_pixel(GlandaGPUState *s, int x, int y, uint32_t color)
{
    if (x >= 0 && x < 640 && y >= 0 && y < 480) {
        uint32_t *vram = (uint32_t *)memory_region_get_ram_ptr(&s->vram);
        vram[y * 640 + x] = color;
    }
}

static void glandagpu_update_irq(GlandaGPUState *s)
{
    /* Check if any enabled interrupt is pending */
    qemu_set_irq(s->irq, !!(s->isr & s->ier));
}

static void glandagpu_execute_command(GlandaGPUState *s)
{
    uint8_t cmd = s->ctrl & 0xF;

    int x0 = s->coord0 & 0x3FF;
    int y0 = (s->coord0 >> 16) & 0x3FF;
    int x1_w = s->coord1 & 0x3FF;
    int y1_h = (s->coord1 >> 16) & 0x3FF;
    uint32_t color = s->color & 0xFFF;

    /* Set BUSY */
    s->status |= 0x1;

    switch (cmd) {
    case 0x1: /* Clear Screen */
    {
        uint32_t *vram = (uint32_t *)memory_region_get_ram_ptr(&s->vram);
        for (int i = 0; i < (640 * 480); i++) {
            vram[i] = color;
        }
        break;
    }
    case 0x2: /* Rectangle */
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
    case 0x3: /* Line (Bresenham's) */
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

    /* Clear BUSY */
    s->status &= ~0x1;

    /* Raise Done interrupt */
    s->isr |= 0x1;
    glandagpu_update_irq(s);
}

/* MMIO Write */
static void glandagpu_mmio_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    GlandaGPUState *s = opaque;

    switch (offset) {
    case 0x00:
        break;
    case 0x04: /* CTRL */
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
    case 0x14: /* W1C */
        s->isr &= ~val;
        glandagpu_update_irq(s);
        break;
    case 0x18: /* IER: write to enable/disable interrupts */
        s->ier = val;
        glandagpu_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "GlandaGPU: Bad write 0x%lx\n", offset);
    }
}

/* Display code */
static void glandagpu_invalidate_display(void *opaque)
{
    GlandaGPUState *s = opaque;
    qemu_console_resize(s->con, GLANDA_WIDTH, GLANDA_HEIGHT);
}

static void glandagpu_update_display(void *opaque)
{
    GlandaGPUState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *dest_pixels;
    uint32_t *src_vram;
    int stride;

    dest_pixels = (uint32_t *)surface_data(surface);
    src_vram = (uint32_t *)memory_region_get_ram_ptr(&s->vram);
    stride = surface_stride(surface) / 4; /* convert bytes to pixels */

    for (int y = 0; y < GLANDA_HEIGHT; y++) {
        for (int x = 0; x < GLANDA_WIDTH; x++) {

            /* 32-bit pixel (only lower 12 bits) */
            uint32_t raw_val = src_vram[y * GLANDA_WIDTH + x];

            /* convert 4-bit to 8-bit */
            uint8_t r = (raw_val >> 8) & 0xF;
            uint8_t g = (raw_val >> 4) & 0xF;
            uint8_t b = (raw_val >> 0) & 0xF;

            r = (r << 4) | r;
            g = (g << 4) | g;
            b = (b << 4) | b;

            dest_pixels[y * stride + x] = (r << 16) | (g << 8) | b;
        }
    }

    dpy_gfx_update(s->con, 0, 0, GLANDA_WIDTH, GLANDA_HEIGHT);
}

static const GraphicHwOps glandagpu_ops = {
    .invalidate = glandagpu_invalidate_display,
    .gfx_update = glandagpu_update_display,
};

static const MemoryRegionOps glandagpu_mmio_ops = {
    .read = glandagpu_mmio_read,
    .write = glandagpu_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void glandagpu_vsync_cb(void *opaque)
{
    GlandaGPUState *s = opaque;

    /* raise VSync interrupt */
    s->isr |= (1 << 1);
    glandagpu_update_irq(s);

    timer_mod(s->vsync_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GLANDA_REFRESH_INTERVAL_NS);
}

void glandagpu_core_realize(GlandaGPUState *s, DeviceState *dev)
{
    Object *owner = OBJECT(dev);

    memory_region_init_ram(&s->vram, owner, "glandagpu.vram",
                            GLANDA_VRAM_BAR_SIZE, &error_fatal);
    memory_region_init_io(&s->mmio, owner, &glandagpu_mmio_ops, s,
                           "glandagpu.mmio", GLANDA_MMIO_SIZE);

    s->con = graphic_console_init(dev, 0, &glandagpu_ops, s);
    qemu_console_resize(s->con, GLANDA_WIDTH, GLANDA_HEIGHT);

    s->vsync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, glandagpu_vsync_cb, s);
    timer_mod(s->vsync_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GLANDA_REFRESH_INTERVAL_NS);
}

void glandagpu_core_exit(GlandaGPUState *s)
{
    timer_free(s->vsync_timer);
}