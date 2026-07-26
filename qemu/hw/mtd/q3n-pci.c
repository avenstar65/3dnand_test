/*
 * PCI front-end for the QEMU 3D NAND model.
 *
 * The PCI wrapper keeps the storage model in q3n-nand.c and only makes the
 * MMIO window discoverable on x86_64 guests through BAR0.
 */

#include "qemu/osdep.h"
#include "hw/mtd/q3n-nand.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/module.h"

typedef struct Q3NNandPciState {
    PCIDevice parent_obj;

    Q3NNandState *nand;
    BlockBackend *blk;
    qemu_irq irq;
} Q3NNandPciState;

OBJECT_DECLARE_SIMPLE_TYPE(Q3NNandPciState, Q3N_NAND_PCI)

static void q3n_pci_realize(PCIDevice *pdev, Error **errp)
{
    Q3NNandPciState *s = Q3N_NAND_PCI(pdev);
    DeviceState *nand_dev;

    s->nand = Q3N_NAND(object_new(TYPE_Q3N_NAND));
    nand_dev = DEVICE(s->nand);
    if (!s->blk) {
        error_setg(errp, "q3n-nand-pci requires a drive");
        object_unref(OBJECT(s->nand));
        s->nand = NULL;
        return;
    }
    qdev_prop_set_drive(nand_dev, "drive", s->blk);
    if (!sysbus_realize(SYS_BUS_DEVICE(nand_dev), errp)) {
        object_unref(OBJECT(s->nand));
        s->nand = NULL;
        return;
    }

    pdev->config[PCI_INTERRUPT_PIN] = 0x01;
    s->irq = pci_allocate_irq(pdev);
    q3n_nand_set_irq(s->nand, s->irq);

    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     q3n_nand_get_mmio(s->nand));
}

static const Property q3n_pci_properties[] = {
    DEFINE_PROP_DRIVE("drive", Q3NNandPciState, blk),
};

static void q3n_pci_exit(PCIDevice *pdev)
{
    Q3NNandPciState *s = Q3N_NAND_PCI(pdev);

    if (s->nand) {
        qdev_unrealize(DEVICE(s->nand));
        object_unref(OBJECT(s->nand));
        s->nand = NULL;
    }
    if (s->irq) {
        qemu_free_irq(s->irq);
        s->irq = NULL;
    }
}

static void q3n_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = q3n_pci_realize;
    k->exit = q3n_pci_exit;
    k->vendor_id = Q3N_PCI_VENDOR_ID;
    k->device_id = Q3N_PCI_DEVICE_ID;
    k->revision = Q3N_PCI_REVISION;
    k->class_id = PCI_CLASS_MEMORY_FLASH;
    device_class_set_props(dc, q3n_pci_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo q3n_pci_type_info = {
    .name = TYPE_Q3N_NAND_PCI,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Q3NNandPciState),
    .class_init = q3n_pci_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void q3n_pci_register_types(void)
{
    type_register_static(&q3n_pci_type_info);
}

type_init(q3n_pci_register_types)
