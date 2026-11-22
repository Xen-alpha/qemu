/*
 * Rockchip RK3326/PX30 Clock and Reset Unit (CRU and PMUCRU)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3326_CRU_H
#define HW_MISC_RK3326_CRU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3326_CRU "rk3326-cru"
OBJECT_DECLARE_SIMPLE_TYPE(RK3326CRUState, RK3326_CRU)

#define RK3326_CRU_SIZE     0x1000
#define RK3326_CRU_NREGS    (RK3326_CRU_SIZE / 4)

struct RK3326CRUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    bool pmucru;            /* property: PMUCRU (GPLL) instead of CRU */
    uint32_t regs[RK3326_CRU_NREGS];
    uint32_t glb_rst_st;    /* survives warm reset */
};

#endif
