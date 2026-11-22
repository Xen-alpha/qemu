/*
 * Rockchip RK3326/PX30 Power Management Unit
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3326_PMU_H
#define HW_MISC_RK3326_PMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3326_PMU "rk3326-pmu"
OBJECT_DECLARE_SIMPLE_TYPE(RK3326PMUState, RK3326_PMU)

#define RK3326_PMU_SIZE     0x1000
#define RK3326_PMU_NREGS    (RK3326_PMU_SIZE / 4)

struct RK3326PMUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[RK3326_PMU_NREGS];
};

#endif
