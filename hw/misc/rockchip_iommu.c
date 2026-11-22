/*
 * Rockchip IOMMU (v1, "rockchip,iommu") model
 *
 * Two-level page table, 4 KiB pages:
 *   iova[31:22] index into the directory table at DTE_ADDR
 *   DTE: [31:12] page table address, [0] valid
 *   iova[21:12] index into the page table
 *   PTE: [31:12] page address, [2] writable, [1] readable, [0] valid
 *
 *   0x00 DTE_ADDR  0x04 STATUS  0x08 COMMAND  0x0c PAGE_FAULT_ADDR
 *   0x10 ZAP_ONE_LINE  0x14 INT_RAWSTAT  0x18 INT_CLEAR  0x1c INT_MASK
 *   0x20 INT_STATUS    0x24 AUTO_GATING
 *
 * There is no IOTLB, so zap commands are no-ops.  The model does not sit
 * in the memory path; bus masters that are behind it (the VOP) call
 * rockchip_iommu_translate().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/rockchip_iommu.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#define RK_MMU_DTE_ADDR         0x00
#define RK_MMU_STATUS           0x04
#define RK_MMU_COMMAND          0x08
#define RK_MMU_PAGE_FAULT_ADDR  0x0c
#define RK_MMU_ZAP_ONE_LINE     0x10
#define RK_MMU_INT_RAWSTAT      0x14
#define RK_MMU_INT_CLEAR        0x18
#define RK_MMU_INT_MASK         0x1c
#define RK_MMU_INT_STATUS       0x20
#define RK_MMU_AUTO_GATING      0x24

#define STATUS_PAGING_ENABLED       (1u << 0)
#define STATUS_PAGE_FAULT_ACTIVE    (1u << 1)
#define STATUS_STALL_ACTIVE         (1u << 2)
#define STATUS_IDLE                 (1u << 3)
#define STATUS_REPLAY_BUFFER_EMPTY  (1u << 4)
#define STATUS_PAGE_FAULT_IS_WRITE  (1u << 5)
#define STATUS_STALL_NOT_ACTIVE     (1u << 31)

#define CMD_ENABLE_PAGING       0
#define CMD_DISABLE_PAGING      1
#define CMD_ENABLE_STALL        2
#define CMD_DISABLE_STALL       3
#define CMD_ZAP_CACHE           4
#define CMD_PAGE_FAULT_DONE     5
#define CMD_FORCE_RESET         6

#define IRQ_PAGE_FAULT          0x01
#define IRQ_BUS_ERROR           0x02

#define DTE_ADDR_MASK           0xfffff000
#define ENTRY_VALID             (1u << 0)
#define PTE_READABLE            (1u << 1)
#define PTE_WRITABLE            (1u << 2)

static void rk_iommu_update_irq(RockchipIOMMUState *s)
{
    qemu_set_irq(s->irq, !!(s->int_rawstat & s->int_mask));
}

static void rk_iommu_regs_reset(RockchipIOMMUState *s)
{
    s->dte_addr = 0;
    s->paging = false;
    s->stall = false;
    s->fault = false;
    s->fault_addr = 0;
    s->int_rawstat = 0;
    s->int_mask = 0;
    s->auto_gating = 0;
}

bool rockchip_iommu_translate(RockchipIOMMUState *s, uint32_t iova,
                              bool is_write, hwaddr *pa)
{
    uint32_t dte, pte;
    MemTxResult r;

    if (!s->paging) {
        *pa = iova;
        return true;
    }

    dte = address_space_ldl_le(&s->dma_as,
                               s->dte_addr + ((iova >> 22) & 0x3ff) * 4,
                               MEMTXATTRS_UNSPECIFIED, &r);
    if (r != MEMTX_OK) {
        goto bus_error;
    }
    if (!(dte & ENTRY_VALID)) {
        goto fault;
    }
    pte = address_space_ldl_le(&s->dma_as,
                               (dte & DTE_ADDR_MASK) +
                               ((iova >> 12) & 0x3ff) * 4,
                               MEMTXATTRS_UNSPECIFIED, &r);
    if (r != MEMTX_OK) {
        goto bus_error;
    }
    if (!(pte & ENTRY_VALID) ||
        !(pte & (is_write ? PTE_WRITABLE : PTE_READABLE))) {
        goto fault;
    }
    *pa = (pte & DTE_ADDR_MASK) | (iova & 0xfff);
    return true;

fault:
    if (!s->fault) {
        s->fault = true;
        s->fault_addr = iova;
        s->int_rawstat |= IRQ_PAGE_FAULT;
        rk_iommu_update_irq(s);
    }
    return false;
bus_error:
    s->int_rawstat |= IRQ_BUS_ERROR;
    rk_iommu_update_irq(s);
    return false;
}

static uint64_t rk_iommu_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipIOMMUState *s = opaque;
    uint32_t val;

    switch (offset) {
    case RK_MMU_DTE_ADDR:
        return s->dte_addr;
    case RK_MMU_STATUS:
        val = STATUS_IDLE | STATUS_REPLAY_BUFFER_EMPTY;
        if (s->paging) {
            val |= STATUS_PAGING_ENABLED;
        }
        if (s->fault) {
            val |= STATUS_PAGE_FAULT_ACTIVE;
        }
        /* stall only takes effect while paging is enabled */
        if (s->stall && s->paging) {
            val |= STATUS_STALL_ACTIVE;
        } else {
            val |= STATUS_STALL_NOT_ACTIVE;
        }
        return val;
    case RK_MMU_PAGE_FAULT_ADDR:
        return s->fault_addr;
    case RK_MMU_INT_RAWSTAT:
        return s->int_rawstat;
    case RK_MMU_INT_MASK:
        return s->int_mask;
    case RK_MMU_INT_STATUS:
        return s->int_rawstat & s->int_mask;
    case RK_MMU_AUTO_GATING:
        return s->auto_gating;
    }
    return 0;
}

