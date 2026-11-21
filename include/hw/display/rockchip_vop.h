/*
 * Rockchip VOP (video output processor), PX30/RK3326 "VOP big" model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_ROCKCHIP_VOP_H
#define HW_DISPLAY_ROCKCHIP_VOP_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_ROCKCHIP_VOP "rockchip-vop"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipVOPState, ROCKCHIP_VOP)

#define ROCKCHIP_VOP_SIZE   0xf00
#define ROCKCHIP_VOP_NREGS  (ROCKCHIP_VOP_SIZE / 4)

struct RockchipIOMMUState;
struct DWMipiDsiState;

struct RockchipVOPState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QemuConsole *con;
    QEMUTimer *frame_timer;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    struct RockchipIOMMUState *iommu;   /* optional link */
    struct DWMipiDsiState *dsi;         /* optional link */
    uint32_t rotation;                  /* property: 0/90/180/270 */

    uint32_t regs[ROCKCHIP_VOP_NREGS];  /* programmed (shadow) values */
    uint32_t active[ROCKCHIP_VOP_NREGS];/* values in use by the scanout */
    uint32_t intr_raw;
    uint32_t intr_en;
    bool cfg_pending;
    bool was_standby;
    uint64_t frame_count;
};

#endif
