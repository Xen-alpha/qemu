/*
 * Rockchip timer block (rk3288-timer style) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_ROCKCHIP_TIMER_H
#define HW_TIMER_ROCKCHIP_TIMER_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_TIMER "rockchip-timer"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipTimerState, ROCKCHIP_TIMER)

#define ROCKCHIP_TIMER_MAX_CHANNELS 6

typedef struct RockchipTimerChannel {
    struct RockchipTimerState *parent;
    int index;
    ptimer_state *ptimer;
    qemu_irq irq;
    uint64_t load;
    uint32_t control;
    uint32_t int_status;
} RockchipTimerChannel;

struct RockchipTimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t num_channels;  /* property */
    uint32_t freq;          /* property, Hz */
    RockchipTimerChannel ch[ROCKCHIP_TIMER_MAX_CHANNELS];
};

#endif