static void rk_iommu_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    RockchipIOMMUState *s = opaque;

    switch (offset) {
    case RK_MMU_DTE_ADDR:
        s->dte_addr = value & DTE_ADDR_MASK;
        break;
    case RK_MMU_COMMAND:
        switch (value & 7) {
        case CMD_ENABLE_PAGING:
            s->paging = true;
            break;
        case CMD_DISABLE_PAGING:
            s->paging = false;
            break;
        case CMD_ENABLE_STALL:
            s->stall = true;
            break;
        case CMD_DISABLE_STALL:
            s->stall = false;
            break;
        case CMD_ZAP_CACHE:
            break;
        case CMD_PAGE_FAULT_DONE:
            s->fault = false;
            break;
        case CMD_FORCE_RESET:
            rk_iommu_regs_reset(s);
            break;
        }
        break;
    case RK_MMU_ZAP_ONE_LINE:
        break;
    case RK_MMU_INT_CLEAR:
        s->int_rawstat &= ~value;
        break;
    case RK_MMU_INT_MASK:
        s->int_mask = value & (IRQ_PAGE_FAULT | IRQ_BUS_ERROR);
        break;
    case RK_MMU_AUTO_GATING:
        s->auto_gating = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-iommu: write to RO/bad "
                      "offset 0x%" HWADDR_PRIx "\n", offset);
    }
    rk_iommu_update_irq(s);
}

static const MemoryRegionOps rk_iommu_ops = {
    .read = rk_iommu_read,
    .write = rk_iommu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rk_iommu_reset_hold(Object *obj, ResetType type)
{
    rk_iommu_regs_reset(ROCKCHIP_IOMMU(obj));
}

static void rk_iommu_realize(DeviceState *dev, Error **errp)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(dev);

    if (s->size < 0x28 || s->size > 0x1000) {
        error_setg(errp, "rockchip-iommu: invalid size 0x%x", s->size);
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &rk_iommu_ops, s,
                          TYPE_ROCKCHIP_IOMMU, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    address_space_init(&s->dma_as, s->dma_mr ? s->dma_mr : get_system_memory(),
                       "rockchip-iommu-dma");
}

static void rk_iommu_init(Object *obj)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property rk_iommu_props[] = {
    DEFINE_PROP_UINT32("size", RockchipIOMMUState, size, 0x100),
    DEFINE_PROP_LINK("dma-memory", RockchipIOMMUState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const VMStateDescription vmstate_rk_iommu = {
    .name = TYPE_ROCKCHIP_IOMMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dte_addr, RockchipIOMMUState),
        VMSTATE_BOOL(paging, RockchipIOMMUState),
        VMSTATE_BOOL(stall, RockchipIOMMUState),
        VMSTATE_BOOL(fault, RockchipIOMMUState),
        VMSTATE_UINT32(fault_addr, RockchipIOMMUState),
        VMSTATE_UINT32(int_rawstat, RockchipIOMMUState),
        VMSTATE_UINT32(int_mask, RockchipIOMMUState),
        VMSTATE_UINT32(auto_gating, RockchipIOMMUState),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_iommu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = rk_iommu_realize;
    dc->vmsd = &vmstate_rk_iommu;
    device_class_set_props(dc, rk_iommu_props);
    rc->phases.hold = rk_iommu_reset_hold;
}

static const TypeInfo rk_iommu_info = {
    .name = TYPE_ROCKCHIP_IOMMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipIOMMUState),
    .instance_init = rk_iommu_init,
    .class_init = rk_iommu_class_init,
};

static void rk_iommu_register_types(void)
{
    type_register_static(&rk_iommu_info);
}

type_init(rk_iommu_register_types)
