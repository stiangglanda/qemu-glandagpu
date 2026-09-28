#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qapi/error.h"
#include "qom/object.h"

#include "glandagpu.h"

/* Reserved for experimental use (docs/specs/pci-ids.rst) */
#define PCI_VENDOR_ID_GLANDA   0x1af4
#define PCI_DEVICE_ID_GLANDA   0x10f0
/*
 * TODO: switch to the permanent device ID once assigned in the 1b36
 * (QEMU/Red Hat) range -- being requested as part of this same patch
 * series.
 */

#define TYPE_GLANDA_GPU_PCI "glandagpu-pci"
OBJECT_DECLARE_SIMPLE_TYPE(GlandaGPUPCIState, GLANDA_GPU_PCI)

struct GlandaGPUPCIState {
    PCIDevice parent_obj;

    GlandaGPUState core;
};

static void glandagpu_pci_realize(PCIDevice *pdev, Error **errp)
{
    GlandaGPUPCIState *s = GLANDA_GPU_PCI(pdev);

    glandagpu_core_realize(&s->core, DEVICE(pdev));

    /* BAR1, prefetchable (framebuffer) */
    pci_register_bar(pdev, 1,
                      PCI_BASE_ADDRESS_SPACE_MEMORY |
                      PCI_BASE_ADDRESS_MEM_PREFETCH,
                      &s->core.vram);

    /* BAR0, non-prefetchable (commands) */
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->core.mmio);

    /* legacy INTx TODO use MSI */
    pdev->config[PCI_INTERRUPT_PIN] = 1;
    s->core.irq = pci_allocate_irq(pdev);
}

static void glandagpu_pci_exit(PCIDevice *pdev)
{
    GlandaGPUPCIState *s = GLANDA_GPU_PCI(pdev);

    glandagpu_core_exit(&s->core);
}

static void glandagpu_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = glandagpu_pci_realize;
    k->exit = glandagpu_pci_exit;
    k->vendor_id = PCI_VENDOR_ID_GLANDA;
    k->device_id = PCI_DEVICE_ID_GLANDA;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;

    dc->desc = "GlandaGPU (PCI)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo glandagpu_pci_types[] = {
    {
        .name          = TYPE_GLANDA_GPU_PCI,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(GlandaGPUPCIState),
        .class_init    = glandagpu_pci_class_init,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(glandagpu_pci_types)