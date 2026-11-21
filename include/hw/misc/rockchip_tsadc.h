/*
 * Rockchip TSADC (v2/v3 register layout, px30-tsadc) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ROCKCHIP_TSADC_H
#define HW_MISC_ROCKCHIP_TSADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_TSADC "rockchip-tsadc"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipTSADCState, ROCKCHIP_TSADC)

#define ROCKCHIP_TSADC_CHANNELS 2
#define ROCKCHIP_TSADC_NREGS    (0x100 / 4)

struct RockchipTSADCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    int32_t temp[ROCKCHIP_TSADC_CHANNELS];  /* milli-Celsius, properties */
    uint32_t regs[ROCKCHIP_TSADC_NREGS];
};

#endif
