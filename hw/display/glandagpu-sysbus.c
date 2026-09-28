#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qom/object.h"

#include "glandagpu.h"

#define TYPE_GLANDA_GPU_SYSBUS "glandagpu-sysbus"
OBJECT_DECLARE_SIMPLE_TYPE(GlandaGPUSysBusState, GLANDA_GPU_SYSBUS)

struct GlandaGPUSysBusState {
    SysBusDevice parent_obj;

    GlandaGPUState core;
};

static void glandagpu_sysbus_realize(DeviceState *dev, Error **errp)
{
    GlandaGPUSysBusState *s = GLANDA_GPU_SYSBUS(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    glandagpu_core_realize(&s->core, dev);

    /* mmio region 0: command/status registers, region 1: framebuffer */
    sysbus_init_mmio(sbd, &s->core.mmio);
    sysbus_init_mmio(sbd, &s->core.vram);
    sysbus_init_irq(sbd, &s->core.irq);
}

static void glandagpu_sysbus_unrealize(DeviceState *dev)
{
    GlandaGPUSysBusState *s = GLANDA_GPU_SYSBUS(dev);

    glandagpu_core_exit(&s->core);
}

static void glandagpu_sysbus_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = glandagpu_sysbus_realize;
    dc->unrealize = glandagpu_sysbus_unrealize;
    dc->desc = "GlandaGPU (sysbus)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->user_creatable = false;
}

static const TypeInfo glandagpu_sysbus_types[] = {
    {
        .name          = TYPE_GLANDA_GPU_SYSBUS,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(GlandaGPUSysBusState),
        .class_init    = glandagpu_sysbus_class_init,
    },
};

DEFINE_TYPES(glandagpu_sysbus_types)