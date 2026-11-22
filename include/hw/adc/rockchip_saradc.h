/*
 * Rockchip SARADC (v1, rk3399-saradc compatible) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ADC_ROCKCHIP_SARADC_H
#define HW_ADC_ROCKCHIP_SARADC_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_SARADC "rockchip-saradc"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipSARADCState, ROCKCHIP_SARADC)

#define ROCKCHIP_SARADC_CHANNELS 6

struct RockchipSARADCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    /* analog inputs as raw 10-bit codes, settable as QOM properties */
    uint16_t input[ROCKCHIP_SARADC_CHANNELS];

    uint32_t data;
    uint32_t ctrl;
    uint32_t dly_pu_soc;
    bool busy;
};

void rockchip_saradc_set_input(RockchipSARADCState *s, int ch, uint16_t val);

#endif
