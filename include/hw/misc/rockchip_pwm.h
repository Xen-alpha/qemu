/*
 * Rockchip PWM v2 (rk3328-pwm / px30-pwm) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ROCKCHIP_PWM_H
#define HW_MISC_ROCKCHIP_PWM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_PWM "rockchip-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipPWMState, ROCKCHIP_PWM)

#define ROCKCHIP_PWM_CHANNELS 4

typedef struct RockchipPWMChannel {
    uint32_t period;
    uint32_t duty;
    uint32_t ctrl;
} RockchipPWMChannel;

struct RockchipPWMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    RockchipPWMChannel ch[ROCKCHIP_PWM_CHANNELS];
    uint32_t intsts;
    uint32_t int_en;
};

/*
 * Effective output level of a channel in 1/1000 of the period, taking
 * enable and polarity into account (used e.g. for backlight emulation).
 */
unsigned rockchip_pwm_get_level(RockchipPWMState *s, int ch);

#endif
