/*
 * Rockchip IOMMU (v1, "rockchip,iommu") model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ROCKCHIP_IOMMU_H
#define HW_MISC_ROCKCHIP_IOMMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_IOMMU "rockchip-iommu"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipIOMMUState, ROCKCHIP_IOMMU)

struct RockchipIOMMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    MemoryRegion *dma_mr;   /* link property "dma-memory" */
    AddressSpace dma_as;
    uint32_t size;          /* property: register window size */

    uint32_t dte_addr;
    bool paging;
    bool stall;
    bool fault;
    uint32_t fault_addr;
    uint32_t int_rawstat;
    uint32_t int_mask;
    uint32_t auto_gating;
};

/*
 * Translate a bus master address.  Returns false (and raises a page
 * fault) if paging is enabled and the address is not mapped.
 */
bool rockchip_iommu_translate(RockchipIOMMUState *s, uint32_t iova,
                              bool is_write, hwaddr *pa);

#endif
